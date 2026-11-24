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
// the phone says first decides whether it is ours, or the phone's knock; but for a link that a rule leaves to another
// program with the knock off, which the agent only primes for that program and does not read (below).
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
//   * Once one read has been made on a link, what the phone sends comes into the node as it comes, for a reader
//     that waits on select() too; and bytes that were read can be put back into the node's queue, for the next
//     reader, while a descriptor stays open on the node: its last close empties the queue (NodePrimer,
//     node_reader.hpp). That is how a link the agent opened goes whole to another program (below).
//   * Closing a node does not end its link. A node closed under a link that is still up, while the phone goes on
//     sending, leaves those bytes waiting ahead of the node, and every later link opens with them: before the bytes
//     of the phone's new link come the old ones, which a reader that waits on select() never brings forward. So no
//     node is closed under a link that may still carry bytes, unless a program that reads such links is there to
//     take the node and what the agent read cannot be put back for it.
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
// node for the same links: the stack gives one node to whatever link comes. Three things keep the two apart (the
// first with the knock off only, `nbtagent --dial-knock off`):
//
//   * The rule. With the knock (DialConfig::knock, the default) there is none: every link a phone opens is opened
//     at once and decided on, whoever the phone is, since it may be that phone's knock (dial_rules.hpp), and
//     leftToOther() and phoneOpenedLeft() are false. With the knock off, a link a phone opens is left to another
//     program, never read by the agent, and no hold put on for
//     the phone's request that brings it, when all three of these hold, at the request and again when its node
//     appears: the car dials (SerialConfig::dial); a program declared for a serial service runs, or may
//     (SerialServices::readers()); and the phone that asked is one of the agent's whose app a link the car dialled
//     has been taken by before (phones.hpp, `dialled`, kept across restarts). That mark is given up when
//     kDialFailsToUnmark dials of the phone in a row have given the agent no link (dial_rules.hpp): the listener
//     hands the phone to the agent, whose thread clears the mark in its record, through the pipe links go by
//     (SerialDial::takeUndialled()), and the phone's own links are decided on again. The request still brings the
//     car's dial, which is how the app is reached then. The agent opens the link's node at once all the same, and
//     primes it for the other program (NodePrimer): one that waits on select() before it reads would otherwise
//     never see what the phone sent. With nothing in the node's queue it makes one read that does not wait, which
//     as a rule takes nothing and has the stack hand on what waits ahead of the node; bytes that read does take are
//     put back. The node is then kept open, unread, until the link ends, since its last close would empty its
//     queue, and it is no link of the agent's for anything else: not for the dial, the hold or serial.reset. A node
//     that cannot be opened or primed is closed again and left as it is. So the other program gets the link whole,
//     with the first bytes the decide step would have read. A link of the agent's app on such a phone is left all
//     the same: that is the rule's cost. Without such a dial the car may be one on which dialling does not work, and
//     the phone's own link stays the way in. A request whose rule no longer holds when its node appears goes as any
//     other. In every other case (no dial, no such program, a phone that is not the agent's or never reached by a
//     dial, a request that gave no address, a node with no request) the two below decide. Only a request that may
//     bring a serial node (one that names the agent's service or the car's serial service) is kept for the nodes at
//     all: one for another service, audio or hands-free, never brings one, and would be matched to the next serial
//     node.
//   * The decide step. The listener's thread takes what the reader brings, for kSerialDecideMs from the open at
//     most. Whole frames of this protocol at its start are what an earlier link of the agent's left: they are
//     counted, dropped and said in the log, and the magic bytes are looked for after them. A link that opens with
//     the magic bytes, so, is the agent's, and goes to the agent with those bytes. With the knock, one that opens
//     with the knock's magic bytes there instead (frame.hpp, kKnockMagic; a part of either waits for the rest
//     within the same time) is the knock of the phone whose request the node was matched to
//     (NodeTracker::takenAddress()): nothing is written into it, it is let go, and its node is read, with what comes
//     dropped, until the link ends, as a link nobody else reads is, but whatever signs of another reader come: the
//     phone's app ends it, and it is nobody else's link. The rules have the car dial that phone (dial_rules.hpp,
//     DialRules::knocked()). A link with no such request, or one that named no phone, is a knock of nobody's,
//     counted and ignored. Any other link is let go, with
//     no byte written into it, and that includes one on which the leftovers are followed by anything else, or by
//     nothing in the time: another program's link may have the agent's old bytes in front of it. If another
//     program may read it (a program declared for a serial service runs, or may run; the gate has seen another
//     program open the node since the link's node appeared; or a program of software put on by hand runs), the
//     reader is stopped, everything it read, leftovers too, is put back into the node's queue in the order it came,
//     with what came into the queue after its last read behind it (NodePrimer), and the node is kept open, unread,
//     until the link ends: that program finds the link whole. Where they cannot be put back (the node is not raw,
//     they do not fit in its queue, a call fails) the node is closed for that program instead, which then finds the
//     link short of what the phone had said by then. If none may, nobody would ever read the link, and its node is
//     read, with what comes dropped, until the link ends or a sign of another reader comes, when it is closed for
//     that reader: what was dropped by then is lost. A link nobody answers is a sign of a service that was left
//     behind, so the list of other programs' services is swept then (serial_services.hpp).
//   * The hold. From a phone's request until its link has been let go, and for as long as a link of the agent's is
//     up, the gate also refuses the opens of the programs that are declared for a serial service, so that none of
//     them takes a link's first bytes or writes into a link of the agent's. A phone's knock keeps the hold on until
//     the phone ends its link, so that no such program writes into it, with no time limit: while it is up the car
//     has no other phone-opened link for such a program to take. Their looks at the node are passed on.
//     A request whose link the rule leaves to another program puts no hold on: that program's opens pass at once.
//     The gate refuses the car's media process all the same.
//
// A link on which the phone says the magic bytes and nothing more would hold its node for as long as the phone
// liked. The agent ends a serial link that has not done `hello` within 30 seconds, and one that has gone silent for
// as long (agent.hpp).
//
// The car can also dial the phone (dial_rules.hpp, SerialConfig::dial): the listener asks the car's Bluetooth service
// to open a serial link to the agent's service on a phone of the agent's, on the car's instance kDialInstance, whose
// node, /dev/nbtapp, no other link uses. With the knock (the default) that is when the phone has knocked, by the
// knock's magic bytes on a link it opened (the decide step, above): a phone's request is no knock, whatever becomes
// of it, and brings no ask; and in the pairing window, at serial.dial and while the agent is on trial
// (agentOnTrial()), for DialConfig::trialDialForMs of the trial at most, after which the log says that the car no
// longer dials by itself for the trial; with ServiceConnect at once, and no ask. With the knock off, once the
// phone's service list, which the car is asked for first, has named that service, at polls and at the phone's
// requests. Where the car refuses a phone's request and its device list shows that phone connected with no service
// connected, the line for the refusal says that the car has no media audio connection to it, while which it refuses
// the phone's own links, and so its app cannot knock (refusalNoMediaAudio()). The phone's app listens on the agent's
// UUID and accepts the link. The ServiceListChanged informs that each such ask brings change nothing here: they are
// only counted (spp_service.hpp). The
// listener opens the node when the car answers 0, and a link whose phone says the magic bytes within
// DialConfig::magicWaitMs of the open is the agent's, with no gate, no hold and no let-go: nobody else's link is on
// that node. A link that does not open with them is ended at once by the car (ServiceDisconnect), and the phone is
// left alone by polls and its own requests for a while. The dialled link is a serial link like any other for the agent.
//
// The re-point (dial_rules.hpp) goes on the Bluetooth service's thread, in sppConnectReply(), the moment the car's
// failed answer to a dial is seen, before that answer is queued for the listener's thread and before anything is
// logged: SppService has cleared its "a ServiceConnect is out" state and let its lock go before it tells the answer,
// and a request may be sent from inside an event of the client's (GapClient::request(); DsiClient sends from the
// library's own thread). The listener's lock is not held while it is sent, since a client may answer within the send.
// Its answer comes the same way and is judged there; the listener's thread says what it came to, and gives it up when
// DialConfig::repointLimitMs have passed (SppService::giveUpConnect(), kLateDropped: a late answer to it is passed
// over).
//
// A dial with no answer in DialConfig::replyLimitMs is given up, and so is one that is out when the listener stops or
// gives up (after DialConfig::disconnectWaitMs more for its answer): the rules back the phone off and doubt the car's
// record (DialRules::gaveUp()), and SppService gives the ServiceConnect up too, so that the next dial may go, and keeps
// what it named (kLateTold). The car's answer that comes after all, while no ServiceConnect of the agent's is out, is
// told as late (sppLateConnectReply()): a failure brings the re-point there as the answer to a dial that is out would,
// and the listener's thread says it in the log and takes it for the answer to nothing, neither an ask nor a dial. A
// late 0 means the car has a link up on the dialled instance after all, which nothing of the agent's reads: the car
// is asked to end it (ServiceDisconnect for the phone it named) once the dial has nothing else under way. A late
// answer that comes while a new dial of the same phone is out cannot be told from that dial's own answer, and is
// taken as it; the new dial's own answer, should another come after it, is then dropped as another client's. A
// listener that stops and is set up again in the same process starts with its rules idle (endDialSteps()), and a late
// answer that comes in between or after is told as above.
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
//     end the phone's link (ServiceDisconnect, spp_service.hpp), which is the one way the agent has of ending one;
//     only while a node of the listener's is there, since one for a phone-opened link the car has no live link for
//     can hold that instance up for well over a minute. A node kept open for another program is not one of the
//     listener's: a ServiceDisconnect then would end that program's link.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <pthread.h>
#include <stdio.h>

