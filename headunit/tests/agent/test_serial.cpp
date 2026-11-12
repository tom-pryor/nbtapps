// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/serial_link.cpp: a link on a serial node, the listener that opens a link's node as soon as
// it is there, and an Agent that phones reach through it.
//
// The car's parts are played here. A serial node is a pseudo-terminal: its far end is the phone, and a symbolic
// link to its near end, made when "the link comes up", is the node the listener finds. The Bluetooth service is a
// GapClient of the test's own, which takes the registration and says what a phone asked for, and the gate is a
// NodeGate that only notes what was done to it. All names are invented.
//
// Unlike the agent's other tests this one runs on real time, with the listener's and the links' threads: it waits
// for what should happen, up to a few seconds each, and for a short while where nothing should.
//
// usage: test_serial [work folder]

#include "../../agent/serial_link.hpp"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include "../../agent/agent.hpp"
#include "../../agent/fsutil.hpp"
#include "../../agent/host_platform.hpp"
#include "test_util.hpp"

using namespace nbt;
using namespace nbt::btspp;

static std::string gWork = "build/bench/work/serial";

static double monoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void pause(int ms) { usleep((useconds_t)ms * 1000); }

static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// ---- what was done, in order, by whichever thread did it ----------------------------------------------------------

class Trace : public SerialLog {
public:
    Trace() { pthread_mutex_init(&mu_, NULL); }
    ~Trace() { pthread_mutex_destroy(&mu_); }

    virtual void line(const std::string& line) { add("log: " + line); }
    void add(const std::string& what) {
        pthread_mutex_lock(&mu_);
        items_.push_back(what);
        pthread_mutex_unlock(&mu_);
    }
    // The place of the first item that holds `part`; below 0 if none does.
    int at(const std::string& part) {
        pthread_mutex_lock(&mu_);
        int found = -1;
        for (size_t i = 0; i < items_.size() && found < 0; ++i)
            if (has(items_[i], part)) found = (int)i;
        pthread_mutex_unlock(&mu_);
        return found;
    }
    bool saw(const std::string& part) { return at(part) >= 0; }
    // How many items hold `part`.
    unsigned count(const std::string& part) {
        pthread_mutex_lock(&mu_);
        unsigned n = 0;
        for (size_t i = 0; i < items_.size(); ++i)
            if (has(items_[i], part)) ++n;
        pthread_mutex_unlock(&mu_);
        return n;
    }
    std::string text() {
        pthread_mutex_lock(&mu_);
        std::string all;
        for (size_t i = 0; i < items_.size(); ++i) all += items_[i] + "\n";
        pthread_mutex_unlock(&mu_);
        return all;
    }

private:
    pthread_mutex_t mu_;
    std::vector<std::string> items_;
};

// Declared after a Trace and before what makes links: on its way out it waits until every link's threads have
// ended, which they must before the trace they write to goes.
struct LinksGone {
    LinksGone() {}
    ~LinksGone() { CHECK(waitForSerialLinks(5000)); }
};

// Waits until the trace holds `part`.
static bool sawWithin(Trace& trace, const std::string& part, int ms) {
    double end = monoNow() + ms / 1000.0;
    while (monoNow() < end) {
        if (trace.saw(part)) return true;
        pause(5);
    }
    return trace.saw(part);
}

// ---- the car's Bluetooth service ----------------------------------------------------------------------------------

class FakeGap : public GapClient {
public:
    explicit FakeGap(Trace* trace) : canOpen(true), registerError(0), events_(0), up_(false), trace_(trace) {}

    virtual bool open(Events* events) {
        trace_->add("gap open");
        events_ = events;
        return canOpen;
    }
    virtual bool attached() const { return up_; }
    virtual bool request(uint32_t id, const std::vector<uint8_t>& payload) {
        if (!up_) return false;
        std::vector<uint8_t> reply;
        if (id == kReqSppUuidRegister) {
            trace_->add("register");
            putU32(reply, registerError);
            events_->gapMessage(kTypeResponse, kRspSppUuidRegister, &reply[0], (uint32_t)reply.size());
        } else if (id == kReqSppUuidDeregister) {
            Reader name(&payload[0], (uint32_t)payload.size());
            trace_->add("deregister " + name.str());
            putU32(reply, 0);
            events_->gapMessage(kTypeResponse, kRspSppUuidDeregister, &reply[0], (uint32_t)reply.size());
        }
        return true;
    }
    virtual bool subscribe(uint32_t) { return up_; }
    virtual std::string describe() const { return "Test.Server 1.0"; }

    void attach() {
        up_ = true;
        events_->gapAttached();
    }
    // A phone asks for a link to `service`, is given it, or is refused.
    void phone(uint32_t informId, const char* service) {
        std::vector<uint8_t> p;
        if (informId == kInfServiceRequest) putString(p, "Test Phone");
        putString(p, "AABBCCDDEEFF");
        putString(p, service);
        events_->gapMessage(0x400, informId, &p[0], (uint32_t)p.size());
    }

    bool canOpen;
    uint32_t registerError;   // what a registration is answered with

private:
    Events* events_;
    bool up_;
    Trace* trace_;
};

class FakeGate : public NodeGate {
public:
    explicit FakeGate(Trace* trace) : inFront(true), trace_(trace) {}
    virtual bool attach(std::string* why) {
        trace_->add("gate attach");
        if (!inFront) *why = "the test's gate is not in front";
        return inFront;
    }
    virtual void watch() {}
    virtual void detach() { trace_->add("gate detach"); }
    virtual void hold(const std::vector<std::string>& programs) {
        std::string all;
        for (size_t i = 0; i < programs.size(); ++i) all += (i ? ", " : "") + programs[i];
        trace_->add("gate hold " + all);
    }
    virtual void release() { trace_->add("gate release"); }

    bool inFront;

private:
    Trace* trace_;
};

// ---- a serial node, and the phone at its far end ------------------------------------------------------------------

