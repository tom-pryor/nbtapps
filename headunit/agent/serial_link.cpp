// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Bluetooth serial links: see serial_link.hpp.

#include "serial_link.hpp"

#include "frame.hpp"
#include "process.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __QNXNTO__
#include <process.h>
#else
#include <spawn.h>
extern char** environ;
#endif

using namespace nbt::btspp;

namespace nbt {

namespace {

double monoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}
uint32_t monoSec() { return (uint32_t)monoNow(); }

void closeOnExec(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC); }

std::string format(const char* fmt, va_list ap) {
    char line[512];
    vsnprintf(line, sizeof(line), fmt, ap);
    return line;
}

// True once `fd` has something to read or `ms` are over; *ready says which. For the link's socket pair: a node is
// never waited on this way (node_reader.hpp).
bool waitReadable(int fd, int ms, bool* ready) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    int r = select(fd + 1, &rf, NULL, NULL, &tv);
    *ready = r > 0;
    return r >= 0 || errno == EINTR;
}

// ---- one link: the node's reader, and two threads between it and a socket pair -----------------------------------
// The node's reader (NodeReader) reads the node. "up" takes what it read to the agent and owns the pump: it ends
// last, closes everything and frees it. "down" carries what the agent sends to the phone, straight into the node.

// One blocking write to the node. Writes of this size are what the node has been seen to take whole.
const size_t kNodeWrite = 1000;
// How long either thread waits before it looks whether the other one has ended the link.
const int kPumpTickMs = kNodeTickMs;

pthread_mutex_t gPumpsMu = PTHREAD_MUTEX_INITIALIZER;
unsigned gPumps = 0;     // links whose node is still open
unsigned gDialled = 0;   // of those, the ones the car dialled
// Of those, the nodes of the ones phones opened, an entry for each link: a node may be in twice for a moment, as one
// link's threads end and the next link's start. Never freed: a link's threads may still end as the process
// does, after the destructors of its statics have run.
std::vector<std::string>* const gPhoneNodes = new std::vector<std::string>();

void countPump(int by, bool dialled, const std::string& path) {
    pthread_mutex_lock(&gPumpsMu);
    gPumps += by;
    if (dialled) gDialled += by;
    std::vector<std::string>& nodes = *gPhoneNodes;
    if (!dialled && by > 0) nodes.push_back(path);
    for (size_t i = 0; !dialled && by < 0 && i < nodes.size(); ++i) {
        if (nodes[i] != path) continue;
        nodes.erase(nodes.begin() + (long)i);
        break;
    }
    pthread_mutex_unlock(&gPumpsMu);
}

}  // namespace

// What a dialled link's threads and the listener share: one reference each, and the last to let go frees it. The
// threads say when the agent's end closed and when the link has ended; the listener, when they are to stop reading
// and close the node because the car has ended the link.
struct DialledLink {
    pthread_mutex_t mu;
    int refs;
    double goneAt;        // the agent's end closed; 0 while it is open
    bool ended;           // the threads have closed the node
    bool closeNow;        // the listener: stop reading, and close the node
    std::string closeWhy;

    DialledLink() : refs(2), goneAt(0), ended(false), closeNow(false) { pthread_mutex_init(&mu, NULL); }
    ~DialledLink() { pthread_mutex_destroy(&mu); }
    void release() {
        pthread_mutex_lock(&mu);
        bool last = --refs == 0;
        pthread_mutex_unlock(&mu);
        if (last) delete this;
    }
    void close(const std::string& why) {
        pthread_mutex_lock(&mu);
        if (!closeNow) closeWhy = why;
        closeNow = true;
        pthread_mutex_unlock(&mu);
    }
    bool closing(std::string* why) {
        pthread_mutex_lock(&mu);
        bool c = closeNow;
        *why = closeWhy;
        pthread_mutex_unlock(&mu);
        return c;
    }
    void gone(double at) {
        pthread_mutex_lock(&mu);
        if (goneAt == 0) goneAt = at;
        pthread_mutex_unlock(&mu);
    }
    double goneSince() {
        pthread_mutex_lock(&mu);
        double at = goneAt;
        pthread_mutex_unlock(&mu);
        return at;
    }
    void setEnded() {
        pthread_mutex_lock(&mu);
        ended = true;
        pthread_mutex_unlock(&mu);
    }
    bool hasEnded() {
        pthread_mutex_lock(&mu);
        bool e = ended;
        pthread_mutex_unlock(&mu);
        return e;
    }
};

namespace {

struct Pump {
    NodeReader* reader;
    int node, sock;          // node: the reader's descriptor, which "down" writes into
    std::string path;
    SerialLog* log;
    pthread_t down;
    unsigned lingerMs;       // once the agent's end has closed: when the log says that the link is still up
    pthread_mutex_t mu;      // done, why, goneAt, goneWhy
    bool done;
    std::string why;
    double goneAt;           // when the agent's end was found closed; 0 while it is open
    std::string goneWhy;     // what closed it, for the log
    uint64_t fromPhone, toPhone;   // each counted by one thread, and read once both have ended
    uint64_t dropped;              // what the phone sent after the agent's end had closed: "up" counts it
    std::vector<uint8_t> first;    // read from the node before the link was made: "up" sends it on first
    DialledLink* dial;             // a link the car dialled: what the listener shares with it; 0 for a phone's
    NodeWriteFn writeNode;         // a write into the node; 0: write()

    Pump()
        : reader(NULL), node(-1), sock(-1), log(NULL), lingerMs(0), done(false), goneAt(0), fromPhone(0), toPhone(0),
          dropped(0), dial(NULL), writeNode(NULL) {
        pthread_mutex_init(&mu, NULL);
    }
    ~Pump() { pthread_mutex_destroy(&mu); }

    // The link has ended. The first reason given is the one the log gets.
    void end(const std::string& reason) {
        pthread_mutex_lock(&mu);
        if (!done) why = reason;
        done = true;
        pthread_mutex_unlock(&mu);
    }
    bool ended() {
        pthread_mutex_lock(&mu);
        bool d = done;
        pthread_mutex_unlock(&mu);
        return d;
    }
    // The agent's end has closed, or what the phone sends can no longer go to it: from the first call of this the
    // node is kept, and read, until the phone ends the link. The first reason is the one the log gets.
    void agentGone(const std::string& reason) {
        pthread_mutex_lock(&mu);
        if (goneAt == 0) {
            goneAt = monoNow();
            goneWhy = reason;
        }
        double at = goneAt;
        pthread_mutex_unlock(&mu);
        if (dial != NULL) dial->gone(at);
    }
    // When that was, and why; 0 while the agent has its end.
    double goneSince(std::string* reason) {
        pthread_mutex_lock(&mu);
        double at = goneAt;
        *reason = goneWhy;
        pthread_mutex_unlock(&mu);
        return at;
    }
};

long plainWrite(int fd, const void* data, size_t len) { return (long)write(fd, data, len); }

// All of it, however many calls that takes: a node may take a write short. False if `fd` will not take it.
bool writeAll(int fd, const uint8_t* data, size_t len, NodeWriteFn put = 0) {
    if (put == 0) put = &plainWrite;
    size_t off = 0;
    while (off < len) {
        long k = put(fd, data + off, len - off);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        off += (size_t)k;
    }
    return true;
}

// What the agent sends, to the phone: small blocking writes. It goes on until the agent's end has closed and all
// it sent has gone out, so that a last answer before a close still reaches the phone. The link does not end with
// that: "up" keeps the node from there, for the phone to end the link.
void* pumpDown(void* arg) {
    Pump* p = static_cast<Pump*>(arg);
    uint8_t buf[kNodeWrite];
    for (;;) {
        bool ready = false;
        if (!waitReadable(p->sock, kPumpTickMs, &ready)) {
            p->agentGone("the link's socket failed");
            break;
        }
        if (!ready) {
            if (p->ended()) break;
            continue;
        }
        ssize_t k = read(p->sock, buf, sizeof(buf));
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (k <= 0) {
            p->agentGone("closed by the agent");
            break;
        }
        if (!writeAll(p->node, buf, (size_t)k, p->writeNode)) {
            // The agent is told that the link has gone; the node stays with "up" until the link has ended.
            p->agentGone(std::string("a write to the node failed (") + strerror(errno) + ")");
            shutdown(p->sock, SHUT_RDWR);
            break;
        }
        p->toPhone += (uint64_t)k;
    }
    return NULL;
}

// How a link ended whose phone ended it: `how` alone while the agent had its end, and after that what closed the
// agent's end, with how long the phone took to follow it.
std::string endedByPhone(const char* how, double goneAt, const std::string& goneWhy) {
    if (goneAt == 0) return how;
    char text[64];
    snprintf(text, sizeof(text), ", and the phone ended the link %.0f ms later", (monoNow() - goneAt) * 1000.0);
    return goneWhy + text;
}

// What the phone sends, to the agent: what the node's reader has read, as it comes.
//
// Once the agent's end has closed, for whatever reason, the node is kept open and read, and what comes is dropped,
// until the phone ends the link, however long that takes. Closing a node does not end its link, and what the phone
// goes on sending into a link whose node was closed waits ahead of the node and comes on the next link that is
// opened. So the node is not closed under a link that is up: the agent tells the phone that it is closing
// (link.closing, agent.hpp), and the phone ends the link when it hears that. When `lingerMs` have passed with the
// link still up, the log says so.
void* pumpUp(void* arg) {
    Pump* p = static_cast<Pump*>(arg);
    if (!p->first.empty()) {
        if (writeAll(p->sock, &p->first[0], p->first.size())) {
            p->fromPhone += p->first.size();
        } else {
            p->agentGone("closed by the agent");
            p->dropped += p->first.size();
        }
    }
    // A dialled link is ended by the car when the agent ends it: the listener sees to that, and the log has no
    // line about a link that stays up.
    bool saidStillUp = p->dial != NULL;
    std::vector<uint8_t> got;
    for (;;) {
        std::string closeWhy;
        if (p->dial != NULL && p->dial->closing(&closeWhy)) {
            std::string goneWhy;
            p->end(endedByPhone(closeWhy.c_str(), p->goneSince(&goneWhy), goneWhy));
            break;
        }
        got.clear();
        NodeReader::Got what = p->reader->next(kPumpTickMs, &got);
        std::string goneWhy;
        double gone = p->goneSince(&goneWhy);
        if (what == NodeReader::kEnded) {
            int error = p->reader->endError();
            // On the car a link that ends takes its node away, and the read then fails.
            if (error == 0) p->end(endedByPhone("closed by the phone", gone, goneWhy));
            else if (gone > 0 || access(p->path.c_str(), F_OK) != 0) p->end(endedByPhone("the phone's link ended", gone, goneWhy));
            else p->end(std::string("a read of the node failed: ") + strerror(error));
            break;
        }
        if (what == NodeReader::kData) {
            if (gone > 0) {
                p->dropped += got.size();
            } else if (!writeAll(p->sock, &got[0], got.size())) {
                p->agentGone("closed by the agent");
                p->dropped += got.size();
            } else {
                p->fromPhone += got.size();
            }
        }
        if (gone > 0 && !saidStillUp && (monoNow() - gone) * 1000.0 >= (double)p->lingerMs) {
            saidStillUp = true;
            char text[200];
            snprintf(text, sizeof(text), ": the link is still up %u ms after the agent's end closed (%s): its node is "
                     "kept, and read, until the phone ends the link", p->lingerMs, goneWhy.c_str());
            p->log->line(p->path + text);
        }
    }
    // The agent sees its end close once it has read what is still on the way to it.
    shutdown(p->sock, SHUT_WR);
    pthread_join(p->down, NULL);
    p->reader->finish();
    close(p->sock);
    char counts[160];
    snprintf(counts, sizeof(counts), ": %llu B from the phone, %llu B to it", (unsigned long long)p->fromPhone,
             (unsigned long long)p->toPhone);
    std::string line = p->path + " closed, " + p->why + counts;
    if (p->dropped != 0) {
        snprintf(counts, sizeof(counts), ", and %llu B that the phone sent after the agent's close were dropped",
                 (unsigned long long)p->dropped);
        line += counts;
    }
    p->log->line(line);
    bool dialled = p->dial != NULL;
    if (dialled) {
        p->dial->setEnded();
        p->dial->release();
    }
    std::string path = p->path;
    delete p;
    countPump(-1, dialled, path);   // last: nothing of the link is touched after this
    return NULL;
}

}  // namespace

namespace {

// The agent's end of a serial link: a socket, which also knows the phone's address, and whether the car dialled it.
class SerialSocketLink : public SocketLink {
public:
    SerialSocketLink(int fd, const std::string& address, bool dialled)
        : SocketLink(fd, "serial"), address_(address), dialled_(dialled) {}
    virtual std::string phoneAddress() const { return address_; }
    virtual bool dialledByCar() const { return dialled_; }

private:
    std::string address_;
    bool dialled_;
};

}  // namespace

// newSerialLink(), and for a link the car dialled, `dial`: its threads take one of its references, and give it up
// when they have ended, or here if they cannot be started.
static Link* startLink(NodeReader* reader, const std::string& path, SerialLog* log, const std::vector<uint8_t>& first,
                       unsigned lingerMs, const std::string& address, DialledLink* dial, NodeWriteFn writeNode) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        log->line(path + ": no socket pair for the link: " + strerror(errno));
        reader->finish();
        if (dial != NULL) dial->release();
        return 0;
    }
    closeOnExec(pair[0]);
    closeOnExec(pair[1]);
    // Room for what arrives while the agent is busy with something else. A system that will not give it is left
    // with what it has.
    int room = 65536;
    setsockopt(pair[0], SOL_SOCKET, SO_RCVBUF, &room, sizeof(room));
    setsockopt(pair[1], SOL_SOCKET, SO_SNDBUF, &room, sizeof(room));

    countPump(1, dial != NULL, path);
    Pump* p = new Pump();
    p->reader = reader;
    p->node = reader->fd();
    p->sock = pair[1];
    p->path = path;
    p->log = log;
    p->first = first;
    p->lingerMs = lingerMs;
    p->dial = dial;
    p->writeNode = writeNode;
    bool down = pthread_create(&p->down, NULL, &pumpDown, p) == 0;
    bool up = false;
    if (down) {
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_t t;
        up = pthread_create(&t, &at, &pumpUp, p) == 0;
        pthread_attr_destroy(&at);
    }
    if (!up) {
        log->line(path + ": the link's threads could not be started");
        if (down) {
            p->end("not started");
            pthread_join(p->down, NULL);
        }
        reader->finish();
        close(pair[0]);
        close(pair[1]);
        delete p;
        countPump(-1, dial != NULL, path);
        if (dial != NULL) dial->release();
        return 0;
    }
    return new SerialSocketLink(pair[0], address, dial != NULL);
}

Link* newSerialLink(NodeReader* reader, const std::string& path, SerialLog* log, const std::vector<uint8_t>& first,
                    unsigned lingerMs, const std::string& address) {
    return startLink(reader, path, log, first, lingerMs, address, NULL, NULL);
}

Link* newSerialLink(int nodeFd, const std::string& path, SerialLog* log, const std::vector<uint8_t>& first,
                    unsigned lingerMs) {
    NodeReader* reader = NodeReader::start(nodeFd, path, log);
    if (reader == 0) return 0;
    return newSerialLink(reader, path, log, first, lingerMs);
}

unsigned serialLinksOpen() {
    pthread_mutex_lock(&gPumpsMu);
    unsigned n = gPumps;
    pthread_mutex_unlock(&gPumpsMu);
    return n;
}

unsigned dialledLinksOpen() {
    pthread_mutex_lock(&gPumpsMu);
    unsigned n = gDialled;
    pthread_mutex_unlock(&gPumpsMu);
    return n;
}

bool phoneLinkOn(const std::string& path) {
    pthread_mutex_lock(&gPumpsMu);
    bool on = false;
    const std::vector<std::string>& nodes = *gPhoneNodes;
    for (size_t i = 0; i < nodes.size() && !on; ++i) on = nodes[i] == path;
    pthread_mutex_unlock(&gPumpsMu);
    return on;
}

bool waitForSerialLinks(int ms) {
    double end = monoNow() + ms / 1000.0;
    while (serialLinksOpen() != 0 && monoNow() < end) usleep(10000);
    return serialLinksOpen() == 0;
}

// ---- the gate ----------------------------------------------------------------------------------------------------

namespace {
// Opens by other programs are logged one by one up to this many; after that they are only counted.
const unsigned kGateLogged = 24;
}  // namespace

SppNodeGate::SppNodeGate(const std::string& self, SerialLog* log)
    : self_(self), log_(log), node_(false), logged_(0), otherAt_(0), buffering_(false), attacher_() {
    pthread_mutex_init(&mu_, NULL);
}

SppNodeGate::~SppNodeGate() {
    detach();
    pthread_mutex_destroy(&mu_);
}

void SppNodeGate::say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string line = format(fmt, ap);
    va_end(ap);
    sayLine(line);
}

void SppNodeGate::sayLine(const std::string& line) {
    pthread_mutex_lock(&mu_);
    bool hold = buffering_ && pthread_equal(attacher_, pthread_self());
    if (hold) held_.push_back(line);
    pthread_mutex_unlock(&mu_);
    if (!hold) log_->line(line);
}

// The gate's thread: an open by another program has reached the gate.
void SppNodeGate::onOpen(const GateOpen& o, void* self) {
    SppNodeGate* g = static_cast<SppNodeGate*>(self);
    // The gate's own check, which is this program run again: counted, and said by the check itself.
    bool ownLook = o.kind == GateOpen::kLook && !o.program.empty() && o.program == programName(g->self_.c_str());
    pthread_mutex_lock(&g->mu_);
    Count& c = g->by_[o.program];
    ++c.opens;
    if (o.answer == EACCES) ++c.refused;
    // Another program that would read a link there: for a link that is let go (SerialListener::otherReader()).
    if (!ownLook && gateOpenReads(o)) {
        g->otherAt_ = monoNow();
        g->other_ = o.program.empty() ? "a program whose name could not be read" : o.program;
    }
    unsigned n = ownLook ? 0 : ++g->logged_;
    pthread_mutex_unlock(&g->mu_);
    if (n == 0 || n > kGateLogged) return;
    g->sayLine(describeOpen(o, kGateBlock, kGateRefuseDefault));
    if (n == kGateLogged) g->sayLine("gate: further opens are counted and no longer logged one by one");
}

// Runs `self --look <path>` as a process of its own and waits for it. False (errno set) if it did not start.
bool SppNodeGate::look(long* pid, int* exitCode) {
#ifdef __QNXNTO__
    pid_t p = spawnlp(P_NOWAIT, self_.c_str(), self_.c_str(), "--look", kGatePath, (char*)NULL);
    if (p == -1) return false;
#else
    pid_t p = -1;
    char* const args[] = { const_cast<char*>(self_.c_str()), const_cast<char*>("--look"),
                           const_cast<char*>(kGatePath), NULL };
    int e = posix_spawnp(&p, self_.c_str(), NULL, NULL, args, environ);
    if (e != 0) { errno = e; return false; }
#endif
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR) return false;
    }
    *pid = (long)p;
    *exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return true;
}