#include <map>
#include <set>
#include <utility>
#include <string>
#include <vector>

#include "dial_rules.hpp"
#include "link.hpp"
#include "node_reader.hpp"
#include "phones.hpp"
#include "problem_log.hpp"
#include "protocol_constants.hpp"
#include "serial_log.hpp"
#include "serial_report.hpp"
#include "serial_dial.hpp"
#include "serial_reset.hpp"
#include "serial_services.hpp"
#include "spp_gate.hpp"
#include "spp_nodes.hpp"
#include "spp_service.hpp"

namespace nbt {

struct DialledLink;

// The agent's serial service, kSerialServiceName and kSerialServiceUuid (the name its registration is filed under,
// and the UUID a phone app connects to: this project's own, random, version 4), is in protocol_constants.hpp.
// The name of the car's own serial service, which the stack gives in a phone's request whatever UUID the phone
// connected to.
const char* const kCarSerialService = "SPP";
// The name the stack gives in a phone's request for the car's dial-up networking service (constants.txt,
// serial.knockDialupUuid), which is a phone's knock by dial-up where SerialDial::knockDialupOn().
const char* const kCarDialupService = "DUN";

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
// `first` is what has been read from the node already: the link gives it before anything else. `address` is the
// Bluetooth address of the phone whose link it is, where that is known: the link's phoneAddress().
Link* newSerialLink(NodeReader* reader, const std::string& path, SerialLog* log,
                    const std::vector<uint8_t>& first = std::vector<uint8_t>(),
                    unsigned lingerMs = (unsigned)kSerialLingerMs, const std::string& address = std::string());
// The same on a node that is open already (blocking, for reading and writing), with a reader of its own started
// on it with the platform's timed read.
Link* newSerialLink(int nodeFd, const std::string& path, SerialLog* log,
                    const std::vector<uint8_t>& first = std::vector<uint8_t>(),
                    unsigned lingerMs = (unsigned)kSerialLingerMs);
// One write into a serial node, as write() returns: the bytes taken, which may be fewer than `len`, or -1 with errno.
typedef long (*NodeWriteFn)(int fd, const void* buf, size_t len);

// How many links of the agent's still have their node open: the dialled one among them.
unsigned serialLinksOpen();
// How many of those are a link the car dialled (SerialListener): on its own node, which no gate is in front of.
unsigned dialledLinksOpen();
// Whether one of them that a phone opened, not the car, has the node `path` open.
bool phoneLinkOn(const std::string& path);
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
    // Whether a program other than this one has opened the node, or tried to, at `since` (monotonic seconds) or
    // later, and would read a link there: a program the gate refuses every time is not one. *who: the last such
    // program. From any thread.
    virtual bool sawOtherOpen(double /*since*/, std::string* /*who*/) { return false; }
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
    virtual bool sawOtherOpen(double since, std::string* who);

private:
    SppNodeGate(const SppNodeGate&);
    SppNodeGate& operator=(const SppNodeGate&);
    struct Count {
        unsigned opens, refused;
        Count() : opens(0), refused(0) {}
    };
    static void onOpen(const btspp::GateOpen& open, void* self);
    bool attachNow(std::string* why);
    void say(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void sayLine(const std::string& line);    // from any thread
    bool look(long* pid, int* exitCode);
    btspp::GateCheck check(const char* when, bool* looked);

    std::string self_;
    SerialLog* log_;
    btspp::SppGate gate_;
    bool node_;                           // the stack's node was behind the gate at the last look
    pthread_mutex_t mu_;                  // everything below up to held_; never held while a line is logged
    std::map<std::string, Count> by_;     // by program name; "" for a name that could not be read
    unsigned logged_;
    double otherAt_;                      // the last open by another program that would read a link; 0: none
    std::string other_;
    // attach(): the lines said on the thread that runs it are held until it is known whether they are news. A line
    // said on another thread meanwhile (the gate's own) is logged at once.
    bool buffering_;
    pthread_t attacher_;
    std::vector<std::string> held_;
    std::string lastFailure_;             // why the last attach failed; empty after one that worked
};

// The hold: no serial registration of the agent's goes to the car's Bluetooth stack until the stack has said for this
// long that it is open (spp_service.hpp). The car's own software makes its registrations within about a second of
// the stack's opening, in answer to that opening; the time is many times that, and short enough that an agent that
// starts with the head unit is registered within its first seconds.
const unsigned kSerialHoldMs = 10000;

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
    unsigned lingerMs;           // a link of the agent's whose agent's end has closed, and that is still up this
                                 // long after: the log says so. Its node is kept and read until the link ends
    NodeReadFn readNode;         // the timed read of a node; 0: the platform's (node_reader.hpp)
    NodeWriteFn writeNode;       // a write into a link's node; 0: write(). A test's may take a write short
    NodeCalls nodeCalls;         // the calls on a node's queue that prime a link and put bytes back for another
                                 // program (node_reader.hpp); a member left 0: the platform's
    unsigned resetArmedMs;       // serial.reset with no node open: for how long the next link is ended at its open
    unsigned checkMs;            // the registration check (SppService::checkRegistration): how often, while the
                                 // service is registered and quiet; 0: never
    unsigned checkQuietMs;       // quiet: no link of the agent's up or being decided on, and no phone's request
                                 // for a link for this long
    unsigned holdMs;             // no serial registration goes until the car's Bluetooth has been on for this long
                                 // (SppService::setHold()); counted from the attach when it is on then
    btspp::SppService::Clock holdClock;   // the clock that hold is counted by; 0: the system's. A test's own
    unsigned attachWaitMs;       // the car's Bluetooth service not attached for this long: the log says so, once
    DialConfig dial;             // the car dials the agent's phones (dial_config.hpp)

