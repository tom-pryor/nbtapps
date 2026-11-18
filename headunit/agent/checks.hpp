// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_CHECKS_HPP
#define NBT_AGENT_CHECKS_HPP

// The compatibility checks: what was put onto a head unit by hand, outside the packages, and stands in the way of
// the head unit's own working. A controller asks for the checks to be run (checks.run), shows what they found, and
// asks for a check's fix once the user has said yes (checks.fix). The checks are a table built into the agent;
// each one holds in its definition its id, its English texts, a detect that reads and writes nothing, the fix if
// it has one, and the restart its fix leaves needed.
//
// What a check finds is a list of paths to remove, each with its kind: a file, a link, a folder, or something else
// (a device, a socket, a fifo); and a list of folders to move, each with where it goes. The token of the two is
// SHA-256, as hex, over the check's id and both lists, sorted by path, kinds and destinations included. The
// controller is shown them with their token and sends the token back with checks.fix.
//
// The rules every fix keeps:
//   - It removes and moves what the check lists now, and only if that is what the controller was shown: it lists
//     again, and refuses with `stale` when the token differs. A file that came after the list stays, and so does
//     its folder.
//   - A folder that is moved is renamed, on one file system, to the destination it was shown with, and never onto
//     anything that is there: a fix does not copy, and writes over nothing.
//   - One path at a time: a file or a link is unlinked by its full path, and a folder is removed only when it is
//     empty, the deepest first. Nothing is removed by a wildcard, nothing is walked at the time of the removal,
//     and nothing is removed with what it holds.
//   - Nothing is followed through a link: a link is listed as a link and unlinked as one. A folder that is a link
//     is not entered. Something that is neither a file, a link nor a folder is never removed, and is reported as
//     left.
//   - The listing stays on the file system its root is on, and is bounded: kCheckMaxDepth levels below a root,
//     kCheckMaxEntries entries in all. A folder on another file system is not entered. Past either bound, or with a
//     name that cannot be shown as text, the check is a problem with no fix.
//   - A path that an installed package's record owns is never listed, and neither is anything in the agent's own
//     folders: a fix leaves both alone.
//   - Every part of it can be done again: a fix that was cut off is run again, from a new run of the check, and
//     goes on where the first one stopped. There is no journal.
//   - Every absolute path goes through Platform::systemPath(), so that on a folder that stands in for a head unit
//     everything stays in that folder.
//
// Each folder a fix moves and each path it removes is a line in the install log (Store::logPath()).
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

#include <set>
#include <string>
#include <vector>

#include "engine.hpp"
#include "platform.hpp"
#include "store.hpp"

namespace nbt {

const unsigned kCheckMaxDepth = 4;      // levels below a root that a listing enters
const size_t kCheckMaxEntries = 256;    // entries a listing holds at most, the roots among them

enum EntryKind { kEntryFile, kEntryLink, kEntryDir, kEntryOther };

struct CheckEntry {
    std::string path;
    EntryKind kind;
    CheckEntry() : kind(kEntryOther) {}
    CheckEntry(const std::string& p, EntryKind k) : path(p), kind(k) {}
};

// A folder a fix moves, by one rename, and where to.
struct CheckMove {
    std::string from, to;
    CheckMove() {}
    CheckMove(const std::string& f, const std::string& t) : from(f), to(t) {}
};
const size_t kCheckMaxMoves = 8;        // folders a check offers to move at most

enum CheckState { kCheckOk, kCheckProblem, kCheckUnknown };
const char* checkStateName(CheckState state);   // "ok", "problem", "unknown"

// What the checks are run with.
struct CheckContext {
    Platform* platform;
    const Store* store;
    bool stepRunning;       // an install step is running, or a job is between two of its steps
    unsigned stopWaitMs;    // how long a program has to end after SIGTERM, before SIGKILL
    unsigned killWaitMs;    // and after SIGKILL, before the fix gives up
    CrashHook* crash;       // test only: called between the parts of a fix; 0 for none

