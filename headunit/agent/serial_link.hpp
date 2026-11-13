// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SERIAL_LINK_HPP
#define NBT_AGENT_SERIAL_LINK_HPP

// Bluetooth serial: the link a phone app opens to the agent through the car's Bluetooth stack, with no network
// between them (plan document, "Bluetooth serial on the car").
//
// The agent registers a serial service of its own with the stack (spp_service.hpp). A phone app connects to the
// service's UUID; the stack says that a phone has asked for a link, and some tens of milliseconds later the link
// is there as a node, /dev/btsppN. The phone dialled, so it speaks first: the agent writes nothing into a link
// until the link has opened with the protocol's magic bytes (peer.hpp), and a link that opens otherwise is closed
// without a byte sent. That matters because the stack does not say whose link a node holds. It names the car's own
// serial service in every request, for a link to our UUID too, so every link a phone opens is looked at, and what
// the phone says first decides whether it is ours.
//
// Three things on the car shape the code:
//
//   * The car's media process opens the first node as well, for the car maker's phone apps, and takes the first
//     bytes of whatever link is there. So the gate (spp_gate.hpp) is put in front of that node before the service
//     is registered, and refuses that process for as long as the listener runs. While it does, the car maker's
//     phone apps do not work over Bluetooth. Without the gate in front the service is not registered at all.
//   * What a phone sends before a reader has the node open does not reach a reader that opens it later. The node
//     has to be opened at once, and the agent's one thread can be inside an install step. So the listener has a
//     thread of its own that opens the node, and hands the open link to the agent through a queue and a pipe.
//   * The node is not a socket. It hands over a message whole or loses the rest of it, and what it does with a
//     write that cannot go out at once has only been seen for blocking writes of about a thousand bytes. So the
//     agent does not read or write the node itself: each link has two threads that copy between the node and a
//     socket pair, in large reads and small blocking writes, and the agent holds the other end of the pair as an
//     ordinary link of kind "serial". A slow agent holds the copying up, and with it the phone.
//
// Another program on the car that takes serial links (a second service with a reader of its own) meets the same
// node for the same links: the stack gives one node to whatever link comes. Two things keep the two apart:
//
//   * The decide step. The listener's thread reads what the phone says first, for kSerialDecideMs from the open at
//     most. A link that opens with the magic bytes is the agent's, and goes to the agent with those bytes. Any other
//     link is let go: its node is closed and not read again, and no byte was written into it, so the program it
//     belongs to finds it as it would have. What the phone had said on it by then is lost to that program. A
//     link nobody answers is a sign of a service that was left behind, so the list of other programs' services is
//     swept then (serial_services.hpp). SerialConfig::letGoKeepsNode (`nbtagent --decide-hold`) keeps the node
//     open instead, unread, until the link ends.
//   * The hold. From a phone's request until its link has been let go, and for as long as a link of the agent's is
//     up, the gate also refuses the opens of the programs that are declared for a serial service, so that none of
//     them takes a link's first bytes or writes into a link of the agent's. Their looks at the node are passed on.
//
// A link on which the phone says the magic bytes and nothing more would hold its node for as long as the phone
// liked. The agent ends a serial link that has not done `hello` within 30 seconds, and one that has gone silent for
// as long (agent.hpp).
//
// How a link of the agent's ends. The phone made the link, and the phone is to end it: a link whose node the agent
// closed while the phone still held it, and went on sending into it, is what the car's Bluetooth does not get over:
// when the phone app holds such a link for 30 seconds or more, every new link after it is given a node on which
// nothing arrives, until the head unit is restarted. A close that the phone follows with its own within a second
// does no harm. So:
//
//   * when the agent ends a link on purpose it tells the phone first, with the event link.closing (agent.hpp), and
//     a phone app ends the link as soon as it hears that;
//   * when the agent's end of a link has closed, for whatever reason, the link's threads keep the node open and
//     go on reading it, dropping what comes, until the phone ends the link. They wait kSerialLingerMs for that
//     (SerialConfig::lingerMs) and close the node then, under a phone that has not heard or does not know the
//     event; the log says which of the two it was;
//   * the program does not end while a link waits like that (waitForSerialLinks).
//
// A link that is let go, as another program's, is not kept that way: its node is closed after the decide step.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <pthread.h>
#include <stdio.h>

#include <map>
#include <string>
#include <vector>