class Node {
public:
    // The node `prefix` + `index`, not there yet.
    Node(const std::string& prefix, int index) : phone(-1) {
        char digit[8];
        snprintf(digit, sizeof(digit), "%d", index);
        path = prefix + digit;
        phone = posix_openpt(O_RDWR | O_NOCTTY);
        CHECK(phone >= 0 && grantpt(phone) == 0 && unlockpt(phone) == 0);
        near_ = ptsname(phone);
        // Bytes as they are, both ways, with no echo.
        int fd = open(near_.c_str(), O_RDWR | O_NOCTTY);
        CHECK(fd >= 0);
        struct termios raw;
        CHECK(tcgetattr(fd, &raw) == 0);
        cfmakeraw(&raw);
        CHECK(tcsetattr(fd, TCSANOW, &raw) == 0);
        close(fd);
        fcntl(phone, F_SETFL, fcntl(phone, F_GETFL, 0) | O_NONBLOCK);
    }
    ~Node() {
        vanish();
        hangUp();
    }

    // The link comes up: the node is there.
    void appear() { CHECK_IN(path, symlink(near_.c_str(), path.c_str()) == 0); }
    void vanish() { unlink(path.c_str()); }
    void hangUp() {
        if (phone >= 0) close(phone);
        phone = -1;
    }
    // Opens the near end as the listener would.
    int openNear() {
        int fd = open(near_.c_str(), O_RDWR | O_NOCTTY);
        CHECK(fd >= 0);
        return fd;
    }

    void say(const std::string& bytes) {
        size_t off = 0;
        double end = monoNow() + 5.0;
        while (off < bytes.size() && monoNow() < end) {
            ssize_t n = write(phone, bytes.data() + off, bytes.size() - off);
            if (n > 0) off += (size_t)n;
            else pause(2);
        }
        CHECK(off == bytes.size());
    }
    // What has arrived at the phone within `ms`, read until `want` bytes are there or the near end is closed.
    // *closed: every holder of the near end has closed it.
    std::string hear(size_t want, int ms, bool* closed) {
        std::string got;
        *closed = false;
        double end = monoNow() + ms / 1000.0;
        while (got.size() < want && monoNow() < end) {
            char buffer[4096];
            ssize_t n = read(phone, buffer, sizeof buffer);
            if (n > 0) {
                got.append(buffer, (size_t)n);
            } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                *closed = true;
                break;
            } else {
                pause(2);
            }
        }
        return got;
    }

    int phone;          // the far end; -1 once hung up or given away
    std::string path;   // where the listener finds the node

private:
    std::string near_;
};

// A pattern that a shifted or repeated piece of itself does not match.
static std::string pattern(size_t n, unsigned seed) {
    std::string s(n, '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        s[i] = (char)(x >> 24);
    }
    return s;
}

// Sends `up` from the phone and `down` from the link at the same time, and checks that each arrives whole.
static void exchange(Node& node, Link* link, const std::string& up, const std::string& down) {
    std::string gotUp, gotDown;
    size_t sentUp = 0, sentDown = 0;
    double end = monoNow() + 20.0;
    while ((gotUp.size() < up.size() || gotDown.size() < down.size()) && monoNow() < end) {
        bool moved = false;
        char buffer[8192];
        if (sentUp < up.size()) {
            ssize_t n = write(node.phone, up.data() + sentUp, up.size() - sentUp);
            if (n > 0) { sentUp += (size_t)n; moved = true; }
        }
        if (sentDown < down.size()) {
            long n = link->write(down.data() + sentDown, down.size() - sentDown);
            CHECK(n >= 0);
            if (n > 0) { sentDown += (size_t)n; moved = true; }
        }
        long n = link->read(buffer, sizeof buffer);
        CHECK(n >= 0);
        if (n > 0) { gotUp.append(buffer, (size_t)n); moved = true; }
        ssize_t m = read(node.phone, buffer, sizeof buffer);
        if (m > 0) { gotDown.append(buffer, (size_t)m); moved = true; }
        if (!moved) pause(1);
    }
    CHECK(gotUp == up);
    CHECK(gotDown == down);
}

// What the link gives within `ms`, until `want` bytes are there.
static std::string readFrom(Link* link, size_t want, int ms) {
    std::string got;
    double end = monoNow() + ms / 1000.0;
    while (got.size() < want && monoNow() < end) {
        char buffer[256];
        size_t room = want - got.size() < sizeof buffer ? want - got.size() : sizeof buffer;
        long n = link->read(buffer, room);
        if (n < 0) break;
        if (n > 0) got.append(buffer, (size_t)n);
        else pause(2);
    }
    return got;
}

// Whether the link has ended, within `ms`.
static bool endsWithin(Link* link, int ms) {
    double end = monoNow() + ms / 1000.0;
    char buffer[256];
    while (monoNow() < end) {
        if (link->read(buffer, sizeof buffer) < 0) return true;
        pause(5);
    }
    return false;
}

static std::string freshFolder(const std::string& name) {
    std::string dir = gWork + "/" + name, why;
    CHECK_IN(why, fs::removeTree(dir, &why));
    CHECK_IN(why, fs::makeDirs(dir, &why));
    return dir;
}

// ---- a link on a node ---------------------------------------------------------------------------------------------

