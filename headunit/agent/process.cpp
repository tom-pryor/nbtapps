// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Runs a hook script: see process.hpp.

#include "process.hpp"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace nbt {

namespace {

// Milliseconds on a clock that does not jump when the head unit's date is set.
long long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Hands on whatever the pipe holds right now. False once the pipe is closed at the other end.
bool drain(int fd, OutputSink* sink) {
    char buffer[4096];
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof buffer);
        if (n > 0) {
            if (sink != 0) sink->output(buffer, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return n < 0;   // EAGAIN: nothing more for now; 0: closed
    }
}

// What the shell runs: $1 is the folder, and what follows it the program and its arguments. The last command is
// for a shell that goes on after an exec that failed.
const char* const kInFolder = "cd \"$1\" 2>/dev/null || exit 126; shift; exec \"$@\"; exit 127";

void closeOnExec(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC); }

// Starts args[0] with `in` as its standard input and `out` as its standard output and error, and no other file of
// ours; in a process group of its own, with no signal blocked and SIGPIPE and SIGHUP at their defaults. The
// child's pid, or -1 with errno set.
pid_t start(char* const* args, char* const* vars, int in, int out) {
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    // What the agent ignores would stay ignored in the child, through every exec and in everything the script
    // starts: a pipeline would not end when its reader does, and nothing would end at a hang-up.
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGHUP);
#ifdef __QNXNTO__
    struct inheritance inherit;
    memset(&inherit, 0, sizeof inherit);
    inherit.flags = SPAWN_SETGROUP | SPAWN_SETSIGMASK | SPAWN_SETSIGDEF;
    inherit.pgroup = SPAWN_NEWPGROUP;
    inherit.sigmask = none;
    inherit.sigdefault = defaults;
    // The child is a new process and gets the files of this list, at these numbers, and no others: the agent's
    // own files (its journal, its links to a phone) are none of the script's business.
    int files[3] = { in, out, out };
    return spawn(args[0], 3, files, &inherit, args, vars);
#else
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t files;
    int e = posix_spawnattr_init(&attr);
    if (e != 0) {
        errno = e;
        return -1;
    }
    e = posix_spawn_file_actions_init(&files);
    if (e != 0) {
        posix_spawnattr_destroy(&attr);
        errno = e;
        return -1;
    }
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    e = posix_spawn_file_actions_adddup2(&files, in, 0);
    if (e == 0) e = posix_spawn_file_actions_adddup2(&files, out, 1);
    if (e == 0) e = posix_spawn_file_actions_adddup2(&files, out, 2);
    // Here the child starts with every file of ours, so the rest are closed one by one. The C library does not
    // mind a number in the list that is not open.
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 34)
    if (e == 0) e = posix_spawn_file_actions_addclosefrom_np(&files, 3);
#else
    long top = sysconf(_SC_OPEN_MAX);
    if (top < 0 || top > 1024) top = 1024;
    for (int fd = 3; e == 0 && fd < top; ++fd) e = posix_spawn_file_actions_addclose(&files, fd);
#endif
    pid_t pid = -1;
    if (e == 0) e = posix_spawn(&pid, args[0], &files, &attr, args, vars);
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    if (e != 0) {
        errno = e;
        return -1;
    }
    return pid;
#endif
}

}  // namespace

bool runProcess(const std::string& shell, const std::vector<std::string>& argv, const std::vector<std::string>& env,
                const std::string& cwd, unsigned timeoutSeconds, OutputSink* sink, ProcessResult* result,
                std::string* error) {
    *result = ProcessResult();
    if (argv.empty()) {
        if (error != 0) *error = "nothing to run";
        return false;
    }
    // The shell takes the folder and the program as they stand, so neither may look like an option to `cd`, or
    // like a command for the shell to find along PATH.
    std::string folder = !cwd.empty() && cwd[0] != '/' ? "./" + cwd : cwd;
    std::string program = argv[0].find('/') == std::string::npos ? "./" + argv[0] : argv[0];
    std::vector<char*> args, vars;
    args.push_back((char*)shell.c_str());
    args.push_back((char*)"-c");
    args.push_back((char*)kInFolder);
    args.push_back((char*)shell.c_str());   // $0: the name in what the shell itself says
    args.push_back((char*)folder.c_str());
    args.push_back((char*)program.c_str());
    for (size_t i = 1; i < argv.size(); ++i) args.push_back((char*)argv[i].c_str());
    args.push_back(0);
    for (size_t i = 0; i < env.size(); ++i) vars.push_back((char*)env[i].c_str());
    vars.push_back(0);

    int null = open("/dev/null", O_RDONLY);
    if (null < 0) {
        if (error != 0) *error = std::string("cannot open /dev/null: ") + strerror(errno);
        return false;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        if (error != 0) *error = std::string("cannot make a pipe: ") + strerror(errno);
        close(null);
        return false;
    }
    // Another thread may start a process of its own at any moment, and that one must not get our end of the pipe.
    closeOnExec(fds[0]);
#ifndef __QNXNTO__
    // The two that are handed on arrive in the child as copies, which this does not touch.
    closeOnExec(fds[1]);
    closeOnExec(null);
#endif

    pid_t pid = start(&args[0], &vars[0], null, fds[1]);
    int startError = errno;
    close(null);
    if (pid < 0) {
        if (error != 0) *error = "cannot start " + shell + ": " + strerror(startError);
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);

    long long deadline = nowMs() + (long long)timeoutSeconds * 1000;
    bool pipeOpen = true;
    int status = 0;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0 && errno != EINTR) {
            status = 0;
            break;
        }
        long long left = deadline - nowMs();
        if (left <= 0) {
            result->timedOut = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            break;
        }
        // The end of the child is noticed by asking, not by a signal. While its output pipe is open, the pipe
        // closing wakes us, so asking ten times a second is enough; once it has closed the child is ending, or
        // has gone quiet on purpose, and we ask every few milliseconds.
        long long wait = pipeOpen ? 100 : 5;
        if (wait > left) wait = left;
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = (long)(wait * 1000);
        if (pipeOpen) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(fds[0], &readable);
            if (select(fds[0] + 1, &readable, 0, 0, &tv) > 0) pipeOpen = drain(fds[0], sink);
        } else {
            select(0, 0, 0, 0, &tv);
        }
        if (sink != 0) sink->idle();
    }
    // What the child wrote before it ended is in the pipe already. Do not wait for the pipe to close: a process the
    // script left behind may hold it open for as long as it lives.
    if (pipeOpen) drain(fds[0], sink);
    close(fds[0]);

    if (!result->timedOut) {
        if (WIFEXITED(status)) {
            result->exited = true;
            result->exitCode = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result->signal = WTERMSIG(status);
        }
    }
    return true;
}

}  // namespace nbt
