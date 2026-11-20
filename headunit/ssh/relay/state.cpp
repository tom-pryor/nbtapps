// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The state file, and what a start does with the one a relay that died left: see relay.hpp, "The state file".
//
// The file, ${tmp}/sshrelay.state, one line of JSON:
//   { "v": 1, "relay": REC, "driver": REC, "servers": [ { "pid", "start", "logins": [ REC, ... ] }, ... ] }
// where REC is { "pid": N, "start": T }: a pid above 1 and the process's start time as the process list gives it
// (processes.hpp), 0 when it was not known. "driver" is left out when there is none. Anything else (another key,
// another type, a pid of 1 or less, more than 64 servers or logins) and the whole file is ignored.
//
// What is checked before a recorded process or session is signalled at a start:
//   - never pid 1 or below, the relay's own pid, group or session;
//   - the older relay: the same pid and start time, not ended, and a program of this relay's file name;
//   - the driver: the same pid and start time, not ended, and a program of the driver's file name;
//   - a server's session (its pid) and each login session (its leader's pid): when a process of that pid is there
//     and not ended, it has to have the recorded start time and still lead that session, and for a server run a
//     program of the server's file name; otherwise (another process has the pid now) the session is left alone.
//     When no process of that pid is there, the session is ended only if processes are still in it: a session's
//     id is its leader's pid, which is not given to a new process while the session has members. So a stranger is
//     hit only if it is in a session whose leader was given the recorded pid after the recorded session had
//     emptied, and has itself ended since, all between the death of the relay and this start; or if it has the
//     recorded pid and start time, which is to say it is the recorded process.
// The SIGHUP goes to every process group of those sessions at once, and the SIGKILL kKillAfterMs later, in the
// loop, to the groups still in them; the driver gets SIGTERM, then SIGKILL.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fsutil.hpp"
#include "json.hpp"
#include "json_writer.hpp"
#include "programs.hpp"
#include "relay.hpp"

namespace sshrelay {

namespace {

const size_t kMaxStateServers = 64;
const size_t kMaxStateBytes = 65536;
const long kStepMs = 20;

void writeRecorded(nbt::json::Writer& w, const Recorded& r) {
    w.beginObject().key("pid").integer(r.pid).key("start").integer((int64_t)r.start).endObject();
}

bool readRecorded(const nbt::json::Value& v, Recorded* out) {
    if (!v.isObject() || v.size() != 2) return false;
    nbt::json::Value pid = v.find("pid"), start = v.find("start");
    if (!pid.isInteger() || !start.isInteger() || pid.integer() <= 1 || pid.integer() > 0x7FFFFFFF || start.integer() < 0) {
        return false;
    }
    out->pid = (long)pid.integer();
    out->start = (unsigned long long)start.integer();
    return true;
}

const ProcessEntry* findEntry(const std::vector<ProcessEntry>& list, long pid) {
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].pid == pid) return &list[i];
    }
    return 0;
}

bool hasMembers(const std::vector<ProcessEntry>& list, long session) {
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].session == session && !list[i].zombie) return true;
    }
    return false;
}

bool isOwn(long pid) {
    return pid <= 1 || pid == (long)getpid() || pid == (long)getpgrp() || pid == (long)getsid(0);
}

void sleepMs(long ms) { usleep((unsigned int)ms * 1000); }

}  // namespace

std::string stateText(const State& state) {
    nbt::json::Writer w;
    w.beginObject().key("v").integer(1).key("relay");
    writeRecorded(w, state.relay);
    if (state.driver.pid != 0) {
        w.key("driver");
        writeRecorded(w, state.driver);
    }
    w.key("servers").beginArray();
    for (size_t i = 0; i < state.servers.size(); ++i) {
        const State::Server& s = state.servers[i];
        w.beginObject().key("pid").integer(s.server.pid).key("start").integer((int64_t)s.server.start);
        w.key("logins").beginArray();
        for (size_t k = 0; k < s.logins.size(); ++k) writeRecorded(w, s.logins[k]);
        w.endArray().endObject();
    }
    w.endArray().endObject();
    return w.text() + "\n";
}

