// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Bluetooth serial links: see serial_link.hpp.

#include "serial_link.hpp"

#include "frame.hpp"

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

// True once `fd` has something to read or `ms` are over; *ready says which.
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

// ---- one link: two threads between the node and a socket pair ----------------------------------------------------
// The node's side of the copying. "up" carries what the phone sends to the agent and owns the pump: it ends last,
// closes everything and frees it. "down" carries what the agent sends to the phone.

// One read takes a whole message of the node, and what does not fit is lost: far above any message seen.
const size_t kNodeRead = 32768;
// One blocking write to the node. Writes of this size are what the node has been seen to take whole.
const size_t kNodeWrite = 1000;
// How long either thread waits before it looks whether the other one has ended the link.
const int kPumpTickMs = 100;

pthread_mutex_t gPumpsMu = PTHREAD_MUTEX_INITIALIZER;
unsigned gPumps = 0;   // links whose threads are still running

void countPump(int by) {
    pthread_mutex_lock(&gPumpsMu);
    gPumps += by;
    pthread_mutex_unlock(&gPumpsMu);
}

struct Pump {
    int node, sock;
    std::string path;
    SerialLog* log;
    pthread_t down;
    unsigned lingerMs;       // how long the node is kept, and read, once the agent's end has closed
    pthread_mutex_t mu;      // done, why, goneAt
    bool done;
    std::string why;
    double goneAt;           // when the agent's end was found closed; 0 while it is open
    uint64_t fromPhone, toPhone;   // each counted by one thread, and read once both have ended
    uint64_t dropped;              // what the phone sent after the agent's end had closed: "up" counts it
    std::vector<uint8_t> first;    // read from the node before the link was made: "up" sends it on first

    Pump() : node(-1), sock(-1), log(NULL), lingerMs(0), done(false), goneAt(0), fromPhone(0), toPhone(0), dropped(0) {
        pthread_mutex_init(&mu, NULL);
    }
    ~Pump() { pthread_mutex_destroy(&mu); }

    // The first reason given is the one the log gets.
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
    // The agent's end has closed: from the first call of this the node is kept for the phone to end the link.
    void agentGone() {
        pthread_mutex_lock(&mu);
        if (goneAt == 0) goneAt = monoNow();
        pthread_mutex_unlock(&mu);
    }
    // When that was; 0 while the agent has its end.
    double goneSince() {
        pthread_mutex_lock(&mu);
        double at = goneAt;
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
        if (!waitReadable(p->sock, kPumpTickMs, &ready)) { p->end("the link's socket failed"); break; }
        if (!ready) {
            if (p->ended()) break;
            continue;
        }
        ssize_t k = read(p->sock, buf, sizeof(buf));
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (k <= 0) { p->agentGone(); break; }
        if (!writeAll(p->node, buf, (size_t)k)) {
            p->end(std::string("a write to the node failed: ") + strerror(errno));
            break;
        }
        p->toPhone += (uint64_t)k;
    }
    return NULL;
}

// How a link ended whose phone ended it: `how` alone while the agent had its end, and after that the agent's close
// with how long the phone took to follow it.
std::string endedByPhone(const char* how, double goneAt) {
    if (goneAt == 0) return how;
    char text[160];
    snprintf(text, sizeof(text), "closed by the agent, and the phone ended the link %.0f ms later",
             (monoNow() - goneAt) * 1000.0);
    return text;
}

