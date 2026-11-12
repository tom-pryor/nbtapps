// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_PROCESS_HPP
#define NBT_AGENT_PROCESS_HPP

// Runs a package's hook script: a child process with its own environment and working folder, its output handed
// back as it arrives, and a hard stop at a timeout.
//
// The child is started with spawn() on the head unit and posix_spawn() elsewhere, never with fork(): on QNX
// Neutrino 6.5 fork() fails with ENOSYS in a process that has more than one thread, and the car's own service
// library starts a thread in whatever process loads it. Neither call can set the child's working folder, so the
// child is a shell that goes to the folder and then becomes the program.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>

#include <string>
#include <vector>

namespace nbt {

class OutputSink {
public:
    virtual ~OutputSink() {}
    // Bytes the child wrote to its standard output or error, in the pieces they arrived in.
    virtual void output(const char* data, size_t len) = 0;
    // Called about ten times a second while the child runs, whether or not it wrote anything: the caller's chance
    // to do other work on the thread that waits here.
    virtual void idle() {}
};

struct ProcessResult {
    bool timedOut;   // killed at the timeout
    bool exited;     // ended by itself; exitCode is then its exit code
    int exitCode;
    int signal;      // the signal that ended it, when it neither exited nor timed out

    ProcessResult() : timedOut(false), exited(false), exitCode(0), signal(0) {}
};

// Runs argv[0] with the given arguments, in the folder `cwd`. `shell` is the shell that takes it there: /bin/sh, or
// /bin/ksh on a head unit. A program named without a '/' is looked for in `cwd`, not along PATH.
// `env` holds "NAME=value" strings and is the whole environment, but for what the shell adds on its way (PWD). The
// child gets its own process group; at `timeoutSeconds` the whole group is killed. Standard input is /dev/null, no
// other file of the caller is open in the child, and SIGPIPE and SIGHUP do in the child what they do by default,
// whatever the caller has made of them.
// Exit code 126: there is no folder `cwd`, or the program is there and cannot be run. 127: there is no such program.
// Returns once the child itself has ended: a process it left running in the background does not hold this up, and
// output that such a process writes later is lost.
// False with *error set if the child could not be started at all, which includes a `shell` that cannot be run.
bool runProcess(const std::string& shell, const std::vector<std::string>& argv, const std::vector<std::string>& env,
                const std::string& cwd, unsigned timeoutSeconds, OutputSink* sink, ProcessResult* result,
                std::string* error);

}  // namespace nbt

#endif
