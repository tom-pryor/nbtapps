// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBTAPP_AGENT_START_HPP
#define NBTAPP_AGENT_START_HPP

// Starting the agent with the head unit, and by hand from the plugin's menu.
//
// Nothing of the head unit's own starts a program from the partition the agent is on. The HMI loads this plugin
// each time it starts, so the plugin is what starts the agent's launcher: the starter, agent/nbtstart.sh, which
// is in the agent's folder as `launcher` and runs the launcher of the version it chooses (agent/nbtlaunch.sh).
// The plugin starts it from create(), as a process of its own that the HMI does not wait for and hears nothing
// more of.
//
// It starts the launcher only if all of this holds, asked in this order:
//
//   - <the agent's folder>/autostart is there. Only a person makes that file (hu_install.sh says how), so a head
//     unit starts no agent by itself until somebody has asked for that;
//   - the launcher is there;
//   - no launcher is running. A launcher keeps its process id in a file on the RAM disk for as long as it runs,
//     the time it waits before its first start included. The HMI starts more than once while a head unit is on
//     (the agent restarts it after it has installed a plugin), and each time the plugin finds the launcher of
//     the first time and leaves it alone;
//   - fewer than kMostStarts starts with the head unit are on record. See below;
//   - this start can be put on record.
//
// The record of starts. The agent puts a gate in front of the car's Bluetooth serial node and talks to the car's
// Bluetooth service. If an agent ever brought a head unit down, it would do so again at the next start, with
// nobody at a console to stop it. So each start with the head unit is written down before it is made, as a line
// with the time in <the agent's folder>/autostart.boots, and the agent removes that file once it has run for
// five minutes, or has been stopped (agent/nbtagent.cpp). The file therefore holds only starts that no agent
// outlasted by that long. With kMostStarts of them on record the plugin starts nothing, and the log says so. A
// start counts for kWindowS seconds; a head unit's clock is often not set when it starts, and then no start can
// be told to be old: with the clock not set now, or for a start written down while it was not, the start counts
// whatever its time. To have the agent started again, remove the file; or start the launcher by hand and leave
// the agent running for five minutes.
//
// That is one small file written to flash for each start with the head unit, and removed five minutes later. A
// head unit with no `autostart` file is written to by none of this.
//
// The start by hand. The plugin's menu has a row that starts the agent (src/phone_screens.hpp, StartScreen): the
// way to the first start of an agent on a head unit with no console, after which a phone can switch `autostart`
// on. It asks for the launcher and for no launcher that is running, and for nothing else:
//
//   - not for `autostart`: a person asks, there and then;
//   - not for the record of starts, which it neither reads nor writes. The record keeps a head unit from starting
//     a bad agent at every start with nobody there; a start by hand is one start with somebody there, and the
//     next start of the head unit still finds what the record says. An agent started by hand that runs for five
//     minutes removes the record, as any agent does, so this is also how the record is cleared without a console.
//
// The kill switch keeps the plugin from making its menu at all, so there is no row to press while it is there.
//
// What the launcher is started with: the shell and the launcher's path, the three standard files on /dev/null
// and no other file of the HMI's, a process group of its own, no signal blocked or ignored, and an environment
// that is written out here and is not the HMI's: the PATH and library path that a shell on a head unit has,
// which is what the agent and the scripts of its packages have been run with by hand, and NBT_LAUNCH_DELAY, so
// that the launcher waits kDelayS seconds before it starts the agent and the agent comes after the programs it
// talks to. A start by hand has the launcher wait for nothing: the HMI is up, and so is what the agent talks to.
//
// The functions here that decide are plain functions of what they are given, and the ones that touch files and
// start the process take their paths from the caller, so all of it is tested on a build host with the real
// launcher (tests/agent/test_agent_start.cpp).
//
// None of this has run on a car. What is not known until it has:
//   - whether a process that a plugin starts goes on when the HMI is restarted. The agent restarts the HMI in
//     the middle of a job;
//   - which priority and scheduling partition the launcher, and so the agent, gets from the HMI's thread;
//   - whether kDelayS is long enough for the car's Bluetooth service to be there. If it is not, the agent's
//     serial listener gives up and sets itself up again half a minute later (agent/serial_link.hpp).
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "../agent/agent_home.hpp"