    CheckContext(Platform* p, const Store* s)
        : platform(p), store(s), stepRunning(false), stopWaitMs(5000), killWaitMs(1000), crash(0) {}
};

// What one run of a check found.
struct CheckFinding {
    CheckState state;
    bool fixable;                     // a problem whose fix may be run on this list
    std::string why;                  // unknown: why; a problem that cannot be fixed: what keeps the fix back
    std::vector<CheckEntry> entries;  // to remove, sorted by path; empty unless a problem
    std::vector<CheckMove> moves;     // to move, sorted by `from`; empty unless a problem
    std::string token;                // of the two, whatever the state

    CheckFinding() : state(kCheckOk), fixable(false) {}
};

enum FixKind {
    kFixNone,       // the check says what it found, and has no fix
    kFixRemove,     // the fix moves and removes what the check lists
    kFixRemount     // the fix puts the system partition back read-only, and lists and removes nothing
};

struct Check {
    const char* id;
    const char* title;       // the problem, in a few words
    const char* detail;      // what it is, and why it matters
    FixKind fix;
    const char* fixLabel;
    const char* fixDetail;   // what the fix does, and what it leaves
    const char* restart;     // what the fix leaves needed when it changed something: none, hmi-restart, required
    const char* stopUnder;   // kFixRemove: the programs whose executable is in this absolute folder (through
                             // systemPath) are stopped before anything is removed; 0 for none
    void (*detect)(const CheckContext& context, CheckFinding* out);
};

// The table, in the order checks.run answers:
//   system-writable   the system partition is writable, and the agent did not make it so. The fix remounts it
//                     read-only, and lists and removes nothing
//   aa-fix            an app that was installed by hand, outside this system: /opt/aa is there, or a plugin
//                     folder whose <Name>.manifest claims the connector id 210 and in which no installed package
//                     owns anything. Such a plugin folder is moved, whole, out of the plugin folder into /opt/aa
//                     as <Name>.old, or <Name>.nbtappsold where that is taken: the HMI may have the plugin loaded,
//                     and its files are not removed under it. One in varPluginsDir() goes into /var/opt/aa
//                     instead, which is on its file system where /opt/aa is not. It is left there. Everything
//                     else in /opt/aa and /var/opt/aa is listed and removed, but for a folder directly in either,
//                     not a link, whose name ends in .old or .nbtappsold: that is parked, and neither it nor
//                     anything in it is listed. Neither folder is listed itself while it receives a plugin or
//                     holds a parked one. The fix stops the programs that run from /opt/aa first. It never reads
//                     or writes the configuration of the packet filter
size_t checkCount();
const Check& checkAt(size_t index);
const Check* findCheck(const std::string& id);

// Runs a check's detect, and works out the token of what it found. Writes nothing anywhere.
void runCheck(const Check& check, const CheckContext& context, CheckFinding* out);
// The token of the lists, for the check with this id.
std::string checkToken(const std::string& id, const std::vector<CheckEntry>& entries,
                       const std::vector<CheckMove>& moves = std::vector<CheckMove>());

// How a fix ended.
struct FixOutcome {
    CheckState state;                   // the check run again afterwards
    size_t removed;                     // paths of the list that are gone
    std::vector<CheckMove> moved;       // the moves that were done
    std::vector<std::string> left;      // paths of the list that are still there, and the folders that were not
                                        // moved, sorted
    std::vector<std::string> stopped;   // the file names of the programs that were stopped
    std::string restart;                // the check's restart if anything was removed or remounted, else none