    SerialConfig()
        : name(kSerialServiceName), uuid(kSerialServiceUuid), carService(kCarSerialService),
          nodePrefix("/dev/btspp"), registerMs(20000), retryMs(30000), requestWindowS(15), decideMs((unsigned)kSerialDecideMs),
          linkWaitMs(3000), lingerMs((unsigned)kSerialLingerMs), readNode(0), writeNode(0),
          resetArmedMs(60000), checkMs(30000), checkQuietMs(30000), holdMs(kSerialHoldMs), holdClock(0),
          attachWaitMs(10000) {}
};

// What the system's log says of the car's Bluetooth service, for a client of it that has not attached: on the head
// unit, the service broker's line that names the version of the interface it has. A client that asks for a
// version the server does not have is never attached, and the broker says so only in the system's log.
class ServerLook {
public:
    virtual ~ServerLook() {}
    // A line for the log; empty, with *why, if none can be given. `iface` is the interface's name. It may take a
    // few seconds, and runs on the listener's thread.
    virtual std::string look(const std::string& iface, std::string* why) = 0;
};

// Finds the broker's line in the system's log as it is read, a piece at a time: the last line that names `iface`
// and the version available. A line longer than kMostLine is passed over, and so is what it holds.
class BrokerLineFinder {
public:
    enum { kMostLine = 1024 };
    explicit BrokerLineFinder(const std::string& iface) : iface_(iface), skipping_(false), bytes_(0) {}
    void feed(const char* data, size_t len) {
        bytes_ += len;
        for (size_t i = 0; i < len; ++i) {
            char c = data[i];
            if (c == '\n') {
                if (!skipping_) look(part_);
                part_.clear();
                skipping_ = false;
            } else if (!skipping_) {
                if (part_.size() >= kMostLine) {
                    part_.clear();
                    skipping_ = true;
                } else {
                    part_ += c;
                }
            }
        }
    }
    // The line found, as the log may show it; empty for none. A last line with no line end counts too.
    std::string line() {
        if (!skipping_ && !part_.empty()) {
            look(part_);
            part_.clear();
        }
        return found_;
    }

private:
    void look(const std::string& l) {
        if (l.find(iface_) == std::string::npos || l.find("version available") == std::string::npos) return;
        std::string shown;
        for (size_t i = 0; i < l.size() && shown.size() < 300; ++i) {
            char c = l[i];
            if (c == '\r') continue;
            shown += (c >= 0x20 && c < 0x7f) ? c : '?';
        }
        while (!shown.empty() && shown[0] == ' ') shown.erase(0, 1);
        found_ = shown;
    }

    std::string iface_, part_, found_;
    bool skipping_;
    unsigned long long bytes_;

public:
    // How much has been fed in all.
    unsigned long long bytes() const { return bytes_; }
};

struct ProcessResult;
// Why a look at the system's log with sloginfo (run by a shell, `timeoutS` at most) gave no broker line: that
// sloginfo could not be started (`ran` false, `error`), was not there or could not be run (the shell's 127 and 126),
// did not end in time, ended with another status or by a signal; or, when it ran to its end, how much of the log it
// gave, which tells an empty or short log from one that has no such line.
std::string brokerLookWhy(bool ran, const std::string& error, const ProcessResult& result, unsigned timeoutS,
                          unsigned long long bytes);

// serial.reset with a node open: how long after the request ServiceDisconnect goes to the car, so that a caller on
// that very link has its answer first; and how long after that the log waits to say what came of it.
const unsigned kSerialResetDelayMs = 300;
const unsigned kSerialResetWatchMs = 3000;