static void testLink() {
    std::string dir = freshFolder("link");
    {   // bytes both ways at once, more than any buffer on the way holds
        Trace trace;
        LinksGone gone;
        Node node(dir + "/btspp", 0);
        Link* link = newSerialLink(node.openNear(), node.path, &trace, std::vector<uint8_t>(), 0);
        CHECK(link != 0 && std::string(link->kind()) == "serial");
        exchange(node, link, pattern(300000, 1), pattern(200000, 2));
        exchange(node, link, "a", "b");
        // The agent's side closes: what was still on its way is sent first, and the node is closed at once only
        // where no time is given for the phone to end the link.
        std::string last = pattern(5000, 3);
        size_t sent = 0;
        while (sent < last.size()) {
            long n = link->write(last.data() + sent, last.size() - sent);
            CHECK(n >= 0);
            sent += (size_t)n;
        }
        delete link;
        bool closed = false;
        CHECK(node.hear(last.size(), 5000, &closed) == last);
        node.hear(1, 3000, &closed);
        CHECK(closed);
        CHECK_IN(trace.text(),
                 sawWithin(trace, "closed, closed by the agent: 300001 B from the phone, 205001 B to it", 2000));
    }
    {   // the agent's side closes and the phone still holds the link: the node stays open and is read, what the
        // phone sends is dropped, and the phone's own end of the link is what closes the node
        Trace trace;
        LinksGone gone;
        Node node(dir + "/btspp", 3);
        Link* link = newSerialLink(node.openNear(), node.path, &trace);   // the protocol's time: 5 s
        CHECK(link != 0);
        exchange(node, link, "hello", "and the last answer");
        double at = monoNow();
        delete link;
        bool closed = false;
        for (int i = 0; i < 4; ++i) {   // a phone that has not heard yet, and goes on talking
            node.say(pattern(3000, 10 + (unsigned)i));
            CHECK(node.hear(1, 150, &closed).empty() && !closed);
        }
        CHECK(serialLinksOpen() == 1 && !trace.saw("closed,"));
        node.hangUp();
        CHECK_IN(trace.text(), sawWithin(trace, "closed, closed by the agent, and the phone ended the link ", 2000));
        CHECK_IN(trace.text(), trace.saw(" ms later: 5 B from the phone, 19 B to it, and 12000 B that the phone sent "
                                         "after the agent's close were dropped"));
        CHECK(waitForSerialLinks(1000) && monoNow() - at < 4.0);   // well inside the time it would have waited
    }
    {   // a phone that does not end the link: the node is closed under it when the time is up, and the log says so
        Trace trace;
        LinksGone gone;
        Node node(dir + "/btspp", 4);
        Link* link = newSerialLink(node.openNear(), node.path, &trace, std::vector<uint8_t>(), 700);
        CHECK(link != 0);
        exchange(node, link, "hello", "bye");
        delete link;
        bool closed = false;
        node.say("still here");
        CHECK(node.hear(1, 300, &closed).empty() && !closed);
        node.hear(1, 3000, &closed);
        CHECK(closed);
        CHECK_IN(trace.text(), sawWithin(trace, "closed, closed by the agent, and the link was still up 700 ms later: its "
                                                "node is closed under it: 5 B from the phone, 3 B to it, and 10 B that "
                                                "the phone sent after the agent's close were dropped", 2000));
    }
    {   // the phone's side ends: the link ends, after what the phone had sent
        Trace trace;
        LinksGone gone;
        Node node(dir + "/btspp", 1);
        Link* link = newSerialLink(node.openNear(), node.path, &trace);
        CHECK(link != 0);
        node.say("last words");
        char buffer[64];
        long n = 0;
        double end = monoNow() + 3.0;
        while (n == 0 && monoNow() < end) {
            n = link->read(buffer, sizeof buffer);
            if (n == 0) pause(2);
        }
        CHECK(n == 10 && memcmp(buffer, "last words", 10) == 0);
        node.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK_IN(trace.text(), sawWithin(trace, ": 10 B from the phone, 0 B to it", 2000));
    }
    {   // what was read from the node before the link was made comes first, and counts as the phone's
        Trace trace;
        LinksGone gone;
        Node node(dir + "/btspp", 2);
        const std::string opening = "NBTL+";
        Link* link = newSerialLink(node.openNear(), node.path, &trace,
                                   std::vector<uint8_t>(opening.begin(), opening.end()));
        CHECK(link != 0);
        node.say(" and more");
        CHECK(readFrom(link, 14, 3000) == "NBTL+ and more");
        node.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK_IN(trace.text(), sawWithin(trace, ": 14 B from the phone, 0 B to it", 2000));
    }
}

// ---- the listener -------------------------------------------------------------------------------------------------

// What a phone app of ours opens its link with.
static const std::string kMagic("NBTL");

// The tests' listener decides on a link in less than the real one's time, which they would otherwise wait out for
// every link that is let go, and gives a phone less time to end a link that the agent has closed, which they
// would wait out for every phone that does not.
static SerialConfig configFor(const std::string& dir) {
    SerialConfig c;
    c.nodePrefix = dir + "/btspp";
    c.decideMs = 300;
    c.lingerMs = 300;
    return c;
}

// The link the listener has ready, waiting up to `ms` for one.
static Link* acceptWithin(SerialListener& listener, int ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(listener.fd(), &rf);
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (select(listener.fd() + 1, &rf, NULL, NULL, &tv) <= 0) return 0;
    return listener.accept();
}

// The phone at `node` opens its link as a phone app of ours does, and the listener hands the link over. It starts
// with the magic bytes the listener read, which are taken from it here.
static Link* dialled(SerialListener& listener, Node& node, Trace& trace) {
    node.say(kMagic);
    Link* link = acceptWithin(listener, 3000);
    CHECK_IN(trace.text(), link != 0);
    CHECK(readFrom(link, kMagic.size(), 3000) == kMagic);
    return link;
}

static bool inactiveWithin(SerialListener& listener, int ms) {
    double end = monoNow() + ms / 1000.0;
    while (monoNow() < end && listener.active()) pause(5);
    return !listener.active();
}

