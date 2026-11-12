// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_PROGRAMS_HPP
#define NBT_AGENT_PROGRAMS_HPP

// Which programs are running, told by the file name of a process's executable. Two users: the gate, which names
// the program that opens a serial node (spp_gate.hpp), and the agent, which has to know whether the program that
// answers for a serial service is still there (serial_services.hpp).
//
// A process is its executable: one that a shell runs from a script is the shell, not the script.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <string.h>

#include <string>

namespace nbt {

// The program's name from the path of its executable, as the system reports it: the part after the last '/',
// without a line end. Empty for no path.
inline std::string programName(const char* exePath) {
    if (!exePath) return std::string();
    size_t n = strlen(exePath);
    while (n > 0 && (exePath[n - 1] == '\n' || exePath[n - 1] == '\r' || exePath[n - 1] == ' ')) --n;
    size_t start = n;
    while (start > 0 && exePath[start - 1] != '/') --start;
    return std::string(exePath + start, n - start);
}

// The path of the executable of process `pid`, read from the system's folder of processes; empty if it cannot be
// read. Three places are tried, in this order:
//   <procDir>/<pid>/exefile   a text file that holds the path: the head unit's system
//   <procDir>/<pid>/exe       a link to the executable: Linux, where the bench runs
//   <procDir>/<pid>/as        on the head unit only: the path the process was loaded from, which is what the
//                             process list shows
std::string exePathOf(long pid, const char* procDir = "/proc");

enum ProgramState {
    kProgramRunning,      // a process with that executable is there
    kProgramNotRunning,   // the processes could be listed and named, and none is it
    kProgramUnknown       // the list could not be read, or not one process in it could be named
};

// Whether a process whose executable has the file name `name` is running. With kProgramUnknown nothing is known,
// and a caller must not act as though the program were gone. *unnamed, if given, is how many processes were
// passed over because their executable could not be read: with kProgramNotRunning, one of those could still be
// the program.
ProgramState programState(const std::string& name, const char* procDir = "/proc", unsigned* unnamed = 0);

}  // namespace nbt

#endif