// ---- the decide step as nbtagent's options set it, and as its first log lines say it ----------------------------
// `--decide MS` sets SerialConfig::decideMs in place of the protocol's serial.decideMs. It is for finding out what a
// car needs; a phone app goes by the protocol's time.

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

// The most `--register-check` takes, in seconds.
const unsigned kSerialCheckMostS = 3600;

// The value of `--register-check`: a whole number of seconds from 0 (no check) to kSerialCheckMostS, in digits and
// nothing else.
inline bool parseSerialCheckS(const char* text, unsigned* out) {
    if (text == 0 || *text == 0) return false;
    unsigned s = 0;
    for (const char* c = text; *c != 0; ++c) {
        if (*c < '0' || *c > '9') return false;
        s = s * 10 + (unsigned)(*c - '0');
        if (s > kSerialCheckMostS) return false;
    }
    *out = s;
    return true;
}

// The most `--register-hold` takes, in seconds.
const unsigned kSerialHoldMostS = 600;

// The value of `--register-hold`: a whole number of seconds from 0 (no hold) to kSerialHoldMostS, in digits and
// nothing else.
inline bool parseSerialHoldS(const char* text, unsigned* out) {
    if (text == 0 || *text == 0) return false;
    unsigned s = 0;
    for (const char* c = text; *c != 0; ++c) {
        if (*c < '0' || *c > '9') return false;
        s = s * 10 + (unsigned)(*c - '0');
        if (s > kSerialHoldMostS) return false;
    }
    *out = s;
    return true;
}

// When the listener registers, and what it does to find out that the car's Bluetooth has dropped the registration.
inline std::string describeSerialCheck(const SerialConfig& config) {
    char hold[200];
    if (config.holdMs == 0)
        snprintf(hold, sizeof hold, "the serial registrations go as soon as the car's Bluetooth says it is on "
                 "(--register-hold 0)");
    else
        snprintf(hold, sizeof hold, "no serial registration goes until the car's Bluetooth has said for %u s that it "
                 "is on, counted from the attach when it is on then", config.holdMs / 1000);
    std::string head = std::string(hold) + ", with the stand-ins for the declared serial services of other programs "
                       "ahead of the agent's own; switching Bluetooth off drops them all, and the car says so. ";
    if (config.checkMs == 0)
        return head + "The registration is not checked (--register-check 0): a registration that the car drops with no "
                      "word stays lost until the agent is started again";
    char every[32], quiet[32];
    snprintf(every, sizeof every, "%u s", config.checkMs / 1000);
    snprintf(quiet, sizeof quiet, "%u s", config.checkQuietMs / 1000);
    return head + "While the service is registered, no link of the agent's is up and no phone has asked for a link "
                  "for " + quiet + ", the registration is checked every " + every +
           " by registering it again, after the stand-ins: a car whose Bluetooth says nothing of being switched off "
           "drops it all the same";
}