#include "link.hpp"
#include "protocol_constants.hpp"
#include "serial_log.hpp"
#include "serial_services.hpp"
#include "spp_gate.hpp"
#include "spp_nodes.hpp"
#include "spp_service.hpp"

namespace nbt {

// The agent's serial service, kSerialServiceName and kSerialServiceUuid (the name its registration is filed under,
// and the UUID a phone app connects to: this project's own, random, version 4), is in protocol_constants.hpp.
// The name of the car's own serial service, which the stack gives in a phone's request whatever UUID the phone
// connected to.
const char* const kCarSerialService = "SPP";

// SerialLog, where the serial code's account of what it does goes, is in serial_log.hpp.

// A link of kind "serial" on a serial node that is open already (blocking, for reading and writing). It takes
// `nodeFd` over: two threads copy between the node and the link until either side ends, and close the node then.
// 0 if it cannot be set up; the node is closed then too. The process must ignore SIGPIPE, as the agent does.
//
// The threads outlive the Link object. When the phone's side has ended they notice within a few hundred
// milliseconds, close the node and say so in the log. When the Link is deleted, or closed, they send on what the
// agent had still written and then keep the node open, reading it and dropping what comes, until the phone ends
// the link, for `lingerMs` at most: see the top of this file. `log` has to be there until they have ended, which
// serialLinksOpen() tells.
//
// `first` is what has been read from the node already: the link gives it before anything else.
Link* newSerialLink(int nodeFd, const std::string& path, SerialLog* log,
                    const std::vector<uint8_t>& first = std::vector<uint8_t>(),
                    unsigned lingerMs = (unsigned)kSerialLingerMs);
// How many links' threads are still running.
unsigned serialLinksOpen();
// Waits until none is, for `ms` at most. False if some still are.
bool waitForSerialLinks(int ms);

// The gate in front of the first serial node, as the listener uses it.
class NodeGate {
public:
    virtual ~NodeGate() {}
    // Puts the gate there and checks that other programs' opens come to it. False with *why if it is not in
    // front of the node, or if that cannot be told; nothing is left attached then.
    virtual bool attach(std::string* why) = 0;
    // Called often. When a link's node has appeared behind the gate since the last call, it checks that the gate
    // is still in front, and puts it there anew if it is not.
    virtual void watch() = 0;
    virtual void detach() = 0;
    // The hold: from here until release() the gate also refuses the opens of these programs, and never their
    // looks at the node. From any thread, attached or not.
    virtual void hold(const std::vector<std::string>& programs) = 0;
    virtual void release() = 0;
};

// The real one, for the head unit only: SppGate on /dev/btspp0, refusing the car's media process. The check is a
// look at the path by another process, because the gate's own process does not meet the gate: `self` is this
// program's own path, and `self --look /dev/btspp0` must exit with btspp::lookExitCode() of one stat() of the
// path.
class SppNodeGate : public NodeGate {
public:
    SppNodeGate(const std::string& self, SerialLog* log);
    virtual ~SppNodeGate();
    virtual bool attach(std::string* why);
    virtual void watch();
    virtual void detach();
    virtual void hold(const std::vector<std::string>& programs);
    virtual void release();

private:
    SppNodeGate(const SppNodeGate&);
    SppNodeGate& operator=(const SppNodeGate&);
    struct Count {
        unsigned opens, refused;
        Count() : opens(0), refused(0) {}
    };
    static void onOpen(const btspp::GateOpen& open, void* self);
    void say(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    bool look(long* pid, int* exitCode);
    btspp::GateCheck check(const char* when, bool* looked);

    std::string self_;
    SerialLog* log_;
    btspp::SppGate gate_;
    bool node_;                           // the stack's node was behind the gate at the last look
    pthread_mutex_t mu_;                  // by_, logged_
    std::map<std::string, Count> by_;     // by program name; "" for a name that could not be read
    unsigned logged_;
};

struct SerialConfig {
    std::string name, uuid;      // the service
    std::string carService;      // a request that names this service, or `name`, may be for a link of ours
    std::string nodePrefix;      // the nodes are this with 0 to 7 behind it
    unsigned registerMs;         // not registered for this long: the listener gives up
    unsigned retryMs;            // how long after giving up it sets itself up again; 0: it stays given up
    unsigned requestWindowS;     // how old a phone's request may be when its node appears
    unsigned decideMs;           // the decide step: how long from the open the phone's magic bytes may take
    unsigned linkWaitMs;         // after a phone's request: how long its node is looked for without a pause, and
                                 // how long the hold waits for it
    bool letGoKeepsNode;         // a link that is let go keeps its node open, unread, until the link ends: for a
                                 // car on which a link does not outlive the close of its node
    unsigned lingerMs;           // a link of the agent's whose agent's end has closed: how long its node is kept
                                 // open and read, for the phone to end the link