// Looks at the gate's path from a process of its own and says what answered. *looked: the look could be made.
GateCheck SppNodeGate::check(const char* when, bool* looked) {
    long pid = 0;
    int rc = 0;
    *looked = look(&pid, &rc);
    if (!*looked) {
        say("gate check (%s): the look could not be started (%s)", when, strerror(errno));
        return kCheckUnclear;
    }
    GateCheck c = judgeGateCheck(gate_.sawPid(pid), rc);
    say("gate check (%s): %s", when, gateCheckText(c));
    return c;
}

// A listener that cannot attach the gate tries again every half minute: what an attach that fails says is logged
// only when it fails otherwise than the one before. Only this thread's lines wait: what the gate's thread says while
// the attach runs is not part of the attach, and say() logs it at once.
bool SppNodeGate::attach(std::string* why) {
    if (gate_.attached()) return true;
    pthread_mutex_lock(&mu_);
    buffering_ = true;
    attacher_ = pthread_self();
    pthread_mutex_unlock(&mu_);
    bool ok = attachNow(why);
    std::vector<std::string> lines;
    pthread_mutex_lock(&mu_);
    buffering_ = false;
    lines.swap(held_);
    pthread_mutex_unlock(&mu_);
    if (!ok && *why == lastFailure_) return false;
    lastFailure_ = ok ? std::string() : *why;
    for (size_t i = 0; i < lines.size(); ++i) log_->line(lines[i]);
    return ok;
}

bool SppNodeGate::attachNow(std::string* why) {
    // This process's own look at the path goes past its gate, now and later: it shows the stack's node only.
    node_ = access(kGatePath, F_OK) == 0;
    if (node_)
        say("%s has a link already: a program that has it open keeps it, the gate only meets opens from now on",
            kGatePath);
    gate_.configure(kGateBlock, kGateRefuseDefault, &SppNodeGate::onOpen, this);
    if (!gate_.attach(kGatePath, why)) {
        *why = std::string("the gate on ") + kGatePath + " could not be attached: " + *why;
        return false;
    }
    say("gate on %s: opens by %s are refused and every other open is passed on. While it is there the car maker's "
        "phone apps do not work over Bluetooth", kGatePath, kGateRefuseDefault);
    bool looked = false;
    GateCheck c = check("just attached", &looked);
    if (looked && (c == kCheckFrontNoNode || c == kCheckFrontNode)) return true;
    gate_.detach();
    say("gate removed from %s", kGatePath);
    *why = looked ? std::string("the gate is not in front of ") + kGatePath + ": " + gateCheckText(c)
                  : std::string("whether the gate is in front of ") + kGatePath + " could not be checked";
    return false;
}

// When a link's node appears behind the gate, the gate is checked again: the stack attaches the node at that
// moment, after the gate, and the gate has to stay in front of it. If it has not, it is attached anew, which makes
// it the later of the two.
void SppNodeGate::watch() {
    if (!gate_.attached()) return;
    bool there = access(kGatePath, F_OK) == 0;
    if (there == node_) return;
    node_ = there;
    if (!there) return;
    bool looked = false;
    if (!gateNotInFront(check("a link's node is there", &looked))) return;
    say("gate: attaching it again, to get back in front of the node");
    gate_.detach();
    std::string why;
    if (!gate_.attach(kGatePath, &why)) {
        say("gate: it could not be attached again: %s", why.c_str());
        return;
    }
    if (gateNotInFront(check("attached again", &looked)))
        say("gate: still not in front. Opens of %s are neither seen nor refused while this link is up", kGatePath);
}

// The hold is the gate's own, and stays as it is when watch() attaches the gate again.
void SppNodeGate::hold(const std::vector<std::string>& programs) {
    gate_.setHolds(programs);
    gate_.hold(true);
}

void SppNodeGate::release() { gate_.hold(false); }

bool SppNodeGate::sawOtherOpen(double since, std::string* who) {
    pthread_mutex_lock(&mu_);
    bool saw = otherAt_ != 0 && otherAt_ >= since;
    if (saw) *who = other_;
    pthread_mutex_unlock(&mu_);
    return saw;
}

void SppNodeGate::detach() {
    if (!gate_.attached()) return;
    gate_.detach();
    pthread_mutex_lock(&mu_);
    std::map<std::string, Count> by = by_;
    by_.clear();
    logged_ = 0;
    pthread_mutex_unlock(&mu_);
    for (std::map<std::string, Count>::const_iterator it = by.begin(); it != by.end(); ++it)
        say("gate: in all %u by %s, %u of them refused", it->second.opens,
            it->first.empty() ? "programs whose name could not be read" : it->first.c_str(), it->second.refused);
    say("gate removed from %s", kGatePath);
}

// ---- the listener ------------------------------------------------------------------------------------------------

SerialListener::SerialListener(GapClient* gap, NodeGate* gate, SerialLog* log, const SerialConfig& config)
    : gate_(gate), log_(log), services_(0), serverLook_(0), config_(config), primer_(config.nodeCalls), spp_(gap, this),
      wakeRead_(-1), wakeWrite_(-1),
      hasThread_(false), watchFrom_(0), decidingNode_(-1), stop_(false), active_(false),
      tracker_(config.requestWindowS), hotUntil_(0),
      reqAt_(0), refused_(0), deciding_(false), held_(false), holdWhy_(0), reading_(0), resetHotUntil_(0), askedAt_(0),
      checkedAt_(0), opened_(false), final_(false), unattachedSince_(0), everAttached_(false), attachSaid_(false), lookDone_(false),
      scannedAt_(0), refusalCount_(0), refusalNode_(RefusalFacts::kNoNode), refusalAt_(0), handLookAt_(0), ours_(0),
      rules_(config.dial), dialPhase_(kDialNone), dialReader_(0), dialOpenedAt_(0), dialLink_(0), discAt_(0),
      discAnswered_(false), discError_(0), staleAtStart_(false), cleanupKind_(kCleanOwn), leftFailing_(false),
      leftTimes_(0), cleanupQuiet_(false), leftPhase_(kLeftNone), leftAt_(0), leftTried_(false), pairingOpen_(false),
      checkedAtShared_(0), lastLinks_(0), problems_(0), restoredSeen_(0), problemsSaid_(false), repointNumber_(0),
      repointDialError_(0), repointStopped_(false), repointDisconnect_(false) {
    for (int i = 0; i < NodeTracker::kNodes; ++i) {
        watched_[i] = false;
        appeared_[i] = 0;
    }
    // A ServiceDisconnect with no answer in the time the dial waits for one is given up, so that the next may go.
    spp_.setDisconnectReplyTime(config.dial.disconnectWaitMs / 1000.0);
    pthread_mutex_init(&mu_, NULL);
    pthread_cond_init(&wake_, NULL);
}

SerialListener::~SerialListener() {
    stop();
    if (wakeRead_ >= 0) close(wakeRead_);
    if (wakeWrite_ >= 0) close(wakeWrite_);
    for (size_t i = 0; i < ready_.size(); ++i) delete ready_[i];
    pthread_cond_destroy(&wake_);
    pthread_mutex_destroy(&mu_);
}

void SerialListener::say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string line = format(fmt, ap);
    va_end(ap);
    log_->line(line);
}

std::string SerialListener::nodePath(int i) const {
    char digit[8];
    snprintf(digit, sizeof(digit), "%d", i);
    return config_.nodePrefix + digit;
}

void SerialListener::scanNodes(bool present[NodeTracker::kNodes]) const {
    for (int i = 0; i < NodeTracker::kNodes; ++i) present[i] = access(nodePath(i).c_str(), F_OK) == 0;
}

bool SerialListener::active() {
    pthread_mutex_lock(&mu_);
    bool a = active_;
    pthread_mutex_unlock(&mu_);
    return a;
}

bool SerialListener::stopping() {
    pthread_mutex_lock(&mu_);
    bool s = stop_;
    pthread_mutex_unlock(&mu_);
    return s;
}

bool SerialListener::start(std::string* why) {
    if (hasThread_ || active()) {
        *why = "the serial listener is started already";
        return false;
    }
    if (wakeRead_ < 0) {
        int ends[2];
        if (pipe(ends) != 0) {
            *why = std::string("no pipe for the serial listener: ") + strerror(errno);
            return false;
        }
        wakeRead_ = ends[0];
        wakeWrite_ = ends[1];
        for (int i = 0; i < 2; ++i) {
            closeOnExec(ends[i]);
            fcntl(ends[i], F_SETFL, fcntl(ends[i], F_GETFL, 0) | O_NONBLOCK);
        }
    }
    bool present[NodeTracker::kNodes];
    scanNodes(present);
    pthread_mutex_lock(&mu_);
    tracker_ = NodeTracker(config_.requestWindowS);
    tracker_.baseline(present);
    stop_ = false;
    pthread_mutex_unlock(&mu_);
    for (int i = 0; i < NodeTracker::kNodes; ++i)
        if (present[i]) say("%s exists already: someone else's link, left alone", nodePath(i).c_str());

    spp_.configure(config_.name, config_.uuid, 0);
    spp_.setPairingInforms(config_.dial.on);
    // A link on the dialled instance from before this listener: ended once the car's phones are known.
    staleAtStart_ = config_.dial.on && access(config_.dial.node.c_str(), F_OK) == 0;
    if (staleAtStart_)
        say("%s exists already: a link an earlier agent left behind. It is ended once the car's phones are known, and "
            "never taken as the agent's", config_.dial.node.c_str());
    std::string failed;
    if (!setUp(&failed)) {
        if (config_.retryMs == 0) {
            *why = failed;
            return false;
        }
        if (final_) {
            *why = failed + ". Bluetooth serial is off until the agent is started again";
            return false;
        }
        // The listener's thread tries again: an agent that starts with the head unit may be there before the
        // car's Bluetooth service and its library are.
        say("Bluetooth serial is not set up: %s. Again in %u s, and each time it is tried the log says why only if "
            "that has changed", failed.c_str(), config_.retryMs / 1000);
        lastTry_ = failed;
    }
    hasThread_ = pthread_create(&thread_, NULL, &SerialListener::threadMain, this) == 0;
    if (!hasThread_) {
        if (active()) shutDown();
        *why = "the serial listener's thread could not be started";
        return false;
    }
    return true;
}

// Puts the gate in front and asks for the service: on the client that is open, or on one opened now. False with
// *why, and nothing left behind, if either does not work.
bool SerialListener::setUp(std::string* why) {
    // Before the service is registered, so that no link of ours can be there ahead of the gate.
    if (!gate_->attach(why)) return false;
    pthread_mutex_lock(&mu_);
    active_ = true;
    pthread_mutex_unlock(&mu_);
    unattachedSince_ = spp_.attached() ? 0 : monoNow();
    if (opened_) {
        spp_.again();
        return true;
    }
    if (spp_.start()) {
        opened_ = true;
        return true;
    }
    pthread_mutex_lock(&mu_);
    active_ = false;
    pthread_mutex_unlock(&mu_);
    gate_->detach();
    final_ = spp_.startFinal();
    *why = final_ ? "the client of the car's Bluetooth service failed to open and cannot be opened again in this run"
                  : "no connection to the car's Bluetooth service";
    return false;
}

void SerialListener::stop() {
    pthread_mutex_lock(&mu_);
    stop_ = true;
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
    if (hasThread_) pthread_join(thread_, NULL);
    hasThread_ = false;
    if (active()) shutDown();
    endDial("the listener stops");
    flushProblems();
    closeKept(kEvery);
    // A link that was opened and that the agent has not taken: nobody serves it from here on.
    std::vector<Link*> unserved;
    pthread_mutex_lock(&mu_);
    unserved.swap(ready_);
    pthread_mutex_unlock(&mu_);
    for (size_t i = 0; i < unserved.size(); ++i) {
        char byte;
        if (read(wakeRead_, &byte, 1) < 0) {}   // the byte that announced it
        delete unserved[i];
    }
    if (!unserved.empty())
        say("%u link%s had been opened and not yet taken by the agent: ended, with nothing said on %s",
            (unsigned)unserved.size(), unserved.size() == 1 ? "" : "s", unserved.size() == 1 ? "it" : "them");
}

// Takes the service and the gate away again: the service first, as it came second. On the thread that ends the
// listener, which is its own when it gives up and the caller's of stop() otherwise; never both.
void SerialListener::shutDown() {
    // A link on the dialled instance keeps the registration from being removed: it goes first.
    endDial("Bluetooth serial stops");
    SppService::CheckCounts c = spp_.checkCounts();
    if (c.sent > 0) {
        char odd[160] = "";
        if (c.lastOddTimes > 0) {
            char type[48];
            snprintf(type, sizeof(type), "a rejection (type 0x%x)", c.lastOddRejected);
            snprintf(odd, sizeof(odd), " (the last: %s, %u time%s in a row)",
                     c.lastOddRejected != 0 ? type : registerErrorText(c.lastOdd).c_str(), c.lastOddTimes,
                     c.lastOddTimes == 1 ? "" : "s");
        }
        say("registration checks so far: %u sent; %u found it there, %u found it dropped and registered it again, %u "
            "found the stack busy, %u had another answer%s, %u had none", c.sent, c.present, c.restored, c.busy,
            c.other, odd, c.unanswered);
    }
    if (spp_.remove("leaving")) {
        double end = monoNow() + 3.0;
        SppService::State s = spp_.state();
        while (s != SppService::kRemoved && s != SppService::kRemoveFailed && monoNow() < end) {
            usleep(20000);
            s = spp_.state();
        }
        uint32_t err = spp_.removeError();
        if (s == SppService::kRemoveFailed)
            say("the car's Bluetooth service did not remove the registration of '%s' (%s): it stays until the car's "
                "Bluetooth is switched off and on or restarts, or an agent removes it", config_.name.c_str(),
                deregisterErrorText(err).c_str());
        else if (s == SppService::kRemoved && err == 10)
            say("'%s' was not registered any more: the car's Bluetooth had dropped the registration, as switching "
                "Bluetooth off and on in iDrive does, so there was nothing to remove", config_.name.c_str());
        else if (s != SppService::kRemoved)
            say("no reply to the removal of '%s' in 3 s: it may stay registered until the car's Bluetooth service "
                "restarts", config_.name.c_str());
    }
    // From here no phone's request turns the hold on again.
    pthread_mutex_lock(&mu_);
    active_ = false;
    bool held = held_;
    held_ = false;
    deciding_ = false;
    decidingNode_ = -1;
    reqAt_ = 0;
    pthread_mutex_unlock(&mu_);
    if (held) gate_->release();
    gate_->detach();
    closeKept(kAllButUp);
}

Link* SerialListener::accept() {
    pthread_mutex_lock(&mu_);
    Link* link = 0;
    if (!ready_.empty()) {
        link = ready_.front();
        ready_.erase(ready_.begin());
    }
    pthread_mutex_unlock(&mu_);
    if (link != 0) {
        char byte;
        if (read(wakeRead_, &byte, 1) < 0) {}   // the byte that announced it
    }
    return link;
}

// ---- what the registration reports, on the Bluetooth service's thread ----------------------------------------

void SerialListener::sppSay(const std::string& line) { log_->line(line); }

void SerialListener::sppState(SppService::State) {
    pthread_mutex_lock(&mu_);
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
}

void SerialListener::sppPhone(SppService::Phone what, const std::string& service, bool parsed,
                              const std::string& address) {
    // The stack does not say more than this about whose link it will be: what the phone says first decides. A request
    // that names neither never brings a serial node (audio, hands-free).
    bool maybeOurs = service == config_.name || service == config_.carService;
    // Or the rule does, which leaves the link to another program without a look at it (leftToOther()).
    std::string leaveWhy;
    bool leave = what == SppService::kPhoneAsked && maybeOurs && leftToOther(address, &leaveWhy);
    pthread_mutex_lock(&mu_);
    double now = monoNow();
    // The car ends a phone's pending request when its own dial to the phone comes up, and says so just before the
    // dial's answer: that is no refusal.
    bool overtaken = what == SppService::kPhoneRefused && maybeOurs && config_.dial.on &&
                     rules_.overtakes((int64_t)(now * 1000.0));
    pthread_mutex_unlock(&mu_);
    if (overtaken)
        say("a phone's request for a serial link (service '%s') ended as the car's own dial came up: not a refusal%s",
            service.c_str(), parsed ? "" : " (the message was not read whole)");
    else
        say("a phone %s (service '%s')%s%s",
            what == SppService::kPhoneAsked ? "asked for a serial link"
                                            : (what == SppService::kPhoneLinked ? "got its serial link"
                                                                                : "was refused a serial link by the car"),
            service.c_str(), maybeOurs || what != SppService::kPhoneAsked ? "" : ": another service's, left alone",
            parsed ? "" : " (the message was not read whole)");
    pthread_mutex_lock(&mu_);
    unsigned refused = 0;
    if (overtaken) {
        tracker_.onRefused(true);
        reqAt_ = 0;   // no link comes of the request: the dial's is the link
        holdWhy_ = "the phone's request ended as the car's own dial came up";
    } else if (what == SppService::kPhoneAsked) {
        askedAt_ = now;   // for any service: the registration check keeps out of the way of a phone that asks
        // Only a request that may bring a serial node is kept for the nodes: one for another service would stay for
        // the tracker's window and have the next serial node, which may be the agent's, left alone.
        if (maybeOurs) tracker_.onPhoneRequest(monoSec(), true, address, leave);
        // A request whose link is left to another program is not one whose link may be the agent's: no hold, and
        // nothing for the dial to wait for.
        if (maybeOurs && !leave) {
            reqAt_ = now;
            holdWhy_ = "a phone has asked for a link";
        }
        // The car is dialled for that phone if no link of the agent's comes of it: the car names the same service
        // for a link to the agent's UUID as for any other, so whose UUID it was for cannot be told. That is how the
        // app is reached when the phone's own link is left to another program.
        if (config_.dial.on) rules_.phoneAsked(address, (int64_t)(now * 1000.0));
    } else if (what == SppService::kPhoneRefused) {
        if (maybeOurs) {
            tracker_.onRefused(true);
            refused = ++refused_;
            reqAt_ = 0;   // no link comes of it
            holdWhy_ = "the car refused the phone's link";
            char text[80];
            snprintf(text, sizeof text, "the car refused a phone's serial link (%u so far)", refused);
            problem("refusal", text);
        }
    }
    // The link's node appears some tens of milliseconds after the request, and has to be opened at once; or, left to
    // another program, marked so before it would be taken.
    if (maybeOurs && what != SppService::kPhoneRefused) {
        hotUntil_ = now + config_.linkWaitMs / 1000.0;
        pthread_cond_signal(&wake_);
    }
    pthread_mutex_unlock(&mu_);
    // On, before the car accepts the link and its node is there; or off again, if the car has refused it.
    if (maybeOurs && what != SppService::kPhoneLinked && !leave) updateHold();
    // What can be found out about it, for the first refusal and every 20th: the serial nodes there now, and what
    // the car's device list says of the phone, which the line waits for.
    if (refused == 1 || (refused && refused % 20 == 0)) {
        RefusalFacts::Node node = RefusalFacts::kNoNode;
        pthread_mutex_lock(&mu_);
        for (int i = 0; i < NodeTracker::kNodes; ++i) {
            NodeTracker::State st = tracker_.state(i);
            if (st == NodeTracker::kForeign || st == NodeTracker::kPending) node = RefusalFacts::kNodeOther;
            else if (st == NodeTracker::kOurs && node == RefusalFacts::kNoNode) node = RefusalFacts::kNodeOurs;
        }
        refusalCount_ = refused;
        refusalNode_ = node;
        refusalAt_ = monoNow();
        pthread_mutex_unlock(&mu_);
        if (!spp_.askAboutRefused()) sayRefusal(RefusalFacts::kListNotAsked, PhoneInList());
    }
}