// What the listener does with a link once it has opened its node, and, while the car dials, which links a phone
// opens it does not read at all.
inline std::string describeSerialDecide(const SerialConfig& config) {
    char ms[96];
    if (config.decideMs == (unsigned)kSerialDecideMs) {
        snprintf(ms, sizeof ms, "%u ms", config.decideMs);
    } else {
        snprintf(ms, sizeof ms, "%u ms (--decide; the protocol's time is %u ms)", config.decideMs, (unsigned)kSerialDecideMs);
    }
    return std::string("a link is the agent's if it opens with the magic bytes within ") + ms +
           " of the open of its node, after any whole frames left over from an earlier link. Any other is let go. If "
           "a program declared for a serial service is running, the gate has seen another program open the node, or "
           "a program of software put on by hand is running, what was read from it is put back into its node, which "
           "stays open, unread, until the link ends, for the program it belongs to; where that cannot be done, its "
           "node is closed for that program. Otherwise it is read until the link ends or one of those signs comes" +
           (config.dial.on && config.dial.knock
                ? ". A link that opens with the knock's magic bytes there instead is the phone's knock: nothing is "
                  "written into it, the car is to dial the phone that opened it, and its node is read, with what comes "
                  "dropped, until the link ends. Every link a phone opens is read so, whoever the phone is"
            : config.dial.on ? ". A link that a phone of the agent's opens, whose app a link the car dialled has "
                               "reached, is not read at all while a program declared for a serial service is "
                               "running, or may be: it is left to that program, its node primed for it with one read "
                               "that does not wait and kept open, unread, until the link ends, and the app is reached "
                               "by the dial"
                             : "");
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

// ---- what the log says when the car refuses a phone's link ----------------------------------------------------

// What the agent can find out about a refusal: what the car's device list says of that phone, and the serial
// nodes there.
struct RefusalFacts {
    enum List {
        kListNotAsked,    // the device list could not be asked for
        kListNoAnswer,    // nor did it come in time
        kListRead         // `phone` says what came
    };
    enum Node { kNoNode, kNodeOurs, kNodeOther };
    List list;
    btspp::PhoneInList phone;
    Node node;
    RefusalFacts() : list(kListNotAsked), node(kNoNode) {}
};

// How long the device list is waited for after a refusal.
const unsigned kRefusalListWaitMs = 3000;

// Whether the device list shows the refused phone connected to the car with no service connected (connection 2):
// the car refuses a phone's own serial links while it has no media audio connection to that phone, and so its app
// cannot knock.
inline bool refusalNoMediaAudio(const RefusalFacts& f) {
    return f.list == RefusalFacts::kListRead && f.phone.ok && f.phone.error == 0 && f.phone.known && f.phone.listed &&
           f.phone.connection == btspp::kConnConnected;
}

// The log's line for the `count`th refusal of a link that may be the agent's: what was found, then the other
// causes, in the order to look at them. Never a phone's name or address.
inline std::string describeRefusal(unsigned count, const RefusalFacts& f) {
    char text[200];
    snprintf(text, sizeof text, "the car refused the phone's link (%u so far). ", count);
    std::string s = text;
    bool pairingSaid = false;
    if (f.list == RefusalFacts::kListNotAsked) {
        s += "The car's device list could not be asked for. ";
    } else if (f.list == RefusalFacts::kListNoAnswer) {
        snprintf(text, sizeof text, "The car's device list did not come in %u s. ", kRefusalListWaitMs / 1000);
        s += text;
    } else if (!f.phone.ok) {
        s += "The car's device list could not be read. ";
    } else if (f.phone.error != 0) {
        s += "The car did not give its device list (" + btspp::gapErrorText(f.phone.error, "") + "). ";
    } else if (!f.phone.known) {
        s += "The refusal named no phone to look for in the car's device list. ";
    } else if (!f.phone.listed) {
        s += "The phone is not in the car's device list: its pairing with the car is missing. ";
        pairingSaid = true;
    } else {
        snprintf(text, sizeof text, "In the car's device list the phone is %s (connection %u), security %u. ",
                 btspp::connectionName(f.phone.connection), f.phone.connection, f.phone.security);
        s += text;
        if (refusalNoMediaAudio(f))
            s += "So the car has no media audio connection to the phone, and the car refuses the phone's own serial "
                 "links while it has none, so the phone's app cannot knock meanwhile: the driver can connect it again "
                 "by choosing the phone in the car's media menu. ";
    }
    if (f.node == RefusalFacts::kNodeOther)
        s += "A serial node is there already and is not the agent's: the car takes one phone-opened serial link at a "
             "time. ";
    else if (f.node == RefusalFacts::kNodeOurs)
        s += "A serial link of the agent's is up already: the car takes one phone-opened serial link at a time. ";
    else
        s += "No serial node is there. ";
    s += "Other causes, in the order to look at them: the iDrive Bluetooth pairing menu is open; the ignition is off; ";
    if (!pairingSaid) s += "the phone's pairing with the car is broken or missing; ";
    return s + "the car's coding does not allow serial links for phones";
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
// The listener holds the agent's one client of the car's Bluetooth service, so it is also what registers the
// stand-ins of other programs' serial services and replaces a registration such a program left behind
// (SerialServiceRemover). Given the list of such services, it has the list swept each time its own service has been
// registered (when the agent starts, when the car's Bluetooth service comes back, and when the car's Bluetooth has
// been switched on again) and when it has let a link go, has the declared programs looked at before each registration
// check, and the programs on the list are the ones the gate's hold keeps off the node. While no registration may go
// (the hold, SerialConfig::holdMs, or the car's Bluetooth off) the listener waits for it, and the time it gives the
// registration (SerialConfig::registerMs) does not count.
//
// The hold goes by the links of the serial kind that are up in this process (serialLinksOpen()), so a process has
// one listener at a time.
class SerialListener : public Listener, public SerialServiceRemover, public SerialReset, public SerialDial,
                       public StackAsk, private btspp::SppService::Events {
public:
    // Nothing passed in is owned; all of it must outlive the listener, and `gap` the process (see DsiGapClient).
    SerialListener(btspp::GapClient* gap, NodeGate* gate, SerialLog* log, const SerialConfig& config = SerialConfig());
    virtual ~SerialListener();

    // Before start(). The services of other programs (serial_services.hpp); 0, which is how it starts, for none.
    void setServices(SerialServices* services) { services_ = services; }
    // Before start(). What the system's log says of the car's Bluetooth service, asked once when it has not attached
    // for SerialConfig::attachWaitMs; 0, which is how it starts, for nothing.
    void setServerLook(ServerLook* look) { serverLook_ = look; }
    // Before start(). The paired phones' addresses, which the car dials (SerialConfig::dial); 0, which is how it
    // starts, for none.
    void setPhoneAddresses(PhoneAddressList* list) { ours_ = list; }
    // Before start(). The problem file (problem_log.hpp): what goes wrong with Bluetooth serial, the first of each
    // kind in a run. 0, which is how it starts, for none.
    void setProblemLog(ProblemLog* problems) { problems_ = problems; }
    // A round of dials now, one to each phone that is connected to the car and paired with it (DialRules::force()):
    // how many phones that is. 0, with *why, if there is none or the car cannot be asked. From any thread; the
    // listener's thread dials.
    unsigned dialNow(std::string* why);
    // SerialDial (serial_dial.hpp). From any thread.
    virtual void pairingWindow(bool open);
    virtual bool dialOn() { return config_.dial.on; }
    virtual bool knockOn() { return config_.dial.on && config_.dial.knock; }
    virtual bool knockDialupOn() { return knockOn() && config_.dial.knockDialup; }
    virtual void agentOnTrial(bool onTrial);
    virtual unsigned dialNow(std::string* why, const char** code);
    virtual SerialStatus serialStatus();
    virtual bool phoneOpenedLeft(const std::string& address);
    virtual std::vector<std::string> takeUndialled();
    // SerialServiceRemover (serial_services.hpp), through the stand-ins of SppService. From any thread.
    virtual Replace replaceSerialService(const std::string& name);
    virtual void serialServiceRuns(const std::string& name) { spp_.standInRuns(name); }
    virtual void serialServicesDeclared(const std::vector<std::string>& names) { spp_.setStandIns(names); }

    // Puts the gate in front and opens the client of the car's Bluetooth service. If either does not work, the
    // listener says why in the log and tries again SerialConfig::retryMs later, and so on, each reason said once;
    // with no retryMs, start() is false with *why instead, and nothing is left behind. False with *why also when
    // the listener cannot run at all (no pipe or thread).
    bool start(std::string* why);
    void stop();
    // False before start(), after stop(), and from when the listener gives up until it has set itself up again.
    bool active();

    virtual int fd() const { return wakeRead_; }
    virtual Link* accept();

    // serial.reset (serial_reset.hpp). With a node open, the one being decided on, a link of the agent's or one
    // that is read to its end, ServiceDisconnect goes to the car kSerialResetDelayMs later, if such a node is still
    // there then: if none is, the link has ended meanwhile, or the listener has let it go to another program, nothing
    // is sent, the log and the problem file say which, and the reset is done. A node kept open, unread, for another
    // program does not count: the car would end that program's link. What counts is what this process reads, not
    // what the node tracker says, which starts afresh each time the listener is set up again. With none, the next link
    // whose node opens within SerialConfig::resetArmedMs is ended at the open, with no decide step, and its node read
    // until the link ends; once. The log says how long after ServiceDisconnect a node that was open then went, or
    // that the link was still up kSerialResetWatchMs after it. One ServiceDisconnect is out at a time: while
    // another, of the dial's or an earlier serial.reset's, waits for its answer, this one goes once that is answered
    // or given up, and the log says so. A later serial.reset takes the place of one that waits. kResetFailed if the listener is not active, the car's
    // Bluetooth service is not attached, or, with a node open, no phone has asked for a link to be named.
    virtual Outcome resetSerialLink(const std::string& device, const std::string& instance, const std::string& service,
                                    int64_t* forMs, std::string* why);

    // For the start-up report (serial_report.hpp): whether the registration has gone to the car's Bluetooth
    // service, and VersionGet. From any thread.
    virtual bool registrationSent() {
        btspp::SppService::State s = spp_.state();
        return s != btspp::SppService::kIdle && s != btspp::SppService::kWaiting && s != btspp::SppService::kHeld;
    }
    virtual bool askStackVersion() { return spp_.askVersion(); }

private:
    SerialListener(const SerialListener&);
    SerialListener& operator=(const SerialListener&);

    virtual void sppSay(const std::string& line);
    virtual void sppState(btspp::SppService::State now);
    virtual void sppPhone(btspp::SppService::Phone what, const std::string& service, bool parsed,
                          const std::string& address);
    virtual void sppRefusedPhone(const btspp::PhoneInList& phone);
    // The phone at `address` asked the car for its dial-up networking service (kCarDialupService) while
    // knockDialupOn(): that phone's knock by dial-up. On the Bluetooth service's thread, from sppPhone(). It does
    // nothing yet: the knock by dial-up is to take it as a knock (DialRules::knocked()) and see the request refused.
    void dialupKnock(const std::string& address);
    virtual void sppDevices(const std::vector<btspp::BtDevice>& devices);
    virtual void sppConnectReply(const btspp::ServiceConnectReply& reply);
    virtual void sppLateConnectReply(const btspp::ServiceConnectReply& reply);
    virtual void sppServiceList(const btspp::ServiceList& list);
    virtual void sppDisconnectReply(const btspp::ServiceDisconnectReply& reply);
    virtual void sppLinkEnded(const btspp::ServiceDisconnected& ended);
    virtual void sppPairing(uint32_t informId, const std::string& address);
    void sayRefusal(RefusalFacts::List list, const btspp::PhoneInList& phone);
    void tendRefusal();

    static void* threadMain(void* self);
    void run();
    void say(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    std::string nodePath(int i) const;
    void scanNodes(bool present[btspp::NodeTracker::kNodes]) const;
    void lookForLinks();
    // `address`: the phone's, from the request the node was matched to (NodeTracker::takenAddress()).
    void take(int node, const std::string& address);
    // What the decide step found. kKnock: the phone's knock (DialConfig::knock).
    enum Decided { kOurs, kKnock, kLetGo, kEnded };
    Decided decide(NodeReader* reader, const std::string& path, double openedAt, std::vector<uint8_t>* first,
                   uint64_t* leftOver, std::string* why);
    // `read`: all that decide() read from the link, in order.
    void letGo(NodeReader* reader, int node, const std::string& path, double openedAt, const std::string& why,
               const std::vector<uint8_t>& read);
    bool otherReader(int node, std::string* why);
    // Whether the link the phone at `address` opens is left to another program, unread (the top of this file).
    bool leftToOther(const std::string& address, std::string* why);
    // Opens node `node`, whose link the rule has left to another program for `rule`, and primes it for that program.
    void primeForOther(int node, const std::string& rule);
    // Node `node` is not the agent's link, and another program may read it: the tracker leaves it alone from now on,
    // so that nothing takes it for one of the listener's. mu_ held.
    void handedOver(int node) { tracker_.leaveAlone(node); }
    void keep(int node, int fd, NodeReader* reader, const char* what);
    enum Closing { kEndedOnly, kAllButUp, kEvery };
    void closeKept(Closing which);
    void tendLetGo();
    void tendReset();
    // The nodes that are there and that the listener or the agent reads now (serial.reset).
    void readNodes(bool nodes[btspp::NodeTracker::kNodes]);
    void watchNodes(const bool nodes[btspp::NodeTracker::kNodes], double from);
    void updateHold();
    void waitForPass();
    void checkRegistrationIfDue();
    // As Bluetooth serial stops: the stand-ins whose program is not running are removed (SppService::removeStandIns()),
    // and their answers waited for a while; with what they came to, for the log.
    void removeStandIns();
    // The declared programs that run are looked for, and their names are not known to hold a stand-in from here.
    void noteRunning();
    bool linkExpected();
    bool stopping();
    void shutDown();
    bool setUp(std::string* why);
    bool comeBack();
    void watchAttach(bool registered);
    // The car dials the phone, on the listener's thread.
    void tendDial();
    DialRules::Facts dialFacts();
    void maybeDial(int64_t now);
    void tendAsk(const std::vector<std::pair<btspp::ServiceList, int64_t> >& lists, int64_t now);
    enum AskEnd { kAskAnswered, kAskNoAnswer, kAskNotSent };
    void listed(DialRules::Listing what, const btspp::ServiceList* list, AskEnd how, int64_t cameAt, int64_t now);
    void askSay(const std::string& answer, const std::string& text);
    void askEnded(const std::string& reason);
    void sendDial(int64_t now);
    // What became of the re-point that a dial's answer may bring (dial_rules.hpp), for the line that says the car's
    // record is doubted: none was due (no answer yet, or none of the car's), it went, it could not be sent, or the
    // re-points have stopped in this run.
    enum RepointWent { kRepointNone, kRepointWent, kRepointNotSent, kRepointStopped };
    void doubtNow(bool before, const std::string& answer, RepointWent repoint);
    // After a dial of dial_'s phone that gave the agent no link: at kDialFailsToUnmark in a row, a phone that is
    // `dialled` is handed to the agent to give that up (takeUndialled()).
    void dialFailed();
    // With the knock: what follows for dial_'s phone after a dial that gave the agent no link, for the log: its
    // knocks ignored for a while, or its next knock dialled. `it`: the line names the phone "it", not "the phone".
    std::string knockAfter(int64_t now, bool it);
    void dialAnswered(const btspp::ServiceConnectReply& reply, int64_t now, RepointWent repoint);
    struct ConnectAnswer;
    // An answer to a dial that was given up: said, and never taken as the answer to anything that is out. A late 0
    // leaves the link it names to be ended (startLateEnd()).
    void lateAnswer(const ConnectAnswer& late);
    bool startLateEnd();
    // The re-point, on the Bluetooth service's thread: sent if `reply` is a failed answer to a dial that brings one,
    // and the re-points have not stopped; and its answer, judged as it comes (`at`).
    RepointWent repointAfter(const btspp::ServiceConnectReply& reply);
    void repointAnswered(const btspp::ServiceConnectReply& reply, int64_t at);
    // On the listener's thread: a re-point with no answer in DialConfig::repointLimitMs is given up, what the
    // re-points came to is said, and a ServiceDisconnect that one answered 0 owes is sent.
    void tendRepoint(int64_t now);
    // When the listener stops or gives up: a re-point that is out has DialConfig::repointSlowMs for its answer, and is
    // then given up, not judged, so that a listener set up again finds none out.
    void endRepoint(const char* why);
    void openDialled(int64_t now);
    void readMagic(int64_t now);
    void watchDialled(int64_t now);
    // What a clean-up of the dialled instance ends, and so what comes after it.
    enum CleanupKind {
        kCleanOwn,        // the dial's own link: nothing more
        kCleanStart,      // a link left behind, there when the listener started: the phones are dialled afresh
        kCleanBusy,       // one a dial found (answer 2): its phone is dialled again at once if it ended, and backed
                          // off if not
        kCleanRegister    // one that may have made the car refuse the registration: registered again if it ended
    };
    void startCleanup(const std::vector<std::string>& phones, const std::string& why);
    void endLeftBehind(const std::vector<std::string>& phones, const std::string& why, CleanupKind kind);
    void tendCleanup(int64_t now, bool afterReset);
    void cleanupSay(const std::string& line);
    void leftOutcome(bool there);
    // The registration refused while the dialled node is there (DialConfig::registerCleanupMs).
    enum LeftStep { kLeftGoesOn, kLeftRegister, kLeftGaveUp };
    bool startLeftForRegistration();
    LeftStep tendLeftForRegistration();
    void endDial(const char* why);
    void endDialSteps(const char* why);
    // A problem for the problem file, from any thread.
    void problem(const std::string& kind, const std::string& text) {
        if (problems_ != 0) problems_->note(kind, text);
    }
    void flushProblems();
    // Whether something of the agent's is on a serial link or about to be, a phone's pending request among it.
    bool dialBusy();
    // The same without the pending request, which *pending tells.
    bool linkBusy(bool* pending);

    // A link that is not the agent's, on the listener's thread only: with its node read and what comes dropped
    // (reader), or kept open and unread for another program (fd >= 0), until the link ends; or closed, and then only
    // for the log to say when its node went.
    struct LetGo {
        int node, fd;
        NodeReader* reader;
        double at;
        const char* what;   // what became of the link, for the log: "let go", "ended at its open", "primed for
                            // another program"
        double handedAt;    // when the listener stopped reading it and left it to another program; 0: it did not
        bool knock;         // the phone's knock: counted in knockLinks_ until it leaves letGo_
    };

    NodeGate* gate_;
    SerialLog* log_;
    SerialServices* services_;
    ServerLook* serverLook_;
    SerialConfig config_;
    NodePrimer primer_;
    btspp::SppService spp_;
    int wakeRead_, wakeWrite_;   // a byte per link in ready_: what the agent's select() waits on
    pthread_t thread_;
    bool hasThread_;
    std::vector<LetGo> letGo_;   // the listener's thread, and stop() once that has ended
    double watchFrom_;           // the listener's thread: when ServiceDisconnect went out for watched_; 0: none
    bool watched_[btspp::NodeTracker::kNodes];
    int decidingNode_;           // the listener's thread: the node being decided on; -1: none

    // What serial.reset asked for, for the listener's thread to do.
    struct Reset {
        bool due;                // ServiceDisconnect is to go out at `at`
        double at;
        bool armed;              // the next link that opens before `until` is ended at its open
        double until;
        // It is due, and another ServiceDisconnect waited for its answer: it goes once that one is answered or given
        // up (SppService::setDisconnectReplyTime), before any more of the dial's.
        bool waiting;
        double askedAt;
        std::string device, instance, service;
        Reset() : due(false), at(0), armed(false), until(0), waiting(false), askedAt(0) {}
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
    // The nodes in letGo_ that are open still and read by the listener: not one kept open, unread, for another
    // program, which is that program's link and no node of the listener's (serial.reset).
    unsigned reading_;
    // The knock links in letGo_, whose node is read until the phone ends the link: the hold stays on for them.
    unsigned knockLinks_;
    Reset reset_;
    double resetHotUntil_;       // until then the thread looks without a pause, for serial.reset
    double askedAt_;             // when a phone last asked for a link, for any service; 0: never
    double checkedAt_;           // the listener's thread: when the registration was last checked, or registered
    // The set-up and the attach, on the thread that sets the listener up: start()'s, then the listener's own.
    bool opened_;                // the client of the car's Bluetooth service is open
    bool final_;                 // it failed to open in a way no later try can mend: no more tries in this run
    std::string lastTry_;        // why the last set-up did not work; empty after one that did
    double unattachedSince_;     // since when the client has not been attached; 0 while it is
    bool everAttached_;
    bool attachSaid_;            // the log has said that it has not attached, since it last was
    bool lookDone_;              // the server look has run, once in the listener's life
    // The listener's thread: when it last looked at the nodes, and for each node the look before it appeared.
    double scannedAt_;
    double appeared_[btspp::NodeTracker::kNodes];
    // A refusal whose line waits for the car's device list (mu_): its count, what was there, and since when.
    unsigned refusalCount_;
    RefusalFacts::Node refusalNode_;
    double refusalAt_;           // 0: none waits
    // The listener's thread: when tendLetGo() last looked for running programs of software put on by hand.
    double handLookAt_;
    // The car dials the phone (SerialConfig::dial, dial_rules.hpp).
    PhoneAddressList* ours_;     // the paired phones' addresses; 0: none
    // mu_: the rules, which hold which of the car's phones are there (asked for each time the service has been
    // registered, and forgotten while the car's Bluetooth service is gone), and what the Bluetooth service's thread
    // has said for the listener's thread to act on.
    DialRules rules_;
    // The answers to the agent's ServiceConnects, with when each came and what became of the re-point it may have
    // brought.
    struct ConnectAnswer {
        btspp::ServiceConnectReply reply;
        int64_t at;
        RepointWent repoint;
        ConnectAnswer(const btspp::ServiceConnectReply& r, int64_t a, RepointWent w) : reply(r), at(a), repoint(w) {}
    };
    std::vector<ConnectAnswer> connectReplies_;
    // The late answers to dials that were given up (sppLateConnectReply()), the same way: kept apart, so that none is
    // taken as the answer to a dial that is out.
    std::vector<ConnectAnswer> lateReplies_;
    std::vector<std::pair<btspp::ServiceList, int64_t> > listReplies_;              // the same
    std::vector<btspp::ServiceDisconnectReply> disconnectReplies_;
    std::vector<btspp::ServiceDisconnected> dialEnds_;
    std::vector<std::string> pairingInforms_;   // the address each named, or empty
    // The phones whose `dialled` mark the agent is to give up, until it takes them (takeUndialled()); a wake byte
    // went into the pipe for each, as for a link in ready_.
    std::vector<std::string> undialled_;
    // The listener's thread: where the dial is.
    enum DialPhase {
        kDialNone,
        kDialAsk,        // ServiceListQuery is out
        kDialWait,       // ServiceConnect is out
        kDialMagic,      // the node is open, and the magic bytes are waited for
        kDialLinked,     // the dialled link is the agent's
        kDialCleanup     // ServiceDisconnect to end what is up on the instance, a phone at a time
    };
    DialPhase dialPhase_;
    DialRules::Dial dial_;             // the ask or dial of the phase, from kDialAsk on
    NodeReader* dialReader_;           // kDialMagic, and kDialCleanup of a node that is read until it ends
    std::vector<uint8_t> dialGot_;     // what came first on the node
    double dialOpenedAt_;
    DialledLink* dialLink_;            // kDialLinked: what the listener and the link's threads share
    std::vector<std::string> cleanupPhones_;   // kDialCleanup: whom ServiceDisconnect is still to name, in turn
    std::string lateLink_;             // the phone a late 0 named, whose link on the instance is to be ended; empty:
                                       // none is owed
    std::string cleanupWhy_;
    double discAt_;                    // when the dial's last ServiceDisconnect went out; 0: none waits
    std::string discAddress_;
    bool discAnswered_;
    uint32_t discError_;
    bool staleAtStart_;                // the dialled node was there when the listener started
    CleanupKind cleanupKind_;          // what kDialCleanup ends
    // A link left behind that the car was asked to end and did not: the first such clean-up is logged in full, and
    // after it, while each comes to the same, only counted. cleanupHeld_ keeps a quiet clean-up's lines until it is
    // known whether they are news.
    bool leftFailing_;                 // the last clean-up of a link left behind left its node there
    std::string leftOutcome_;          // what it came to
    unsigned leftTimes_;               // how many more came to the same since it was said
    bool cleanupQuiet_;
    std::vector<std::string> cleanupHeld_;
    std::string cleanupOutcome_;       // the answers of this clean-up's ServiceDisconnects, for comparing
    // The registration refused while the dialled node is there: the link left behind is ended first, once for each
    // refusal that follows a set-up.
    enum LeftPhase { kLeftNone, kLeftList, kLeftEnding };
    LeftPhase leftPhase_;
    double leftAt_;                    // when it began
    bool leftTried_;                   // since the last set-up or registration
    bool pairingOpen_;                 // mu_: the agent's pairing window is open
    bool onTrial_;                     // mu_: this version of the agent is on trial (agentOnTrial())
    // What serial.status counts (mu_), and when the registration was last checked or registered.
    SerialStatus counts_;
    double checkedAtShared_;
    // The listener's thread: how many links of the agent's were up at the last pass, to tell when one comes and goes.
    unsigned lastLinks_;
    // The problem file, and on the listener's thread: the registration checks that found it dropped, as last seen,
    // whether a failed write of the file has been said, and for each phone the answer its last dial was given and
    // how many times since it came again, which the log only counts; the same for the asks of its service list.
    ProblemLog* problems_;
    unsigned restoredSeen_;
    bool problemsSaid_;
    struct Repeat {
        std::string answer;
        unsigned times;
        Repeat() : times(0) {}
    };
    std::map<unsigned, Repeat> repeats_;
    std::map<unsigned, Repeat> askRepeats_;
    std::map<unsigned, Repeat> askEnds_;   // the asks that were ended, by why: a record of their own (askEnded())
    // The listener's thread: the phones, by addressKey(), whose mark the log has said is given up, until a dialled
    // link of theirs is the agent's again.
    std::set<std::string> undialSaid_;
    // The re-point (dial_rules.hpp); whether one is out, and since when, is the rules'. mu_: the number of the phone
    // whose dial's answer brought the one that is out or went last, and that answer; what each came to, for the
    // listener's thread to say; and whether the re-points have stopped for the rest of this run, which nothing turns
    // back, a re-attach and a new set-up of the listener included.
    enum RepointEnd {
        kRepointExpected,   // 5, within repointSlowMs
        kRepointOther,      // another answer, 0 among them
        kRepointNoCode,     // a rejection, or a reply that could not be read
        kRepointSlow,       // 5, after repointSlowMs or more
        kRepointNoAnswer,   // none in repointLimitMs: given up
        kRepointUnsent,     // it could not be sent: not the car's doing, so only counted
        kRepointDropped     // the listener stopped or gave up while it waited: given up, and not judged
    };
    struct RepointOutcome {
        RepointEnd end;
        uint32_t error;     // the car's answer to the re-point
        int64_t ms;         // how long after it went
        unsigned number;
        uint32_t dialError; // the dial's answer that brought it
        std::string why;    // kRepointUnsent, kRepointDropped: what kept it from going, or stopped the listener
        RepointOutcome() : end(kRepointExpected), error(0), ms(0), number(0), dialError(0) {}
    };
    unsigned repointNumber_;
    uint32_t repointDialError_;
    std::vector<RepointOutcome> repointEnds_;
    bool repointStopped_;
    // The listener's thread: a re-point answered 0 owes one ServiceDisconnect for the address it named, sent at the
    // first pass at which none of another's waits; and each phone's last re-point outcome, which the log only counts.
    bool repointDisconnect_;
    std::map<unsigned, Repeat> repointRepeats_;
};

}  // namespace nbt

#endif