    SerialConfig()
        : name(kSerialServiceName), uuid(kSerialServiceUuid), carService(kCarSerialService),
          nodePrefix("/dev/btspp"), registerMs(20000), retryMs(30000), requestWindowS(15), decideMs((unsigned)kSerialDecideMs),
          linkWaitMs(3000), letGoKeepsNode(false), lingerMs((unsigned)kSerialLingerMs) {}
};

// ---- the decide step as nbtagent's options set it, and as its first log lines say it ----------------------------
// `--decide MS` sets SerialConfig::decideMs in place of the protocol's serial.decideMs, and `--decide-hold` sets
// letGoKeepsNode. Both are for finding out what a car needs; a phone app goes by the protocol's time.

// The most `--decide` takes, in milliseconds. A link that opens with the magic bytes still has to say `hello`.
const unsigned kSerialDecideMostMs = 10000;

// The value of `--decide`: a whole number of milliseconds from 1 to kSerialDecideMostMs, in digits and nothing else.
inline bool parseSerialDecideMs(const char* text, unsigned* out) {
    if (text == 0 || *text == 0) return false;
    unsigned ms = 0;
    for (const char* c = text; *c != 0; ++c) {
        if (*c < '0' || *c > '9') return false;
        ms = ms * 10 + (unsigned)(*c - '0');
        if (ms > kSerialDecideMostMs) return false;
    }
    if (ms == 0) return false;
    *out = ms;
    return true;
}

// What the listener does with a link once it has opened its node.
inline std::string describeSerialDecide(const SerialConfig& config) {
    char ms[96];
    if (config.decideMs == (unsigned)kSerialDecideMs) {
        snprintf(ms, sizeof ms, "%u ms", config.decideMs);
    } else {
        snprintf(ms, sizeof ms, "%u ms (--decide; the protocol's time is %u ms)", config.decideMs, (unsigned)kSerialDecideMs);
    }
    return std::string("a link is the agent's if it opens with the magic bytes within ") + ms +
           " of the open of its node. Any other is let go: " +
           (config.letGoKeepsNode ? "its node stays open, unread, until the link ends (--decide-hold)"
                                  : "its node is closed for the program it belongs to");
}

// What becomes of a link of the agent's when the agent ends it.
inline std::string describeSerialLinger(const SerialConfig& config) {
    char ms[32];
    snprintf(ms, sizeof ms, "%u ms", config.lingerMs);
    return std::string("a link the agent ends keeps its node, read and with what comes dropped, until the phone ends "
                       "the link, for ") + ms + " at most";
}

// What the listener does when the car's Bluetooth service does not take the registration, or loses it.
inline std::string describeSerialRetry(const SerialConfig& config) {
    char lost[32], again[32];
    snprintf(lost, sizeof lost, "%u s", config.registerMs / 1000);
    snprintf(again, sizeof again, "%u s", config.retryMs / 1000);
    std::string text = std::string("a serial service that the car's Bluetooth service does not take, or that is not "
                                   "registered for ") + lost + ", is given up: the gate is taken away, and ";
    return text + (config.retryMs == 0 ? "Bluetooth serial stays off until the agent is started again"
                                       : std::string("both are set up again ") + again + " later, for as long as it takes");
}

// Which programs the gate's hold keeps off the node, as declared now. The listener takes the list anew each time
// the hold goes on.
inline std::string describeSerialHold(const std::vector<std::string>& programs) {
    if (programs.empty()) {
        return "no program is declared for a serial service, so the gate holds none off a link that is being "
               "decided on or is the agent's";
    }
    std::string names;
    for (size_t i = 0; i < programs.size(); ++i) names += (i ? ", " : "") + programs[i];
    return "while a link is being decided on or is the agent's, the gate also refuses opens by " + names;
}

// Where phone apps connect to the agent over Bluetooth serial.
//
// start() puts the gate in front, registers the service and starts the thread that opens a link's node as soon as
// it is there. A listener that cannot get its service registered, or loses it for too long, gives up by itself:
// it takes the gate away again and is a listener nothing comes from, until it sets both up again
// (SerialConfig::retryMs later, and so on until the service is registered: an agent that starts with the head
// unit may be there before the car's Bluetooth service is). stop() removes the registration and the gate. Links already handed out are not touched by either: each lasts until one of its sides ends it. A link
// that stop() finds opened and not yet handed out has nobody to serve it, and is ended as the agent ends a link.
//
// The listener holds the agent's one client of the car's Bluetooth service, so it is also what removes another
// program's registration that was left behind (SerialServiceRemover). Given the list of such services, it has the
// list swept each time its own service has been registered (when the agent starts, and when the car's Bluetooth
// service comes back) and when it has let a link go, and the programs on the list are the ones the gate's hold
// keeps off the node.
//
// The hold goes by the links of the serial kind that are up in this process (serialLinksOpen()), so a process has
// one listener at a time.
class SerialListener : public Listener, public SerialServiceRemover, private btspp::SppService::Events {
public:
    // Nothing passed in is owned; all of it must outlive the listener, and `gap` the process (see DsiGapClient).
    SerialListener(btspp::GapClient* gap, NodeGate* gate, SerialLog* log, const SerialConfig& config = SerialConfig());
    virtual ~SerialListener();

