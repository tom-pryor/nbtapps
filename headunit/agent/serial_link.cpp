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
unsigned gPumps = 0;   // links whose node is still open

void countPump(int by) {
    pthread_mutex_lock(&gPumpsMu);
    gPumps += by;
    pthread_mutex_unlock(&gPumpsMu);
}

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

    Pump()
        : reader(NULL), node(-1), sock(-1), log(NULL), lingerMs(0), done(false), goneAt(0), fromPhone(0), toPhone(0),
          dropped(0) {
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
        pthread_mutex_unlock(&mu);
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

// All of it, however many calls that takes. False if `fd` will not take it.
bool writeAll(int fd, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t k = write(fd, data + off, len - off);
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
        if (!writeAll(p->node, buf, (size_t)k)) {
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
    bool saidStillUp = false;
    std::vector<uint8_t> got;
    for (;;) {
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
    delete p;
    countPump(-1);   // last: nothing of the link is touched after this
    return NULL;
}

}  // namespace

namespace {

// The agent's end of a serial link: a socket, which also knows the phone's address.
class SerialSocketLink : public SocketLink {
public:
    SerialSocketLink(int fd, const std::string& address) : SocketLink(fd, "serial"), address_(address) {}
    virtual std::string phoneAddress() const { return address_; }

private:
    std::string address_;
};

}  // namespace

Link* newSerialLink(NodeReader* reader, const std::string& path, SerialLog* log, const std::vector<uint8_t>& first,
                    unsigned lingerMs, const std::string& address) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        log->line(path + ": no socket pair for the link: " + strerror(errno));
        reader->finish();
        return 0;
    }
    closeOnExec(pair[0]);
    closeOnExec(pair[1]);
    // Room for what arrives while the agent is busy with something else. A system that will not give it is left
    // with what it has.
    int room = 65536;
    setsockopt(pair[0], SOL_SOCKET, SO_RCVBUF, &room, sizeof(room));
    setsockopt(pair[1], SOL_SOCKET, SO_SNDBUF, &room, sizeof(room));

    countPump(1);
    Pump* p = new Pump();
    p->reader = reader;
    p->node = reader->fd();
    p->sock = pair[1];
    p->path = path;
    p->log = log;
    p->first = first;
    p->lingerMs = lingerMs;
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
        countPump(-1);
        return 0;
    }
    return new SerialSocketLink(pair[0], address);
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
    : gate_(gate), log_(log), services_(0), serverLook_(0), config_(config), spp_(gap, this), wakeRead_(-1), wakeWrite_(-1),
      hasThread_(false), watchFrom_(0), stop_(false), active_(false), tracker_(config.requestWindowS), hotUntil_(0),
      reqAt_(0), refused_(0), deciding_(false), held_(false), holdWhy_(0), kept_(0), resetHotUntil_(0), askedAt_(0),
      checkedAt_(0), opened_(false), final_(false), unattachedSince_(0), everAttached_(false), attachSaid_(false), lookDone_(false),
      scannedAt_(0), refusalCount_(0), refusalNode_(RefusalFacts::kNoNode), refusalAt_(0), handLookAt_(0) {
    for (int i = 0; i < NodeTracker::kNodes; ++i) {
        watched_[i] = false;
        appeared_[i] = 0;
    }
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
    reqAt_ = 0;
    pthread_mutex_unlock(&mu_);
    if (held) gate_->release();
    gate_->detach();
    closeKept(kAllButRead);
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
    // The stack does not say more than this about whose link it will be: what the phone says first decides.
    bool maybeOurs = service == config_.name || service == config_.carService;
    say("a phone %s (service '%s')%s%s",
        what == SppService::kPhoneAsked ? "asked for a serial link"
                                        : (what == SppService::kPhoneLinked ? "got its serial link"
                                                                            : "was refused a serial link by the car"),
        service.c_str(), maybeOurs || what != SppService::kPhoneAsked ? "" : ": another service's, left alone",
        parsed ? "" : " (the message was not read whole)");
    pthread_mutex_lock(&mu_);
    double now = monoNow();
    unsigned refused = 0;
    if (what == SppService::kPhoneAsked) {
        askedAt_ = now;   // for any service: the registration check keeps out of the way of a phone that asks
        tracker_.onPhoneRequest(monoSec(), maybeOurs, address);
        if (maybeOurs) {
            reqAt_ = now;
            holdWhy_ = "a phone has asked for a link";
        }
    } else if (what == SppService::kPhoneRefused) {
        tracker_.onRefused(maybeOurs);
        if (maybeOurs) {
            refused = ++refused_;
            reqAt_ = 0;   // no link comes of it
            holdWhy_ = "the car refused the phone's link";
        }
    }
    // The link's node appears some tens of milliseconds after the request, and has to be opened at once.
    if (maybeOurs && what != SppService::kPhoneRefused) {
        hotUntil_ = now + config_.linkWaitMs / 1000.0;
        pthread_cond_signal(&wake_);
    }
    pthread_mutex_unlock(&mu_);
    // On, before the car accepts the link and its node is there; or off again, if the car has refused it.
    if (maybeOurs && what != SppService::kPhoneLinked) updateHold();
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
// what has changed of whether each is there, and of its security.
void SerialListener::sppDevices(const std::vector<BtDevice>& devices) {
    if (!config_.dial.on) return;
    std::vector<Presence::Change> changes;
    pthread_mutex_lock(&mu_);
    presence_.update(devices, (int64_t)(monoNow() * 1000.0), &changes);
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
    for (size_t i = 0; i < changes.size(); ++i) {
        const Presence::Change& c = changes[i];
        bool there = (c.connectionBefore >= kConnConnected) != (c.connection >= kConnConnected);
        if (c.added || c.removed || there || c.securityChanged) say("%s", presenceLine(c).c_str());
    }
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
    pthread_mutex_lock(&mu_);
    double now = monoNow();
    bool hot = now < hotUntil_;
    if (!hot && !stop_) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += now < resetHotUntil_ ? 20000000L : 200000000L;
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
    spp_.checkRegistration(config_.checkMs / 1000.0);
}

// ---- serial.reset ------------------------------------------------------------------------------------------------

// On the agent's thread: only says what is to happen, for the listener's thread to do.
SerialReset::Outcome SerialListener::resetSerialLink(const std::string& device, const std::string& instance,
                                                     const std::string& service, int64_t* forMs, std::string* why) {
    unsigned agents = serialLinksOpen();
    pthread_mutex_lock(&mu_);
    bool active = active_;
    bool open = deciding_ || kept_ > 0 || agents > 0;
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

// On the listener's thread, at every pass and during the decide step: what serial.reset asked for that is due, and
// what has come of a ServiceDisconnect so far.
void SerialListener::tendReset() {
    double now = monoNow();
    pthread_mutex_lock(&mu_);
    bool due = reset_.due && now >= reset_.at;
    if (due) reset_.due = false;
    bool ranOut = reset_.armed && now >= reset_.until;
    if (ranOut) reset_.armed = false;
    Reset asked = reset_;
    pthread_mutex_unlock(&mu_);
    if (ranOut)
        say("serial.reset: no serial link came within %u s to be ended at its open: nothing is asked any more",
            config_.resetArmedMs / 1000);
    if (due) {
        // The nodes this listener opened, that are there now. Someone else's are not watched.
        bool present[NodeTracker::kNodes];
        scanNodes(present);
        pthread_mutex_lock(&mu_);
        for (int i = 0; i < NodeTracker::kNodes; ++i) present[i] = present[i] && tracker_.state(i) == NodeTracker::kOurs;
        pthread_mutex_unlock(&mu_);
        if (spp_.disconnectPhone(asked.device, asked.instance, asked.service)) {
            say("serial.reset: ServiceDisconnect sent, %.0f ms after it was asked for", (now - asked.askedAt) * 1000.0);
            watchNodes(present, now);
        } else {
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

// Turns the gate's hold on or off, as things stand: it is on while a phone's request waits for its node, while a
// link is being decided on, and while a link of the agent's is up. The programs it holds off are the ones declared
// for a serial service at the moment it goes on; with none declared there is no hold. From the listener's thread
// and the Bluetooth service's: whichever of them makes the change gives holdWhy_ as the reason in the log, which
// is what was last put there together with a change of what the hold goes by.
void SerialListener::updateHold() {
    if (services_ == 0) return;
    unsigned up = serialLinksOpen();
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
// nobody's. Leftovers alone do not make it the agent's: another program's link may have the agent's old bytes in
// front of it, and letGo() gives it to that program as it gives any other. kEnded: the link ended first. *why says
// which, for the log. Nothing is written.
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
            return kLetGo;
        }
        if (stopping()) {
            *why = "the listener was stopped";
            return kEnded;
        }
        tendReset();
        double left = by - monoNow();
        if (left <= 0) {
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

// Lets the link on the node go: it was not the agent's, and nothing was written into it. Who reads it now decides
// what becomes of its node (otherReader()). If another program may read it, the reader is stopped and the node
// closed for that program. With none, nobody would ever read the link, and bytes that wait unread ahead of a node
// come on the next link: the node is read, and what comes dropped, until the link ends or a sign of another reader
// comes after all (tendLetGo()).
void SerialListener::letGo(NodeReader* reader, int node, const std::string& path, double openedAt, const std::string& why) {
    double ms = (monoNow() - openedAt) * 1000.0;
    if (config_.letGoKeepsNode) {
        int fd = reader->stop();
        say("%s let go %.0f ms after the open (%s). Its node stays open, unread, until the link ends", path.c_str(), ms,
            why.c_str());
        keep(node, fd, 0, "let go");
        return;
    }
    std::string reason;
    if (otherReader(node, &reason)) {
        reader->finish();
        bool there = access(path.c_str(), F_OK) == 0;
        say("%s let go %.0f ms after the open (%s) and closed: %s. It is closed for another reader: %s", path.c_str(),
            ms, why.c_str(), there ? "its node is still there" : "its node went with the close", reason.c_str());
        if (there) keep(node, -1, 0, "let go");   // until its node goes
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
    letGo_.push_back(kept);
    if (fd >= 0 || reader != 0) {
        pthread_mutex_lock(&mu_);
        ++kept_;
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
        pthread_mutex_lock(&mu_);
        --kept_;
        pthread_mutex_unlock(&mu_);
        std::string path = nodePath(kept.node);
        say("%s: reading it stopped, and its node closed for another reader, %.1f s after it was let go: %s. %llu B "
            "had been read and dropped by then", path.c_str(), monoNow() - kept.at, sign.c_str(),
            (unsigned long long)dropped);
        // It stays on the list, closed, for the log to say when its node goes.
    }
}

// The nodes that are not the agent's link and whose link has ended since: a node that is read or kept open is
// closed. kEvery: every one of them, the listener's thread having ended. kAllButRead: the same, except that a node
// whose link is still up and is read goes on being read.
void SerialListener::closeKept(Closing which) {
    for (size_t i = 0; i < letGo_.size();) {
        const LetGo kept = letGo_[i];
        std::string path = nodePath(kept.node);
        bool ended = kept.reader != 0 ? kept.reader->ended() : access(path.c_str(), F_OK) != 0;
        if (!ended && (which == kEndedOnly || (which == kAllButRead && kept.reader != 0))) {
            ++i;
            continue;
        }
        letGo_.erase(letGo_.begin() + (long)i);
        if (kept.fd >= 0 || kept.reader != 0) {
            pthread_mutex_lock(&mu_);
            --kept_;
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
        } else if (which == kEndedOnly) {
            say(kept.fd >= 0 ? "%s: the link ended %.1f s after it was %s, and its node is closed now"
                             : "%s is gone, %.1f s after it was %s and closed",
                path.c_str(), after, kept.what);
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
        bool sent = spp_.disconnectPhone(asked.device, asked.instance, asked.service);
        say("%s: serial.reset asked %.1f s ago for the next serial link to be ended at its open. %s, and its node is "
            "read, with what comes dropped, until the link ends", path.c_str(), openedAt - asked.askedAt,
            sent ? "ServiceDisconnect is sent at once, with no decide step" : "ServiceDisconnect could not be sent");
        keep(node, -1, reader, "ended at its open");
        if (sent) {
            bool one[NodeTracker::kNodes];
            for (int i = 0; i < NodeTracker::kNodes; ++i) one[i] = i == node;
            watchNodes(one, monoNow());
        }
        pthread_mutex_lock(&mu_);
        deciding_ = false;
        holdWhy_ = "the link is ended at its open";
        pthread_mutex_unlock(&mu_);
        updateHold();
        return;
    }

    std::vector<uint8_t> first;
    std::string why;
    uint64_t leftOver = 0;
    Decided found = decide(reader, path, openedAt, &first, &leftOver, &why);
    if (leftOver > 0)
        say("%s: %llu B left over from an earlier link came first, as whole frames, and were dropped", path.c_str(),
            (unsigned long long)leftOver);
    Link* link = 0;
    const char* holdWhy = 0;
    if (found == kOurs) {
        say("%s: it opened with the magic bytes, so the link is the agent's", path.c_str());
        // Counted as up from here, which keeps the hold on.
        link = newSerialLink(reader, path, log_, first, config_.lingerMs, address);
        if (link == 0) holdWhy = "the link could not be set up";
    } else if (found == kLetGo) {
        letGo(reader, node, path, openedAt, why);
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
    bool other[NodeTracker::kNodes];
    pthread_mutex_lock(&mu_);
    for (int i = 0; i < NodeTracker::kNodes; ++i) before[i] = tracker_.state(i);
    int ours = tracker_.scan(present, monoSec(), false);
    std::string address = tracker_.takenAddress();
    for (int i = 0; i < NodeTracker::kNodes; ++i) {
        after[i] = tracker_.state(i);
        other[i] = tracker_.otherService(i);
    }
    pthread_mutex_unlock(&mu_);
    // A node that is new has appeared since the look before this one.
    for (int i = 0; i < NodeTracker::kNodes; ++i)
        if (present[i] && before[i] == NodeTracker::kAbsent) appeared_[i] = lastLook > 0 ? lastLook : scannedAt_;
    if (ours >= 0) take(ours, address);
    for (int i = 0; i < NodeTracker::kNodes; ++i) {
        if (before[i] == after[i] || after[i] != NodeTracker::kForeign) continue;
        say("%s: %s, so it is not ours: left alone", nodePath(i).c_str(),
            other[i] ? "the phone asked for another service with it" : "no request from a phone came with it");
    }
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
                // Which of the car's phones are there, for the car to dial them: asked anew each time, since the
                // car's Bluetooth service may have restarted.
                if (config_.dial.on && !spp_.askDeviceList()) say("the car's device list could not be asked for");
            }
            registered = true;
            checkRegistrationIfDue();
        } else {
            if (registered) deadline = monoNow() + config_.registerMs / 1000.0;
            registered = false;
            // What the car's Bluetooth service said of the phones went with it.
            if (!spp_.attached()) {
                pthread_mutex_lock(&mu_);
                presence_.forget();
                pthread_mutex_unlock(&mu_);
            }
            if (s == SppService::kFailed || monoNow() >= deadline) {
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
                continue;
            }
        }
        watchAttach(registered);
        lookForLinks();
        tendLetGo();
        tendReset();
        tendRefusal();
        // The hold goes off when the agent's link has ended, or when no link has come of a phone's request.
        updateHold();
        // Not while a link is expected: the check takes some ten milliseconds, and the open must not wait for it.
        if (!linkExpected()) gate_->watch();
        waitForPass();
    }
}

}  // namespace nbt