// The car's device list has come for the refusal that waits for it: on the Bluetooth service's thread.
void SerialListener::sppRefusedPhone(const PhoneInList& phone) { sayRefusal(RefusalFacts::kListRead, phone); }

namespace {
// How the log says what a device list changed for a phone: whether it is there, and its security, which is what
// the car dialling it goes by. The phone by its number only.
std::string presenceLine(const Presence::Change& c) {
    char text[200];
    if (c.removed) {
        snprintf(text, sizeof text, "the car's device list: phone %u is no longer in it", c.number);
    } else {
        snprintf(text, sizeof text, "the car's device list: phone %u is %s (connection %u), security %u%s", c.number,
                 connectionName(c.connection), c.connection, c.security,
                 c.security == kSecurityKeyMissing ? ": the car has its key and the phone has not" : "");
    }
    return text;
}
}  // namespace

// A device list has come: on the Bluetooth service's thread. What it says of the phones is kept; the log says
// what has changed of whether each is there, and of its security, and when a phone's dials stop for it.
void SerialListener::sppDevices(const std::vector<BtDevice>& devices) {
    if (!config_.dial.on) return;
    std::vector<Presence::Change> changes;
    std::vector<unsigned> troubled;
    pthread_mutex_lock(&mu_);
    rules_.devices(devices, (int64_t)(monoNow() * 1000.0), &changes, &troubled);
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
    for (size_t i = 0; i < changes.size(); ++i) {
        const Presence::Change& c = changes[i];
        bool there = (c.connectionBefore >= kConnConnected) != (c.connection >= kConnConnected);
        if (c.added || c.removed || there || c.securityChanged) say("%s", presenceLine(c).c_str());
    }
    for (size_t i = 0; i < troubled.size(); ++i)
        say("phone %u is not dialled until the car's device list shows it paired again after a change: a dial would "
            "have the car start a new pairing, with a prompt on both screens", troubled[i]);
}

// What the car says that the dial waits for, on the Bluetooth service's thread: kept for the listener's thread.
void SerialListener::pairingWindow(bool open) {
    if (!config_.dial.on) return;
    pthread_mutex_lock(&mu_);
    bool changed = pairingOpen_ != open;
    pairingOpen_ = open;
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
    if (!changed) return;
    if (open)
        say("the pairing window is open: every phone connected to the car and paired with it is dialled in turn, every "
            "%u s", config_.dial.pairingRoundMs / 1000);
    else
        say("the pairing window has closed: the car dials the agent's own phones only");
}

void SerialListener::sppConnectReply(const ServiceConnectReply& reply) {
    int64_t at = (int64_t)(monoNow() * 1000.0);
    // The re-point first, before the answer is queued or anything is said: it helps only if it reaches the car before
    // the next phone-opened link ends.
    RepointWent repoint = repointAfter(reply);
    if (reply.service == kRepointService && addressKey(reply.address) == addressKey(kRepointAddress)) {
        repointAnswered(reply, at);
        return;
    }
    pthread_mutex_lock(&mu_);
    connectReplies_.push_back(ConnectAnswer(reply, at, repoint));
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
}

// The answer is the car's to a dial of the agent's: SppService tells only the answer to its own ServiceConnect, and
// every one of the agent's that names the agent's service is a dial, given up for no answer or not. A failure that
// leaves the car's record doubtful brings the re-point, unless the re-points have stopped. The listener's lock is let
// go before the request is sent: a client may answer it within the send, on this thread.
SerialListener::RepointWent SerialListener::repointAfter(const ServiceConnectReply& reply) {
    if (!config_.dial.on || reply.service != config_.name || !DialRules::doubtingAnswer(reply.error)) return kRepointNone;
    int64_t now = (int64_t)(monoNow() * 1000.0);
    pthread_mutex_lock(&mu_);
    bool stopped = repointStopped_;
    // One ServiceConnect of the agent's at a time, and this answer has just ended the one that was out: none is out.
    bool out = rules_.repointOut();
    if (!stopped && !out) {
        rules_.repointSent(now);
        repointNumber_ = rules_.number(reply.address);
        repointDialError_ = reply.error;
        ++counts_.repoints;
    }
    pthread_mutex_unlock(&mu_);
    if (stopped) return kRepointStopped;
    if (out) return kRepointNone;
    if (spp_.connectService(kDialDevice, kDialInstance, kRepointAddress, kRepointService)) return kRepointWent;
    // Not sent at all: that is nothing the car did, so it is only counted.
    RepointOutcome o;
    o.end = kRepointUnsent;
    o.dialError = reply.error;
    o.why = spp_.attached() ? "the car's Bluetooth service did not take the request"
                            : "the car's Bluetooth service is not attached";
    pthread_mutex_lock(&mu_);
    rules_.repointDone();
    o.number = repointNumber_;
    --counts_.repoints;
    ++counts_.repointsNotSent;
    repointEnds_.push_back(o);
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
    return kRepointNotSent;
}

// What is expected of the car is 5 within repointSlowMs; anything else stops the re-points for the rest of the run.
// An answer that comes when none is out is one to a re-point that was given up, and passed over.
void SerialListener::repointAnswered(const ServiceConnectReply& reply, int64_t at) {
    pthread_mutex_lock(&mu_);
    if (!rules_.repointOut()) {
        pthread_mutex_unlock(&mu_);
        return;
    }
    RepointOutcome o;
    o.error = reply.error;
    o.ms = at - rules_.repointAt();
    o.number = repointNumber_;
    o.dialError = repointDialError_;
    if (reply.error == kNoError) o.end = kRepointNoCode;
    else if (reply.error != 5) o.end = kRepointOther;
    else if (o.ms >= (int64_t)config_.dial.repointSlowMs) o.end = kRepointSlow;
    else o.end = kRepointExpected;
    rules_.repointDone();
    if (o.end == kRepointExpected) {
        ++counts_.repointsExpected;
    } else {
        ++counts_.repointsUnexpected;
        repointStopped_ = true;
    }
    repointEnds_.push_back(o);
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
}

void SerialListener::sppDisconnectReply(const ServiceDisconnectReply& reply) {
    pthread_mutex_lock(&mu_);
    disconnectReplies_.push_back(reply);
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
}

void SerialListener::sppServiceList(const ServiceList& list) {
    if (!config_.dial.on) return;
    pthread_mutex_lock(&mu_);
    listReplies_.push_back(std::make_pair(list, (int64_t)(monoNow() * 1000.0)));
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
}

void SerialListener::sppLinkEnded(const ServiceDisconnected& ended) {
    if (!config_.dial.on) return;
    // The end of a phone-opened link, said as the car says it when its record of such links is right: a doubt about
    // that record since a failed dial goes (DialRules::recordDoubtful()).
    if (ended.device == kDisconnectDevice && ended.instance == kDisconnectInstance) {
        pthread_mutex_lock(&mu_);
        bool doubted = rules_.recordDoubtful();
        rules_.phoneLinkEnded();
        pthread_mutex_unlock(&mu_);
        if (doubted)
            say("the car says a phone-opened serial link ended (instance '%s'), as it does when its record of such "
                "links is right: that record is taken as right again", kDisconnectInstance);
        return;
    }
    if (ended.instance != kDialInstance) return;
    pthread_mutex_lock(&mu_);
    dialEnds_.push_back(ended);
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
}

void SerialListener::sppPairing(uint32_t, const std::string& address) {
    if (!config_.dial.on) return;
    pthread_mutex_lock(&mu_);
    pairingInforms_.push_back(address);
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
}

// The line for the refusal that waits, if one does, with what is known of the device list.
void SerialListener::sayRefusal(RefusalFacts::List list, const PhoneInList& phone) {
    RefusalFacts f;
    f.list = list;
    f.phone = phone;
    pthread_mutex_lock(&mu_);
    bool waits = refusalAt_ != 0;
    refusalAt_ = 0;
    unsigned count = refusalCount_;
    f.node = refusalNode_;
    pthread_mutex_unlock(&mu_);
    if (waits) say("%s", describeRefusal(count, f).c_str());
}

// On the listener's thread: a refusal whose device list has not come in time is said without it.
void SerialListener::tendRefusal() {
    pthread_mutex_lock(&mu_);
    bool late = refusalAt_ != 0 && monoNow() - refusalAt_ >= kRefusalListWaitMs / 1000.0;
    pthread_mutex_unlock(&mu_);
    if (late) sayRefusal(RefusalFacts::kListNoAnswer, PhoneInList());
}

// ---- the listener's thread -------------------------------------------------------------------------------------

void* SerialListener::threadMain(void* self) {
    static_cast<SerialListener*>(self)->run();
    return NULL;
}

bool SerialListener::linkExpected() {
    pthread_mutex_lock(&mu_);
    bool hot = monoNow() < hotUntil_;
    pthread_mutex_unlock(&mu_);
    return hot;
}

// Between two looks at the nodes: a moment while a link is expected, otherwise until something happens, and
// 200 ms at most, or 20 ms while serial.reset has something to do or to watch.
void SerialListener::waitForPass() {
    bool dialHot = dialPhase_ == kDialMagic || dialPhase_ == kDialCleanup || dialPhase_ == kDialLinked;
    pthread_mutex_lock(&mu_);
    double now = monoNow();
    bool hot = now < hotUntil_;
    // What the Bluetooth service's thread has said for the dial, while the listener's own thread was asking it.
    bool told = !connectReplies_.empty() || !listReplies_.empty() || !disconnectReplies_.empty() || !dialEnds_.empty() ||
                !pairingInforms_.empty() || !repointEnds_.empty();
    if (!hot && !stop_ && !told) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += now < resetHotUntil_ || reset_.waiting || dialHot ? 20000000L : 200000000L;
        if (ts.tv_nsec >= 1000000000L) { ++ts.tv_sec; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&wake_, &mu_, &ts);
    }
    pthread_mutex_unlock(&mu_);
    if (hot) usleep(2000);
}

// The registration check (SppService::checkRegistration), on the listener's thread while the service is
// registered: every SerialConfig::checkMs, and only while all is quiet. A registration sent while a phone asks for
// a link can get in that link's way, and one sent while a link of the agent's is up is not needed: the link is
// there.
void SerialListener::checkRegistrationIfDue() {
    if (config_.checkMs == 0) return;
    double now = monoNow();
    if (now - checkedAt_ < config_.checkMs / 1000.0) return;
    pthread_mutex_lock(&mu_);
    bool quiet = !deciding_ && reqAt_ == 0 && (askedAt_ == 0 || now - askedAt_ >= config_.checkQuietMs / 1000.0);
    pthread_mutex_unlock(&mu_);
    if (!quiet || serialLinksOpen() != 0) return;
    checkedAt_ = now;
    pthread_mutex_lock(&mu_);
    checkedAtShared_ = now;
    pthread_mutex_unlock(&mu_);
    spp_.checkRegistration(config_.checkMs / 1000.0);
}

// ---- serial.reset ------------------------------------------------------------------------------------------------

// On the agent's thread: only says what is to happen, for the listener's thread to do.
SerialReset::Outcome SerialListener::resetSerialLink(const std::string& device, const std::string& instance,
                                                     const std::string& service, int64_t* forMs, std::string* why) {
    // A link the car dialled: the agent ends it, and the listener's thread has the car end it after the phone's
    // time (watchDialled()).
    if (dialledLinksOpen() > 0) {
        say("serial.reset: a link the car dialled is up: the agent ends it, and the car is asked to end it if the phone "
            "has not in %u ms", config_.dial.closeWaitMs);
        problem("reset", "serial.reset: the link the car dialled is ended");
        return kResetDialled;
    }
    unsigned agents = serialLinksOpen();
    pthread_mutex_lock(&mu_);
    bool active = active_;
    // A node kept open, unread, for another program is not counted: ending its link would end that program's.
    bool open = deciding_ || reading_ > 0 || agents > 0;
    pthread_mutex_unlock(&mu_);
    if (!active) {
        *why = "Bluetooth serial is not set up just now";
        return kResetFailed;
    }
    if (!spp_.attached()) {
        *why = "the car's Bluetooth service is not attached";
        return kResetFailed;
    }
    if (open && !spp_.knowsPhone()) {
        *why = "no phone has asked for a serial link since the agent started, so there is no link to name";
        return kResetFailed;
    }
    *forMs = (int64_t)config_.resetArmedMs;
    pthread_mutex_lock(&mu_);
    double now = monoNow();
    reset_.device = device;
    reset_.instance = instance;
    reset_.service = service;
    reset_.askedAt = now;
    reset_.waiting = false;   // this one takes its place
    if (open) {
        reset_.due = true;
        reset_.at = now + kSerialResetDelayMs / 1000.0;
        resetHotUntil_ = reset_.at + kSerialResetWatchMs / 1000.0;
    } else {
        reset_.armed = true;
        reset_.until = now + config_.resetArmedMs / 1000.0;
    }
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
    problem("reset", open ? "serial.reset: the car is asked to end the phone's serial link"
                          : "serial.reset: the next serial link is to be ended at its open");
    if (open) {
        say("serial.reset: a serial node is open, so ServiceDisconnect goes to the car in %u ms", kSerialResetDelayMs);
    } else {
        say("serial.reset: no serial node is open, so the next serial link whose node opens within %u s is ended at "
            "its open", config_.resetArmedMs / 1000);
    }
    return open ? kResetSent : kResetArmed;
}

// The nodes that are there when ServiceDisconnect has gone out, sent at `from`: the log says when they go.
void SerialListener::watchNodes(const bool nodes[NodeTracker::kNodes], double from) {
    bool any = false;
    for (int i = 0; i < NodeTracker::kNodes; ++i) {
        watched_[i] = nodes[i];
        any = any || nodes[i];
    }
    watchFrom_ = any ? from : 0;
    if (!any) say("serial.reset: no serial node was there when ServiceDisconnect went out");
}

// On the listener's thread: the nodes that are there now and that a reader of this process reads, the listener's or
// the agent's. Those are the node being decided on, one that was let go and is read with what comes dropped (or was
// ended at its open), and the node of a link of the agent's that a phone opened, whether or not the listener has
// been set up again since that link came. Not a node kept open, unread, for another program, nor one that was read
// and then left to such a program: that is that program's link, and the car would end it.
void SerialListener::readNodes(bool nodes[NodeTracker::kNodes]) {
    bool present[NodeTracker::kNodes];
    scanNodes(present);
    for (int i = 0; i < NodeTracker::kNodes; ++i) nodes[i] = false;
    if (decidingNode_ >= 0 && decidingNode_ < NodeTracker::kNodes) nodes[decidingNode_] = true;
    for (size_t k = 0; k < letGo_.size(); ++k) {
        const LetGo& kept = letGo_[k];
        if (kept.reader != 0 && !kept.reader->ended() && kept.node >= 0 && kept.node < NodeTracker::kNodes)
            nodes[kept.node] = true;
    }
    for (int i = 0; i < NodeTracker::kNodes; ++i) {
        if (!nodes[i] && present[i]) nodes[i] = phoneLinkOn(nodePath(i));
        nodes[i] = nodes[i] && present[i];
    }
}