static void testListener() {
    std::string dir = freshFolder("listener");
    {
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        Node before(dir + "/btspp", 7);
        before.appear();                                  // a link that was up before the agent came
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        std::string why;
        CHECK(!listener.active());
        CHECK_IN(why, listener.start(&why));
        CHECK(listener.active() && listener.fd() >= 0 && listener.accept() == 0);
        // The gate is there before the stack is asked for anything, and nothing is registered before it answers.
        CHECK(trace.at("gate attach") >= 0 && trace.at("gate attach") < trace.at("gap open") && !trace.saw("register"));
        gap.attach();
        CHECK(trace.saw("register"));
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));

        // A phone asks, the node appears, the phone says the magic bytes, and the link is there: opened by the
        // listener, handed over once.
        Node first(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        first.appear();
        Link* link = dialled(listener, first, trace);
        CHECK(std::string(link->kind()) == "serial" && listener.accept() == 0 && acceptWithin(listener, 50) == 0);
        CHECK(trace.saw("btspp0: it opened with the magic bytes, so the link is the agent's"));
        gap.phone(kInfServiceRequestComplete, kCarSerialService);
        exchange(first, link, pattern(20000, 4), pattern(20000, 5));
        CHECK(trace.saw("btspp0 opened") && trace.saw("a phone asked for a serial link (service 'SPP')"));
        CHECK(!trace.saw("Test Phone") && !trace.saw("AABBCCDDEEFF"));

        // A second phone, while the first link is up, and by the service's own name.
        Node second(dir + "/btspp", 1);
        gap.phone(kInfServiceRequest, kSerialServiceName);
        second.appear();
        Link* other = dialled(listener, second, trace);
        exchange(second, other, "two", "zwei");
        exchange(first, link, "one", "eins");

        // The first link ends and its node goes; the same node comes back with a new link.
        first.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
        first.vanish();
        pause(450);                                       // the listener sees that it is gone
        Node again(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        again.appear();
        link = dialled(listener, again, trace);
        exchange(again, link, "again", "wieder");

        // Not ours: a request for another service, a link no phone asked for, one the car refused, and the node
        // that was there at the start.
        Node foreign(dir + "/btspp", 2), unasked(dir + "/btspp", 3);
        gap.phone(kInfServiceRequest, "Another");
        foreign.appear();
        CHECK(acceptWithin(listener, 500) == 0);
        CHECK_IN(trace.text(), trace.saw("btspp2: the phone asked for another service with it"));
        gap.phone(kInfServiceRequest, kCarSerialService);
        gap.phone(kInfServiceRequestError, kCarSerialService);
        CHECK(trace.saw("the car refused the phone's link (1 so far)"));
        unasked.appear();
        CHECK(acceptWithin(listener, 500) == 0);
        CHECK(trace.saw("btspp7 exists already") && !trace.saw("btspp7 opened") && !trace.saw("btspp3 opened"));

        // stop(): the service goes, then the gate. The links that are up stay up.
        CHECK(!trace.saw("deregister") && !trace.saw("gate detach"));
        listener.stop();
        CHECK(!listener.active());
        CHECK(trace.at("deregister") >= 0 && trace.at("deregister") < trace.at("gate detach"));
        exchange(again, link, "still", "noch");
        exchange(second, other, "here", "da");
        listener.stop();                                  // twice does nothing more
        delete link;
        delete other;
    }
    {   // the gate is not in front: the stack is never asked, and nothing is left behind
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gate.inFront = false;
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        std::string why;
        CHECK(!listener.start(&why) && has(why, "not in front") && !listener.active());
        CHECK(!trace.saw("gap open") && !trace.saw("register"));
    }
    {   // the stack cannot be reached: the gate is taken away again
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gap.canOpen = false;
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        std::string why;
        CHECK(!listener.start(&why) && !why.empty() && !listener.active());
        CHECK(trace.at("gate attach") >= 0 && trace.at("gate attach") < trace.at("gate detach"));
    }
    {   // the stack does not take the registration: the listener gives up and takes the gate away, for good when
        // it is told not to try again
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gap.registerError = 5;
        SerialConfig config = configFor(dir);
        config.retryMs = 0;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK(inactiveWithin(listener, 3000));
        CHECK_IN(trace.text(), trace.saw("did not take the registration: Bluetooth serial is off until the agent is "
                                         "started again") && trace.saw("gate detach"));
        CHECK(listener.accept() == 0);
        pause(300);
        CHECK_IN(trace.text(), trace.count("gate attach") == 1);
        listener.stop();
    }
    {   // otherwise it sets itself up again, as often as it takes, and is then a listener like any other
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gap.registerError = 5;
        SerialConfig config = configFor(dir);
        config.retryMs = 1000;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK(inactiveWithin(listener, 3000));
        CHECK_IN(trace.text(), trace.saw("did not take the registration: Bluetooth serial is off, and is set up again in 1 s"));
        CHECK(trace.count("gate attach") == 1 && trace.count("gate detach") == 1 && trace.count("gap open") == 1);
        // The second time the gate is not in front, the third the stack refuses again, the fourth it works.
        gate.inFront = false;
        CHECK_IN(trace.text(), sawWithin(trace, "Bluetooth serial is not set up again: the test's gate is not in front. Again in 1 s", 3000));
        CHECK(!listener.active());
        gate.inFront = true;
        CHECK_IN(trace.text(), sawWithin(trace, "setting Bluetooth serial up again", 3000));
        double end = monoNow() + 3.0;
        while (monoNow() < end && trace.count("gate detach") < 2) pause(5);
        CHECK_IN(trace.text(), trace.count("is set up again in 1 s") == 2 && trace.count("gate detach") == 2);
        gap.registerError = 0;
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 3000));
        CHECK(listener.active() && trace.count("gap open") == 1);   // on the client it had: that one is opened once
        CHECK(trace.count("gate attach") == 4);

        Node node(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        node.appear();
        Link* link = dialled(listener, node, trace);
        exchange(node, link, "back", "again");
        delete link;
        listener.stop();
        CHECK_IN(trace.text(), trace.count("gate detach") == 3);
    }
    {   // stop() does not wait for a listener that is waiting to set itself up again
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gap.registerError = 5;
        SerialConfig config = configFor(dir);
        config.retryMs = 60000;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK(inactiveWithin(listener, 3000));
        double before = monoNow();
        listener.stop();
        CHECK(monoNow() - before < 1.0 && trace.count("gate attach") == 1 && trace.count("gate detach") == 1);
    }
    {   // the stack never answers: the same, once it has been too long
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        SerialConfig config = configFor(dir);
        config.registerMs = 300;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        CHECK(listener.active());
        CHECK(inactiveWithin(listener, 3000));
        CHECK_IN(trace.text(), trace.saw("has not been for too long: Bluetooth serial is off, and is set up again in 30 s") &&
                                   trace.saw("gate detach"));
        CHECK(!trace.saw("deregister"));   // the stack was never there to be asked
    }
}

// ---- an agent behind the listener ---------------------------------------------------------------------------------

// The phone's side of a link. It counts the agent's pings and does not answer them.
class Answers : public PeerHandler {
public:
    Answers() : pings(0) {}
    virtual void onRequest(Peer*, int64_t, const std::string& method, const json::Value&) {
        if (method == "ping") ++pings;
    }
    virtual void onResponse(Peer*, int64_t id, bool ok, const json::Value& result, const std::string& code,
                            const std::string&) {
        if (!ok) {
            got[id] = code;
            return;
        }
        json::Value role = result.find("role"), maxFrame = result.find("maxFrame");
        char frame[32];
        snprintf(frame, sizeof frame, " maxFrame=%lld", maxFrame.isInteger() ? (long long)maxFrame.integer() : -1LL);
        got[id] = std::string("ok") + (role.isString() ? " role=" + role.string() + frame : std::string());
    }
    virtual void onEvent(Peer*, const std::string& name, int64_t, const json::Value& params) {
        json::Value reason = params.find("reason");
        events.push_back(name + (reason.isString() ? " " + reason.string() : std::string()));
    }
    virtual void onData(Peer*, uint16_t, const std::string&, bool) {}

    std::map<int64_t, std::string> got;
    std::vector<std::string> events;   // each event's name, and its reason if it has one
    unsigned pings;
};