bool readState(const std::string& text, State* out) {
    *out = State();
    nbt::json::Document doc;
    if (!doc.parse(text, 0)) return false;
    nbt::json::Value root = doc.root();
    nbt::json::Value v = root.find("v"), relay = root.find("relay"), driver = root.find("driver");
    nbt::json::Value servers = root.find("servers");
    if (!root.isObject() || root.size() != (driver.present() ? 4u : 3u)) return false;
    if (!v.isInteger() || v.integer() != 1 || !readRecorded(relay, &out->relay)) return false;
    if (driver.present() && !readRecorded(driver, &out->driver)) return false;
    if (!servers.isArray() || servers.size() > kMaxStateServers) return false;
    for (size_t i = 0; i < servers.size(); ++i) {
        nbt::json::Value s = servers.at(i);
        if (!s.isObject() || s.size() != 3) return false;
        nbt::json::Value pid = s.find("pid"), start = s.find("start"), logins = s.find("logins");
        State::Server server;
        if (!pid.isInteger() || !start.isInteger() || pid.integer() <= 1 || pid.integer() > 0x7FFFFFFF ||
            start.integer() < 0) {
            return false;
        }
        server.server = Recorded((long)pid.integer(), (unsigned long long)start.integer());
        if (!logins.isArray() || logins.size() > kMaxNoted) return false;
        for (size_t k = 0; k < logins.size(); ++k) {
            Recorded r;
            if (!readRecorded(logins.at(k), &r)) return false;
            server.logins.push_back(r);
        }
        out->servers.push_back(server);
    }
    return true;
}

bool Relay::isServerName(const std::string& name) const {
    if (name.empty()) return false;
    for (size_t i = 0; i < serverNames_.size(); ++i) {
        if (serverNames_[i] == name) return true;
    }
    return false;
}

// The same process as recorded: the pid is there, has not ended, and started when it did.
bool Relay::sameProcess(const Recorded& r, ProcessEntry* entry) {
    return r.pid > 1 && r.start != 0 && readProcess(r.pid, entry) && !entry->zombie && entry->start == r.start;
}

bool Relay::loadState(State* out) {
    if (nbt::fs::kindOf(config_.statePath) == nbt::fs::kNone) return false;
    std::string text, error;
    if (nbt::fs::readFile(config_.statePath, kMaxStateBytes, &text, &error) && readState(text, out)) return true;
    logLine("the state file " + config_.statePath + " cannot be read: ignored, nothing it names is signalled");
    *out = State();
    return false;
}

// An older relay that still runs is ended before this one listens, since it removes the socket file at its end.
// At most kOlderRelayWaitMs + kOlderRelayGoneMs. True if it was one, whether or not it went.
bool Relay::endOlderRelay(const Recorded& older) {
    ProcessEntry entry;
    if (isOwn(older.pid) || !sameProcess(older, &entry)) return false;
    std::string name = nbt::programName(nbt::exePathOf(older.pid).c_str());
    if (selfName_.empty() || name != selfName_) return false;
    logLine("an older relay, pid " + number(older.pid) + ", still runs: SIGTERM");
    kill((pid_t)older.pid, SIGTERM);
    long long until = nowMs() + kOlderRelayWaitMs;
    while (nowMs() < until && sameProcess(older, &entry)) sleepMs(kStepMs);
    if (!sameProcess(older, &entry)) {
        logLine("the older relay has ended");
        return true;
    }
    logLine("the older relay is still there after " + number(kOlderRelayWaitMs / 1000) + " s: SIGKILL");
    kill((pid_t)older.pid, SIGKILL);
    until = nowMs() + kOlderRelayGoneMs;
    while (nowMs() < until && sameProcess(older, &entry)) sleepMs(kStepMs);
    if (sameProcess(older, &entry)) logLine("the older relay is still there; going on");
    return true;
}

