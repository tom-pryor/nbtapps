// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_SSH_RELAY_PROCESSES_HPP
#define NBT_SSH_RELAY_PROCESSES_HPP

// The process list, as the relay walks it to find what a server left behind: each process's parent, process group
// and session. QNX has no call that signals a session, so the relay signals each process group whose session is one
// of a server's (relay.hpp, "Ends"). Also each process's start time, which tells a process from a later one given
// the same pid (relay.hpp, "The state file"), and whether it has ended and waits to be collected.
//
//   QNX    /proc/<pid>/as, devctl DCMD_PROC_INFO: the parent, group, session, start time (ns) and flags of one
//          process
//   Linux  /proc/<pid>/stat: the fields after the program's name are its state, parent, group and session, and the
//          20th after the name its start time (clock ticks since boot)
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <string>
#include <vector>

namespace sshrelay {

struct ProcessEntry {
    long pid, parent, group, session;
    unsigned long long start;   // when it started, in the system's own unit; 0 when not known
    bool zombie;                // it has ended and waits for its parent to collect it
    ProcessEntry() : pid(0), parent(0), group(0), session(0), start(0), zombie(false) {}
};

// Every process that can be read, in the order of the folder. A process that ends while the list is made is left
// out. False if the folder cannot be read at all.
bool listProcesses(std::vector<ProcessEntry>* out, const char* procDir = "/proc");

// One process. False if it is not there or cannot be read.
bool readProcess(long pid, ProcessEntry* out, const char* procDir = "/proc");

// The children of `parent` that lead a session of their own: what a server starts for a login on a pty.
std::vector<long> sessionLeadersStartedBy(const std::vector<ProcessEntry>& list, long parent);

// The process groups, each once, of the processes whose session is one of `sessions`. Never group 0 or 1.
std::vector<long> groupsInSessions(const std::vector<ProcessEntry>& list, const std::vector<long>& sessions);

// Linux only, for the walk and its test: the fields of one /proc/<pid>/stat text. False for a text that is not one;
// a text cut short after the session gives a start of 0.
bool parseStat(const std::string& text, ProcessEntry* out);

#ifdef NBT_SSHRELAY_TEST_HOOK
// Tests only: added to every start time that is not 0, so that the relay's tests on this machine meet start times
// of the size QNX gives (nanoseconds, 18 digits and more).
void setStartOffset(unsigned long long offset);
#endif

}  // namespace sshrelay

#endif
