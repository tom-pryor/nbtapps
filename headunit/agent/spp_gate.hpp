// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SPP_GATE_HPP
#define NBT_AGENT_SPP_GATE_HPP

// A gate in front of /dev/btspp0: it sees every other program's open of that path before the Bluetooth stack's
// own node does, and decides whether the open goes on to the node.
//
// Why: every serial link a phone opens to the car lands on /dev/btspp0, and the car's media process opens that
// node for the car maker's own phone apps and reads the first bytes of whatever link is there. A link of ours on
// the node then has two readers, and the phone's bytes go to the wrong one.
//
// How: a resource manager attached at the same path, asked to resolve before the others there. It answers opens
// only, and only in two ways:
//   ENOENT   "not here": the opener's C library goes on to the next server for the path, which is the stack's
//            node if a link is up. The open then works as if the gate were not there.
//   EACCES   the open fails. The stack's node is never asked.
// No open ever succeeds at the gate, so no data passes through it. The process that attached the gate does not
// meet it: its own opens of the path go straight to the stack's node. Nothing is written to disk, and the gate
// is gone as soon as its process ends.
//
// Two modes. `log` passes every open on and reports who opened: it changes nothing but the order of the answers,
// and shows which programs open the node. `block` refuses the opens of one program, by the name of its
// executable, and passes the others on. A program whose name cannot be read is never refused.
//
// The hold. `block` can refuse the opens of more programs for a time: other programs that take serial links on the
// same car, each link being either ours or one of theirs. Only what the phone says first on a link tells whose it
// is, and until the gate's owner has read that, no other program must have the node open: it would take the
// phone's first bytes, or write its own into a link of ours. So the owner turns the hold on when a phone asks for
// a link, and off when it has let the link go; for a link of its own the hold stays on until the link has ended.
//
// While the gate is attached the path's name is there all the time, link or no link. The car's media detector
// watches for the name to appear and to go, so it reports once when the gate is attached and stays silent about
// the links that come and go behind it.
//
// This header holds the rule and the log text (plain C++98, host-tested in tests/agent/test_spp_gate.cpp) and the
// interface of the gate itself, which spp_gate.cpp implements for the head unit only.

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "programs.hpp"

namespace nbt {
namespace btspp {

// The node the car's media process is tied to, whichever node a link gets.
const char* const kGatePath = "/dev/btspp0";
// The program `block` refuses unless told otherwise: the media process.
const char* const kGateRefuseDefault = "NBTMediaMainApp";

enum GateMode { kGateOff = 0, kGateLog, kGateBlock };

inline bool parseGateMode(const std::string& s, GateMode* out) {
    if (s == "log") { *out = kGateLog; return true; }
    if (s == "block") { *out = kGateBlock; return true; }
    return false;
}

// A program's name from the path of its executable: programs.hpp.
using nbt::programName;

// What the gate answers an open by `program`: ENOENT passes it on, EACCES refuses it.
inline int gateAnswer(GateMode mode, const std::string& program, const std::string& refuse) {
    if (mode == kGateBlock && !program.empty() && !refuse.empty() && program == refuse) return EACCES;
    return ENOENT;
}

// One open that reached the gate.
struct GateOpen {
    // kOpen: a plain open. kLook: open, one request and close in one message, which is how stat() and access()
    // arrive. kCombined: open with a first request, the file kept open.
    enum Kind { kOpen = 0, kLook, kCombined };
    long pid;
    std::string program;   // empty: its name could not be read
    Kind kind;
    bool read, write;
    int answer;            // ENOENT or EACCES
    bool held;             // refused, and only because the hold was on
    GateOpen() : pid(0), kind(kOpen), read(false), write(false), answer(ENOENT), held(false) {}
};

// Whether the hold refuses an open that gateAnswer() would pass on: an open by one of the programs `holds`, while
// `held`. Their looks at the path (stat, access) are passed on all the same. A look is how a program finds out
// that a link is there, and a program that is told "nothing there" while a link is up takes the link for a new one
// each time it sees it again.
inline bool gateHolds(GateMode mode, const std::string& program, const std::vector<std::string>& holds, bool held,
                      GateOpen::Kind kind) {
    if (mode != kGateBlock || !held || kind == GateOpen::kLook || program.empty()) return false;
    for (size_t i = 0; i < holds.size(); ++i)
        if (holds[i] == program) return true;
    return false;
}

// The log line for an open.
inline std::string describeOpen(const GateOpen& o, GateMode mode, const std::string& refuse) {
    std::string s = "gate: ";
    if (o.kind == GateOpen::kLook) {
        s += "look (stat or access)";
    } else {
        s += o.kind == GateOpen::kCombined ? "open with a first request" : "open";
        s += o.read && o.write ? " for read and write" : (o.write ? " for write" : (o.read ? " for read" : ""));
    }
    char pid[32];
    snprintf(pid, sizeof(pid), " (pid %ld)", o.pid);
    s += " by ";
    s += o.program.empty() ? "a program whose name could not be read" : o.program;
    s += pid;
    if (o.answer == EACCES) {
        s += o.held ? ": refused for now (a link is being decided on, or is ours)" : ": refused";
    } else {
        s += ": passed on";
        if (mode == kGateLog && !o.program.empty() && o.program == refuse) s += " (--gate block refuses this one)";
    }
    return s;
}

// Folds a run of the same log line into the first one and a count of the rest.
class RepeatFold {
public:
    RepeatFold() : held_(0) {}