// On the listener's thread, at every pass and during the decide step: what serial.reset asked for that is due, and
// what has come of a ServiceDisconnect so far.
void SerialListener::tendReset() {
    double now = monoNow();
    pthread_mutex_lock(&mu_);
    bool due = reset_.due && now >= reset_.at;
    if (due) reset_.due = false;
    bool waited = !due && reset_.waiting;
    bool ranOut = reset_.armed && now >= reset_.until;
    if (ranOut) reset_.armed = false;
    Reset asked = reset_;
    pthread_mutex_unlock(&mu_);
    if (ranOut)
        say("serial.reset: no serial link came within %u s to be ended at its open: nothing is asked any more",
            config_.resetArmedMs / 1000);
    if (due || waited) {
        // The nodes that are there and read in this process (readNodes()), which are watched once ServiceDisconnect
        // has gone. Someone else's are not, and neither is one the listener keeps open for another program. What the
        // agent holds counts, not what the node tracker says: the tracker starts afresh when the listener is set up
        // again, with every node there as someone else's, and a link of the agent's may still be up then.
        bool present[NodeTracker::kNodes];
        readNodes(present);
        bool any = false;
        for (int i = 0; i < NodeTracker::kNodes; ++i) any = any || present[i];
        // None is there any more, in the delay or while another ServiceDisconnect waited: the link has ended, or the
        // listener has let it go to another program since. Nothing is sent then, and the reset is done, with nothing
        // to watch. A ServiceDisconnect for the phone-opened link when the car has no live link there can hold that
        // instance up for well over a minute, which keeps phones' links out for as long; for a link that another
        // program has, it would end that program's link.
        bool waits = false, sent = false;
        if (any) {
            // One ServiceDisconnect at a time: while another waits for its answer, this one waits its turn, which
            // comes within the time the other is given (SppService::setDisconnectReplyTime): the dial's clean-up sends
            // no more while it waits (tendCleanup()), and this comes first at every pass.
            sent = spp_.disconnectPhone(asked.device, asked.instance, asked.service, &waits);
        }
        pthread_mutex_lock(&mu_);
        if (reset_.askedAt == asked.askedAt) reset_.waiting = waits;   // unless a later serial.reset took its place
        pthread_mutex_unlock(&mu_);
        // A node that the listener left to another program after serial.reset was asked for, and that is still there.
        double leftAt = 0;
        for (size_t k = 0; !any && k < letGo_.size(); ++k) {
            const LetGo& kept = letGo_[k];
            if (kept.handedAt > 0 && kept.handedAt >= asked.askedAt && access(nodePath(kept.node).c_str(), F_OK) == 0)
                leftAt = kept.handedAt;
        }
        if (!any && leftAt > 0) {
            say("serial.reset: the link was let go to another program %.0f ms after serial.reset was asked for, and is "
                "that program's now: nothing is sent to the car, which would end that program's link",
                (leftAt - asked.askedAt) * 1000.0);
            problem("reset-not-sent", "serial.reset: the link was let go to another program before ServiceDisconnect "
                                      "was to go, so nothing was sent");
        } else if (!any) {
            say("serial.reset: no serial node of the listener's is there any more, %.0f ms after it was asked for: the "
                "link has ended, so nothing is sent to the car", (now - asked.askedAt) * 1000.0);
            problem("reset-not-sent", "serial.reset: the link had ended before ServiceDisconnect was to go, so nothing "
                                      "was sent");
        } else if (sent) {
            say("serial.reset: ServiceDisconnect sent, %.0f ms after it was asked for%s", (now - asked.askedAt) * 1000.0,
                waited ? ", once the one before it was answered or given up" : "");
            watchNodes(present, now);
        } else if (waits && due) {
            say("serial.reset: another ServiceDisconnect waits for the car's answer: this one goes once that is "
                "answered, or given up after %u ms", config_.dial.disconnectWaitMs);
        } else if (!waits) {
            say("serial.reset: ServiceDisconnect could not be sent: the car's Bluetooth service is not attached, or "
                "did not take the request");
        }
    }
    if (watchFrom_ == 0) return;
    bool present[NodeTracker::kNodes];
    scanNodes(present);
    bool left = false;
    std::string up;
    for (int i = 0; i < NodeTracker::kNodes; ++i) {
        if (!watched_[i]) continue;
        if (present[i]) {
            left = true;
            up += (up.empty() ? "" : ", ") + nodePath(i);
            continue;
        }
        watched_[i] = false;
        say("serial.reset: %s went %.0f ms after ServiceDisconnect was sent", nodePath(i).c_str(),
            (now - watchFrom_) * 1000.0);
    }
    if (!left) {
        watchFrom_ = 0;
    } else if (now - watchFrom_ >= kSerialResetWatchMs / 1000.0) {
        say("serial.reset: the link was still up %u s after ServiceDisconnect was sent: %s goes on being read until "
            "the link ends", kSerialResetWatchMs / 1000, up.c_str());
        for (int i = 0; i < NodeTracker::kNodes; ++i) watched_[i] = false;
        watchFrom_ = 0;
    }
}

// ---- the hold ----------------------------------------------------------------------------------------------------

namespace {
std::string joined(const std::vector<std::string>& names) {
    std::string all;
    for (size_t i = 0; i < names.size(); ++i) all += (i ? ", " : "") + names[i];
    return all;
}
}  // namespace

// Turns the gate's hold on or off, as things stand: it is on while a phone's request waits for its node (not one
// whose link is left to another program: leftToOther()), while a link is being decided on, and while a link of the
// agent's is up. The programs it holds off are the ones declared for a serial service at the moment it goes on; with
// none declared there is no hold. From the listener's thread and the Bluetooth service's: whichever of them makes
// the change gives holdWhy_ as the reason in the log, which is what was last put there together with a change of
// what the hold goes by.
void SerialListener::updateHold() {
    if (services_ == 0) return;
    // A link the car dialled is on a node of its own: the gate has nothing to hold off for it.
    unsigned up = serialLinksOpen() - dialledLinksOpen();
    std::vector<std::string> programs;
    const char* why = 0;
    for (int round = 0; round < 2; ++round) {
        pthread_mutex_lock(&mu_);
        if (holdWhy_ != 0) why = holdWhy_;
        holdWhy_ = 0;
        bool waiting = reqAt_ > 0 && monoNow() < hotUntil_;
        bool expired = reqAt_ > 0 && !waiting;
        if (expired) reqAt_ = 0;   // its node has not come: a later link is not this request's
        bool want = active_ && (deciding_ || up != 0 || waiting);
        if (want == held_) {
            pthread_mutex_unlock(&mu_);
            return;
        }
        if (!want) {
            held_ = false;
            gate_->release();
            std::string text = heldText_;
            pthread_mutex_unlock(&mu_);
            if (expired) why = "no link came of the phone's request";
            say("gate: opens by %s are passed on again (%s)", text.c_str(), why != 0 ? why : "the agent's link has ended");
            return;
        }
        if (round == 1) {
            if (programs.empty()) {
                pthread_mutex_unlock(&mu_);
                return;
            }
            held_ = true;
            heldText_ = joined(programs);
            gate_->hold(programs);
            pthread_mutex_unlock(&mu_);
            say("gate: opens by %s are refused as well from now on (%s)", joined(programs).c_str(),
                why != 0 ? why : "a link of the agent's is up");
            return;
        }
        pthread_mutex_unlock(&mu_);
        programs = services_->programs();   // with a lock of its own, so not under ours
    }
}

// ---- the decide step ---------------------------------------------------------------------------------------------

namespace {

// What the bytes at `p` are, as the start of what came on a link.
enum Opening {
    kOpenMagic,       // the magic bytes
    kOpenMagicPart,   // the start of them, and no more yet
    kOpenFrame,       // a whole frame of the protocol with a good check value, of *frameBytes
    kOpenFramePart,   // what may be the start of one
    kOpenOther        // neither
};

Opening judgeOpening(const uint8_t* p, size_t n, size_t* frameBytes) {
    size_t m = n < kLinkMagicBytes ? n : kLinkMagicBytes;
    if (memcmp(p, kLinkMagic, m) == 0) return m == kLinkMagicBytes ? kOpenMagic : kOpenMagicPart;
    const size_t most = kFrameHeaderBytes + kMaxFramePayload + kFrameCheckBytes;
    FrameDecoder frames;
    frames.feed(p, n < most ? n : most);
    Frame frame;
    FrameDecoder::Result r = frames.next(&frame, 0);
    if (r == FrameDecoder::kFrame) {
        *frameBytes = kFrameHeaderBytes + frame.payload.size() + kFrameCheckBytes;
        return kOpenFrame;
    }
    return r == FrameDecoder::kNeedMore ? kOpenFramePart : kOpenOther;
}

}  // namespace

// Reads what came first on the node, from its reader, until it tells whose link this is or the time for it is over.
//
// What a phone sent into an earlier link that nobody read comes ahead of what it sends on this one, as one run of
// bytes. When that run starts with whole frames of this protocol, they were the agent's link's: they are counted in
// *leftOver and passed over, and the magic bytes are looked for after them, within the same time.
//
// kOurs: the magic bytes came, at the start or after such leftovers, and *first holds what was read from them on.
// kLetGo: anything else came, or nothing did, before or after such leftovers: the link is another program's, or
// nobody's, and *first holds all that was read, leftovers too, in order. Leftovers alone do not make it the agent's:
// another program's link may have the agent's old bytes in front of it, and letGo() gives it to that program as it
// gives any other. kEnded: the link ended first. *why says which, for the log. Nothing is written.
SerialListener::Decided SerialListener::decide(NodeReader* reader, const std::string& path, double openedAt,
                                               std::vector<uint8_t>* first, uint64_t* leftOver, std::string* why) {
    const double by = openedAt + config_.decideMs / 1000.0;
    std::vector<uint8_t> got;
    size_t at = 0;   // where what has not been passed over as leftovers starts
    *leftOver = 0;
    for (;;) {
        Opening o = kOpenMagicPart;
        while (at < got.size()) {
            size_t frame = 0;
            o = judgeOpening(&got[at], got.size() - at, &frame);
            if (o != kOpenFrame) break;
            at += frame;
            *leftOver += frame;
        }
        if (at < got.size() && o == kOpenMagic) {
            first->assign(got.begin() + (long)at, got.end());
            return kOurs;
        }
        if (at < got.size() && o == kOpenOther) {
            *why = *leftOver > 0 ? "after the leftovers came bytes that are neither a frame nor the magic bytes"
                                 : "it did not open with the magic bytes";
            first->assign(got.begin(), got.end());
            return kLetGo;
        }
        if (stopping()) {
            *why = "the listener was stopped";
            return kEnded;
        }
        tendReset();
        double left = by - monoNow();
        if (left <= 0) {
            first->assign(got.begin(), got.end());
            if (*leftOver > 0) {
                *why = at == got.size() ? "nothing more came after the leftovers in that time"
                                        : "neither a whole frame nor the magic bytes came after the leftovers in that time";
                return kLetGo;
            }
            *why = got.empty() ? "nothing came from the phone in that time"
                               : (o == kOpenMagicPart ? "only a part of the magic bytes came in that time"
                                                      : "it did not open with the magic bytes in that time");
            return kLetGo;
        }
        int ms = (int)(left * 1000.0) + 1;
        bool none = got.empty();
        NodeReader::Got what = reader->next(ms < kPumpTickMs ? ms : kPumpTickMs, &got);
        if (what == NodeReader::kEnded) {
            int error = reader->endError();
            if (error == 0) *why = "closed by the phone";
            else if (access(path.c_str(), F_OK) != 0) *why = "the phone's link ended";
            else *why = std::string("a read of the node failed: ") + strerror(error);
            return kEnded;
        }
        if (what == NodeReader::kData && none)
            say("%s: its first message came %.0f ms after the open", path.c_str(), (monoNow() - openedAt) * 1000.0);
    }
}

// Whether another program may read the link on node `node`, and *why: a program declared for a serial service
// runs, or may; the gate has seen another program open the node, or try to, since this link's node appeared; or a
// program of software put on by hand runs, which is not declared and may read serial links of its own. *why says
// which, or, with none, that none of them is so.
bool SerialListener::otherReader(int node, std::string* why) {
    SerialServices::Readers readers = services_ != 0 ? services_->readers() : SerialServices::kNoneDeclared;
    if (readers == SerialServices::kSomeMayRun) {
        *why = "a program declared for a serial service is running, or whether one is cannot be told";
        return true;
    }
    std::string who;
    // The gate is in front of the first node only.
    if (node == 0 && appeared_[node] > 0 && gate_->sawOtherOpen(appeared_[node], &who)) {
        *why = "the gate saw " + who + " open the node, or try to, since this link's node appeared";
        return true;
    }
    std::string names;
    if (services_ != 0 && services_->handInstalledRunning(&names) == kProgramRunning) {
        *why = "a program of software put on by hand is running (" + names + "), which may read serial links";
        return true;
    }
    *why = std::string(readers == SerialServices::kNoneDeclared ? "No program is declared for a serial service"
                                                                : "No program declared for a serial service is running") +
           ", the gate has seen no other program open the node since the link's node appeared, and no program of "
           "software put on by hand is running";
    return false;
}

namespace {
std::string sizeText(size_t n) {
    char text[32];
    snprintf(text, sizeof text, "%u B", (unsigned)n);
    return text;
}

// What a put-back came to, for the log: `read` bytes the agent had read, and what NodePrimer did with them.
std::string putBackText(size_t read, const NodePrimer::Result& r) {
    if (r.putBack == 0) return "Nothing had been read from it";
    std::string text = "The " + sizeText(read) + " read from it were put back, in the order they came";
    if (r.fromQueue > 0) text += ", with " + sizeText(r.fromQueue) + " that came after the last read behind them";
    return text;
}
}  // namespace

// Lets the link on the node go: it was not the agent's, and nothing was written into it. Who reads it now decides
// what becomes of its node (otherReader()). If another program may read it, the reader is stopped, what was read
// (`read`, and what the reader had not handed on yet) is put back into the node's queue, and the node is kept open,
// unread, until the link ends, for that program; where that cannot be done the node is closed for it. With none,
// nobody would ever read the link, and bytes that wait unread ahead of a node come on the next link: the node is
// read, and what comes dropped, until the link ends or a sign of another reader comes after all (tendLetGo()).
void SerialListener::letGo(NodeReader* reader, int node, const std::string& path, double openedAt, const std::string& why,
                           const std::vector<uint8_t>& read) {
    double ms = (monoNow() - openedAt) * 1000.0;
    std::string reason;
    if (otherReader(node, &reason)) {
        pthread_mutex_lock(&mu_);
        handedOver(node);
        pthread_mutex_unlock(&mu_);
        std::vector<uint8_t> all = read;
        int fd = reader->stop(&all);
        NodePrimer::Result back;
        std::string failed;
        bool kept = false;
        if (fd < 0) {
            failed = "its read did not stop in time, and the node was closed under it";
            back.lost = all.size();
        } else {
            kept = primer_.putBack(fd, all, &back, &failed);
        }
        char head[160];
        snprintf(head, sizeof head, " let go %.0f ms after the open (", ms);
        if (kept) {
            if (back.putBack > 0) {
                pthread_mutex_lock(&mu_);
                ++counts_.putBack;
                pthread_mutex_unlock(&mu_);
            }
            log_->line(path + head + why + "). " + putBackText(all.size(), back) +
                       ", and its node is kept open, unread, until the link ends, for another reader: " + reason);
            keep(node, fd, 0, "let go");
            letGo_.back().handedAt = letGo_.back().at;
            return;
        }
        if (fd >= 0) close(fd);
        bool there = access(path.c_str(), F_OK) == 0;
        std::string lost = back.lost > 0 ? "The " + sizeText(back.lost) + " read from it could not be put back: "
                                         : "Nothing could be put back: ";
        log_->line(path + head + why + ") and closed: " + (there ? "its node is still there" : "its node went with the close") +
                   ". It is closed for another reader: " + reason + ". " + lost + failed);
        if (there) {
            keep(node, -1, 0, "let go");   // until its node goes
            letGo_.back().handedAt = letGo_.back().at;
        }
        return;
    }
    reader->drain();
    handLookAt_ = monoNow();   // otherReader() has just looked
    say("%s let go %.0f ms after the open (%s). %s, so nobody would read it: its node is read, with what comes "
        "dropped, until the link ends", path.c_str(), ms, why.c_str(), reason.c_str());
    keep(node, -1, reader, "let go");
}

// A node that is not the agent's link, from now until it goes: read by `reader`, or kept open unread (fd >= 0), or
// closed already (neither), and then only for the log to say when it went.
void SerialListener::keep(int node, int fd, NodeReader* reader, const char* what) {
    LetGo kept;
    kept.node = node;
    kept.fd = fd;
    kept.reader = reader;
    kept.at = monoNow();
    kept.what = what;
    kept.handedAt = 0;
    letGo_.push_back(kept);
    if (reader != 0) {
        pthread_mutex_lock(&mu_);
        ++reading_;
        pthread_mutex_unlock(&mu_);
    }
}

namespace {
// How often tendLetGo() looks for running programs of software put on by hand: the look walks the process list.
const double kHandLookEveryS = 2.0;
}  // namespace

// A link that was let go and is read and dropped had no sign of another reader when it was let go. One may come
// later: a program that the gate sees open the node, or try to, since the link was let go (the first node only),
// or a program of software put on by hand that has started to run. Then the reading stops and the node is closed
// for that program. The bytes read and dropped before that moment are gone; the log says how many.
void SerialListener::tendLetGo() {
    bool handLooked = false;
    std::string hand;
    for (size_t i = 0; i < letGo_.size(); ++i) {
        LetGo& kept = letGo_[i];
        if (kept.reader == 0 || strcmp(kept.what, "let go") != 0 || kept.reader->ended()) continue;
        std::string who, sign;
        if (kept.node == 0 && gate_->sawOtherOpen(kept.at, &who)) {
            sign = "the gate saw " + who + " open the node, or try to";
        } else {
            double now = monoNow();
            if (!handLooked && services_ != 0 && (handLookAt_ == 0 || now - handLookAt_ >= kHandLookEveryS)) {
                handLooked = true;
                handLookAt_ = now;
                std::string names;
                if (services_->handInstalledRunning(&names) == kProgramRunning) hand = names;
            }
            if (!hand.empty()) sign = "a program of software put on by hand is running (" + hand + ")";
        }
        if (sign.empty()) continue;
        uint64_t dropped = kept.reader->dropped();
        kept.reader->finish();
        kept.reader = 0;
        kept.handedAt = monoNow();
        pthread_mutex_lock(&mu_);
        --reading_;
        handedOver(kept.node);
        pthread_mutex_unlock(&mu_);
        std::string path = nodePath(kept.node);
        say("%s: reading it stopped, and its node closed for another reader, %.1f s after it was let go: %s. %llu B "
            "had been read and dropped by then", path.c_str(), monoNow() - kept.at, sign.c_str(),
            (unsigned long long)dropped);
        // It stays on the list, closed, for the log to say when its node goes.
    }
}

// The nodes that are not the agent's link and whose link has ended since: a node that is read or kept open is
// closed. kEvery: every one of them, the listener's thread having ended. kAllButUp: the same, except that a node
// whose link is still up goes on being read, or kept open for another program.
void SerialListener::closeKept(Closing which) {
    for (size_t i = 0; i < letGo_.size();) {
        const LetGo kept = letGo_[i];
        std::string path = nodePath(kept.node);
        bool ended = kept.reader != 0 ? kept.reader->ended() : access(path.c_str(), F_OK) != 0;
        bool open = kept.reader != 0 || kept.fd >= 0;
        if (!ended && (which == kEndedOnly || (which == kAllButUp && open))) {
            ++i;
            continue;
        }
        letGo_.erase(letGo_.begin() + (long)i);
        if (kept.reader != 0) {
            pthread_mutex_lock(&mu_);
            --reading_;
            pthread_mutex_unlock(&mu_);
        }
        uint64_t dropped = 0;
        if (kept.reader != 0) {
            dropped = kept.reader->dropped();
            kept.reader->finish();
        } else if (kept.fd >= 0) {
            close(kept.fd);
        }
        double after = monoNow() - kept.at;
        if (kept.reader != 0) {
            say(ended ? "%s: the link ended %.1f s after it was %s; %llu B were dropped, and its node is closed now"
                      : "%s: the link was still up %.1f s after it was %s, and its node is closed as the listener "
                        "stops; %llu B were dropped",
                path.c_str(), after, kept.what, (unsigned long long)dropped);
        } else if (kept.fd >= 0) {
            say(ended ? "%s: the link ended %.1f s after it was %s, and its node is closed now"
                      : "%s: the link was still up %.1f s after it was %s, and its node is closed as the listener stops",
                path.c_str(), after, kept.what);
        } else if (which == kEndedOnly) {
            say("%s is gone, %.1f s after it was %s and closed", path.c_str(), after, kept.what);
        }
    }
}

