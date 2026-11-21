// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_SSH_RELAY_CHILD_HPP
#define NBT_SSH_RELAY_CHILD_HPP

// Starting the programs the relay runs: the server for each connection, and the key tool.
//
// The child gets `in`, `out` and `err` as its standard input, output and error and no other file of the relay's;
// no signal blocked, and SIGPIPE, SIGHUP, SIGINT, SIGTERM and SIGCHLD at their defaults (the relay ignores
// SIGPIPE, and what a process ignores stays ignored through exec and in everything it starts); the environment
// `env` and nothing of the relay's own. With `newSession` it leads a session, and so a process group, of its own.
//
//   QNX    spawn(), with a list of the three files and SPAWN_SETSID: the way the agent starts its programs
//          (agent/process.cpp), where the child gets the files of the list and no others. No fork.
//   Linux  fork(), then in the child setsid(), the three files put in place, every other one closed, the
//          signals set, and execve(). The relay has one thread, so the child is a plain copy of it. A pipe that
//          the exec closes tells the relay whether the exec failed, so that, as with spawn(), a program that cannot
//          be started is an error here and not a child that ends at once.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <signal.h>
#include <sys/types.h>

#include <string>
#include <vector>

namespace sshrelay {

// The child's pid, or -1 with *error set. argv[0] is the program's path.
pid_t startChild(const std::vector<std::string>& argv, const std::vector<std::string>& env, int in, int out, int err,
                 bool newSession, std::string* error);

// Runs a program to its end, its standard output collected (up to 64 KiB) and its standard error the relay's:
// the key tool, as the relay starts. It is ended with SIGKILL if it has not ended in `timeoutMs`, or once *stop
// is set (the relay's SIGTERM). True if it exited with 0.
bool runToEnd(const std::vector<std::string>& argv, const std::vector<std::string>& env, long timeoutMs,
              const volatile sig_atomic_t* stop, std::string* output, std::string* error);

}  // namespace sshrelay

#endif