// The time, and as much more as the test says has passed.
class SkewClock : public Clock {
public:
    SkewClock() : ahead(0) {}
    virtual int64_t nowMs() { return real_.nowMs() + ahead; }
    int64_t ahead;

private:
    SystemClock real_;
};

class ZeroRandom : public RandomSource {
public:
    virtual void bytes(uint8_t* out, size_t len) { memset(out, 0, len); }
    virtual bool secret(uint8_t*, size_t) { return false; }   // nothing here pairs a phone or does auth
};

struct Car {
    Trace trace;
    LinksGone gone;
    FakeGap gap;
    FakeGate gate;
    std::string dir;
    HostPlatform platform;
    SkewClock clock;
    ZeroRandom random;
    Agent agent;
    SerialListener listener;

    Car() : gap(&trace), gate(&trace), dir(freshFolder("agent")), platform(dir + "/root"),
            agent(&platform, std::vector<TrustedKey>(), "1.0.0", &clock, &random),
            listener(&gap, &gate, &trace, configFor(dir)) {
        std::string why;
        CHECK_IN(why, platform.prepare(&why));
        agent.setLog(0);
        CHECK_IN(why, agent.init(&why));
        CHECK_IN(why, listener.start(&why));
        agent.addListener(&listener);
        gap.attach();
    }

    // A phone connects: its link is `node`.
    void connect(Node& node) {
        gap.phone(kInfServiceRequest, kCarSerialService);
        node.appear();
    }
    // The agent takes turns until it has `sessions` links, for `ms` at most.
    bool sessionsWithin(size_t sessions, int ms) {
        double end = monoNow() + ms / 1000.0;
        while (monoNow() < end && agent.sessions() != sessions) agent.turn(10);
        return agent.sessions() == sessions;
    }
};

static void testAgent() {
    Car car;
    {   // a phone that opens with the magic bytes and says hello gets its answer
        Node node(car.dir + "/btspp", 0);
        car.connect(node);
        Answers answers;
        Peer phone(new SocketLink(node.phone, "phone"), true, &answers);
        node.phone = -1;   // the peer's now
        phone.flush();     // the magic bytes, at once
        int64_t id = phone.request("hello", "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"phone\",\"phoneId\":"
                                            "\"11111111111111111111111111111111\",\"name\":\"Test phone\","
                                            "\"maxFrame\":65536,\"capabilities\":[]}");
        CHECK(id > 0);
        double end = monoNow() + 5.0;
        while (answers.got.empty() && phone.open() && monoNow() < end) {
            car.agent.turn(10);
            phone.flush();
            phone.readable();
        }
        // The link the listener made is of the serial kind: the agent gives the serial frame size.
        CHECK_IN(car.trace.text() + phone.closeReason(), answers.got[id] == "ok role=agent maxFrame=4096");
        CHECK(car.agent.sessions() == 1);
        // ping, both ways: the phone's is answered; the agent's comes after 10 s with nothing heard.
        id = phone.request("ping", "{}");
        end = monoNow() + 5.0;
        while (answers.got.count(id) == 0 && phone.open() && monoNow() < end) {
            car.agent.turn(10);
            phone.flush();
            phone.readable();
        }
        CHECK_IN(phone.closeReason(), answers.got[id] == "ok" && answers.pings == 0);
        car.clock.ahead += kSerialPingAfterMs;
        end = monoNow() + 5.0;
        while (answers.pings == 0 && phone.open() && monoNow() < end) {
            car.agent.turn(10);
            phone.readable();
        }
        CHECK_IN(phone.closeReason(), answers.pings == 1 && car.agent.sessions() == 1);
        phone.close("done");
        CHECK(car.sessionsWithin(0, 3000));
    }
    {   // a link that opens with anything else never reaches the agent: it is let go, with not a byte written
        Node node(car.dir + "/btspp", 1);
        car.connect(node);
        node.say("\x10\x20\x30\x40 another app's opening");
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "btspp1 let go", 3000));
        CHECK(!car.sessionsWithin(1, 200));
        bool closed = false;
        CHECK(node.hear(1, 3000, &closed).empty() && closed);
    }
    {   // nor does one on which the phone says nothing
        Node node(car.dir + "/btspp", 2);
        car.connect(node);
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "btspp2 let go", 3000));
        CHECK(!car.sessionsWithin(1, 200));
        bool closed = false;
        CHECK(node.hear(1, 3000, &closed).empty() && closed);
    }
    {   // one that opens with the magic bytes and then says nothing is the agent's, which gives it 30 s for its
        // hello: then the agent ends it, its node is closed, and nothing was written
        Node node(car.dir + "/btspp", 3);
        car.connect(node);
        node.say(kMagic);
        CHECK(car.sessionsWithin(1, 3000));
        car.clock.ahead += kSerialSilenceMs - 8000;   // well short of it, however slowly this machine runs
        CHECK(!car.sessionsWithin(0, 400));
        car.clock.ahead += 8000;
        CHECK(car.sessionsWithin(0, 3000));
        bool closed = false;
        CHECK(node.hear(1, 3000, &closed).empty() && closed);
    }
    {   // the agent stops: a phone that has said hello is told so, with link.closing as the last thing on its
        // link, and when the phone ends the link on that, the node closes with the phone and not under it
        Node node(car.dir + "/btspp", 4);
        car.connect(node);
        Answers answers;
        Peer phone(new SocketLink(node.phone, "phone"), true, &answers);
        node.phone = -1;
        phone.flush();
        int64_t id = phone.request("hello", "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"phone\",\"phoneId\":"
                                            "\"22222222222222222222222222222222\",\"name\":\"Test phone\","
                                            "\"maxFrame\":4096,\"capabilities\":[]}");
        double end = monoNow() + 5.0;
        while (answers.got.empty() && phone.open() && monoNow() < end) {
            car.agent.turn(10);
            phone.flush();
            phone.readable();
        }
        CHECK_IN(car.trace.text() + phone.closeReason(), answers.got[id] == "ok role=agent maxFrame=4096");
        CHECK(car.agent.sessions() == 1 && answers.events.empty() && !car.trace.saw("btspp4 closed"));
        car.agent.endPhoneLinks(kClosingStopping, 2000);
        CHECK(car.agent.sessions() == 0);
        end = monoNow() + 5.0;
        while (answers.events.empty() && phone.open() && monoNow() < end) {
            phone.readable();
            pause(2);
        }
        CHECK_IN(phone.closeReason(), answers.events.size() == 1 && answers.events[0] == "link.closing stopping" && phone.open());
        pause(100);   // inside the time the node waits for the phone, with the node still open
        CHECK(!phone.readable() && phone.open() && serialLinksOpen() == 1 && !car.trace.saw("btspp4 closed"));
        phone.close("told that the agent is stopping");
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "btspp4 closed, closed by the agent, and the phone ended the link ", 2000));
        CHECK(waitForSerialLinks(1000));
    }
    {   // a link that a phone opens while the agent is going, and that the agent never takes: stop() ends it
        Node node(car.dir + "/btspp", 5);
        car.connect(node);
        node.say(kMagic);
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "btspp5: it opened with the magic bytes, so the link is the agent's", 3000));
        CHECK(serialLinksOpen() == 1);
        car.listener.stop();
        CHECK(car.trace.saw("1 link had been opened and not yet taken by the agent"));
        CHECK(car.listener.accept() == 0 && car.agent.sessions() == 0);
        bool closed = false;
        CHECK(node.hear(1, 3000, &closed).empty() && closed);   // not a byte was written into it
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "btspp5 closed, closed by the agent, and the link was still up 300 ms later", 2000));
    }
    car.listener.stop();
    CHECK(car.trace.saw("deregister") && car.trace.saw("gate detach"));
}