namespace nbt {
namespace agentstart {

// ---- the record of starts -----------------------------------------------------------------------------------------

const int64_t kClockSetFrom = 1577836800;   // 2020-01-01 UTC: an earlier time is a clock that has not been set
const int64_t kWindowS = 15 * 60;           // how long a start counts, where its age can be told
const int kMostStarts = 3;                  // with this many on record, the next start is not made
const size_t kMostKept = 8;                 // lines the file holds at most
const int kDelayS = 15;                     // what the launcher waits before its first start with the head unit

inline bool clockSet(int64_t t) { return t >= kClockSetFrom; }

// The times in the file's text: one to a line, digits only. A line that is anything else is passed over.
inline std::vector<int64_t> parseStarts(const std::string& text) {
    std::vector<int64_t> starts;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        bool digits = end > at && end - at <= 12;
        int64_t value = 0;
        for (size_t i = at; digits && i < end; ++i) {
            if (text[i] < '0' || text[i] > '9') digits = false;
            else value = value * 10 + (text[i] - '0');
        }
        if (digits) starts.push_back(value);
        at = end + 1;
    }
    return starts;
}

inline std::string formatStarts(const std::vector<int64_t>& starts) {
    std::string text;
    for (size_t i = 0; i < starts.size(); ++i) {
        char line[32];
        snprintf(line, sizeof line, "%lld\n", (long long)starts[i]);
        text += line;
    }
    return text;
}

// The starts on record that count at `now`, oldest first, the last kMostKept of them. One from a clock that was
// later put right, and so lies ahead of `now`, does not count.
inline std::vector<int64_t> counted(const std::vector<int64_t>& starts, int64_t now) {
    std::vector<int64_t> out;
    for (size_t i = 0; i < starts.size(); ++i) {
        int64_t t = starts[i];
        if (!clockSet(now) || !clockSet(t) || (t <= now && now - t < kWindowS)) out.push_back(t);
    }
    if (out.size() > kMostKept) out.erase(out.begin(), out.end() - kMostKept);
    return out;
}

inline bool allowed(size_t onRecord) { return onRecord < (size_t)kMostStarts; }

// ---- files and the process ----------------------------------------------------------------------------------------

struct Paths {
    std::string home;    // the agent's folder, where hu_install.sh puts the starter as `launcher`
    std::string lock;    // where a running starter keeps its process id (nbtstart.sh)
    std::string shell;   // what runs the launcher

    Paths() : home("/var/opt/nbtapp/agent"), lock("/tmp/nbtapp/log/launcher.pid"), shell("/bin/ksh") {}
};

// What a shell on a head unit has, and the launcher's wait: kDelayS with the head unit, 0 by hand.
inline std::vector<std::string> headUnitEnvironment(int delayS) {
    std::vector<std::string> vars;
    vars.push_back("PATH=/bin:/opt/os/bin:/opt/sys/bin:/opt/core/bin:/opt/car/bin");
    vars.push_back("LD_LIBRARY_PATH=/lib:/usr/lib:/opt/os/lib:/opt/sys/lib:/opt/core/lib:/opt/car/lib");
    vars.push_back("HOME=/tmp");
    vars.push_back("SHELL=/bin/ksh");
    char delay[32];
    snprintf(delay, sizeof delay, "NBT_LAUNCH_DELAY=%d", delayS);
    vars.push_back(delay);
    return vars;
}

inline bool isFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// The start of a small file; empty if it cannot be read.
inline std::string readSmall(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return std::string();
    char buffer[512];
    ssize_t n = read(fd, buffer, sizeof buffer);
    close(fd);
    return n > 0 ? std::string(buffer, (size_t)n) : std::string();
}

// Writes the file anew and has it on the disk before it returns: a record that a power cut can lose is none.
inline bool writeWhole(const std::string& path, const std::string& text) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();
    int e = errno;
    if (ok && fsync(fd) != 0) {
        ok = false;
        e = errno;
    }
    close(fd);
    errno = e;
    return ok;
}