// Opens node `node`, which has just become a link that may be ours, and hands it to the agent if what the phone
// says first shows that it is.
void SerialListener::take(int node, const std::string& address) {
    std::string path = nodePath(node);
    int fd = open(path.c_str(), O_RDWR);
    double openedAt = monoNow();
    if (fd < 0) {
        say("open %s: %s", path.c_str(), strerror(errno));
        return;
    }
    // The first read goes out at once: a read is what brings forward what the phone has sent.
    NodeReader* reader = NodeReader::start(fd, path, log_, config_.readNode);
    if (reader == 0) return;
    closeOnExec(fd);
    pthread_mutex_lock(&mu_);
    double reqAt = reqAt_;
    reqAt_ = 0;
    deciding_ = true;
    decidingNode_ = node;
    holdWhy_ = "a link's node has been opened";
    pthread_mutex_unlock(&mu_);
    if (reqAt > 0) say("%s opened %.0f ms after the phone's request", path.c_str(), (openedAt - reqAt) * 1000.0);
    else say("%s opened", path.c_str());
    // For a link that came long after its request. With a request just before it the hold is on already.
    updateHold();
    // The gate is looked at only now, with the node open: the open comes before everything else. The time for the
    // decide step runs from the open all the same.
    gate_->watch();

    // serial.reset asked for the next link to be ended: this one, with no decide step.
    pthread_mutex_lock(&mu_);
    bool reset = reset_.armed && openedAt < reset_.until;
    Reset asked = reset_;
    if (reset) {
        reset_.armed = false;
        resetHotUntil_ = monoNow() + kSerialResetWatchMs / 1000.0;
    }
    pthread_mutex_unlock(&mu_);
    if (reset) {
        reader->drain();
        bool waits = false;
        bool sent = spp_.disconnectPhone(asked.device, asked.instance, asked.service, &waits);
        if (waits) {
            // Its turn comes at a later pass (tendReset()).
            pthread_mutex_lock(&mu_);
            if (reset_.askedAt == asked.askedAt) reset_.waiting = true;
            pthread_mutex_unlock(&mu_);
        }
        say("%s: serial.reset asked %.1f s ago for the next serial link to be ended at its open. %s, and its node is "
            "read, with what comes dropped, until the link ends", path.c_str(), openedAt - asked.askedAt,
            sent    ? "ServiceDisconnect is sent at once, with no decide step"
            : waits ? "With no decide step, ServiceDisconnect goes once another that waits for the car's answer is "
                      "answered or given up"
                    : "ServiceDisconnect could not be sent");
        keep(node, -1, reader, "ended at its open");
        if (sent) {
            bool one[NodeTracker::kNodes];
            for (int i = 0; i < NodeTracker::kNodes; ++i) one[i] = i == node;
            watchNodes(one, monoNow());
        }
        pthread_mutex_lock(&mu_);
        deciding_ = false;
        decidingNode_ = -1;
        holdWhy_ = "the link is ended at its open";
        pthread_mutex_unlock(&mu_);
        updateHold();
        return;
    }

    std::vector<uint8_t> first;
    std::string why;
    uint64_t leftOver = 0;
    Decided found = decide(reader, path, openedAt, &first, &leftOver, &why);
    decidingNode_ = -1;   // from here the node is the agent's link, or in letGo_, or closed
    // Of a link that is let go they go where the rest of what was read goes: letGo() says.
    if (leftOver > 0)
        say("%s: %llu B left over from an earlier link came first, as whole frames, and were %s", path.c_str(),
            (unsigned long long)leftOver, found == kLetGo ? "passed over" : "dropped");
    Link* link = 0;
    const char* holdWhy = 0;
    if (found == kOurs) {
        say("%s: it opened with the magic bytes, so the link is the agent's", path.c_str());
        // Counted as up from here, which keeps the hold on.
        link = newSerialLink(reader, path, log_, first, config_.lingerMs, address);
        if (link == 0) holdWhy = "the link could not be set up";
    } else if (found == kLetGo) {
        letGo(reader, node, path, openedAt, why, first);
        holdWhy = "the link was let go";
    } else {
        reader->finish();
        say("%s closed, %s, before it was known whose link it is", path.c_str(), why.c_str());
        holdWhy = "the link ended before it was decided on";
    }
    pthread_mutex_lock(&mu_);
    deciding_ = false;
    holdWhy_ = holdWhy;
    if (link != 0) ready_.push_back(link);
    pthread_mutex_unlock(&mu_);
    if (link != 0) {
        char byte = 1;
        if (write(wakeWrite_, &byte, 1) < 0) {}   // a full pipe has woken the agent already
        return;
    }
    updateHold();
    // A link that nothing answers is what a service leaves behind whose program has gone.
    if (found == kLetGo && services_ != 0) services_->sweepIfDue("a phone's link was not the agent's");
}

void SerialListener::lookForLinks() {
    closeKept(kEndedOnly);
    bool present[NodeTracker::kNodes];
    double lastLook = scannedAt_;
    scannedAt_ = monoNow();
    scanNodes(present);
    NodeTracker::State before[NodeTracker::kNodes], after[NodeTracker::kNodes];
    pthread_mutex_lock(&mu_);
    for (int i = 0; i < NodeTracker::kNodes; ++i) before[i] = tracker_.state(i);
    int ours = tracker_.scan(present, monoSec(), false);
    std::string address = tracker_.takenAddress();
    bool leave = ours >= 0 && tracker_.takenLeave();
    for (int i = 0; i < NodeTracker::kNodes; ++i) after[i] = tracker_.state(i);
    pthread_mutex_unlock(&mu_);
    // A node that is new has appeared since the look before this one.
    for (int i = 0; i < NodeTracker::kNodes; ++i)
        if (present[i] && before[i] == NodeTracker::kAbsent) appeared_[i] = lastLook > 0 ? lastLook : scannedAt_;
    // The request it came with was left to another program: so is the link, if the rule still holds now, primed for
    // that program and never read. Otherwise it is decided on as any other, which is the safe side for the agent's
    // own app.
    if (leave) {
        std::string why;
        if (leftToOther(address, &why)) {
            pthread_mutex_lock(&mu_);
            handedOver(ours);
            ++counts_.leftUnread;
            pthread_mutex_unlock(&mu_);
            primeForOther(ours, why);
            ours = -1;
        } else {
            say("%s: the phone's request was to be left to another program, but %s now: the link is decided on as any "
                "other", nodePath(ours).c_str(), why.c_str());
        }
    }
    if (ours >= 0) take(ours, address);
    // Only requests that may bring a serial node are kept (sppPhone()), so no node is another service's.
    for (int i = 0; i < NodeTracker::kNodes; ++i) {
        if (before[i] == after[i] || after[i] != NodeTracker::kForeign) continue;
        say("%s: no request from a phone came with it, so it is not ours: left alone", nodePath(i).c_str());
    }
}

// A link the rule leaves to another program (leftToOther()), on the listener's thread as soon as its node is there.
// A program that waits on select() before it reads would never see what the phone sent before a read was made on
// the link, so the node is opened and primed for it (NodePrimer::prime()): with nothing in its queue, one read that
// does not wait, which as a rule takes nothing; what it does take is put back. Then the node is kept open, unread,
// until the link ends, as the last close would empty its queue. One that cannot be opened or primed is closed again,
// and left as it is. The log says which, once for the link.
void SerialListener::primeForOther(int node, const std::string& rule) {
    std::string path = nodePath(node);
    std::string line = path + ": left to another program, and never read: " + rule + ". ";
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) {
        log_->line(line + "Its node could not be opened to prime it for that program (" + strerror(errno) +
                   "), so it is left as it is");
        return;
    }
    closeOnExec(fd);
    NodePrimer::Result r;
    std::string failed;
    if (!primer_.prime(fd, &r, &failed)) {
        close(fd);
        std::string lost = r.lost > 0 ? ", and the " + sizeText(r.lost) + " its read took are lost" : "";
        log_->line(line + "Its node could not be primed for that program (" + failed + ")" + lost +
                   ": it is closed again, and left as it is");
        return;
    }
    if (r.putBack > 0) {
        pthread_mutex_lock(&mu_);
        ++counts_.putBack;
        pthread_mutex_unlock(&mu_);
    }
    if (r.queued > 0) {
        line += "Its node is opened, and " + sizeText((size_t)r.queued) + " were in its queue already, so nothing is "
                "read";
    } else if (r.taken == 0) {
        line += "Its node is opened and primed for that program with one read that does not wait, which took nothing";
    } else {
        line += "Its node is opened and primed for that program with one read that does not wait, which took " +
                sizeText(r.taken) + " that had just come: they were put back";
        if (r.fromQueue > 0) line += ", with " + sizeText(r.fromQueue) + " that came after them behind them";
    }
    log_->line(line + ". It is kept open, unread, until the link ends");
    keep(node, fd, 0, "primed for another program");
    // As after any open of a link's node: the gate is checked once the node is open.
    gate_->watch();
}

// The rule for a link that a phone opens: it is left to another program, its node never read and no hold put on for
// it, but primed for that program (primeForOther()), when the car dials (SerialConfig::dial), a program declared
// for a serial service runs or may (SerialServices::readers()), and the phone that asked is one of the agent's whose
// app a link the car dialled has reached before (PhoneAddressList::dialledBefore()). The app is reached by the dial
// then, and the other program gets the link whole, its first bytes too, which the decide step would have read. A
// link that the agent's app opens on such a phone is left all the same: that is the rule's cost. Without a dial that
// has reached the phone, the car may be one on which dialling does not work, and the phone's own link stays the way
// in. *why says, for the log, why the rule holds, or which part of it does not. From either thread: the cheap parts
// are looked at first.
bool SerialListener::leftToOther(const std::string& address, std::string* why) {
    if (!config_.dial.on) {
        *why = "the car does not dial phones in this run";
        return false;
    }
    if (ours_ == 0 || !ours_->has(address)) {
        *why = "the phone is not one of the agent's";
        return false;
    }
    if (!ours_->dialledBefore(address)) {
        *why = "no link the car dialled to the phone has been the agent's";
        return false;
    }
    if (services_ == 0 || services_->readers() != SerialServices::kSomeMayRun) {
        *why = "no program declared for a serial service is running";
        return false;
    }
    *why = "the car dials this phone of the agent's, whose app a dialled link has reached, and a program declared for "
           "a serial service is running, or whether one is cannot be told";
    return true;
}

// The rule above for the phone at `address`, asked for its app (agent.hpp, info.get): the why is not wanted there.
bool SerialListener::phoneOpenedLeft(const std::string& address) {
    std::string why;
    return !address.empty() && leftToOther(address, &why);
}

// After the listener has given up, on its thread: waits, then puts the gate in front and asks for the service
// again, as start() did, and once more after each time that does not work. False when stop() came first.
bool SerialListener::comeBack() {
    for (;;) {
        double until = monoNow() + config_.retryMs / 1000.0;
        pthread_mutex_lock(&mu_);
        while (!stop_ && monoNow() < until) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000L;
            if (ts.tv_nsec >= 1000000000L) { ++ts.tv_sec; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&wake_, &mu_, &ts);
        }
        bool stop = stop_;
        pthread_mutex_unlock(&mu_);
        if (stop) return false;

        bool present[NodeTracker::kNodes];
        scanNodes(present);
        pthread_mutex_lock(&mu_);
        tracker_ = NodeTracker(config_.requestWindowS);
        tracker_.baseline(present);
        pthread_mutex_unlock(&mu_);
        std::string why;
        if (!setUp(&why)) {
            if (final_) {
                say("Bluetooth serial is not set up again: %s. It is off until the agent is started again, and not "
                    "tried again", why.c_str());
                return false;
            }
            // Each reason once, however many tries it takes.
            if (why != lastTry_)
                say("Bluetooth serial is not set up again: %s. Again in %u s", why.c_str(), config_.retryMs / 1000);
            lastTry_ = why;
            continue;
        }
        lastTry_.clear();
        say("setting Bluetooth serial up again");
        return true;
    }
}

// On the listener's thread at every pass: says once when the car's Bluetooth service has not attached for
// SerialConfig::attachWaitMs, with the version asked for, and asks the server look, once in the listener's life,
// what the system's log says of it.
void SerialListener::watchAttach(bool registered) {
    double now = monoNow();
    if (spp_.attached()) {
        everAttached_ = true;
        unattachedSince_ = 0;
        attachSaid_ = false;
        return;
    }
    if (registered) return;
    if (unattachedSince_ == 0) unattachedSince_ = now;
    if (attachSaid_ || now - unattachedSince_ < config_.attachWaitMs / 1000.0) return;
    attachSaid_ = true;
    if (everAttached_) {
        say("the car's Bluetooth service has been gone for %g s: the agent waits for it to come back (it asks the "
            "service broker for %s)", config_.attachWaitMs / 1000.0, spp_.server().c_str());
    } else {
        say("the car's Bluetooth service has not attached %g s after the agent asked the service broker for %s. The "
            "broker attaches a client only to a server with the same major version and a minor version as high or "
            "higher, and otherwise tells the client nothing", config_.attachWaitMs / 1000.0, spp_.server().c_str());
        problem("never-attached", "the car's Bluetooth service has not attached; the agent asked for " + spp_.server());
    }
    if (serverLook_ == 0 || lookDone_) return;
    lookDone_ = true;
    double from = monoNow();
    std::string why;
    std::string line = serverLook_->look(btspp::kGapInterface, &why);
    if (!line.empty())
        say("the system log, %.1f s to read, says of the service: %s", monoNow() - from, line.c_str());
    else
        say("the system log gave no line about the service's version: %s", why.c_str());
}

namespace {
std::string amount(unsigned long long bytes) {
    char b[48];
    if (bytes < 1024) snprintf(b, sizeof(b), "%llu B", bytes);
    else snprintf(b, sizeof(b), "%llu KB", (bytes + 512) / 1024);
    return b;
}
}  // namespace

std::string brokerLookWhy(bool ran, const std::string& error, const ProcessResult& result, unsigned timeoutS,
                          unsigned long long bytes) {
    char b[256];
    const char* none = "with no line of the service broker's about it";
    if (!ran) return "sloginfo could not be started: " + error;
    if (result.timedOut) {
        snprintf(b, sizeof(b), "sloginfo did not end in %u s; it had given %s by then, %s", timeoutS,
                 amount(bytes).c_str(), none);
    } else if (result.exited && result.exitCode == 127) {
        snprintf(b, sizeof(b), "sloginfo is not there (the shell's status 127)");
    } else if (result.exited && result.exitCode == 126) {
        snprintf(b, sizeof(b), "sloginfo is there but could not be run (the shell's status 126)");
    } else if (result.exited && result.exitCode != 0) {
        snprintf(b, sizeof(b), "sloginfo ended with status %d after giving %s, %s", result.exitCode,
                 amount(bytes).c_str(), none);
    } else if (!result.exited) {
        snprintf(b, sizeof(b), "sloginfo was ended by signal %d after giving %s, %s", result.signal,
                 amount(bytes).c_str(), none);
    } else if (bytes == 0) {
        snprintf(b, sizeof(b), "sloginfo ran and gave nothing: the system log is empty, or could not be read");
    } else {
        snprintf(b, sizeof(b), "sloginfo read %s of the system log, %s", amount(bytes).c_str(), none);
    }
    return b;
}

// ---- the car dials the phone -------------------------------------------------------------------------------------

namespace {

int64_t msOf(double seconds) { return (int64_t)(seconds * 1000.0); }

const char* whyName(DialRules::Why why) {
    switch (why) {
    case DialRules::kForced: return "asked for";
    case DialRules::kPoll: return "poll";
    case DialRules::kSignal: return "the phone asked for a link";
    case DialRules::kPairing: return "pairing window";
    case DialRules::kRetry: return "retried";
    case DialRules::kAfresh: return "afresh, after a link left behind";
    }
    return "?";
}

// What came first on a link, after any whole frames an earlier link left: *at is where that ends, and the bytes
// before it are added to *leftOver. What it is, if anything has come after them.
Opening judgeStart(const std::vector<uint8_t>& got, size_t* at, uint64_t* leftOver) {
    Opening o = kOpenMagicPart;
    while (*at < got.size()) {
        size_t frame = 0;
        o = judgeOpening(&got[*at], got.size() - *at, &frame);
        if (o != kOpenFrame) break;
        *at += frame;
        *leftOver += frame;
    }
    return o;
}

}  // namespace

unsigned SerialListener::dialNow(std::string* why) {
    const char* code = 0;
    return dialNow(why, &code);
}

namespace {
const char* dialStateName(DialRules::State s) {
    switch (s) {
    case DialRules::kIdle: return "idle";
    case DialRules::kAsking: return "asking";
    case DialRules::kDialling: return "dialling";
    case DialRules::kUp: return "up";
    case DialRules::kEnding: return "ending";
    }
    return "?";
}
}  // namespace

SerialStatus SerialListener::serialStatus() {
    SerialStatus s;
    unsigned all = serialLinksOpen(), dialled = dialledLinksOpen();
    std::vector<std::string> ours;
    if (ours_ != 0) ours = ours_->get();
    btspp::SppService::CheckCounts checks = spp_.checkCounts();
    btspp::SppService::Heard heard = spp_.heard();
    pthread_mutex_lock(&mu_);
    s = counts_;
    s.registered = active_ && spp_.state() == SppService::kRegistered;
    s.checkedMsAgo = checkedAtShared_ > 0 ? (int64_t)((monoNow() - checkedAtShared_) * 1000.0) : -1;
    s.phonesPresent = rules_.presence().presentCount();
    DialRules::Facts facts;
    facts.ours = ours;
    facts.pairingOpen = pairingOpen_;
    s.phonesEligible = config_.dial.on ? rules_.eligibleCount(facts) : 0;
    s.dialState = dialStateName(rules_.state());
    s.recordDoubtful = rules_.recordDoubtful();
    s.repointStopped = repointStopped_;
    s.refusals = refused_;
    pthread_mutex_unlock(&mu_);
    s.dialOn = config_.dial.on;
    s.stack = heard.version;
    s.checks = checks.sent;
    s.checksRestored = checks.restored;
    s.link = all == 0 ? "none" : (dialled == 0 ? "phone-opened" : (dialled == all ? "dialled" : "both"));
    if (problems_ != 0) {
        s.problemsRecorded = problems_->recorded();
        s.problemsCounted = problems_->counted();
    }
    return s;
}

unsigned SerialListener::dialNow(std::string* why, const char** code) {
    *code = "unsupported";
    if (!config_.dial.on) {
        *why = "the car does not dial phones in this run (--dial off)";
        return 0;
    }
    if (!active() || !spp_.attached()) {
        *why = "Bluetooth serial is not set up just now";
        *code = "io-error";
        return 0;
    }
    if (dialBusy()) {
        *why = "a link of the agent's is up, or a dial is out";
        *code = "busy";
        return 0;
    }
    *code = "not-found";
    pthread_mutex_lock(&mu_);
    bool known = rules_.presence().known();
    unsigned n = rules_.force();
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
    if (n == 0)
        *why = known ? "no phone is connected to the car and paired with it" : "the car's device list has not come yet";
    else
        say("a dial of %u phone%s is asked for", n, n == 1 ? "" : "s");
    return n;
}