// ---- what other programs left registered ----------------------------------------------------------------------------

// A machine on which the programs the test names run, and no other.
class Machine : public HostPlatform {
public:
    explicit Machine(const std::string& root) : HostPlatform(root) {}
    virtual ProgramState programState(const std::string& name) const {
        return name == running ? kProgramRunning : kProgramNotRunning;
    }
    std::string running;
};

static SerialServiceOwner declares(const char* name, const char* program) {
    SerialServiceOwner o;
    o.name = name;
    o.program = program;
    o.from = "the test";
    return o;
}

// The listener has the list of other programs' services swept each time its own has been registered, and is what
// the removals go through.
static void testLeftBehind() {
    std::string dir = freshFolder("left-behind");
    Trace trace;
    FakeGap gap(&trace);
    FakeGate gate(&trace);
    Machine machine(dir + "/root");
    Store store(&machine);
    SerialServices services(&machine, &store, &trace);
    std::vector<SerialServiceOwner> declared;
    declared.push_back(declares("TestRadio", "radiod"));
    declared.push_back(declares("Other", "otherd"));
    services.setDeclared(declared);
    machine.running = "otherd";

    SerialListener listener(&gap, &gate, &trace, configFor(dir));
    services.setRemover(&listener);
    listener.setServices(&services);
    std::string why;
    // Before the car's Bluetooth service is there, nothing can be removed.
    CHECK(!listener.removeSerialService("TestRadio") && services.sweep("too early") == 0 && !trace.saw("deregister"));
    CHECK_IN(why, listener.start(&why));
    gap.attach();
    CHECK_IN(trace.text(), sawWithin(trace, "deregister TestRadio", 2000));
    CHECK_IN(trace.text(), sawWithin(trace, "SppUuidDeregister reply for 'TestRadio': 0 OK", 2000));
    // After its own registration, and for the service whose program is gone only.
    CHECK(trace.at("the serial service 'NBTApp' is registered") < trace.at("deregister TestRadio"));
    CHECK(trace.saw("serial service 'TestRadio': its program (radiod) is not running") && !trace.saw("deregister Other"));
    CHECK(!trace.saw("deregister NBTApp") && listener.active());
    // The listener's own name is not one it removes this way, and its service stays.
    CHECK(!listener.removeSerialService(kSerialServiceName) && listener.removeSerialService("Other"));
    CHECK_IN(trace.text(), sawWithin(trace, "deregister Other", 2000) && listener.active());
    listener.stop();
    services.setRemover(0);
    CHECK(trace.saw("deregister NBTApp") && trace.saw("gate detach"));
}

// ---- the decide step ------------------------------------------------------------------------------------------------

// Whether the phone's end shows that the node was closed, with nothing written to it first.
static bool closedUnwritten(Node& node, int ms) {
    bool closed = false;
    return node.hear(1, ms, &closed).empty() && closed;
}