    FixOutcome() : state(kCheckOk), removed(0), restart("none") {}
};

// Runs a check's fix with the token the controller was shown. False, with *code and *message, when it is refused:
//   unsupported   the check has no fix, or none for what it finds now
//   stale         the token is not that of what the check finds now: run the check again
//   io-error      the check cannot be run now, a program could not be stopped, or the partition could not be put
//                 back read-only. Nothing was removed
// A check that finds nothing now, given the token of nothing, is answered as fixed, and nothing is done.
//
// kFixRemove, in this order, each part of which can be done again:
//   1. lists again, and compares the token
//   2. stops the programs whose executable is in stopUnder: SIGTERM, up to stopWaitMs, then SIGKILL, up to
//      killWaitMs. One that is still there, or processes that cannot be read, end the fix here
//   3. raises the restart mark to the check's restart, before anything is moved or removed
//   4. the moves, before any removal: makeWritable for both ends, the destination's folder made if it is not
//      there, a look that nothing is at the destination, and one rename
//   5. removes the paths of the list one at a time, each after makeWritable for it: each file or link is unlinked,
//      and then each folder is removed if it is empty, the deepest first. The folders go while the partition is
//      writable, as the system partition's have to
//   6. restoreReadOnly
//   7. runs the check again, for the state it answers
// A move or a path that cannot be done does not end the fix: the folder or the path is in `left`, and the install
// log says why. Each move and each path that is removed is a line in the install log.
bool fixCheck(const Check& check, const CheckContext& context, const std::string& token, FixOutcome* out,
              const char** code, std::string* message);

// ---- the parts, for the checks and their tests ---------------------------------------------------------------------

// What a listing never enters, and a fix never removes.
struct Fence {
    std::set<std::string> owned;      // every path an installed package's record has: files, links, folders
    std::vector<std::string> keep;    // the agent's own folders: nothing in them or below them
    std::set<std::string> parked;     // folders a check has parked: neither they nor anything in them
};
// The fence of the head unit as it is now. False with *error if the installed records cannot be read.
bool fenceOf(const CheckContext& context, Fence* out, std::string* error);
bool fenced(const Fence& fence, const std::string& path);
// True if a path the fence holds as owned is `dir` or inside it.
bool ownsUnder(const Fence& fence, const std::string& dir);

// What a listing does with a folder it has found `depth` levels below its root, whose file system is
// `folderDevice` where the root's is `rootDevice`.
enum FolderStep {
    kFolderEnter,         // its entries are listed, and its folders looked at the same way
    kFolderLast,          // it is as deep as a listing goes: it is listed only to see that it is empty
    kFolderOtherDevice    // it is on another file system: not entered, and the check has no fix
};
FolderStep folderStep(uint64_t rootDevice, uint64_t folderDevice, unsigned depth);

struct Listing {
    std::vector<CheckEntry> entries;   // sorted by path
    bool tooMany;       // there were more than kCheckMaxEntries
    bool tooDeep;       // a folder kCheckMaxDepth levels below a root holds something
    bool otherDevice;   // a folder is on another file system than its root
    bool badName;       // a path is not text that can be shown: not UTF-8, or with a control character in it
    std::string error;  // not empty: a folder could not be listed, and this is why

    Listing() : tooMany(false), tooDeep(false), otherDevice(false), badName(false) {}
};
// Every entry at and below `roots` that the fence does not hold, as the rules at the top of this file say. A root
// that is not there adds nothing; one that is a link, a file or something else is listed as that and nothing more.
void listBounded(const std::vector<std::string>& roots, const Fence& fence, Listing* out);

// A path as text that can be shown and logged: a byte that is not part of UTF-8 as U+FFFD, and every control
// character as '?'.
std::string shownPath(const std::string& path);

// The restart an installed package or a fix still waits for, kept in the run folder as the file restart-needed:
// none, hmi-restart or required. A power cut clears it, and it is also the restart. raiseRestartMark writes the
// stronger of what is there and `need`, and nothing when that is what is there.
std::string restartMark(const std::string& runDir);
void raiseRestartMark(const std::string& runDir, const std::string& need);

}  // namespace nbt

#endif