// What the phone sends, to the agent: one large read for each message of the node.
//
// Once the agent's end has closed, the node is kept open and read, and what comes is dropped, until the phone ends
// the link or `lingerMs` have passed. On the car, when the agent had closed a link's node and the phone went on
// holding the link and sending into it, nothing arrived on any later link until the head unit was restarted. So
// the node is not closed under a phone that is about to end the link itself: the agent tells the phone that it is
// closing (link.closing, agent.hpp), and the phone ends the link when it hears that.
void* pumpUp(void* arg) {
    Pump* p = static_cast<Pump*>(arg);
    std::vector<uint8_t> buf(kNodeRead);
    if (!p->first.empty()) {
        if (writeAll(p->sock, &p->first[0], p->first.size())) p->fromPhone += p->first.size();
        else p->agentGone();
    }
    while (!p->ended()) {
        double gone = p->goneSince();
        if (gone > 0 && (monoNow() - gone) * 1000.0 >= (double)p->lingerMs) {
            char text[160];
            if (p->lingerMs == 0) {
                snprintf(text, sizeof(text), "closed by the agent");
            } else {
                snprintf(text, sizeof(text), "closed by the agent, and the link was still up %u ms later: its node is "
                                             "closed under it", p->lingerMs);
            }
            p->end(text);
            break;
        }
        bool ready = false;
        if (!waitReadable(p->node, kPumpTickMs, &ready)) { p->end("the node failed"); break; }
        if (!ready) continue;
        ssize_t k = read(p->node, &buf[0], buf.size());
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (k == 0) { p->end(endedByPhone("closed by the phone", gone)); break; }
        if (k < 0) {
            // On the car a link that ends takes its node away, and the read then fails.
            if (gone > 0 || access(p->path.c_str(), F_OK) != 0) p->end(endedByPhone("the phone's link ended", gone));
            else p->end(std::string("a read of the node failed: ") + strerror(errno));
            break;
        }
        if (gone > 0) {
            p->dropped += (uint64_t)k;
            continue;
        }
        if ((size_t)k == buf.size())
            p->log->line(p->path + ": a read filled the whole buffer, so the rest of that message may be lost");
        if (!writeAll(p->sock, &buf[0], (size_t)k)) {
            p->agentGone();
            p->dropped += (uint64_t)k;
            continue;
        }
        p->fromPhone += (uint64_t)k;
    }
    // The agent sees its end close once it has read what is still on the way to it.
    shutdown(p->sock, SHUT_WR);
    pthread_join(p->down, NULL);
    close(p->node);
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

Link* newSerialLink(int nodeFd, const std::string& path, SerialLog* log, const std::vector<uint8_t>& first,
                    unsigned lingerMs) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        log->line(path + ": no socket pair for the link: " + strerror(errno));
        close(nodeFd);
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
    p->node = nodeFd;
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
        close(nodeFd);
        close(pair[0]);
        close(pair[1]);
        delete p;
        countPump(-1);
        return 0;
    }
    return new SocketLink(pair[0], "serial");
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