// What the phone says first decides whose link it is. One that is not the agent's is let go: closed, unread from
// then on, with nothing written into it, and never handed to the agent.
static void testDecide() {
    std::string dir = freshFolder("decide");
    {
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));

        // Nothing from the phone in the time it has.
        Node silent(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        silent.appear();
        double asked = monoNow();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp0 let go", 3000));
        CHECK(monoNow() - asked >= 0.25);   // not before the time was over
        CHECK_IN(trace.text(), trace.saw("(nothing came from the phone in that time) and closed: its node is still there"));
        CHECK(closedUnwritten(silent, 2000) && acceptWithin(listener, 50) == 0);
        // The log says when the node of a link that was let go went.
        silent.vanish();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp0 is gone, ", 2000) && trace.saw("s after it was let go and closed"));

        // Another app's opening: let go for what it said, without the wait.
        Node foreign(dir + "/btspp", 1);
        gap.phone(kInfServiceRequest, kCarSerialService);
        foreign.appear();
        foreign.say("\x10\x20\x30\x40 another app's opening");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp1 let go", 3000));
        CHECK_IN(trace.text(), trace.saw("btspp1: its first message came") &&
                               trace.saw("(it did not open with the magic bytes) and closed"));
        CHECK(closedUnwritten(foreign, 2000) && acceptWithin(listener, 50) == 0);
        CHECK(!trace.saw("another app's opening"));   // what another app said is not for the log

        // One that starts like the magic bytes and goes on otherwise.
        Node almost(dir + "/btspp", 2);
        gap.phone(kInfServiceRequest, kCarSerialService);
        almost.appear();
        almost.say("NBTX");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp2 let go", 3000) && closedUnwritten(almost, 2000));

        // The magic bytes in two messages: the agent's, and the link gives all that was read, in order.
        Node split(dir + "/btspp", 3);
        gap.phone(kInfServiceRequest, kCarSerialService);
        split.appear();
        split.say("NB");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp3: its first message came", 2000));
        split.say("TL and the rest");
        Link* link = acceptWithin(listener, 3000);
        CHECK_IN(trace.text(), link != 0);
        const std::string whole = "NBTL and the rest";
        CHECK(readFrom(link, whole.size(), 3000) == whole);
        exchange(split, link, "up", "down");
        split.hangUp();
        split.vanish();   // or its name would lead to the next node's terminal
        CHECK(endsWithin(link, 3000));
        delete link;

        // A part of them and no more.
        Node part(dir + "/btspp", 4);
        gap.phone(kInfServiceRequest, kCarSerialService);
        part.appear();
        part.say("NB");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp4 let go", 3000));
        CHECK(trace.saw("(only a part of the magic bytes came in that time)") && closedUnwritten(part, 2000));

        // The phone goes before it has said anything: closed, and not counted as let go.
        Node leaver(dir + "/btspp", 5);
        gap.phone(kInfServiceRequest, kCarSerialService);
        leaver.appear();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp5 opened", 2000));
        leaver.hangUp();
        CHECK_IN(trace.text(), sawWithin(trace, "before it was known whose link it is", 3000));
        CHECK(!trace.saw("btspp5 let go") && acceptWithin(listener, 50) == 0);
        listener.stop();
    }
    {   // for a car on which a link does not outlive the close of its node: the node stays open, unread, until the
        // link ends
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        SerialConfig config = configFor(dir);
        config.letGoKeepsNode = true;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));

        Node kept(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        kept.appear();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp0 let go", 3000));
        CHECK(trace.saw("Its node stays open, unread, until the link ends") && !trace.saw("and closed"));
        bool closed = false;
        kept.say("for whoever reads the node now");
        CHECK(kept.hear(1, 300, &closed).empty() && !closed);
        // The link ends, which on the car takes its node away: now it is closed.
        kept.vanish();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp0: the link ended", 2000) && trace.saw("its node is closed now"));
        CHECK(closedUnwritten(kept, 2000));

        // One that is still kept when the listener stops is closed then.
        Node last(dir + "/btspp", 1);
        gap.phone(kInfServiceRequest, kCarSerialService);
        last.appear();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp1 let go", 3000));
        CHECK(last.hear(1, 100, &closed).empty() && !closed);
        listener.stop();
        CHECK(closedUnwritten(last, 2000));
    }
    {   // a link that was let go is a sign of a service that nothing answers: the other programs' services are
        // swept, and no more often than they may be
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        Machine machine(dir + "/root");
        Store store(&machine);
        SerialServices services(&machine, &store, &trace);
        services.setDeclared(std::vector<SerialServiceOwner>(1, declares("TestRadio", "radiod")));
        services.setSweepEvery(1.0);
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        services.setRemover(&listener);
        listener.setServices(&services);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "deregister TestRadio", 2000));
        pause(1050);

        // A link of the agent's is no such sign.
        Node ours(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        ours.appear();
        Link* link = dialled(listener, ours, trace);
        ours.hangUp();
        ours.vanish();
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK(trace.count("deregister TestRadio") == 1);

        Node first(dir + "/btspp", 1), second(dir + "/btspp", 2);
        gap.phone(kInfServiceRequest, kCarSerialService);
        first.appear();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp1 let go", 3000));
        CHECK_IN(trace.text(), sawWithin(trace, "(a phone's link was not the agent's)", 2000));
        for (int i = 0; i < 400 && trace.count("deregister TestRadio") < 2; ++i) pause(5);
        CHECK_IN(trace.text(), trace.count("deregister TestRadio") == 2);
        gap.phone(kInfServiceRequest, kCarSerialService);
        second.appear();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp2 let go", 3000));
        pause(100);
        CHECK_IN(trace.text(), trace.count("deregister TestRadio") == 2);
        listener.stop();
        services.setRemover(0);
    }
}

// ---- the gate's hold ------------------------------------------------------------------------------------------------

// The programs that are declared for a serial service are kept off the node from a phone's request until its link
// has been let go, and for as long as a link of the agent's is up.
static void testHold() {
    std::string dir = freshFolder("hold");
    const std::string on = "gate: opens by otherd, radiod are refused as well from now on ";
    const std::string off = "gate: opens by otherd, radiod are passed on again ";
    {
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        Machine machine(dir + "/root");
        machine.running = "radiod";
        Store store(&machine);
        SerialServices services(&machine, &store, &trace);
        std::vector<SerialServiceOwner> declared;
        declared.push_back(declares("TestRadio", "radiod"));
        declared.push_back(declares("TestRadio-2", "radiod"));
        declared.push_back(declares("Other", "otherd"));
        services.setDeclared(declared);
        SerialConfig config = configFor(dir);
        config.linkWaitMs = 600;
        SerialListener listener(&gap, &gate, &trace, config);
        services.setRemover(&listener);
        listener.setServices(&services);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));
        pause(300);
        CHECK(!trace.saw("gate hold") && !trace.saw("gate release"));

        // A phone asks: the hold is on before its node is there, with each program once. The link is not the
        // agent's, and the hold goes off when it has been let go.
        Node silent(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        CHECK_IN(trace.text(), trace.count("gate hold otherd, radiod") == 1);
        CHECK_IN(trace.text(), sawWithin(trace, on + "(a phone has asked for a link)", 2000));
        silent.appear();
        CHECK_IN(trace.text(), sawWithin(trace, off + "(the link was let go)", 3000) && trace.saw("gate release"));
        CHECK(trace.at("gate hold") < trace.at("btspp0 opened") && trace.at("btspp0 opened") < trace.at("btspp0 let go") &&
              trace.at("btspp0 let go") < trace.at("gate release"));

        // The car refuses the phone: off at once.
        gap.phone(kInfServiceRequest, kCarSerialService);
        CHECK(trace.count("gate hold") == 2 && trace.count("gate release") == 1);
        gap.phone(kInfServiceRequestError, kCarSerialService);
        CHECK_IN(trace.text(), trace.count("gate release") == 2);
        CHECK_IN(trace.text(), sawWithin(trace, off + "(the car refused the phone's link)", 2000));

        // No link comes of the request: off when the wait for it is over, and not before.
        gap.phone(kInfServiceRequest, kCarSerialService);
        CHECK(trace.count("gate hold") == 3);
        pause(200);
        CHECK(trace.count("gate release") == 2);
        CHECK_IN(trace.text(), sawWithin(trace, off + "(no link came of the phone's request)", 3000));
        CHECK(trace.count("gate release") == 3);

        // A link of the agent's: the hold stays on for as long as the link is up, whatever else happens.
        Node ours(dir + "/btspp", 1);
        gap.phone(kInfServiceRequest, kCarSerialService);
        ours.appear();
        Link* link = dialled(listener, ours, trace);
        CHECK(trace.count("gate hold") == 4);
        pause(700);                                       // well past the wait for a request's link
        gap.phone(kInfServiceRequest, kCarSerialService);
        gap.phone(kInfServiceRequestError, kCarSerialService);
        pause(300);
        exchange(ours, link, "held", "gehalten");
        CHECK_IN(trace.text(), trace.count("gate hold") == 4 && trace.count("gate release") == 3);
        ours.hangUp();
        ours.vanish();
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK_IN(trace.text(), sawWithin(trace, off + "(the agent's link has ended)", 3000));
        CHECK(trace.count("gate release") == 4);

        // The listener stops with the hold on: off, and then the gate goes.
        gap.phone(kInfServiceRequest, kCarSerialService);
        CHECK(trace.count("gate hold") == 5);
        listener.stop();
        CHECK_IN(trace.text(), trace.count("gate release") == 5 && trace.saw("gate detach"));
        // And a request that comes after that turns nothing on.
        gap.phone(kInfServiceRequest, kCarSerialService);
        CHECK(trace.count("gate hold") == 5);
        services.setRemover(0);
    }
    {   // nothing declared: no hold, and the links work as they do
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        Machine machine(dir + "/root");
        Store store(&machine);
        SerialServices services(&machine, &store, &trace);
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        services.setRemover(&listener);
        listener.setServices(&services);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        Node ours(dir + "/btspp", 2);
        gap.phone(kInfServiceRequest, kCarSerialService);
        ours.appear();
        Link* link = dialled(listener, ours, trace);
        exchange(ours, link, "free", "frei");
        // A package's program is declared while the link is up: it is held off from then on.
        services.setDeclared(std::vector<SerialServiceOwner>(1, declares("TestRadio", "radiod")));
        CHECK_IN(trace.text(), sawWithin(trace, "gate hold radiod", 2000));
        CHECK(trace.saw("gate: opens by radiod are refused as well from now on (a link of the agent's is up)"));
        ours.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK_IN(trace.text(), sawWithin(trace, "gate release", 3000));
        listener.stop();
        services.setRemover(0);
    }
}

