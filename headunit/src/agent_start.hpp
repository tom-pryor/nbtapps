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
// more of. create() runs on the HMI's thread that drives the iDrive screen, so all of the start with the head unit,
// the looks at files, the record written to flash and the start of the process, is done on a thread of its own
// that create() starts and does not wait for (StartThread, at the end of this file). That thread says how it came
// out in the log, and ends.
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
//   - the agent was not stopped since the head unit powered up: no file agent.stopped is on the RAM disk. See
//     below;
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
// head unit with no `autostart` file is written to by none of this. The write, and the wait for it to be on the
// disk, are on the start's own thread, never on the HMI's.
//
// The stop. The plugin's menu has a row that stops the agent (src/phone_screens.hpp, StopScreen), as `nbtctl stop`
// does: both ask the agent for agent.stop, and the agent ends with its launcher and leaves the mark agent.stopped
// in its run folder on the RAM disk (agent/agent_home.hpp). When the HMI starts again while the head unit is on,
// the plugin would start the launcher once more; the mark keeps it from that. It goes when the head unit powers
// down, which empties the RAM disk, or when an agent starts, which removes it: Start the agent in the menu, or the
// launcher started by hand. A start with the head unit that the mark keeps off is not made, so nothing goes on
// record for it. An agent stopped with a signal leaves no mark, and the plugin goes on as before.
//
// The start by hand. The plugin's menu has a row that starts the agent (src/phone_screens.hpp, StartScreen): the
// way to the first start of an agent on a head unit with no console, after which a phone can switch `autostart`
// on. It asks for the launcher and for no launcher that is running, and for nothing else:
//
//   - not for `autostart`: a person asks, there and then;
//   - not for agent.stopped: a person who stopped the agent may start it again, and that agent removes the mark;
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
// If kDelayS is not long enough for the car's Bluetooth service to be there, the agent's serial listener gives up
// and sets itself up again half a minute later (agent/serial_link.hpp).
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "../agent/agent_home.hpp"
#include "call_limit.hpp"

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
    std::string stopped; // the mark an agent that a controller stopped leaves in its run folder (agent_home.hpp)

    Paths()
        : home("/var/opt/nbtapp/agent"), lock("/tmp/nbtapp/log/launcher.pid"), shell("/bin/ksh"),
          stopped(std::string("/tmp/nbtapp/") + kStoppedMark) {}
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

// `limited` below: the call is made on the HMI's thread, and each of its calls that can wait in the kernel has a
// time limit (call_limit.hpp). One that runs out of time fails, and is taken as the same call is when it fails
// otherwise. The start by hand is made on the HMI's thread, and the start with the head unit on a thread of its own,
// with no limit: there a read of the record that ran out of time would be taken for a record with no starts.