// The process id that the launcher's file holds; 0 if there is no file or it holds none.
inline long holderOf(const std::string& lock) {
    std::string text = readSmall(lock);
    long pid = 0;
    size_t i = 0;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9' && i < 10; ++i) pid = pid * 10 + (text[i] - '0');
    if (i == 0 || (i < text.size() && text[i] != '\n')) return 0;
    return pid;
}

inline bool isRunning(long pid) { return pid > 0 && (kill((pid_t)pid, 0) == 0 || errno == EPERM); }

// Starts the launcher as the top of this file says. Its process id; -1, with *error, if it could not be started.
inline long spawnLauncher(const Paths& paths, const std::vector<std::string>& environment, std::string* error) {
    const std::string launcher = paths.home + "/launcher";
    std::vector<char*> vars;
    for (size_t i = 0; i < environment.size(); ++i) vars.push_back(const_cast<char*>(environment[i].c_str()));
    vars.push_back(0);
    char* args[] = { const_cast<char*>(paths.shell.c_str()), const_cast<char*>(launcher.c_str()), 0 };

    int null = open("/dev/null", O_RDWR);
    if (null < 0) {
        *error = std::string("cannot open /dev/null: ") + strerror(errno);
        return -1;
    }
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    // What the HMI ignores would stay ignored in the launcher and in the agent, which is stopped by a signal.
    const int signals[] = { SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGALRM, SIGTERM, SIGUSR1, SIGUSR2, SIGCHLD };
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; ++i) sigaddset(&defaults, signals[i]);

    pid_t pid = -1;
    int e = 0;
#ifdef __QNXNTO__
    struct inheritance inherit;
    memset(&inherit, 0, sizeof inherit);
    // SPAWN_NOZOMBIE: the HMI does not wait for the launcher, and is not to be left with what remains of it.
    inherit.flags = SPAWN_SETGROUP | SPAWN_SETSIGMASK | SPAWN_SETSIGDEF | SPAWN_NOZOMBIE;
    inherit.pgroup = SPAWN_NEWPGROUP;
    inherit.sigmask = none;
    inherit.sigdefault = defaults;
    // The child gets the files of this list, at these numbers, and no others.
    int files[3] = { null, null, null };
    pid = spawn(args[0], 3, files, &inherit, args, &vars[0]);
    if (pid < 0) e = errno;
#else
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t files;
    e = posix_spawnattr_init(&attr);
    if (e == 0) {
        e = posix_spawn_file_actions_init(&files);
        if (e == 0) {
            posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
            posix_spawnattr_setpgroup(&attr, 0);
            posix_spawnattr_setsigmask(&attr, &none);
            posix_spawnattr_setsigdefault(&attr, &defaults);
            for (int fd = 0; e == 0 && fd < 3; ++fd) e = posix_spawn_file_actions_adddup2(&files, null, fd);
            // A build host's child starts with every file of the parent's, so the rest are closed one by one.
            long top = sysconf(_SC_OPEN_MAX);
            if (top < 0 || top > 1024) top = 1024;
            for (int fd = 3; e == 0 && fd < top; ++fd) e = posix_spawn_file_actions_addclose(&files, fd);
            if (e == 0) e = posix_spawn(&pid, args[0], &files, &attr, args, &vars[0]);
            posix_spawn_file_actions_destroy(&files);
        }
        posix_spawnattr_destroy(&attr);
    }
    if (e != 0) pid = -1;
#endif
    close(null);
    if (pid < 0) {
        *error = "cannot start " + paths.shell + " " + launcher + ": " + strerror(e);
        return -1;
    }
    return (long)pid;
}

// ---- the start ----------------------------------------------------------------------------------------------------

