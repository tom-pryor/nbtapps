// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// A stand-in for QNX's pty driver, devc-pty, in the relay's tests (run_relay.py), which copy it to a folder of their
// own under the name devc-pty and give the relay its path through NBT_SSHRELAY_TEST_PTY_DRIVER. Linux only.
//
//   devc-pty ARGS...
//
// In the forms a driver may have: it runs on under a new pid, as a process of its own (it forks, and the child leads
// a session of its own while the first process exits with 0); or the first process is the driver and keeps its pid
// (QNX's devc-pty does that, and tells its starter it has ended, which nothing on Linux can; here the first process
// just runs on). Files beside its own path P:
//   P.mode          read at its start, one word: run (the default: forks), keep (does not fork: the first process is
//                   the driver), none (exits with nothing left running), late (the first process waits 0.5 s before
//                   it forks), linger (the first process stays 0.5 s after it forks, beside the driver, and exits
//                   with 0 then), die (the driver ends by itself after 1 s), stubborn (the driver ignores SIGTERM)
//   P.pid-<pid>     made by the driver, holding its arguments; removed when it ends by SIGTERM or by itself
//   P.log           a line for each event: "start <pid> <args>", "TERM <pid>", "TERM-ignored <pid>", "gone <pid>",
//                   "none <pid>"
// The driver ends by itself after 300 s whatever its mode, so that a test cannot leave it for long.

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>

namespace {

volatile sig_atomic_t gTerm = 0;

void onTerm(int) { gTerm = 1; }

void append(const std::string& path, const std::string& line) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    std::string text = line + "\n";
    ssize_t ignored = write(fd, text.data(), text.size());
    (void)ignored;
    close(fd);
}

std::string readWord(const std::string& path) {
    char text[64];
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return std::string();
    ssize_t n = read(fd, text, sizeof text - 1);
    close(fd);
    if (n <= 0) return std::string();
    std::string word(text, (size_t)n);
    while (!word.empty() && (word[word.size() - 1] == '\n' || word[word.size() - 1] == ' ')) word.erase(word.size() - 1);
    return word;
}

void nap(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

}  // namespace

int main(int argc, char** argv) {
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return 2;
    std::string path(self, (size_t)n);
    std::string mode = readWord(path + ".mode");
    std::string args;
    for (int i = 1; i < argc; ++i) args += std::string(i > 1 ? " " : "") + argv[i];
    char pid[32];
    snprintf(pid, sizeof pid, "%ld", (long)getpid());
    if (mode == "none") {
        append(path + ".log", std::string("none ") + pid);
        return 0;
    }
    if (mode == "late") nap(500);
    if (mode != "keep") {
        pid_t child = fork();
        if (child < 0) return 1;
        if (child > 0) {
            if (mode == "linger") nap(500);
            return 0;
        }
        setsid();
    }
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
        if (null > 2) close(null);
    }
    snprintf(pid, sizeof pid, "%ld", (long)getpid());
    std::string marker = path + ".pid-" + pid;
    int fd = open(marker.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        ssize_t ignored = write(fd, args.data(), args.size());
        (void)ignored;
        close(fd);
    }
    append(path + ".log", std::string("start ") + pid + " " + args);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = onTerm;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, 0);
    long lived = 0;
    for (;;) {
        nap(20);
        lived += 20;
        if (gTerm) {
            gTerm = 0;
            if (mode == "stubborn") {
                append(path + ".log", std::string("TERM-ignored ") + pid);
                continue;
            }
            unlink(marker.c_str());
            append(path + ".log", std::string("TERM ") + pid);
            return 0;
        }
        if ((mode == "die" && lived >= 1000) || lived >= 300000) {
            unlink(marker.c_str());
            append(path + ".log", std::string("gone ") + pid);
            return 0;
        }
    }
}
