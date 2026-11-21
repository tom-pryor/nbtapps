// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_PROGRAMS_HPP
#define NBT_AGENT_PROGRAMS_HPP

// Which programs are running, told by the file name of a process's executable. Four users: the gate, which names
// the program that opens a serial node (spp_gate.hpp), the agent, which has to know whether the program that
// answers for a serial service is still there (serial_services.hpp), the checks, which stop the programs of an app
// before its files are removed (checks.hpp): those are found by the folder their executable is in; and the agent
// again, at its start, for the program of a channel an earlier agent started (runsProgram).
//
// A process is its executable: one that a shell runs from a script is the shell, not the script.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <string.h>

#include <string>
#include <vector>

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

// The path of the executable of process `pid`, read from the system's folder of processes, from the root: one that
// the system reports without a leading '/' is given one, unless it begins with "./" or "../" (a program started by
// a relative path, from a folder not known here). Empty if it cannot be read. Three places are tried, in this
// order:
//   <procDir>/<pid>/exefile   a text file that holds the path, where the system has one; the head unit's QNX 6.5
//                             has none
//   <procDir>/<pid>/exe       a link to the executable: Linux, where the bench runs
//   <procDir>/<pid>/as        QNX only, and on the head unit the one there is: the path the process was started by,
//                             which is what the process list shows. QNX gives it as it was given, without its
//                             leading '/' ("bin/sh"), and does not follow links in it
std::string exePathOf(long pid, const char* procDir = "/proc");

// Whether process `pid` runs the program in the file `programPath` (absolute), which an earlier agent started in a
// process group of its own (channels.hpp): its executable has the file's name; or the file is a script, whose
// process is its interpreter, and the process leads its own process group, and its executable has the file name of
// the interpreter on the script's first line (`#!/bin/sh`), or of the file that path leads to through links. Another
// process given the same pid since passes only if it looks like that too: for a script, a shell that leads a group
// of its own.
bool runsProgram(long pid, const std::string& programPath, const char* procDir = "/proc");

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

// A process, and the path of its executable as exePathOf gives it, without a line end.
struct RunningProgram {
    long pid;
    std::string path;
    RunningProgram() : pid(0) {}
};

// The processes whose executable is in the folder `dir` or below it, by path: `dir` as it is given, or the path the
// links in it lead to, since the system may report either. kProgramRunning with them in *out if there is one;
// kProgramNotRunning if the processes could be listed and named and none is; kProgramUnknown, as programState
// says, when nothing is known, and then a caller must not act as though no such process ran. *unnamed is as
// there.
ProgramState programsUnder(const std::string& dir, std::vector<RunningProgram>* out, const char* procDir = "/proc",
                           unsigned* unnamed = 0);

}  // namespace nbt

#endif