inline bool isFile(const std::string& path, bool limited = false) {
    struct stat st;
    if (limited) limitNextCall();
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// The start of a small file; empty if it cannot be read.
inline std::string readSmall(const std::string& path, bool limited = false) {
    if (limited) limitNextCall();
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return std::string();
    char buffer[512];
    if (limited) limitNextCall();
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
inline long holderOf(const std::string& lock, bool limited = false) {
    std::string text = readSmall(lock, limited);
    long pid = 0;
    size_t i = 0;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9' && i < 10; ++i) pid = pid * 10 + (text[i] - '0');
    if (i == 0 || (i < text.size() && text[i] != '\n')) return 0;
    return pid;
}

inline bool isRunning(long pid) { return pid > 0 && (kill((pid_t)pid, 0) == 0 || errno == EPERM); }

// Starts the launcher as the top of this file says. Its process id; -1, with *error, if it could not be started.
inline long spawnLauncher(const Paths& paths, const std::vector<std::string>& environment, std::string* error,
                          bool limited = false) {
    const std::string launcher = paths.home + "/launcher";
    std::vector<char*> vars;
    for (size_t i = 0; i < environment.size(); ++i) vars.push_back(const_cast<char*>(environment[i].c_str()));
    vars.push_back(0);
    char* args[] = { const_cast<char*>(paths.shell.c_str()), const_cast<char*>(launcher.c_str()), 0 };

    if (limited) limitNextCall();
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
    // No time limit here, even on the HMI's thread. spawn() is one message to the process manager, which makes the
    // process while the caller waits; a limit that ran out while it did so could leave a process running that the
    // caller is told was not started: Start the agent would say that the start failed while a launcher runs, and a
    // start with the head unit, were it limited, would be taken off the record.
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
    kStoppedByHand,   // the agent was stopped from the menu or with nbtctl since the head unit powered up (with the
                      // head unit only)
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
inline std::string runningLauncher(const Paths& paths, bool limited = false) {
    long holder = holderOf(paths.lock, limited);
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
    if (isFile(paths.stopped)) {
        return outcome(kStoppedByHand,
                       "NOT started: the agent was stopped from the menu or with nbtctl, says " + paths.stopped +
                           ". Start the agent in the menu starts it again, and so does the head unit once it has powered down");
    }

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
// the record of starts neither read nor written (the top of this file says why). On the HMI's thread, so with the
// time limits above.
inline Outcome startByHand(const Paths& paths, const std::vector<std::string>& environment) {
    const std::string launcher = paths.home + "/launcher";
    if (!isFile(launcher, true)) return outcome(kNoLauncher, launcher + " is not there: nothing to start");
    const std::string running = runningLauncher(paths, true);
    if (!running.empty()) return outcome(kRunning, running);
    std::string why;
    long pid = spawnLauncher(paths, environment, &why, true);
    if (pid < 0) return outcome(kNotStarted, "NOT started by hand: " + why);
    return outcome(kStarted, "started the launcher by hand (process " + numberText(pid) + ")", pid);
}

// ---- the start with the head unit, on a thread of its own ---------------------------------------------------------

// What hears how a start on a thread of its own came out, on that thread. It outlives the start: a start whose owner
// stopped waiting for it still says how it came out when it ends.
class StartReport {
public:
    virtual ~StartReport() {}
    virtual void startedWithHeadUnit(const Outcome& outcome) = 0;
};

// startWithHeadUnit() on a thread of its own, which ends when that has returned and told its report. create() runs
// on the HMI's thread that drives the iDrive screen, and the start writes its record to flash and waits for that
// write to be on the disk: on this thread, the HMI waits for none of it. The thread asks the same things in the
// same order as startWithHeadUnit() does anywhere, and calls nothing of the HU Framework's.
class StartThread {
public:
    // The thread is started; 0, with *error, if it could not be.
    static StartThread* begin(const Paths& paths, int64_t now, const std::vector<std::string>& environment,
                              StartReport* report, std::string* error) {
        StartThread* start = new StartThread(paths, now, environment, report);
        int e = pthread_create(&start->thread_, NULL, &StartThread::threadMain, start);
        if (e != 0) {
            *error = strerror(e);
            delete start;
            return 0;
        }
        return start;
    }

    // Waits for the thread to end, `ms` at most. True if it has, and the object is gone with it. False if it has
    // not: the thread goes on, and the object goes when it ends.
    bool finish(int ms) {
        pthread_mutex_lock(&mu_);
        struct timespec until = deadlineIn(ms);
        while (!done_) {
            if (pthread_cond_timedwait(&cv_, &mu_, &until) == ETIMEDOUT) break;
        }
        if (done_) {
            pthread_mutex_unlock(&mu_);
            pthread_join(thread_, NULL);
            delete this;
            return true;
        }
        released_ = true;
        pthread_t thread = thread_;
        pthread_mutex_unlock(&mu_);
        // From here the thread may free the object at any moment: only the copy is used.
        pthread_detach(thread);
        return false;
    }

private:
    StartThread(const Paths& paths, int64_t now, const std::vector<std::string>& environment, StartReport* report)
        : paths_(paths), now_(now), environment_(environment), report_(report), done_(false), released_(false) {
        pthread_mutex_init(&mu_, NULL);
        pthread_cond_init(&cv_, NULL);
    }
    ~StartThread() {
        pthread_cond_destroy(&cv_);
        pthread_mutex_destroy(&mu_);
    }
    StartThread(const StartThread&);
    StartThread& operator=(const StartThread&);

    // `ms` from now, as pthread_cond_timedwait takes it.
    static struct timespec deadlineIn(int ms) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += ms / 1000;
        ts.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ++ts.tv_sec;
            ts.tv_nsec -= 1000000000L;
        }
        return ts;
    }

    static void* threadMain(void* self) {
        static_cast<StartThread*>(self)->run();
        return NULL;
    }

    void run() {
        // Nothing thrown here gets out of the thread, which would end the HMI's process.
        try {
            Outcome o;
            try {
                o = startWithHeadUnit(paths_, now_, environment_);
            } catch (...) {
                o = outcome(kNotStarted, "NOT started: the start with the head unit failed");
            }
            report_->startedWithHeadUnit(o);
        } catch (...) {
        }
        pthread_mutex_lock(&mu_);
        done_ = true;
        bool released = released_;
        pthread_cond_broadcast(&cv_);
        pthread_mutex_unlock(&mu_);
        // Its owner has gone, and left the object to the thread.
        if (released) delete this;
    }

    const Paths paths_;
    const int64_t now_;
    const std::vector<std::string> environment_;
    StartReport* report_;
    pthread_t thread_;
    pthread_mutex_t mu_;
    pthread_cond_t cv_;
    bool done_;       // the thread has done all but end
    bool released_;   // its owner stopped waiting for it
};

}  // namespace agentstart
}  // namespace nbt

#endif