    // Before start(). The services of other programs (serial_services.hpp); 0, which is how it starts, for none.
    void setServices(SerialServices* services) { services_ = services; }
    virtual bool removeSerialService(const std::string& name) { return spp_.removeOther(name); }

    // False with *why if the gate is not in front or the stack cannot be reached; nothing is left behind then.
    bool start(std::string* why);
    void stop();
    // False before start(), after stop(), and from when the listener gives up until it has set itself up again.
    bool active();

    virtual int fd() const { return wakeRead_; }
    virtual Link* accept();

private:
    SerialListener(const SerialListener&);
    SerialListener& operator=(const SerialListener&);

    virtual void sppSay(const std::string& line);
    virtual void sppState(btspp::SppService::State now);
    virtual void sppPhone(btspp::SppService::Phone what, const std::string& service, bool parsed);

    static void* threadMain(void* self);
    void run();
    void say(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    std::string nodePath(int i) const;
    void scanNodes(bool present[btspp::NodeTracker::kNodes]) const;
    void lookForLinks();
    void take(int node);
    // What the decide step found.
    enum Decided { kOurs, kLetGo, kEnded };
    Decided decide(int fd, const std::string& path, double openedAt, std::vector<uint8_t>* first, std::string* why);
    void letGo(int fd, int node, const std::string& path, double openedAt, const std::string& why);
    void closeKept(bool all);
    void updateHold();
    void waitForPass();
    bool linkExpected();
    bool stopping();
    void shutDown();
    bool comeBack();

    // A link that was let go, on the listener's thread only: with its node kept open (fd >= 0) until the link
    // ends, or closed, and then only for the log to say when its node went.
    struct LetGo {
        int node, fd;
        double at;
    };

    NodeGate* gate_;
    SerialLog* log_;
    SerialServices* services_;
    SerialConfig config_;
    btspp::SppService spp_;
    int wakeRead_, wakeWrite_;   // a byte per link in ready_: what the agent's select() waits on
    pthread_t thread_;
    bool hasThread_;
    std::vector<LetGo> letGo_;   // the listener's thread, and stop() once that has ended

    pthread_mutex_t mu_;         // everything below
    pthread_cond_t wake_;        // a phone has asked for a link, the registration has changed, or stop()
    bool stop_, active_;
    btspp::NodeTracker tracker_;
    double hotUntil_;            // until then the thread looks for the link's node without a pause
    double reqAt_;               // when the last request that may be ours came, until its node is opened; 0: none
    unsigned refused_;
    std::vector<Link*> ready_;   // opened, and not yet handed to the agent
    bool deciding_;              // a node is open and what the phone says first has not been read
    bool held_;                  // the gate's hold is on
    std::string heldText_;       // the programs it holds off, for the log
    const char* holdWhy_;        // what has just happened that may turn the hold on or off, for the log; 0: nothing
};

}  // namespace nbt

#endif