// What nbtagent's --decide and --decide-hold are made of: the value of the one, and what the agent's first log
// lines say of the decide step and of the gate's hold.
static void testOptions() {
    unsigned ms = 7;
    CHECK(parseSerialDecideMs("1", &ms) && ms == 1);
    CHECK(parseSerialDecideMs("500", &ms) && ms == 500);
    CHECK(parseSerialDecideMs("0800", &ms) && ms == 800);
    CHECK(parseSerialDecideMs("10000", &ms) && ms == kSerialDecideMostMs);
    ms = 7;
    CHECK(!parseSerialDecideMs("0", &ms) && !parseSerialDecideMs("000", &ms) && !parseSerialDecideMs("10001", &ms));
    CHECK(!parseSerialDecideMs("", &ms) && !parseSerialDecideMs(0, &ms) && !parseSerialDecideMs("-5", &ms));
    CHECK(!parseSerialDecideMs("5 ", &ms) && !parseSerialDecideMs(" 5", &ms) && !parseSerialDecideMs("5ms", &ms));
    CHECK(!parseSerialDecideMs("0.5", &ms) && !parseSerialDecideMs("+5", &ms) && !parseSerialDecideMs("0x10", &ms));
    // More digits than a number holds: refused, and not wrapped round into the range.
    CHECK(!parseSerialDecideMs("4294967297", &ms) && !parseSerialDecideMs("99999999999999999999", &ms));
    CHECK(ms == 7);   // a value that is refused changes nothing
    // The protocol's own time is one the option takes.
    CHECK((unsigned)kSerialDecideMs >= 1 && (unsigned)kSerialDecideMs <= kSerialDecideMostMs);

    SerialConfig config;
    CHECK(config.decideMs == (unsigned)kSerialDecideMs && !config.letGoKeepsNode);
    CHECK(describeSerialDecide(config) ==
          "a link is the agent's if it opens with the magic bytes within 500 ms of the open of its node. Any other is "
          "let go: its node is closed for the program it belongs to");
    config.decideMs = 1200;
    config.letGoKeepsNode = true;
    CHECK(describeSerialDecide(config) ==
          "a link is the agent's if it opens with the magic bytes within 1200 ms (--decide; the protocol's time is "
          "500 ms) of the open of its node. Any other is let go: its node stays open, unread, until the link ends "
          "(--decide-hold)");
    CHECK(config.lingerMs == (unsigned)kSerialLingerMs &&
          describeSerialLinger(config) == "a link the agent ends keeps its node, read and with what comes dropped, until "
                                          "the phone ends the link, for 5000 ms at most");

    CHECK(config.registerMs == 20000 && config.retryMs == 30000);
    CHECK(describeSerialRetry(config) ==
          "a serial service that the car's Bluetooth service does not take, or that is not registered for 20 s, is "
          "given up: the gate is taken away, and both are set up again 30 s later, for as long as it takes");
    config.retryMs = 0;
    CHECK(describeSerialRetry(config) ==
          "a serial service that the car's Bluetooth service does not take, or that is not registered for 20 s, is "
          "given up: the gate is taken away, and Bluetooth serial stays off until the agent is started again");

    std::vector<std::string> programs;
    CHECK(describeSerialHold(programs) ==
          "no program is declared for a serial service, so the gate holds none off a link that is being decided on "
          "or is the agent's");
    programs.push_back("otherd");
    CHECK(describeSerialHold(programs) ==
          "while a link is being decided on or is the agent's, the gate also refuses opens by otherd");
    programs.push_back("radiod");
    CHECK(describeSerialHold(programs) ==
          "while a link is being decided on or is the agent's, the gate also refuses opens by otherd, radiod");
}

int main(int argc, char** argv) {
    if (argc > 1) gWork = argv[1];
    // As the agent has it: a link whose other side has gone shows as an error, not as a signal.
    signal(SIGPIPE, SIG_IGN);
    testOptions();
    testLink();
    testListener();
    testAgent();
    testLeftBehind();
    testDecide();
    testHold();
    printf("test_serial: ok\n");
    return 0;
}
