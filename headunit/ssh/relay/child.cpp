// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Starting the relay's programs: see child.hpp.

#include "child.hpp"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __QNXNTO__
#include <spawn.h>
#endif

namespace sshrelay {

namespace {

const int kDefaultSignals[] = { SIGPIPE, SIGHUP, SIGINT, SIGTERM, SIGCHLD };

long long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace

pid_t startChild(const std::vector<std::string>& argv, const std::vector<std::string>& env, int in, int out, int err,
                 bool newSession, std::string* error) {
    if (argv.empty()) {
        *error = "nothing to start";
        return -1;
    }
    std::vector<char*> args, vars;
    for (size_t i = 0; i < argv.size(); ++i) args.push_back((char*)argv[i].c_str());
    args.push_back(0);
    for (size_t i = 0; i < env.size(); ++i) vars.push_back((char*)env[i].c_str());
    vars.push_back(0);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (size_t i = 0; i < sizeof kDefaultSignals / sizeof kDefaultSignals[0]; ++i) {
        sigaddset(&defaults, kDefaultSignals[i]);
    }
#ifdef __QNXNTO__
    struct inheritance inherit;
    memset(&inherit, 0, sizeof inherit);
    inherit.flags = SPAWN_SETSIGMASK | SPAWN_SETSIGDEF;
    if (newSession) inherit.flags |= SPAWN_SETSID;
    inherit.sigmask = none;
    inherit.sigdefault = defaults;
    int files[3] = { in, out, err };
    pid_t pid = spawn(args[0], 3, files, &inherit, &args[0], &vars[0]);
    if (pid < 0) *error = std::string("cannot start ") + argv[0] + ": " + strerror(errno);
    return pid;
#else
    long top = sysconf(_SC_OPEN_MAX);
    if (top < 0 || top > 4096) top = 4096;
    // A pipe that closes at the child's exec, or carries its errno when the exec fails: so a program that cannot
    // be started is known here, as spawn() tells it on QNX. The wait is the child's way to its exec, no longer.
    int report[2];
    if (pipe(report) != 0) {
        *error = std::string("cannot make a pipe: ") + strerror(errno);
        return -1;
    }
    fcntl(report[0], F_SETFD, FD_CLOEXEC);
    fcntl(report[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        *error = std::string("cannot fork: ") + strerror(errno);
        close(report[0]);
        close(report[1]);
        return -1;
    }
    if (pid == 0) {
        // The child: only calls that are safe after a fork, until execve.
        if (newSession) setsid();
        // Moved above 2 first, so that putting one in place cannot close another that is still to be moved.
        int a = fcntl(in, F_DUPFD, 3), b = fcntl(out, F_DUPFD, 3), c = fcntl(err, F_DUPFD, 3);
        if (a >= 0 && b >= 0 && c >= 0 && dup2(a, 0) >= 0 && dup2(b, 1) >= 0 && dup2(c, 2) >= 0) {
            for (int fd = 3; fd < top; ++fd) {
                if (fd != report[1]) close(fd);
            }
            struct sigaction dfl;
            memset(&dfl, 0, sizeof dfl);
            dfl.sa_handler = SIG_DFL;
            sigemptyset(&dfl.sa_mask);
            for (size_t i = 0; i < sizeof kDefaultSignals / sizeof kDefaultSignals[0]; ++i) {
                sigaction(kDefaultSignals[i], &dfl, 0);
            }
            sigprocmask(SIG_SETMASK, &none, 0);
            execve(args[0], &args[0], &vars[0]);
        }
        int e = errno;
        ssize_t ignored = write(report[1], &e, sizeof e);
        (void)ignored;
        _exit(127);
    }
    close(report[1]);
    int e = 0;
    ssize_t n;
    while ((n = read(report[0], &e, sizeof e)) < 0 && errno == EINTR) {
    }
    close(report[0]);
    if (n == (ssize_t)sizeof e) {
        while (waitpid(pid, 0, 0) < 0 && errno == EINTR) {
        }
        *error = std::string("cannot start ") + argv[0] + ": " + strerror(e);
        return -1;
    }
    return pid;
#endif
}

bool runToEnd(const std::vector<std::string>& argv, const std::vector<std::string>& env, long timeoutMs,
              const volatile sig_atomic_t* stop, std::string* output, std::string* error) {
    output->clear();
    int null = open("/dev/null", O_RDWR);
    if (null < 0) {
        *error = std::string("cannot open /dev/null: ") + strerror(errno);
        return false;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        *error = std::string("cannot make a pipe: ") + strerror(errno);
        close(null);
        return false;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    pid_t pid = startChild(argv, env, null, fds[1], 2, false, error);
    close(null);
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return false;
    }
    long long deadline = nowMs() + timeoutMs;
    bool timedOut = false, stopped = false;
    for (;;) {
        long long left = deadline - nowMs();
        if (left <= 0) {
            timedOut = true;
            break;
        }
        if (*stop) {
            stopped = true;
            break;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fds[0], &readable);
        struct timeval tv;
        tv.tv_sec = (long)(left / 1000);
        tv.tv_usec = (long)(left % 1000) * 1000;
        int r = select(fds[0] + 1, &readable, 0, 0, &tv);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) break;
        if (r == 0) continue;
        char buffer[4096];
        ssize_t n = read(fds[0], buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (output->size() < 65536) output->append(buffer, (size_t)n);
    }
    close(fds[0]);
    if (timedOut || stopped) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (timedOut || stopped) {
        *error = argv[0] + (stopped ? " was ended with the relay" : " did not end in time");
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        char text[64];
        if (WIFEXITED(status)) snprintf(text, sizeof text, "exited with %d", WEXITSTATUS(status));
        else snprintf(text, sizeof text, "ended by signal %d", WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        *error = argv[0] + " " + argv[1 < argv.size() ? 1 : 0] + " " + text;
        return false;
    }
    return true;
}

}  // namespace sshrelay
