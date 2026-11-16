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
// until the link has opened with the protocol's magic bytes (peer.hpp), and a link that opens otherwise is let go
// without a byte sent. That matters because the stack does not say whose link a node holds. It names the car's own
// serial service in every request, for a link to our UUID too, so every link a phone opens is looked at, and what
// the phone says first decides whether it is ours.
//
// What is understood about the node shapes the code (node_reader.hpp has the details):
//
//   * What the phone sends waits in the stack, ahead of the node, and moves into the node only when new bytes come
//     while nothing waits there, or when a read() reaches the node. select() does not ask the stack, so it says that
//     the node is empty while bytes wait. Bytes that nobody read outlive their link and come on a later link, ahead
//     of what the phone sends on that one, as one run of bytes. So every open node has one reader, a thread that
//     keeps a read out on the node from the open until the close (NodeReader), and nothing waits on a node with
//     select(). The agent's one thread can be inside an install step, so the listener has a thread of its own that
//     opens the node at once and hands the open link to the agent through a queue and a pipe.
//   * Closing a node does not end its link. A node closed under a link that is still up, while the phone goes on
//     sending, leaves those bytes waiting ahead of the node, and every later link opens with them: before the bytes
//     of the phone's new link come the old ones, which a reader that waits on select() never brings forward. So no
//     node is closed under a link that may still carry bytes, unless a program that reads such links is there to
//     take the node.
//   * The car's media process opens the first node as well, for the car maker's phone apps, and takes the first
//     bytes of whatever link is there. So the gate (spp_gate.hpp) is put in front of that node before the service
//     is registered, and refuses that process for as long as the listener runs. While it does, the car maker's
//     phone apps do not work over Bluetooth. Without the gate in front the service is not registered at all.
//   * What the node does with a write that cannot go out at once has only been seen for blocking writes of about a
//     thousand bytes. So the agent does not read or write the node itself: each link of the agent's has its reader
//     and two threads that copy between it, the node and a socket pair, in small blocking writes, and the agent
//     holds the other end of the pair as an ordinary link of kind "serial".
//
// Another program on the car that takes serial links (a second service with a reader of its own) meets the same
// node for the same links: the stack gives one node to whatever link comes. Two things keep the two apart:
//
//   * The decide step. The listener's thread takes what the reader brings, for kSerialDecideMs from the open at
//     most. Whole frames of this protocol at its start are what an earlier link of the agent's left: they are
//     counted, dropped and said in the log, and the magic bytes are looked for after them. A link that opens with
//     the magic bytes, so, is the agent's, and goes to the agent with those bytes. Any other link is let go, with
//     no byte written into it, and that includes one on which the leftovers are followed by anything else, or by
//     nothing in the time: another program's link may have the agent's old bytes in front of it. If a program
//     declared for a serial service runs, or may run, its node is closed for that program, which finds the link as
//     it would have, short of what the phone had said by then. If none does, nobody would ever read the link, and
//     its node is read, with what comes dropped, until the link ends. A link nobody answers is a sign of a service that was left behind, so
//     the list of other programs' services is swept then (serial_services.hpp). SerialConfig::letGoKeepsNode
//     (`nbtagent --decide-hold`) keeps the node of a link that is let go open instead, unread, until the link ends.
//   * The hold. From a phone's request until its link has been let go, and for as long as a link of the agent's is
//     up, the gate also refuses the opens of the programs that are declared for a serial service, so that none of
//     them takes a link's first bytes or writes into a link of the agent's. Their looks at the node are passed on.
//
// A link on which the phone says the magic bytes and nothing more would hold its node for as long as the phone
// liked. The agent ends a serial link that has not done `hello` within 30 seconds, and one that has gone silent for
// as long (agent.hpp).
//
// How a link of the agent's ends. The phone made the link, and the phone ends it:
//
//   * when the agent ends a link on purpose it tells the phone first, with the event link.closing (agent.hpp), and
//     a phone app ends the link as soon as it hears that;
//   * when the agent's end of a link has closed, for whatever reason, the link's threads keep the node open and go
//     on reading it, dropping what comes, until the phone ends the link, however long that takes. If the link is
//     still up kSerialLingerMs after the agent's close (SerialConfig::lingerMs), the log says so once; the link
//     counts in serialLinksOpen() until its node is closed;
//   * the program waits a while before it ends for the links that are still up (waitForSerialLinks);
//   * when the agent is asked to (serial.reset, serial_reset.hpp), the listener asks the car's Bluetooth service to
//     end the phone's link (ServiceDisconnect, spp_service.hpp), which is the one way the agent has of ending one.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <pthread.h>
#include <stdio.h>

#include <map>
#include <string>
#include <vector>

#include "link.hpp"
#include "node_reader.hpp"
#include "protocol_constants.hpp"
#include "serial_log.hpp"
#include "serial_reset.hpp"
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