    // A line is about to be logged. True: log it, after reporting *repeats more of the line before it if that
    // is above 0. False: it is the last line again, counted and not logged.
    bool next(const std::string& line, unsigned* repeats) {
        *repeats = 0;
        if (line == last_) { ++held_; return false; }
        *repeats = held_;
        held_ = 0;
        last_ = line;
        return true;
    }

    // The repeats counted and not yet reported; they count as reported from here. For a pause and for the end.
    unsigned take() {
        unsigned n = held_;
        held_ = 0;
        return n;
    }

private:
    std::string last_;
    unsigned held_;
};

// ---- checking the gate from outside ----------------------------------------------------------------------------
// The gate's own process cannot see it, so the check is a look at the path (stat) by another process: the program
// run again as `nbtagent --look <path>`. What that process's exit code says, and whether the gate got its open, tell where the
// gate stands.

// The exit code of `nbtagent --look` for the outcome of its stat() (0 or an errno). 2 is taken by a usage error.
enum { kLookThere = 0, kLookNothing = 1, kLookRefused = 3, kLookOther = 4 };
inline int lookExitCode(int err) {
    return err == 0 ? kLookThere : (err == ENOENT ? kLookNothing : (err == EACCES ? kLookRefused : kLookOther));
}

enum GateCheck {
    kCheckFrontNoNode,   // the gate answered, and no node is behind it
    kCheckFrontNode,     // the gate answered, and the look went on to the node behind it
    kCheckRefused,       // the gate refused the look
    kCheckBehind,        // the node answered and the gate never got the look: the gate is not in front
    kCheckNotReached,    // nothing answered, the gate neither: it is not on the path at all
    kCheckUnclear
};

// `saw`: the gate got an open by the looking process. `lookExit`: that process's exit code.
inline GateCheck judgeGateCheck(bool saw, int lookExit) {
    if (saw && lookExit == kLookNothing) return kCheckFrontNoNode;
    if (saw && lookExit == kLookThere) return kCheckFrontNode;
    if (saw && lookExit == kLookRefused) return kCheckRefused;
    if (!saw && lookExit == kLookThere) return kCheckBehind;
    if (!saw && lookExit == kLookNothing) return kCheckNotReached;
    return kCheckUnclear;
}

// Whether the check shows that other programs' opens do not come to the gate.
inline bool gateNotInFront(GateCheck c) { return c == kCheckBehind || c == kCheckNotReached; }

inline const char* gateCheckText(GateCheck c) {
    switch (c) {
    case kCheckFrontNoNode: return "the gate answered first, and no node is behind it";
    case kCheckFrontNode:   return "the gate answered first, and the look went on to the node behind it";
    case kCheckRefused:     return "the gate refused it";
    case kCheckBehind:      return "the node answered and the gate was not asked: the gate is NOT in front";
    case kCheckNotReached:  return "nothing answered and the gate was not asked: the gate is NOT on the path";
    default:                return "no clear result";
    }
}

// The gate itself. Head unit only: on any other system attach() fails and says so.
class SppGate {
public:
    // Called for every open that reaches the gate, on the gate's own thread, after the answer is decided and
    // before the opener gets it.
    typedef void (*OpenCb)(const GateOpen& open, void* user);

    SppGate();
    ~SppGate();

    // Before attach(). `refuse` is the program `block` refuses.
    void configure(GateMode mode, const std::string& refuse, OpenCb cb, void* user);
    // The programs whose opens `block` also refuses while the hold is on. From any thread, attached or not.
    void setHolds(const std::vector<std::string>& programs);
    // The same for one program; empty for none.
    void setHold(const std::string& program);
    // Turns the hold on or off. From any thread. It is off until it is first turned on, and stays as it was set
    // when the gate is taken away and attached again.
    void hold(bool on);
    // Attaches at `path` and starts answering. False with *error set if it cannot.
    bool attach(const char* path, std::string* error);
    // Stops answering and takes the gate away. attach() may follow.
    void detach();
    bool attached() const;
    // Whether an open by this process is among the last few that reached the gate.
    bool sawPid(long pid);

private:
    SppGate(const SppGate&);
    SppGate& operator=(const SppGate&);
    struct Impl;
    Impl* impl_;
};

}  // namespace btspp
}  // namespace nbt

#endif