// Whether something of the agent's is on a serial link, or about to be: serial.dial waits for it, and for an ask.
bool SerialListener::dialBusy() {
    bool pending = false;
    bool busy = linkBusy(&pending);
    pthread_mutex_lock(&mu_);
    bool asking = rules_.state() == DialRules::kAsking;
    pthread_mutex_unlock(&mu_);
    return busy || pending || asking;
}

// An ask of the rules' own is not a link: what holds it back or ends it is the rest.
bool SerialListener::linkBusy(bool* pending) {
    pthread_mutex_lock(&mu_);
    DialRules::State state = rules_.state();
    bool busy = deciding_ || !ready_.empty() || (state != DialRules::kIdle && state != DialRules::kAsking);
    *pending = reqAt_ > 0 && monoNow() < hotUntil_;
    pthread_mutex_unlock(&mu_);
    return busy || serialLinksOpen() != 0;
}

DialRules::Facts SerialListener::dialFacts() {
    DialRules::Facts facts;
    facts.registered = spp_.state() == SppService::kRegistered && spp_.attached();
    facts.linkOfOurs = linkBusy(&facts.requestPending);
    if (ours_ != 0) facts.ours = ours_->get();
    pthread_mutex_lock(&mu_);
    facts.pairingOpen = pairingOpen_;
    pthread_mutex_unlock(&mu_);
    return facts;
}

// The next ask, if one is due: ServiceListQuery for the phone, and the dial once its answer names the agent's
// service. A phone's pending request holds back every ask but the one it brings itself, signalWaitMs after it
// (DialRules::next()).
void SerialListener::maybeDial(int64_t now) {
    DialRules::Facts facts = dialFacts();
    DialRules::Dial d;
    pthread_mutex_lock(&mu_);
    bool due = rules_.next(now, facts, &d);
    pthread_mutex_unlock(&mu_);
    if (!due) return;
    dial_ = d;
    if (spp_.queryServices(d.address)) {
        pthread_mutex_lock(&mu_);
        ++counts_.asks;
        pthread_mutex_unlock(&mu_);
        dialPhase_ = kDialAsk;   // the log says it with the answer
        return;
    }
    // Not sent: as an ask answered with an error, which cost the car nothing.
    pthread_mutex_lock(&mu_);
    DialRules::Listing what = rules_.notSent(now);
    pthread_mutex_unlock(&mu_);
    listed(what, 0, kAskNotSent, now, now);
}

// While ServiceListQuery is out: a link of the agent's that has come, a phone's request that is pending or the
// registration's going ends the ask first; then the answers that have come, and the ask's time. An answer to an ask
// that was ended, or for another phone, is passed over (DialRules::listing()); it is fed to the rules all the same,
// since it tells that the car is free for the next.
void SerialListener::tendAsk(const std::vector<std::pair<ServiceList, int64_t> >& lists, int64_t now) {
    if (dialPhase_ != kDialAsk && lists.empty()) return;
    // The facts as they are now: what brought the ask must still hold for an answer that lists the service.
    DialRules::Facts facts = dialFacts();
    if (dialPhase_ == kDialAsk) {
        pthread_mutex_lock(&mu_);
        bool repoint = rules_.repointWaits(now);
        bool still = rules_.keepAsking(facts, now);
        pthread_mutex_unlock(&mu_);
        if (!still) {
            dialPhase_ = kDialNone;
            askEnded(facts.linkOfOurs ? "a link of the agent's is up or being decided on"
                     : repoint ? "a re-point request waits for the car's answer, and no dial may follow the ask meanwhile"
                     : facts.requestPending ? "a phone has asked the car for a link"
                                            : "the serial service is not registered");
        }
    }
    for (size_t i = 0; i < lists.size(); ++i) {
        const ServiceList& l = lists[i].first;
        bool listedOurs = l.ok && l.error == 0 && listsService(l, config_.name);
        pthread_mutex_lock(&mu_);
        DialRules::Listing what = rules_.listing(l.address, l.ok ? l.error : kNoError, listedOurs, facts, now);
        pthread_mutex_unlock(&mu_);
        if (what == DialRules::kNotThisAsk || dialPhase_ != kDialAsk) continue;
        listed(what, &l, kAskAnswered, lists[i].second, now);
    }
    if (dialPhase_ != kDialAsk) return;
    pthread_mutex_lock(&mu_);
    bool overdue = rules_.askOverdue(now);
    DialRules::Listing what = overdue ? rules_.listingLost(now) : DialRules::kNotThisAsk;
    pthread_mutex_unlock(&mu_);
    if (overdue) listed(what, 0, kAskNoAnswer, now, now);
}

// The line for an ask: said when its answer differs from the last that phone's asks had, and otherwise only
// counted; the next line that differs says how many came.
void SerialListener::askSay(const std::string& answer, const std::string& text) {
    Repeat& last = askRepeats_[dial_.number];
    if (last.answer == answer) {
        ++last.times;
        return;
    }
    std::string before;
    if (last.times > 0) {
        char times[160];
        snprintf(times, sizeof times, " (%s came %u more time%s before this)", last.answer.c_str(), last.times,
                 last.times == 1 ? "" : "s");
        before = times;
    }
    last.answer = answer;
    last.times = 0;
    say("phone %u (%s): %s%s. The same answer again is only counted", dial_.number, whyName(dial_.why), text.c_str(),
        before.c_str());
}

// The line for an ask that is ended, by the same rule with a record of its own: said when its reason differs from
// that phone's last ended ask, and otherwise only counted; the next line of this kind that differs says how many
// came. The answers' record is not touched, so asks that are ended between answers that do not list the service
// bring no line for either.
void SerialListener::askEnded(const std::string& reason) {
    Repeat& last = askEnds_[dial_.number];
    if (last.answer == reason) {
        ++last.times;
        return;
    }
    std::string before;
    if (last.times > 0) {
        char times[200];
        snprintf(times, sizeof times, " (ended %u more time%s before this: %s)", last.times, last.times == 1 ? "" : "s",
                 last.answer.c_str());
        before = times;
    }
    last.answer = reason;
    last.times = 0;
    say("phone %u (%s): the ask for its service list is ended, and its answer will be passed over: %s%s. The same "
        "again is only counted", dial_.number, whyName(dial_.why), reason.c_str(), before.c_str());
}

// What an ask came to: `list` is the answer, which came at `cameAt`, or 0 for none (`how` says why). The dial goes
// at once if the answer named the agent's service.
void SerialListener::listed(DialRules::Listing what, const ServiceList* list, AskEnd how, int64_t cameAt, int64_t now) {
    pthread_mutex_lock(&mu_);
    dial_ = rules_.current();
    unsigned misses = rules_.askMisses(dial_.address);
    if (what == DialRules::kDialNow || what == DialRules::kNoLongerDue) ++counts_.asksListed;
    else if (what == DialRules::kNotListed) ++counts_.asksNotListed;
    else if (how == kAskNoAnswer) ++counts_.asksNoAnswer;
    else ++counts_.askErrors;
    pthread_mutex_unlock(&mu_);
    dialPhase_ = kDialNone;
    char text[400];
    double ms = (double)(cameAt - dial_.askedAt);
    const char* name = config_.name.c_str();
    switch (what) {
    case DialRules::kDialNow: {
        // The dial's own line says what comes of it: this is said only after asks that brought no dial.
        Repeat& last = askRepeats_[dial_.number];
        snprintf(text, sizeof text, "its service list came in %.0f ms and names the agent's service '%s' again: the "
                 "car dials it", ms, name);
        if (last.answer.empty()) last.answer = "listed";
        else askSay("listed", text);
        sendDial(now);
        return;
    }
    case DialRules::kNoLongerDue:
        snprintf(text, sizeof text, "its service list names the agent's service '%s', and what brought the ask no "
                 "longer holds (since the ask the phone has left the car, lost its pairing or had pairing trouble, or "
                 "it is not one of the agent's phones and the pairing window is not open): it is not dialled", name);
        askSay("no longer due", text);
        return;
    case DialRules::kNotListed:
        snprintf(text, sizeof text, "its service list came in %.0f ms and does not name the agent's service '%s': its "
                 "app is not listening, and it is not dialled", ms, name);
        askSay("not listed", text);
        return;
    default:
        break;
    }
    std::string answer;
    if (how == kAskNotSent) {
        answer = "the request could not be sent";
        snprintf(text, sizeof text, "the car could not be asked for its service list: taken as not listening, and it "
                 "is not dialled");
    } else if (how == kAskNoAnswer || list == 0) {
        answer = "no answer";
        snprintf(text, sizeof text, "no answer to the ask for its service list in %u ms: taken as not listening, and "
                 "it is not dialled", config_.dial.askLimitMs);
    } else if (list->rejected) {
        answer = "a rejection";
        snprintf(text, sizeof text, "the car's Bluetooth service rejected the ask for its service list: taken as not "
                 "listening, and it is not dialled");
    } else {
        answer = list->error != kNoError ? gapErrorText(list->error, "") : std::string("a reply not read");
        snprintf(text, sizeof text, "the car answered the ask for its service list with %s: taken as not listening, and "
                 "it is not dialled", answer.c_str());
    }
    askSay(answer, text);
    // An ask does no harm: the next poll asks again whatever came of this one. Said once in a run.
    if (misses == kAskMissesToSay)
        say("phone %u: %u asks of its service list in a row had an error or no answer; polls go on asking it", dial_.number,
            kAskMissesToSay);
}

// The phone's service list named the agent's service: ServiceConnect, at once.
void SerialListener::sendDial(int64_t now) {
    if (spp_.connectService(kDialDevice, kDialInstance, dial_.address, config_.name)) {
        pthread_mutex_lock(&mu_);
        ++counts_.dials;
        pthread_mutex_unlock(&mu_);
        dialPhase_ = kDialWait;   // the log says it with the answer
        return;
    }
    pthread_mutex_lock(&mu_);
    rules_.reply(dial_.address, config_.name, config_.name, kNoError, now);
    pthread_mutex_unlock(&mu_);
    say("phone %u: the car could not be asked to dial it (%s): again in %u s", dial_.number, whyName(dial_.why),
        config_.dial.retryMs / 1000);
}

// A dial that failed although the phone's service list named the agent's service: the car's record of phone-opened
// links may be wrong from here on. Said once, when that becomes so, with what became of the re-point, which keeps a
// link end that the car books against the agent's instance from counting against the phone's connected services and
// leaves the record as it is (dial_rules.hpp).
void SerialListener::doubtNow(bool before, const std::string& answer, RepointWent repoint) {
    pthread_mutex_lock(&mu_);
    bool now = rules_.recordDoubtful();
    pthread_mutex_unlock(&mu_);
    if (before == now) return;
    if (!now) {
        say("phone %u: the car's dial answered 0: its record of phone-opened serial links is taken as right again",
            dial_.number);
        return;
    }
    const char* sent =
        repoint == kRepointWent
            ? "That record may stay wrong; the agent has sent the car one re-point request, so that the end of such a "
              "link that the car books against the agent's instance is not counted against the phone's connected "
              "services"
        : repoint == kRepointNotSent
            ? "That record may stay wrong; the re-point request, which keeps the end of such a link that the car books "
              "against the agent's instance from counting against the phone's connected services, could not be sent"
        : repoint == kRepointStopped
            ? "That record may stay wrong, and no re-point request is sent for it: they have stopped in this run"
            : "That record may stay wrong; should the car answer the dial after all, one re-point request is sent then, "
              "so that the end of such a link that the car books against the agent's instance is not counted against "
              "the phone's connected services";
    say("phone %u: the car's dial came to %s although the phone's service list named the agent's service. Such a dial "
        "may leave the car's record of phone-opened serial links wrong, and the car may then refuse such links, another "
        "program's too, after one of them ends. %s; serial.status says the record is doubted until a dial is answered "
        "0, the car says that such a link ended, or its Bluetooth service goes", dial_.number, answer.c_str(), sent);
    char text[200];
    snprintf(text, sizeof text, "phone %u: the car's dial came to %s after the phone listed the agent's service: the "
             "car's record of phone-opened links may be wrong", dial_.number, answer.c_str());
    problem("record-doubtful", text);
}

// The mark that a dialled link has reached the phone's app is what leaves the phone's own links to another program
// (leftToOther()). Dials that keep failing say that it no longer holds: the agent gives it up, on its own thread,
// which the store of paired phones belongs to; it is handed over as a link is, through ready_'s pipe.
void SerialListener::dialFailed() {
    pthread_mutex_lock(&mu_);
    unsigned fails = rules_.dialFailures(dial_.address);
    pthread_mutex_unlock(&mu_);
    if (fails < kDialFailsToUnmark || ours_ == 0 || !ours_->dialledBefore(dial_.address)) return;
    std::string key = addressKey(dial_.address);
    pthread_mutex_lock(&mu_);
    bool queued = false;
    for (size_t i = 0; i < undialled_.size() && !queued; ++i) queued = addressKey(undialled_[i]) == key;
    if (!queued) undialled_.push_back(dial_.address);
    pthread_mutex_unlock(&mu_);
    if (!queued) {
        char byte = 1;
        if (write(wakeWrite_, &byte, 1) < 0) {}   // a full pipe has woken the agent already
    }
    if (!undialSaid_.insert(key).second) return;
    say("phone %u: %u dials in a row gave the agent no link: the mark that a dialled link has reached its app is given "
        "up, and a link the phone opens itself is decided on again as any other", dial_.number, fails);
}

std::vector<std::string> SerialListener::takeUndialled() {
    std::vector<std::string> taken;
    pthread_mutex_lock(&mu_);
    taken.swap(undialled_);
    pthread_mutex_unlock(&mu_);
    for (size_t i = 0; i < taken.size(); ++i) {
        char byte;
        if (read(wakeRead_, &byte, 1) < 0) {}   // the byte that announced it
    }
    return taken;
}

// The car's answer to the dial. A re-point it brought has gone already (sppConnectReply()).
void SerialListener::dialAnswered(const ServiceConnectReply& reply, int64_t now, RepointWent repoint) {
    pthread_mutex_lock(&mu_);
    bool doubted = rules_.recordDoubtful();
    DialRules::Reply r = rules_.reply(reply.address, reply.service, config_.name, reply.error, now);
    int64_t backOff = rules_.backedOffUntil(dial_.address) - now;
    pthread_mutex_unlock(&mu_);
    if (r == DialRules::kNotThis) return;
    pthread_mutex_lock(&mu_);
    counts_.lastResult = reply.error == kNoError ? "rejected" : gapErrorName(reply.error);
    if (reply.error == 0) ++counts_.answeredOk;
    else if (reply.error == 5) ++counts_.notListening;
    else if (reply.error == 14) ++counts_.notInReach;
    else if (reply.error == 2) ++counts_.busy;
    else ++counts_.otherAnswers;
    pthread_mutex_unlock(&mu_);
    std::string answer = reply.error == kNoError ? std::string("a rejection") : gapErrorText(reply.error, "");
    bool stale = r == DialRules::kLookForNode && access(config_.dial.node.c_str(), F_OK) == 0;
    std::string meaning;
    char line[96];
    // A dial that failed is not made again soon: it may have left the car's record wrong (dial_rules.hpp).
    snprintf(line, sizeof line, "it is not dialled by a poll for %lld s", (long long)(backOff / 1000));
    std::string backedOff = line;
    switch (r) {
    case DialRules::kOpenNode: meaning = "the link is up"; break;
    case DialRules::kLookForNode:
        meaning = stale ? "a link on the instance is up already"
                        : "a link on the instance is up already, and its node is not there: again later";
        break;
    case DialRules::kNotListening:
        // The ask before it listed the agent's service: the app stopped listening since, or something else failed.
        meaning = "the phone did not take the link, although its service list named the agent's service just before: "
                  "its app may have stopped listening since, or something else went wrong; " + backedOff;
        break;
    case DialRules::kUnreachable:
        meaning = "the phone is not in reach, and " + backedOff;
        break;
    case DialRules::kFailed:
        meaning = backedOff;
        break;
    default:
        snprintf(line, sizeof line, "again in %u s", config_.dial.retryMs / 1000);
        meaning = line;
        break;
    }
    // One line for the dial, with its answer; the same answer to this phone again is only counted, and the next line
    // that differs says how many times it came.
    Repeat& last = repeats_[dial_.number];
    bool same = reply.error != 0 && last.answer == answer;
    std::string before;
    if (same) {
        ++last.times;
    } else {
        if (last.times > 0) {
            char times[160];
            snprintf(times, sizeof times, " (%s came %u more time%s before this)", last.answer.c_str(), last.times,
                     last.times == 1 ? "" : "s");
            before = times;
        }
        last.answer = answer;
        last.times = 0;
        say("phone %u (%s): the car's dial answered %s in %.0f ms: %s%s%s", dial_.number, whyName(dial_.why),
            answer.c_str(), (double)(now - dial_.at), meaning.c_str(), before.c_str(),
            reply.error != 0 ? ". The same answer again is only counted" : "");
    }
    // What is not how a dial goes when all is well goes to the problem file: an answer other than 0, 5 and 14.
    if (reply.error != 0 && reply.error != 5 && reply.error != 14) {
        char text[160];
        snprintf(text, sizeof text, "phone %u: the car's dial answered %s", dial_.number, answer.c_str());
        problem(std::string("dial-") + (reply.error == kNoError ? "rejected" : gapErrorName(reply.error)), text);
    }
    doubtNow(doubted, answer, repoint);
    if (r == DialRules::kNotListening || r == DialRules::kUnreachable || r == DialRules::kFailed) dialFailed();
    switch (r) {
    case DialRules::kOpenNode:
        openDialled(now);
        return;
    case DialRules::kLookForNode:
        if (stale) {
            pthread_mutex_lock(&mu_);
            rules_.stale();
            pthread_mutex_unlock(&mu_);
            endLeftBehind(std::vector<std::string>(1, dial_.address), "a link left behind on the dialled instance",
                          kCleanBusy);
            return;
        }
        pthread_mutex_lock(&mu_);
        rules_.notThere(now);
        pthread_mutex_unlock(&mu_);
        break;
    default:
        break;
    }
    dialPhase_ = kDialNone;
}