void Relay::tidy(const State& old, long long now) {
    std::vector<ProcessEntry> list;
    if (!listProcesses(&list)) {
        logLine("the process list cannot be read: nothing the state file names is ended");
        return;
    }
    tidySessions_.clear();
    unsigned left = 0;
    for (size_t i = 0; i < old.servers.size(); ++i) {
        const State::Server& s = old.servers[i];
        std::vector<Recorded> sessions;
        sessions.push_back(s.server);
        sessions.insert(sessions.end(), s.logins.begin(), s.logins.end());
        for (size_t k = 0; k < sessions.size(); ++k) {
            const Recorded& r = sessions[k];
            if (isOwn(r.pid)) continue;
            const ProcessEntry* e = findEntry(list, r.pid);
            if (e != 0 && !e->zombie) {
                // There: only if it is what was recorded.
                if (r.start == 0 || e->start != r.start || e->session != r.pid) continue;
                if (k == 0 && !isServerName(nbt::programName(nbt::exePathOf(r.pid).c_str()))) continue;
                if (k == 0) ++left;
            } else if (!hasMembers(list, r.pid)) {
                continue;
            }
            tidySessions_.push_back(r);
        }
    }
    std::vector<long> ids;
    for (size_t i = 0; i < tidySessions_.size(); ++i) ids.push_back(tidySessions_[i].pid);
    std::vector<long> groups = groupsInSessions(list, ids);
    unsigned sent = 0;
    for (size_t i = 0; i < groups.size(); ++i) {
        if (!isOwn(groups[i]) && kill(-(pid_t)groups[i], SIGHUP) == 0) ++sent;
    }
    logLine("left by the relay before: " + number((long long)old.servers.size()) + " server(s) recorded, " +
            number(left) + " still running; SIGHUP to " + number(sent) + " process group(s) of " +
            number((long long)tidySessions_.size()) + " session(s)");
    if (old.driver.pid != 0) {
        if (!isOwn(old.driver.pid) && isOurDriver(old.driver)) {
            kill((pid_t)old.driver.pid, SIGTERM);
            tidyDriver_ = old.driver;
            logLine("its pty driver, pid " + number(old.driver.pid) + ": SIGTERM");
        } else {
            logLine("its pty driver, pid " + number(old.driver.pid) + ", is not there");
        }
    }
    if (sent != 0 || tidyDriver_.pid != 0) tidyKillAt_ = now + kKillAfterMs;
}

void Relay::finishTidy() {
    tidyKillAt_ = 0;
    std::vector<ProcessEntry> list;
    unsigned sent = 0;
    if (listProcesses(&list)) {
        std::vector<long> ids;
        for (size_t i = 0; i < tidySessions_.size(); ++i) {
            const Recorded& r = tidySessions_[i];
            const ProcessEntry* e = findEntry(list, r.pid);
            // The leader that was there is still the same one, or has gone.
            if (e != 0 && !e->zombie && e->start != r.start) continue;
            ids.push_back(r.pid);
        }
        std::vector<long> groups = groupsInSessions(list, ids);
        for (size_t i = 0; i < groups.size(); ++i) {
            if (!isOwn(groups[i]) && kill(-(pid_t)groups[i], SIGKILL) == 0) ++sent;
        }
    }
    std::string text = "left by the relay before: " +
                       (sent == 0 ? std::string("none left for SIGKILL") : "SIGKILL to " + number(sent) + " process group(s)");
    if (tidyDriver_.pid != 0) {
        if (isOurDriver(tidyDriver_)) {
            kill((pid_t)tidyDriver_.pid, SIGKILL);
            text += "; its pty driver still there: SIGKILL";
        } else {
            text += "; its pty driver has ended";
        }
    }
    logLine(text);
    tidySessions_.clear();
    tidyDriver_ = Recorded();
}

// Written whole when something in it changed: a new file, renamed over the old one, so that a reader never sees
// half of one. The RAM disk needs no sync.
void Relay::saveState() {
    if (!stateDirty_) return;
    stateDirty_ = false;
    State state;
    state.relay = self_;
    if (driverState_ == kDriverRunning || driverState_ == kDriverEnding) state.driver = driver_;
    for (size_t i = 0; i < connections_.size(); ++i) {
        const Connection& c = connections_[i];
        if (c.serverPid == 0 || c.cleaned || c.sessions.empty()) continue;
        State::Server s;
        s.server = Recorded(c.sessions[0], c.starts[0]);
        for (size_t k = 1; k < c.sessions.size(); ++k) s.logins.push_back(Recorded(c.sessions[k], c.starts[k]));
        state.servers.push_back(s);
    }
    std::string text = stateText(state);
    std::string temp = config_.statePath + ".new";
    bool ok = false;
    int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        size_t at = 0;
        while (at < text.size()) {
            ssize_t n = write(fd, text.data() + at, text.size() - at);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            at += (size_t)n;
        }
        ok = close(fd) == 0 && at == text.size() && rename(temp.c_str(), config_.statePath.c_str()) == 0;
        if (!ok) unlink(temp.c_str());
    }
    if (!ok && !stateFailed_) logLine("cannot write the state file " + config_.statePath + ": " + strerror(errno));
    stateFailed_ = !ok;
}

}  // namespace sshrelay