enum Decision {
    kStarted,         // the launcher was started
    kNotAsked,        // no autostart file (with the head unit only)
    kNoLauncher,      // no launcher in the agent's folder
    kRunning,         // a launcher is running
    kTooManyStarts,   // the record holds kMostStarts starts (with the head unit only)
    kNotCounted,      // this start could not be put on record (with the head unit only)
    kNotStarted       // the launcher could not be started
};

struct Outcome {
    Decision decision;
    std::string line;   // for the log
    long pid;           // the launcher's, when kStarted

    Outcome() : decision(kNotAsked), pid(-1) {}
};

inline Outcome outcome(Decision decision, const std::string& line, long pid = -1) {
    Outcome o;
    o.decision = decision;
    o.line = line;
    o.pid = pid;
    return o;
}

inline std::string numberText(long long n) {
    char text[32];
    snprintf(text, sizeof text, "%lld", n);
    return text;
}

// What a running launcher is, as a line for the log; empty if none is running.
inline std::string runningLauncher(const Paths& paths) {
    long holder = holderOf(paths.lock);
    if (!isRunning(holder)) return std::string();
    return "a launcher is running (process " + numberText(holder) + ", says " + paths.lock + "): left as it is";
}

// Starts the launcher if the head unit is to start the agent by itself and nothing speaks against it. `now` is
// the clock's time in seconds; `environment` is what the launcher gets.
inline Outcome startWithHeadUnit(const Paths& paths, int64_t now, const std::vector<std::string>& environment) {
    const std::string wanted = paths.home + "/" + kAutostartSwitch;
    const std::string launcher = paths.home + "/launcher";
    const std::string record = paths.home + "/" + kAutostartBoots;

    if (!isFile(wanted)) return outcome(kNotAsked, "no " + wanted + ": the agent is not started with the head unit");
    if (!isFile(launcher)) return outcome(kNoLauncher, launcher + " is not there: nothing to start");
    const std::string running = runningLauncher(paths);
    if (!running.empty()) return outcome(kRunning, running);

    std::vector<int64_t> starts = counted(parseStarts(readSmall(record)), now);
    if (!allowed(starts.size())) {
        return outcome(kTooManyStarts,
                       "NOT started: " + numberText((long long)starts.size()) + " starts with the head unit are on record in " +
                           record + ", and no agent ran for five minutes after any of them. Remove that file to " +
                           "have the agent started with the head unit again");
    }
    const std::vector<int64_t> before = starts;
    starts.push_back(now);
    if (!writeWhole(record, formatStarts(starts))) {
        return outcome(kNotCounted, "NOT started: this start cannot be put on record in " + record + ": " + strerror(errno));
    }

    std::string why;
    long pid = spawnLauncher(paths, environment, &why);
    if (pid < 0) {
        // Nothing was started, so nothing is on record for it.
        if (before.empty()) unlink(record.c_str());
        else writeWhole(record, formatStarts(before));
        return outcome(kNotStarted, "NOT started: " + why);
    }
    return outcome(kStarted,
                   "started the launcher (process " + numberText(pid) + "); on record in " + record + " as start " +
                       numberText((long long)starts.size()) + " of the " + numberText(kMostStarts) +
                       " that may go by with no agent that runs for five minutes",
                   pid);
}

// Starts the launcher because a person asked for it in the plugin's menu: with no autostart file asked for, and
// the record of starts neither read nor written (the top of this file says why).
inline Outcome startByHand(const Paths& paths, const std::vector<std::string>& environment) {
    const std::string launcher = paths.home + "/launcher";
    if (!isFile(launcher)) return outcome(kNoLauncher, launcher + " is not there: nothing to start");
    const std::string running = runningLauncher(paths);
    if (!running.empty()) return outcome(kRunning, running);
    std::string why;
    long pid = spawnLauncher(paths, environment, &why);
    if (pid < 0) return outcome(kNotStarted, "NOT started by hand: " + why);
    return outcome(kStarted, "started the launcher by hand (process " + numberText(pid) + ")", pid);
}

}  // namespace agentstart
}  // namespace nbt

#endif