// Reply 0: the dialled link's node is there. It is opened at once and read, and what the phone says first decides.
void SerialListener::openDialled(int64_t now) {
    const std::string& path = config_.dial.node;
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) {
        say("phone %u: open %s: %s. The car is asked to end the link", dial_.number, path.c_str(), strerror(errno));
        pthread_mutex_lock(&mu_);
        rules_.notTheApp(now);
        pthread_mutex_unlock(&mu_);
        dialFailed();
        startCleanup(std::vector<std::string>(1, dial_.address), "its node could not be opened");
        return;
    }
    closeOnExec(fd);
    dialReader_ = NodeReader::start(fd, path, log_, config_.readNode);
    if (dialReader_ == 0) {
        pthread_mutex_lock(&mu_);
        rules_.notTheApp(now);
        pthread_mutex_unlock(&mu_);
        dialFailed();
        startCleanup(std::vector<std::string>(1, dial_.address), "its node could not be read");
        return;
    }
    dialGot_.clear();
    dialOpenedAt_ = monoNow();
    dialPhase_ = kDialMagic;
}

// The dialled node is open: the magic bytes are waited for, SerialConfig::dial.magicWaitMs from the open, since the
// app accepts the link before it writes. No gate is in front of the node and nobody else's link is on it, so
// nothing is let go: a link that does not open with them is ended.
void SerialListener::readMagic(int64_t now) {
    const std::string& path = config_.dial.node;
    bool none = dialGot_.empty();
    NodeReader::Got what = dialReader_->next(10, &dialGot_);
    double after = (monoNow() - dialOpenedAt_) * 1000.0;
    if (what == NodeReader::kEnded) {
        dialReader_->finish();
        dialReader_ = 0;
        pthread_mutex_lock(&mu_);
        rules_.notTheApp(now);
        rules_.ended(now);
        int64_t backOff = rules_.backedOffUntil(dial_.address) - now;
        pthread_mutex_unlock(&mu_);
        dialPhase_ = kDialNone;
        say("phone %u: %s closed %.0f ms after the open, before the app said anything: the phone is not dialled by a "
            "poll for %lld s", dial_.number, path.c_str(), after, (long long)(backOff / 1000));
        dialFailed();
        return;
    }
    if (what == NodeReader::kData && none)
        say("%s: its first message came %.0f ms after the open", path.c_str(), after);
    size_t at = 0;
    uint64_t leftOver = 0;
    Opening o = judgeStart(dialGot_, &at, &leftOver);
    if (at < dialGot_.size() && o == kOpenMagic) {
        std::vector<uint8_t> first(dialGot_.begin() + (long)at, dialGot_.end());
        DialledLink* share = new DialledLink();
        Link* link = startLink(dialReader_, path, log_, first, config_.lingerMs, dial_.address, share, config_.writeNode);
        dialReader_ = 0;
        pthread_mutex_lock(&mu_);
        rules_.linkUp(now);
        if (link != 0) ready_.push_back(link);
        pthread_mutex_unlock(&mu_);
        // The agent marks the phone `dialled` again as it takes the link (agent.hpp): a later run of failed dials is
        // said anew.
        undialSaid_.erase(addressKey(dial_.address));
        if (link == 0) {
            share->release();
            pthread_mutex_lock(&mu_);
            rules_.ending();
            pthread_mutex_unlock(&mu_);
            startCleanup(std::vector<std::string>(1, dial_.address), "the link could not be set up");
            return;
        }
        dialLink_ = share;
        dialPhase_ = kDialLinked;
        if (leftOver > 0)
            say("%s: %llu B left over from an earlier link came first, as whole frames, and were dropped", path.c_str(),
                (unsigned long long)leftOver);
        say("phone %u: %s opened with the magic bytes %.0f ms after the open: the dialled link is the agent's",
            dial_.number, path.c_str(), after);
        char byte = 1;
        if (write(wakeWrite_, &byte, 1) < 0) {}   // a full pipe has woken the agent already
        return;
    }
    bool other = at < dialGot_.size() && o == kOpenOther;
    if (!other && after < config_.dial.magicWaitMs) return;
    pthread_mutex_lock(&mu_);
    rules_.notTheApp(now);
    int64_t backOff = rules_.backedOffUntil(dial_.address) - now;
    ++counts_.notTheApp;
    counts_.lastResult = "not the app";
    pthread_mutex_unlock(&mu_);
    say("phone %u: something answered on the agent's UUID that is not the app: %s. The car is asked to end the link, "
        "and the phone is not dialled by a poll for %lld s", dial_.number,
        other ? "the link did not open with the magic bytes"
              : (dialGot_.empty() ? "nothing came in the time" : "the magic bytes did not come in the time"),
        (long long)(backOff / 1000));
    char text[160];
    snprintf(text, sizeof text, "phone %u: something answered on the agent's UUID that is not the app", dial_.number);
    problem("dial-not-the-app", text);
    dialFailed();
    // Read, with what comes dropped, until it has ended.
    dialReader_->drain();
    startCleanup(std::vector<std::string>(1, dial_.address), "it was not the app's");
}

// The dialled link is the agent's: its end, whichever side makes it. When the agent has ended it (link.closing,
// agent.hpp), the phone has DialConfig::closeWaitMs to end the link; then the car is asked to, and once it has, or
// DialConfig::disconnectWaitMs later, the node is closed: unlike a phone's own link's, it is not kept and read.
void SerialListener::watchDialled(int64_t now) {
    double gone = dialLink_->goneSince();
    if (!dialLink_->hasEnded() && gone > 0 && monoNow() - gone >= config_.dial.closeWaitMs / 1000.0) {
        pthread_mutex_lock(&mu_);
        rules_.ending();
        pthread_mutex_unlock(&mu_);
        say("phone %u: the agent ended the dialled link and the phone has not in %u ms: the car is asked to end it",
            dial_.number, config_.dial.closeWaitMs);
        startCleanup(std::vector<std::string>(1, dial_.address), "the agent ended the link");
        return;
    }
    if (!dialLink_->hasEnded()) return;
    dialLink_->release();
    dialLink_ = 0;
    pthread_mutex_lock(&mu_);
    rules_.ended(now);
    pthread_mutex_unlock(&mu_);
    dialPhase_ = kDialNone;
    say("phone %u: the dialled link has ended, %.1f s after the dial", dial_.number, (double)(now - dial_.at) / 1000.0);
}

// Ends what is up on the dialled instance: ServiceDisconnect for each of `phones` in turn, each answer waited for
// SerialConfig::dial.disconnectWaitMs at most, until the node has gone. A phone that has no such link answers an
// error, which does no harm. `afterReset`, at the listener's passes: a ServiceDisconnect of serial.reset's that
// waits for its turn goes before the next of these. endDial(), which has no passes to send that one at, does not
// wait for it.
void SerialListener::startCleanup(const std::vector<std::string>& phones, const std::string& why) {
    cleanupPhones_ = phones;
    cleanupWhy_ = why;
    discAt_ = 0;
    cleanupKind_ = kCleanOwn;
    cleanupQuiet_ = false;
    cleanupHeld_.clear();
    cleanupOutcome_.clear();
    dialPhase_ = kDialCleanup;
}

void SerialListener::tendCleanup(int64_t now, bool afterReset) {
    const std::string& path = config_.dial.node;
    bool there = access(path.c_str(), F_OK) == 0;
    if (dialReader_ != 0 && dialReader_->ended()) there = false;
    if (dialLink_ != 0 && dialLink_->hasEnded()) there = false;
    // The answer, and with an answer that ended the link, the node's going, are waited for.
    bool timeUp = discAt_ != 0 && monoNow() - discAt_ >= config_.dial.disconnectWaitMs / 1000.0;
    bool done = discAt_ != 0 && (!there || timeUp || (discAnswered_ && discError_ != 0));
    if (discAt_ != 0 && !done) return;
    if (discAt_ != 0) {
        double ms = (monoNow() - discAt_) * 1000.0;
        pthread_mutex_lock(&mu_);
        unsigned n = rules_.number(discAddress_);
        pthread_mutex_unlock(&mu_);
        // The node can go a moment before the answer comes, which is then logged on its own: that is no "no answer",
        // which is said only where the wait ran out.
        bool goneFirst = !discAnswered_ && !there && !timeUp;
        std::string answer = goneFirst ? std::string("gone before an answer")
                             : !discAnswered_ ? std::string("no answer")
                                              : (discError_ == kNoError ? std::string("a rejection")
                                                                        : gapErrorText(discError_, ""));
        char line[512];
        if (goneFirst)
            snprintf(line, sizeof line, "phone %u: %s went %.0f ms after ServiceDisconnect on the dialled instance, before "
                     "its answer", n, path.c_str(), ms);
        else if (!discAnswered_)
            snprintf(line, sizeof line, "phone %u: no answer to ServiceDisconnect on the dialled instance in %.0f ms; %s is %s",
                     n, ms, path.c_str(), there ? "still there" : "gone");
        else
            snprintf(line, sizeof line, "phone %u: ServiceDisconnect on the dialled instance answered %s in %.0f ms, and %s %s",
                     n, answer.c_str(), ms, path.c_str(), there ? "is still there" : "is not there");
        cleanupSay(line);
        char key[96];
        snprintf(key, sizeof key, "phone %u: %s; ", n, answer.c_str());
        cleanupOutcome_ += key;
        discAt_ = 0;
    }
    if (there && !cleanupPhones_.empty()) {
        // serial.reset's ServiceDisconnect that waits for its turn goes first: tendReset() sends it at the next pass.
        if (afterReset) {
            pthread_mutex_lock(&mu_);
            bool resetWaits = reset_.waiting;
            pthread_mutex_unlock(&mu_);
            if (resetWaits) return;
        }
        std::string next = cleanupPhones_.front();
        // False while another ServiceDisconnect waits for its answer: on a later pass. One with no answer in
        // DialConfig::disconnectWaitMs, this clean-up's own among them, is given up then, so this cannot wait for ever.
        if (!spp_.disconnectService(kDialDevice, kDialInstance, next, config_.name, cleanupQuiet_)) {
            if (!spp_.attached()) cleanupPhones_.clear();
            return;
        }
        cleanupPhones_.erase(cleanupPhones_.begin());
        discAt_ = monoNow();
        discAddress_ = next;
        discAnswered_ = false;
        discError_ = kNoError;
        return;
    }
    bool left = cleanupKind_ != kCleanOwn;
    if (there && !left)
        say("%s is still there after the car was asked to end its link (%s): it is left as it is, and nothing of "
            "the agent's uses it", path.c_str(), cleanupWhy_.c_str());
    if (dialReader_ != 0) {
        dialReader_->finish();
        dialReader_ = 0;
    }
    if (dialLink_ != 0) {
        // Its threads stop reading and close the node within a tick.
        dialLink_->close(there ? "closed after the car was asked to end the link" : "ended by the car, as asked");
        dialLink_->release();
        dialLink_ = 0;
        say("phone %u: the dialled link has ended, %.1f s after the dial", dial_.number, (double)(now - dial_.at) / 1000.0);
    }
    pthread_mutex_lock(&mu_);
    // A link that a dial found and the car did not end: its phone is backed off, and not dialled again at once,
    // which would only find it again. One that did end has its phone dialled again (DialRules::stale()).
    if (cleanupKind_ == kCleanBusy && there) rules_.notEnded(now);
    else rules_.ended(now);
    // A link left behind when the agent started is gone, or as gone as the car will make it: the phones are dialled
    // afresh.
    if (cleanupKind_ == kCleanStart) rules_.afresh();
    int64_t backOff = rules_.backedOffUntil(dial_.address) - now;
    pthread_mutex_unlock(&mu_);
    if (left) leftOutcome(there);
    if (cleanupKind_ == kCleanStart && !cleanupQuiet_)
        say("the link left behind on %s is %s: the car dials the phones afresh", path.c_str(),
            there ? "still there, and is left alone" : "ended");
    if (cleanupKind_ == kCleanBusy && there && !cleanupQuiet_)
        say("phone %u is not dialled again at once, and not by a poll for %lld s", dial_.number,
            (long long)(backOff / 1000));
    cleanupKind_ = kCleanOwn;
    cleanupQuiet_ = false;
    cleanupHeld_.clear();
    cleanupOutcome_.clear();
    dialPhase_ = kDialNone;
}

// A line of the clean-up's: said, or held while the clean-up is a quiet one.
void SerialListener::cleanupSay(const std::string& line) {
    if (cleanupQuiet_) cleanupHeld_.push_back(line);
    else say("%s", line.c_str());
}

// What a clean-up of a link left behind came to. The first that leaves the node there is said in full and goes to
// the problem file; after it, one that comes to the same is only counted, and one that comes to something else is
// said with its held lines and how many came before it.
void SerialListener::leftOutcome(bool there) {
    const std::string& path = config_.dial.node;
    std::string outcome = cleanupOutcome_ + (there ? "still there" : "gone");
    char before[96] = "";
    if (leftFailing_ && leftTimes_ > 0)
        snprintf(before, sizeof before, " (the same outcome as before came %u more time%s before this)", leftTimes_,
                 leftTimes_ == 1 ? "" : "s");
    if (!there) {
        if (cleanupQuiet_) {
            for (size_t i = 0; i < cleanupHeld_.size(); ++i) say("%s", cleanupHeld_[i].c_str());
            say("the link left behind on %s has ended this time%s", path.c_str(), before);
        }
        leftFailing_ = false;
        leftOutcome_.clear();
        leftTimes_ = 0;
        return;
    }
    if (!leftFailing_) {
        say("%s is still there after the car was asked to end its link (%s): it is left as it is, and nothing of "
            "the agent's uses it. The same outcome again is only counted", path.c_str(), cleanupWhy_.c_str());
        problem("left-behind-stays", "a link left behind on the dialled instance was not ended");
        leftFailing_ = true;
        leftOutcome_ = outcome;
        leftTimes_ = 0;
        return;
    }
    if (outcome == leftOutcome_) {
        ++leftTimes_;
        return;
    }
    for (size_t i = 0; i < cleanupHeld_.size(); ++i) say("%s", cleanupHeld_[i].c_str());
    say("%s is still there after the car was asked again to end its link (%s)%s. The same outcome again is only "
        "counted", path.c_str(), cleanupWhy_.c_str(), before);
    leftOutcome_ = outcome;
    leftTimes_ = 0;
}

// A link on the dialled instance that an earlier agent left behind, when it was killed: never taken as the
// agent's. ServiceDisconnect for the phone that was dialled, if that is known, and otherwise for each phone that is
// connected to the car in turn, until the node has gone; then what `kind` says. While earlier clean-ups of one left
// it there, this one is quiet until it is known whether it comes to something else (leftOutcome()).
void SerialListener::endLeftBehind(const std::vector<std::string>& phones, const std::string& why, CleanupKind kind) {
    pthread_mutex_lock(&mu_);
    ++counts_.leftBehind;
    pthread_mutex_unlock(&mu_);
    startCleanup(phones, why);
    cleanupKind_ = kind;
    cleanupQuiet_ = leftFailing_;
    char line[300];
    snprintf(line, sizeof line, "%s is there: %s, which the agent does not take. The car is asked to end it",
             config_.dial.node.c_str(), why.c_str());
    cleanupSay(line);
    if (!leftFailing_) problem("left-behind", "the car is asked to end a link left behind on the dialled instance");
}

// The car's Bluetooth service has refused the registration while the dialled node is there. A link on the agent's
// service that an earlier agent left behind, when it was killed, makes the car answer 5 to SppUuidRegister and to
// SppUuidDeregister alike, so it is ended first: the car's device list is asked for, and ServiceDisconnect goes for
// each phone there in turn, as for a link left behind at the start, with no registration needed. Once for each
// refusal after a set-up: false, and the refusal goes as any other, if the dial is off, this has been tried since,
// the car's Bluetooth service is not attached, a dial of the agent's is under way, the node is not there, or the
// device list cannot be asked for.
bool SerialListener::startLeftForRegistration() {
    if (!config_.dial.on || leftTried_ || dialPhase_ != kDialNone || !spp_.attached() ||
        access(config_.dial.node.c_str(), F_OK) != 0)
        return false;
    leftTried_ = true;
    if (!spp_.askDeviceList()) {
        say("the car's Bluetooth service did not take the registration while %s is there, and its device list could "
            "not be asked for to end that link", config_.dial.node.c_str());
        return false;
    }
    say("the car's Bluetooth service did not take the registration while %s is there: a link an earlier agent left "
        "behind may hold the service. The car is asked to end it first, for each phone in its device list, and the "
        "service is registered again at once if it has gone", config_.dial.node.c_str());
    leftPhase_ = kLeftList;
    leftAt_ = monoNow();
    return true;
}

// At each pass while it runs: kLeftGoesOn until it has ended; kLeftRegister when the node has gone, and kLeftGaveUp
// when the device list did not come in DialConfig::listWaitMs, no phone is there to name, the clean-up did not end
// within DialConfig::registerCleanupMs, or the node is still there after it.
SerialListener::LeftStep SerialListener::tendLeftForRegistration() {
    const std::string& path = config_.dial.node;
    double since = monoNow() - leftAt_;
    if (leftPhase_ == kLeftList) {
        std::vector<std::string> present;
        pthread_mutex_lock(&mu_);
        bool known = rules_.presence().known();
        for (size_t i = 0; known && i < rules_.presence().phones().size(); ++i) {
            if (Presence::present(rules_.presence().phones()[i])) present.push_back(rules_.presence().phones()[i].address);
        }
        pthread_mutex_unlock(&mu_);
        if (!known && since < config_.dial.listWaitMs / 1000.0) return kLeftGoesOn;
        leftPhase_ = kLeftNone;
        if (!known) {
            say("the car's device list did not come in %u ms: the link left behind on %s is not ended",
                config_.dial.listWaitMs, path.c_str());
            return kLeftGaveUp;
        }
        if (present.empty()) {
            say("no phone is connected to the car to end the link left behind on %s for", path.c_str());
            return kLeftGaveUp;
        }
        endLeftBehind(present, "a link an earlier agent left behind when it was stopped without ending it", kCleanRegister);
        leftPhase_ = kLeftEnding;
        return kLeftGoesOn;
    }
    // The clean-up runs at the listener's passes (tendDial()), and says what each ServiceDisconnect came to.
    if (dialPhase_ == kDialCleanup) {
        if (since < config_.dial.registerCleanupMs / 1000.0) return kLeftGoesOn;
        leftPhase_ = kLeftNone;
        say("the clean-up of the link left behind on %s has not ended in %u s", path.c_str(),
            config_.dial.registerCleanupMs / 1000);
        return kLeftGaveUp;
    }
    leftPhase_ = kLeftNone;
    if (access(path.c_str(), F_OK) == 0) return kLeftGaveUp;
    say("the link left behind on %s has ended: the serial service is registered again now", path.c_str());
    staleAtStart_ = false;
    return kLeftRegister;
}