// A link of kind "serial" on a serial node whose reader is running (node_reader.hpp). It takes `reader` over: two
// threads copy between the reader, the node and the link, and the node is closed when the link has ended. 0 if it
// cannot be set up; the node is closed then too. The process must ignore SIGPIPE, as the agent does.
//
// The threads outlive the Link object. When the phone's side has ended they notice within a tick, close the node
// and say so in the log. When the Link is deleted, or closed, they send on what the agent had still written and
// then keep the node open, reading it and dropping what comes, until the phone ends the link, however long that
// takes; once `lingerMs` have passed the log says that the link is still up. See the top of this file. `log` has
// to be there until they have ended, which serialLinksOpen() tells.
//
// `first` is what has been read from the node already: the link gives it before anything else.
Link* newSerialLink(NodeReader* reader, const std::string& path, SerialLog* log,
                    const std::vector<uint8_t>& first = std::vector<uint8_t>(),
                    unsigned lingerMs = (unsigned)kSerialLingerMs);
// The same on a node that is open already (blocking, for reading and writing), with a reader of its own started
// on it with the platform's timed read.
Link* newSerialLink(int nodeFd, const std::string& path, SerialLog* log,
                    const std::vector<uint8_t>& first = std::vector<uint8_t>(),
                    unsigned lingerMs = (unsigned)kSerialLingerMs);
// How many links of the agent's still have their node open.
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
    unsigned lingerMs;           // a link of the agent's whose agent's end has closed, and that is still up this
                                 // long after: the log says so. Its node is kept and read until the link ends
    NodeReadFn readNode;         // the timed read of a node; 0: the platform's (node_reader.hpp)
    unsigned resetArmedMs;       // serial.reset with no node open: for how long the next link is ended at its open

    SerialConfig()
        : name(kSerialServiceName), uuid(kSerialServiceUuid), carService(kCarSerialService),
          nodePrefix("/dev/btspp"), registerMs(20000), retryMs(30000), requestWindowS(15), decideMs((unsigned)kSerialDecideMs),
          linkWaitMs(3000), letGoKeepsNode(false), lingerMs((unsigned)kSerialLingerMs), readNode(0),
          resetArmedMs(60000) {}
};

// serial.reset with a node open: how long after the request ServiceDisconnect goes to the car, so that a caller on
// that very link has its answer first; and how long after that the log waits to say what came of it.
const unsigned kSerialResetDelayMs = 300;
const unsigned kSerialResetWatchMs = 3000;

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
           " of the open of its node, after any whole frames left over from an earlier link. Any other is let go: " +
           (config.letGoKeepsNode ? "its node stays open, unread, until the link ends (--decide-hold)"
                                  : "its node is closed for the program it belongs to if a program declared for a "
                                    "serial service is running, and read until the link ends if none is");
}

// What becomes of a link of the agent's when the agent ends it.
inline std::string describeSerialLinger(const SerialConfig& config) {
    char ms[32];
    snprintf(ms, sizeof ms, "%u ms", config.lingerMs);
    return std::string("a link the agent ends keeps its node, read and with what comes dropped, until the phone ends "
                       "the link; the log says so if it is still up ") + ms + " after the agent's end closed";
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
class SerialListener : public Listener, public SerialServiceRemover, public SerialReset,
                       private btspp::SppService::Events {
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

    // serial.reset (serial_reset.hpp). With a node open, the one being decided on, a link of the agent's or one
    // that is read to its end, ServiceDisconnect goes to the car kSerialResetDelayMs later. With none, the next link
    // whose node opens within SerialConfig::resetArmedMs is ended at the open, with no decide step, and its node read
    // until the link ends; once. The log says how long after ServiceDisconnect a node that was open then went, or
    // that the link was still up kSerialResetWatchMs after it. kResetFailed if the listener is not active, the car's
    // Bluetooth service is not attached, or, with a node open, no phone has asked for a link to be named.
    virtual Outcome resetSerialLink(const std::string& device, const std::string& instance, const std::string& service,
                                    int64_t* forMs, std::string* why);

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
    Decided decide(NodeReader* reader, const std::string& path, double openedAt, std::vector<uint8_t>* first,
                   uint64_t* leftOver, std::string* why);
    void letGo(NodeReader* reader, int node, const std::string& path, double openedAt, const std::string& why);
    void keep(int node, int fd, NodeReader* reader, const char* what);
    enum Closing { kEndedOnly, kAllButRead, kEvery };
    void closeKept(Closing which);
    void tendReset();
    void watchNodes(const bool nodes[btspp::NodeTracker::kNodes], double from);
    void updateHold();
    void waitForPass();
    bool linkExpected();
    bool stopping();
    void shutDown();
    bool comeBack();

    // A link that is not the agent's, on the listener's thread only: with its node read and what comes dropped
    // (reader), or kept open and unread (fd >= 0), until the link ends; or closed, and then only for the log to say
    // when its node went.
    struct LetGo {
        int node, fd;
        NodeReader* reader;
        double at;
        const char* what;   // what became of the link, for the log: "let go", "ended at its open"
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
    double watchFrom_;           // the listener's thread: when ServiceDisconnect went out for watched_; 0: none
    bool watched_[btspp::NodeTracker::kNodes];

    // What serial.reset asked for, for the listener's thread to do.
    struct Reset {
        bool due;                // ServiceDisconnect is to go out at `at`
        double at;
        bool armed;              // the next link that opens before `until` is ended at its open
        double until;
        double askedAt;
        std::string device, instance, service;
        Reset() : due(false), at(0), armed(false), until(0), askedAt(0) {}
    };

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
    unsigned kept_;              // the nodes in letGo_ that are open still: read, or kept unread
    Reset reset_;
    double resetHotUntil_;       // until then the thread looks without a pause, for serial.reset
};

}  // namespace nbt

#endif
