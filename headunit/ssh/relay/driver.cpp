// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The pty driver, started on demand: see relay.hpp, "The pty driver".
//
// On QNX ptys come from the driver devc-pty, a program of the car's own debug set-up, which may not be installed and
// may already run with ptys of its own. `devc-pty -n 4` returns at once and the driver runs on under a new pid, as
// a process of its own: so the relay knows its driver by the process list before and after, never as its child,
// and signals no devc-pty it did not find that way. Elsewhere there is no such driver and ptys are always there.

#include <algorithm>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "child.hpp"
#include "programs.hpp"
#include "relay.hpp"

namespace sshrelay {

bool Relay::isDriverName(const std::string& name) const {
    if (name.empty()) return false;
    for (size_t i = 0; i < driverNames_.size(); ++i) {
        if (driverNames_[i] == name) return true;
    }
    return false;
}

// Whether `r` is still the driver it was: the same pid and start time, not ended, and a program of the driver's
// file name.
bool Relay::isOurDriver(const Recorded& r) {
    ProcessEntry entry;
    return sameProcess(r, &entry) && isDriverName(nbt::programName(nbt::exePathOf(r.pid).c_str()));
}

// At a keys message: whether the connection is to wait for the driver. Starts one when it is wanted and none of
// the relay's own runs.
bool Relay::driverWait(long long now) {
    if (config_.driverPath.empty()) return false;
    if (driverState_ == kDriverRunning && driverAlive()) return false;
    if (driverState_ == kDriverLooking) return true;
    // One that is being ended is not waited for: its SIGKILL is sent now, and a new one started.
    if (driverState_ == kDriverEnding) finishDriver(now);
    startDriver(now);
    return driverState_ == kDriverLooking;
}

void Relay::startDriver(long long now) {
    const std::string& path = config_.driverPath;
    if (access(path.c_str(), X_OK) != 0) {
        logLine("no pty driver started: " + path + ": " + strerror(errno));
        return;
    }
    if (driverFailedAt_ != 0 && now < driverFailedAt_ + config_.driverRetryMs) {
        logLine("no pty driver started: the last start, " + number((now - driverFailedAt_) / 1000) +
                " s ago, found none");
        return;
    }
    std::vector<ProcessEntry> list;
    if (!listProcesses(&list)) {
        logLine("no pty driver started: the process list cannot be read");
        driverFailedAt_ = now;
        return;
    }
    driverBefore_.clear();
    for (size_t i = 0; i < list.size(); ++i) driverBefore_.push_back(list[i].pid);
    int null = open("/dev/null", O_RDWR);
    if (null < 0) {
        logLine(std::string("no pty driver started: cannot open /dev/null: ") + strerror(errno));
        driverFailedAt_ = now;
        return;
    }
    std::vector<std::string> args;
    args.push_back(path);
    args.push_back("-n");
    args.push_back("4");
    std::string error;
    // In a session of its own, so that nothing of the relay's group or terminal is the driver's.
    pid_t pid = startChild(args, config_.env, null, null, 2, true, &error);
    close(null);
    if (pid < 0) {
        logLine("no pty driver started: " + error);
        driverFailedAt_ = now;
        return;
    }
    // The command's own process is not the driver, whatever its name.
    driverBefore_.push_back((long)pid);
    std::sort(driverBefore_.begin(), driverBefore_.end());
    driverCommand_ = pid;
    driverCommandStatus_ = -1;
    driverState_ = kDriverLooking;
    driverLookUntil_ = now + kDriverLookMs;
    driverNextLook_ = now + kDriverLookEveryMs;
    logLine("starting the pty driver: " + path + " -n 4");
}

void Relay::lookForDriver(long long now) {
    std::vector<ProcessEntry> list, found;
    if (listProcesses(&list)) {
        for (size_t i = 0; i < list.size(); ++i) {
            const ProcessEntry& e = list[i];
            if (e.zombie || std::binary_search(driverBefore_.begin(), driverBefore_.end(), e.pid)) continue;
            if (isDriverName(nbt::programName(nbt::exePathOf(e.pid).c_str()))) found.push_back(e);
        }
    }
    if (found.size() == 1 && found[0].start != 0) {
        driver_ = Recorded(found[0].pid, found[0].start);
        driverState_ = kDriverRunning;
        stateDirty_ = true;
        logLine("pty driver started, pid " + number(driver_.pid));
        driverDecided(now);
        return;
    }
    if (found.size() > 1) {
        logLine("no pty driver taken: " + number((long long)found.size()) + " new processes run it");
    } else if (driverCommandStatus_ > 0) {
        logLine("no pty driver: its command failed and none appeared");
    } else if (now >= driverLookUntil_) {
        logLine("no pty driver: none appeared within " + number(kDriverLookMs / 1000) + " s");
    } else {
        driverNextLook_ = now + kDriverLookEveryMs;
        return;
    }
    driverState_ = kDriverNone;
    driverFailedAt_ = now;
    driverDecided(now);
}

// The look is over: the connections that waited for it go on; with the relay ending, a driver found is ended.
void Relay::driverDecided(long long now) {
    if (stopping_) {
        if (driverState_ == kDriverRunning) endDriver(now, kStopKillAfterMs, "the relay was ended");
        return;
    }
    for (size_t i = 0; i < connections_.size(); ++i) {
        Connection& c = connections_[i];
        if (c.waiting && c.ending.empty()) proceed(c);
    }
}

// Whether the relay's driver still runs; when it has gone by itself, that is said and forgotten, and the next
// connection starts one again.
bool Relay::driverAlive() {
    if (driverState_ != kDriverRunning) return false;
    if (isOurDriver(driver_)) return true;
    logLine("the pty driver, pid " + number(driver_.pid) + ", has gone");
    driverState_ = kDriverNone;
    driver_ = Recorded();
    stateDirty_ = true;
    return false;
}

void Relay::endDriver(long long now, long killAfterMs, const char* why) {
    if (!driverAlive()) return;
    kill((pid_t)driver_.pid, SIGTERM);
    logLine("the pty driver, pid " + number(driver_.pid) + ": SIGTERM, " + why);
    driverState_ = kDriverEnding;
    driverKillAt_ = now + killAfterMs;
}

void Relay::finishDriver(long long now) {
    (void)now;
    if (isOurDriver(driver_)) {
        kill((pid_t)driver_.pid, SIGKILL);
        logLine("the pty driver, pid " + number(driver_.pid) + ", still there: SIGKILL");
    } else {
        logLine("the pty driver, pid " + number(driver_.pid) + ", has ended");
    }
    driverState_ = kDriverNone;
    driver_ = Recorded();
    stateDirty_ = true;
}

// Each turn of the loop: the look, the end, and the time with no connection. *wait is shortened to the next of
// them.
void Relay::serveDriver(long long now, long long* wait) {
    if (driverState_ == kDriverLooking) {
        if (now >= driverNextLook_) lookForDriver(now);
        if (driverState_ == kDriverLooking && driverNextLook_ - now < *wait) *wait = driverNextLook_ - now;
    }
    if (driverState_ == kDriverEnding) {
        if (!isOurDriver(driver_)) {
            logLine("the pty driver, pid " + number(driver_.pid) + ", has ended");
            driverState_ = kDriverNone;
            driver_ = Recorded();
            stateDirty_ = true;
        } else if (now >= driverKillAt_) {
            finishDriver(now);
        } else {
            long long next = driverKillAt_ < now + kDriverLookEveryMs ? driverKillAt_ : now + kDriverLookEveryMs;
            if (next - now < *wait) *wait = next - now;
        }
    }
    if (driverState_ == kDriverRunning && !stopping_ && idleSince_ != 0) {
        long long due = idleSince_ + config_.driverIdleMs;
        if (now >= due) {
            endDriver(now, kKillAfterMs, ("no connection for " + number(config_.driverIdleMs / 1000) + " s").c_str());
            if (driverState_ == kDriverEnding && kDriverLookEveryMs < *wait) *wait = kDriverLookEveryMs;
        } else if (due - now < *wait) {
            *wait = due - now;
        }
    }
}

// Whether a server can have a pty: the relay's driver runs, or the system has pty names of another driver's (QNX:
// /dev/ptyp*), or the system is one where ptys are always there.
bool Relay::hasPty() {
    if (driverState_ == kDriverRunning) return true;
    if (config_.ptyDevDir.empty()) return true;
    DIR* dir = opendir(config_.ptyDevDir.c_str());
    if (dir == 0) return false;
    bool found = false;
    struct dirent* entry;
    while (!found && (entry = readdir(dir)) != 0) found = strncmp(entry->d_name, "ptyp", 4) == 0;
    closedir(dir);
    return found;
}

}  // namespace sshrelay