// On the listener's thread at every pass: what the car has said for the dial, and the dial's next step.
void SerialListener::tendDial() {
    if (!config_.dial.on) return;
    int64_t now = msOf(monoNow());
    // A link of the agent's that comes answers a phone's request; one that ends has polls come at the first rate.
    unsigned links = serialLinksOpen();
    if (links != lastLinks_) {
        pthread_mutex_lock(&mu_);
        if (links > lastLinks_) rules_.linkCame();
        else rules_.linkEnded(now);
        pthread_mutex_unlock(&mu_);
        lastLinks_ = links;
    }
    std::vector<ConnectAnswer> answers;
    std::vector<std::pair<ServiceList, int64_t> > lists;
    std::vector<ServiceDisconnectReply> disconnects;
    std::vector<ServiceDisconnected> ends;
    std::vector<std::string> pairings;
    pthread_mutex_lock(&mu_);
    answers.swap(connectReplies_);
    lists.swap(listReplies_);
    disconnects.swap(disconnectReplies_);
    ends.swap(dialEnds_);
    pairings.swap(pairingInforms_);
    std::vector<unsigned> troubled;
    for (size_t i = 0; i < pairings.size(); ++i) {
        unsigned n = rules_.pairingTrouble(pairings[i]);
        if (n != 0) troubled.push_back(n);
    }
    pthread_mutex_unlock(&mu_);
    for (size_t i = 0; i < troubled.size(); ++i)
        say("phone %u: the car is pairing it anew, or a pairing failed: it is not dialled until the car's device list "
            "shows it paired again after a change", troubled[i]);
    for (size_t i = 0; i < disconnects.size(); ++i) {
        if (discAt_ == 0 || addressKey(disconnects[i].address) != addressKey(discAddress_)) continue;
        discAnswered_ = true;
        discError_ = disconnects[i].error;
    }
    for (size_t i = 0; i < ends.size(); ++i) {
        // An end that names the re-point's address is of what a re-point answered 0 left, never of a dialled link.
        if (addressKey(ends[i].address) == addressKey(kRepointAddress)) continue;
        // The car says that the link on the dialled instance has ended: its node is closed now.
        if (dialLink_ != 0) dialLink_->close(ends[i].reason == 1 ? "the phone ended the link" : "the car ended the link");
    }
    // The phone may have ended it meanwhile: then there is nothing to end.
    if (staleAtStart_ && dialPhase_ == kDialNone && access(config_.dial.node.c_str(), F_OK) != 0) {
        staleAtStart_ = false;
        leftFailing_ = false;
        leftOutcome_.clear();
        leftTimes_ = 0;
        say("%s, which was there when the agent started, has gone without the agent ending it", config_.dial.node.c_str());
    }
    if (staleAtStart_ && dialPhase_ == kDialNone && spp_.state() == SppService::kRegistered) {
        std::vector<std::string> present;
        pthread_mutex_lock(&mu_);
        bool known = rules_.presence().known();
        for (size_t i = 0; known && i < rules_.presence().phones().size(); ++i) {
            if (Presence::present(rules_.presence().phones()[i])) present.push_back(rules_.presence().phones()[i].address);
        }
        if (known) rules_.stale();
        pthread_mutex_unlock(&mu_);
        if (known) {
            staleAtStart_ = false;
            if (present.empty()) {
                pthread_mutex_lock(&mu_);
                rules_.ended(now);
                pthread_mutex_unlock(&mu_);
                say("%s was there when the agent started, and no phone is connected to the car to end it for: a dial "
                    "that finds it ends it", config_.dial.node.c_str());
            } else {
                endLeftBehind(present, "a link an earlier agent left behind when it was stopped without ending it",
                              kCleanStart);
            }
        }
    }
    // The answers to asks, whatever the phase: one to an ask that was ended still tells that the car is free.
    tendAsk(lists, now);
    switch (dialPhase_) {
    case kDialWait:
        for (size_t i = 0; i < answers.size() && dialPhase_ == kDialWait; ++i)
            dialAnswered(answers[i].reply, answers[i].at, answers[i].repoint);
        if (dialPhase_ == kDialWait) {
            pthread_mutex_lock(&mu_);
            bool overdue = rules_.overdue(now);
            bool doubted = rules_.recordDoubtful();
            if (overdue) {
                rules_.gaveUp(now);
                ++counts_.noAnswer;
                counts_.lastResult = "no answer";
            }
            pthread_mutex_unlock(&mu_);
            if (overdue) {
                bool there = access(config_.dial.node.c_str(), F_OK) == 0;
                say("phone %u: no answer to the car's dial in %u s; %s is %s. The car is asked to end what may be up, "
                    "and the phone is not dialled by a poll for a while", dial_.number,
                    config_.dial.replyLimitMs / 1000, config_.dial.node.c_str(), there ? "there" : "not there");
                char text[160];
                snprintf(text, sizeof text, "phone %u: no answer to the car's dial in %u s", dial_.number,
                         config_.dial.replyLimitMs / 1000);
                problem("dial-no-answer", text);
                doubtNow(doubted, "no answer", kRepointNone);
                dialFailed();
                startCleanup(std::vector<std::string>(1, dial_.address), "the dial had no answer");
                // One ServiceDisconnect, whether or not the node is there.
                if (spp_.disconnectService(kDialDevice, kDialInstance, dial_.address, config_.name)) {
                    discAt_ = monoNow();
                    discAddress_ = dial_.address;
                    discAnswered_ = false;
                    discError_ = kNoError;
                    cleanupPhones_.clear();
                }
            }
        }
        break;
    case kDialMagic:
        readMagic(now);
        break;
    case kDialLinked:
        watchDialled(now);
        break;
    case kDialCleanup:
        tendCleanup(now, true);
        break;
    default:
        break;
    }
    // After the dial's own lines, and before the next ask: a re-point given up in time lets that go.
    tendRepoint(now);
    if (dialPhase_ == kDialNone) maybeDial(now);
}

void SerialListener::tendRepoint(int64_t now) {
    bool giveUp = false;
    std::vector<RepointOutcome> ends;
    pthread_mutex_lock(&mu_);
    if (rules_.repointOverdue(now)) {
        // No answer in its time: not what is expected of the car. Done here, under the lock, so that an answer that
        // comes meanwhile on the Bluetooth service's thread finds none out and is passed over.
        RepointOutcome o;
        o.end = kRepointNoAnswer;
        o.ms = now - rules_.repointAt();
        o.number = repointNumber_;
        o.dialError = repointDialError_;
        rules_.repointDone();
        ++counts_.repointsUnexpected;
        repointStopped_ = true;
        repointEnds_.push_back(o);
        giveUp = true;
    }
    ends.swap(repointEnds_);
    pthread_mutex_unlock(&mu_);
    // So that the next ServiceConnect may go: a late answer is then another client's (SppService::giveUpConnect()).
    if (giveUp) spp_.giveUpConnect(kRepointAddress, kRepointService);
    for (size_t i = 0; i < ends.size(); ++i) {
        const RepointOutcome& o = ends[i];
        std::string dial = o.dialError == kNoError ? std::string("no error code") : gapErrorText(o.dialError, "");
        char line[600];
        if (o.end == kRepointExpected || o.end == kRepointUnsent) {
            // As the dial's lines: the same outcome for the same phone again is only counted, and the next line that
            // differs says how many came.
            std::string outcome = o.end == kRepointExpected ? "answered as expected" : "not sent";
            Repeat& last = repointRepeats_[o.number];
            if (last.answer == outcome) {
                ++last.times;
                continue;
            }
            char before[160] = "";
            if (last.times > 0)
                snprintf(before, sizeof before, " (%s came %u more time%s before this)", last.answer.c_str(), last.times,
                         last.times == 1 ? "" : "s");
            last.answer = outcome;
            last.times = 0;
            if (o.end == kRepointExpected)
                snprintf(line, sizeof line, "phone %u: the re-point request after the dial's answer %s was answered 5 "
                         "DEFAULT in %lld ms, as expected: a link end that the car books against the agent's instance "
                         "does not count against the phone's connected services%s. The same outcome again is only counted",
                         o.number, dial.c_str(), (long long)o.ms, before);
            else
                snprintf(line, sizeof line, "phone %u: the re-point request after the dial's answer %s could not be sent "
                         "(%s)%s. The same outcome again is only counted", o.number, dial.c_str(), o.why.c_str(), before);
            say("%s", line);
            continue;
        }
        if (o.end == kRepointDropped) {
            say("phone %u: the re-point request after the dial's answer %s had no answer when %s: it is given up, and "
                "not judged", o.number, dial.c_str(), o.why.c_str());
            continue;
        }
        // Not what is expected of the car: said once, as the re-points stop here, and in the problem file.
        char came[200];
        if (o.end == kRepointNoAnswer)
            snprintf(came, sizeof came, "had no answer in %u ms", config_.dial.repointLimitMs);
        else if (o.end == kRepointNoCode)
            snprintf(came, sizeof came, "was rejected, or its reply could not be read, in %lld ms", (long long)o.ms);
        else
            snprintf(came, sizeof came, "was answered %s in %lld ms", gapErrorText(o.error, "").c_str(), (long long)o.ms);
        bool linked = o.end == kRepointOther && o.error == 0;
        say("phone %u: the re-point request after the dial's answer %s %s%s, where 5 within %u ms is expected of the "
            "car: no more re-point requests are sent in this run of the agent%s", o.number, dial.c_str(), came,
            o.end == kRepointNoAnswer ? ", and is given up" : "", config_.dial.repointSlowMs,
            linked ? ". The car says it has a link for the request, so it is asked to end that link" : "");
        // The problem file's lines are short.
        snprintf(line, sizeof line, "phone %u: the re-point after a failed dial %s, not 5 within %u ms: no more are sent "
                 "in this run", o.number, came, config_.dial.repointSlowMs);
        problem("repoint-unexpected", line);
        if (linked) repointDisconnect_ = true;
    }
    // The ServiceDisconnect a re-point answered 0 owes: once none of another's waits. Its answer names an address no
    // phone has, so the dial's own ServiceDisconnects never take it for theirs (discAddress_).
    if (repointDisconnect_ &&
        (!spp_.attached() || spp_.disconnectService(kDialDevice, kDialInstance, kRepointAddress, kRepointService)))
        repointDisconnect_ = false;
}

void SerialListener::endRepoint(const char* why) {
    if (!config_.dial.on) return;
    tendRepoint(msOf(monoNow()));
    // A short wait for an answer that is due: the car's comes within some tens of milliseconds.
    double end = monoNow() + config_.dial.repointSlowMs / 1000.0;
    for (;;) {
        pthread_mutex_lock(&mu_);
        bool out = rules_.repointOut();
        pthread_mutex_unlock(&mu_);
        if (!out || monoNow() >= end) break;
        usleep(10000);
    }
    pthread_mutex_lock(&mu_);
    bool out = rules_.repointOut();
    if (out) {
        RepointOutcome o;
        o.end = kRepointDropped;
        o.number = repointNumber_;
        o.dialError = repointDialError_;
        o.why = why;
        rules_.repointDone();
        repointEnds_.push_back(o);
    }
    pthread_mutex_unlock(&mu_);
    if (out) spp_.giveUpConnect(kRepointAddress, kRepointService);
    tendRepoint(msOf(monoNow()));
    // A ServiceDisconnect that is owed and could not go yet: one more try, and then it is not owed any more.
    repointDisconnect_ = false;
}

// Ends what the dial has up, when the listener stops: a node being decided on is closed, and a link of the agent's
// on the dialled instance is ended by the car, the answer waited for, and its node closed; then a re-point that is out
// (which a dial's answer in the meantime may have brought) has a moment for its answer, and is given up. On the
// thread that stops the listener, once its own thread is gone.
void SerialListener::endDial(const char* why) {
    endDialSteps(why);
    endRepoint(why);
}

void SerialListener::endDialSteps(const char* why) {
    if (!config_.dial.on || dialPhase_ == kDialNone) return;
    if (dialPhase_ == kDialAsk) {
        // An ask brings no link: it is dropped, and its answer passed over.
        pthread_mutex_lock(&mu_);
        rules_.askDropped(msOf(monoNow()));
        pthread_mutex_unlock(&mu_);
        dialPhase_ = kDialNone;
        return;
    }
    if (dialPhase_ == kDialWait) {
        // Never a ServiceDisconnect while the dial is out: its answer is waited for, a while.
        double end = monoNow() + config_.dial.disconnectWaitMs / 1000.0;
        while (dialPhase_ == kDialWait && monoNow() < end) {
            std::vector<ConnectAnswer> answers;
            pthread_mutex_lock(&mu_);
            answers.swap(connectReplies_);
            pthread_mutex_unlock(&mu_);
            for (size_t i = 0; i < answers.size() && dialPhase_ == kDialWait; ++i)
                dialAnswered(answers[i].reply, answers[i].at, answers[i].repoint);
            if (dialPhase_ == kDialWait) usleep(20000);
        }
        if (dialPhase_ == kDialWait) {
            say("phone %u: the car's dial had no answer when %s", dial_.number, why);
            dialPhase_ = kDialNone;
            return;
        }
    }
    if (dialPhase_ == kDialMagic) {
        dialReader_->drain();
        startCleanup(std::vector<std::string>(1, dial_.address), why);
    }
    if (dialPhase_ == kDialLinked) {
        if (!dialLink_->hasEnded()) startCleanup(std::vector<std::string>(1, dial_.address), why);
        else watchDialled(msOf(monoNow()));
    }
    double end = monoNow() + 2.0 * config_.dial.disconnectWaitMs / 1000.0 + 1.0;
    while (dialPhase_ == kDialCleanup && monoNow() < end && spp_.attached()) {
        std::vector<ServiceDisconnectReply> disconnects;
        pthread_mutex_lock(&mu_);
        disconnects.swap(disconnectReplies_);
        pthread_mutex_unlock(&mu_);
        for (size_t i = 0; i < disconnects.size(); ++i) {
            if (discAt_ == 0 || addressKey(disconnects[i].address) != addressKey(discAddress_)) continue;
            discAnswered_ = true;
            discError_ = disconnects[i].error;
        }
        tendCleanup(msOf(monoNow()), false);
        if (dialPhase_ == kDialCleanup) usleep(20000);
    }
    if (dialReader_ != 0) {
        dialReader_->finish();
        dialReader_ = 0;
    }
    if (dialLink_ != 0) {
        dialLink_->close("the agent stops");
        dialLink_->release();
        dialLink_ = 0;
    }
    dialPhase_ = kDialNone;
}

// A registration that the check found dropped and restored is a problem for the file; then what is queued for the
// file is written, on the listener's thread, which can wait for the flash.
void SerialListener::flushProblems() {
    if (problems_ == 0) return;
    btspp::SppService::CheckCounts c = spp_.checkCounts();
    if (c.restored > restoredSeen_) {
        restoredSeen_ = c.restored;
        problem("registration-restored", "the car's Bluetooth had dropped the registration of '" + config_.name +
                                             "', and the check registered it again");
    }
    std::string why;
    if (problems_->flush(&why) || problemsSaid_) return;
    problemsSaid_ = true;
    say("the problem file could not be written (%s): what goes wrong is only counted", why.c_str());
}

void SerialListener::run() {
    // start() could not set the listener up: from here it tries again.
    if (!active() && !comeBack()) return;
    bool registered = false;
    double deadline = monoNow() + config_.registerMs / 1000.0;
    while (!stopping()) {
        SppService::State s = spp_.state();
        if (s == SppService::kRegistered) {
            if (!registered) {
                say("the serial service '%s' is registered: a phone app connects to UUID %s", config_.name.c_str(),
                    config_.uuid.c_str());
                // What another program left registered would have a phone open links that nothing answers, and
                // those keep the phone's app from this service.
                if (services_ != 0) services_->sweep("the agent's serial service has just been registered");
                checkedAt_ = monoNow();
                pthread_mutex_lock(&mu_);
                checkedAtShared_ = checkedAt_;
                pthread_mutex_unlock(&mu_);
                // Which of the car's phones are there, for the car to dial them: asked anew each time, since the
                // car's Bluetooth service may have restarted.
                if (config_.dial.on && !spp_.askDeviceList()) say("the car's device list could not be asked for");
            }
            registered = true;
            leftTried_ = false;
            checkRegistrationIfDue();
        } else {
            if (registered) deadline = monoNow() + config_.registerMs / 1000.0;
            registered = false;
            // What the car's Bluetooth service said of the phones went with it, and so did an ask or a dial that was
            // out, and the record a failed dial may have left wrong.
            if (!spp_.attached()) {
                pthread_mutex_lock(&mu_);
                bool doubted = rules_.recordDoubtful();
                rules_.serviceGone();
                listReplies_.clear();
                pthread_mutex_unlock(&mu_);
                if (dialPhase_ == kDialWait || dialPhase_ == kDialAsk) dialPhase_ = kDialNone;
                if (doubted)
                    say("the car's Bluetooth service has gone: its record of phone-opened serial links starts afresh "
                        "when it comes back");
            }
            // A refusal while a link left behind on the dialled instance may hold the service: that link is ended
            // first, and the service registered again at once if it has gone. Otherwise, or if it is still there, the
            // refusal goes as any other.
            bool leftWaits = false;
            if (s == SppService::kFailed && leftPhase_ == kLeftNone) startLeftForRegistration();
            if (leftPhase_ != kLeftNone) {
                LeftStep step = tendLeftForRegistration();
                if (step == kLeftRegister) {
                    spp_.again();
                    deadline = monoNow() + config_.registerMs / 1000.0;
                }
                leftWaits = step != kLeftGaveUp;
            }
            if (!leftWaits && (s == SppService::kFailed || monoNow() >= deadline)) {
                std::string never = "the car's Bluetooth service never attached (the agent asked the service broker "
                                    "for " + spp_.server() + ")";
                const char* what =
                    s == SppService::kFailed ? "the car's Bluetooth service did not take the registration"
                    : !spp_.attached() ? (everAttached_ ? "the car's Bluetooth service went away and has not come back"
                                                        : never.c_str())
                    : (s == SppService::kRegSent || s == SppService::kClearSent || s == SppService::kRetrySent)
                        ? "the car's Bluetooth service is attached and has not answered the registration"
                        : "the serial service is not registered, and has not been for too long";
                if (config_.retryMs == 0) {
                    say("%s: Bluetooth serial is off until the agent is started again", what);
                    shutDown();
                    return;
                }
                say("%s: Bluetooth serial is off, and is set up again in %u s", what, config_.retryMs / 1000);
                shutDown();
                if (!comeBack()) return;
                deadline = monoNow() + config_.registerMs / 1000.0;
                leftTried_ = false;
                continue;
            }
        }
        watchAttach(registered);
        lookForLinks();
        tendLetGo();
        tendReset();
        tendRefusal();
        tendDial();
        flushProblems();
        // The hold goes off when the agent's link has ended, or when no link has come of a phone's request.
        updateHold();
        // Not while a link is expected: the check takes some ten milliseconds, and the open must not wait for it.
        if (!linkExpected()) gate_->watch();
        waitForPass();
    }
}

}  // namespace nbt