SppNodeGate::SppNodeGate(const std::string& self, SerialLog* log) : self_(self), log_(log), node_(false), logged_(0) {
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
    log_->line(line);
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
    unsigned n = ownLook ? 0 : ++g->logged_;
    pthread_mutex_unlock(&g->mu_);
    if (n == 0 || n > kGateLogged) return;
    g->log_->line(describeOpen(o, kGateBlock, kGateRefuseDefault));
    if (n == kGateLogged) g->log_->line("gate: further opens are counted and no longer logged one by one");
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

bool SppNodeGate::attach(std::string* why) {
    if (gate_.attached()) return true;
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
    : gate_(gate), log_(log), services_(0), config_(config), spp_(gap, this), wakeRead_(-1), wakeWrite_(-1),
      hasThread_(false), stop_(false), active_(false), tracker_(config.requestWindowS), hotUntil_(0), reqAt_(0),
      refused_(0), deciding_(false), held_(false), holdWhy_(0) {
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

    // Before the service is registered, so that no link of ours can be there ahead of the gate.
    if (!gate_->attach(why)) return false;
    spp_.configure(config_.name, config_.uuid, 0);
    if (!spp_.start()) {
        gate_->detach();
        *why = "no connection to the car's Bluetooth service";
        return false;
    }
    pthread_mutex_lock(&mu_);
    active_ = true;
    pthread_mutex_unlock(&mu_);
    hasThread_ = pthread_create(&thread_, NULL, &SerialListener::threadMain, this) == 0;
    if (!hasThread_) {
        shutDown();
        *why = "the serial listener's thread could not be started";
        return false;
    }
    return true;
}

void SerialListener::stop() {
    pthread_mutex_lock(&mu_);
    stop_ = true;
    pthread_cond_signal(&wake_);
    pthread_mutex_unlock(&mu_);
    if (hasThread_) pthread_join(thread_, NULL);
    hasThread_ = false;
    if (active()) shutDown();
    closeKept(true);
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
    if (spp_.remove("leaving")) {
        double end = monoNow() + 3.0;
        SppService::State s = spp_.state();
        while (s != SppService::kRemoved && s != SppService::kRemoveFailed && monoNow() < end) {
            usleep(20000);
            s = spp_.state();
        }
        if (s == SppService::kRemoveFailed)
            say("the car's Bluetooth service did not remove the registration of '%s': it stays until that service "
                "restarts", config_.name.c_str());
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
    closeKept(true);
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

void SerialListener::sppPhone(SppService::Phone what, const std::string& service, bool parsed) {
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
        tracker_.onPhoneRequest(monoSec(), maybeOurs);
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
    if (refused == 1 || (refused && refused % 20 == 0))
        say("the car refused the phone's link (%u so far). It accepts a phone-opened serial link only while that "
            "phone's Bluetooth audio is connected, with the ignition on and the pairing menu closed", refused);
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
// 200 ms at most.
void SerialListener::waitForPass() {
    pthread_mutex_lock(&mu_);
    bool hot = monoNow() < hotUntil_;
    if (!hot && !stop_) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200000000L;
        if (ts.tv_nsec >= 1000000000L) { ++ts.tv_sec; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&wake_, &mu_, &ts);
    }
    pthread_mutex_unlock(&mu_);
    if (hot) usleep(2000);
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

// Reads what the phone says first on the open node `fd`, until it tells whose link this is or the time for it is
// over. kOurs: it opened with the magic bytes, and *first holds all that was read. kLetGo: it is another
// program's, or nobody's. kEnded: the link ended first. *why says which, for the log. Nothing is written.
SerialListener::Decided SerialListener::decide(int fd, const std::string& path, double openedAt,
                                               std::vector<uint8_t>* first, std::string* why) {
    const double by = openedAt + config_.decideMs / 1000.0;
    std::vector<uint8_t> buf(kNodeRead);
    for (;;) {
        if (stopping()) {
            *why = "the listener was stopped";
            return kEnded;
        }
        double left = by - monoNow();
        if (left <= 0) {
            *why = first->empty() ? "nothing came from the phone in that time"
                                  : "only a part of the magic bytes came in that time";
            return kLetGo;
        }
        int ms = (int)(left * 1000.0) + 1;
        bool ready = false;
        if (!waitReadable(fd, ms < kPumpTickMs ? ms : kPumpTickMs, &ready)) {
            *why = "the node failed";
            return kEnded;
        }
        if (!ready) continue;
        ssize_t k = read(fd, &buf[0], buf.size());
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (k == 0) {
            *why = "closed by the phone";
            return kEnded;
        }
        if (k < 0) {
            *why = access(path.c_str(), F_OK) != 0 ? std::string("the phone's link ended")
                                                  : std::string("a read of the node failed: ") + strerror(errno);
            return kEnded;
        }
        if (first->empty())
            say("%s: its first message came %.0f ms after the open", path.c_str(), (monoNow() - openedAt) * 1000.0);
        if ((size_t)k == buf.size())
            say("%s: a read filled the whole buffer, so the rest of that message may be lost", path.c_str());
        first->insert(first->end(), buf.begin(), buf.begin() + k);
        size_t have = first->size() < kLinkMagicBytes ? first->size() : kLinkMagicBytes;
        if (memcmp(&(*first)[0], kLinkMagic, have) != 0) {
            *why = "it did not open with the magic bytes";
            return kLetGo;
        }
        if (have == kLinkMagicBytes) return kOurs;
    }
}

// Lets the link on the open node `fd` go: it is not read again, and nothing was written into it.
void SerialListener::letGo(int fd, int node, const std::string& path, double openedAt, const std::string& why) {
    LetGo gone;
    gone.node = node;
    gone.fd = -1;
    gone.at = monoNow();
    if (config_.letGoKeepsNode) {
        gone.fd = fd;
        say("%s let go %.0f ms after the open (%s). Its node stays open, unread, until the link ends", path.c_str(),
            (gone.at - openedAt) * 1000.0, why.c_str());
    } else {
        close(fd);
        bool there = access(path.c_str(), F_OK) == 0;
        say("%s let go %.0f ms after the open (%s) and closed: %s", path.c_str(), (gone.at - openedAt) * 1000.0,
            why.c_str(), there ? "its node is still there" : "its node went with the close");
        if (!there) return;
    }
    letGo_.push_back(gone);   // until its node goes
}

// The links that were let go and whose node has gone since, or with `all` every one: a node that was kept open is
// closed.
void SerialListener::closeKept(bool all) {
    for (size_t i = 0; i < letGo_.size();) {
        const LetGo& gone = letGo_[i];
        std::string path = nodePath(gone.node);
        if (!all && access(path.c_str(), F_OK) == 0) {
            ++i;
            continue;
        }
        if (gone.fd >= 0) close(gone.fd);
        if (!all)
            say(gone.fd >= 0 ? "%s: the link ended %.1f s after it was let go, and its node is closed now"
                             : "%s is gone, %.1f s after it was let go and closed",
                path.c_str(), monoNow() - gone.at);
        letGo_.erase(letGo_.begin() + (long)i);
    }
}

// Opens node `node`, which has just become a link that may be ours, and hands it to the agent if what the phone
// says first shows that it is.
void SerialListener::take(int node) {
    std::string path = nodePath(node);
    int fd = open(path.c_str(), O_RDWR);
    double openedAt = monoNow();
    if (fd < 0) {
        say("open %s: %s", path.c_str(), strerror(errno));
        return;
    }
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

    std::vector<uint8_t> first;
    std::string why;
    Decided found = decide(fd, path, openedAt, &first, &why);
    Link* link = 0;
    const char* holdWhy = 0;
    if (found == kOurs) {
        say("%s: it opened with the magic bytes, so the link is the agent's", path.c_str());
        // Counted as up from here, which keeps the hold on.
        link = newSerialLink(fd, path, log_, first, config_.lingerMs);
        if (link == 0) holdWhy = "the link could not be set up";
    } else if (found == kLetGo) {
        letGo(fd, node, path, openedAt, why);
        holdWhy = "the link was let go";
    } else {
        close(fd);
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
    closeKept(false);
    bool present[NodeTracker::kNodes];
    scanNodes(present);
    NodeTracker::State before[NodeTracker::kNodes], after[NodeTracker::kNodes];
    bool other[NodeTracker::kNodes];
    pthread_mutex_lock(&mu_);
    for (int i = 0; i < NodeTracker::kNodes; ++i) before[i] = tracker_.state(i);
    int ours = tracker_.scan(present, monoSec(), false);
    for (int i = 0; i < NodeTracker::kNodes; ++i) {
        after[i] = tracker_.state(i);
        other[i] = tracker_.otherService(i);
    }
    pthread_mutex_unlock(&mu_);
    if (ours >= 0) take(ours);
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
        // Before the service is registered, as in start().
        if (!gate_->attach(&why)) {
            say("Bluetooth serial is not set up again: %s. Again in %u s", why.c_str(), config_.retryMs / 1000);
            continue;
        }
        say("setting Bluetooth serial up again");
        pthread_mutex_lock(&mu_);
        active_ = true;
        pthread_mutex_unlock(&mu_);
        spp_.again();
        return true;
    }
}

void SerialListener::run() {
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
            }
            registered = true;
        } else {
            if (registered) deadline = monoNow() + config_.registerMs / 1000.0;
            registered = false;
            if (s == SppService::kFailed || monoNow() >= deadline) {
                const char* what = s == SppService::kFailed ? "the car's Bluetooth service did not take the registration"
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
        lookForLinks();
        // The hold goes off when the agent's link has ended, or when no link has come of a phone's request.
        updateHold();
        // Not while a link is expected: the check takes some ten milliseconds, and the open must not wait for it.
        if (!linkExpected()) gate_->watch();
        waitForPass();
    }
}

}  // namespace nbt
