// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The relay's loop: see relay.hpp. The pty driver is in driver.cpp, the state file and the start's tidying in
// state.cpp.

#include "relay.hpp"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "child.hpp"
#include "fsutil.hpp"
#include "json.hpp"
#include "json_writer.hpp"
#include "key_lines.hpp"
#include "programs.hpp"

namespace sshrelay {

namespace {

volatile sig_atomic_t gStop = 0;
volatile sig_atomic_t gStopSignal = 0;
int gWake[2] = { -1, -1 };

void setNonBlocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
void setCloseOnExec(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC); }

// Text from outside (a phone's name) as the log shows it: printable ASCII, 64 characters at most.
std::string shown(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size() && out.size() < 64; ++i) {
        unsigned char c = (unsigned char)text[i];
        out.push_back(c >= 0x20 && c <= 0x7E ? (char)c : '?');
    }
    return out;
}

void frame(const std::string& message, std::string* out) {
    out->push_back((char)((message.size() >> 8) & 0xFF));
    out->push_back((char)(message.size() & 0xFF));
    out->append(message);
}

std::string groupList(const std::vector<long>& groups) {
    std::string text;
    for (size_t i = 0; i < groups.size() && i < 8; ++i) text += (i ? " " : "") + number(groups[i]);
    if (groups.size() > 8) text += " ...";
    return text;
}

bool contains(const std::vector<long>& list, long value) {
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i] == value) return true;
    }
    return false;
}

// The file names a program's process may be known by: that of the path, and that of the file the path leads to
// through links, since the system may report either.
std::vector<std::string> namesOf(const std::string& path) {
    std::vector<std::string> names;
    if (path.empty()) return names;
    names.push_back(nbt::programName(path.c_str()));
    char real[PATH_MAX + 1];
    if (realpath(path.c_str(), real) != 0) {
        std::string other = nbt::programName(real);
        if (!other.empty() && other != names[0]) names.push_back(other);
    }
    return names;
}

}  // namespace

long long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

std::string number(long long value) {
    char text[32];
    snprintf(text, sizeof text, "%lld", value);
    return text;
}

void logLine(const std::string& text) {
    char stamp[16] = "";
    time_t t = time(0);
    struct tm local;
    if (localtime_r(&t, &local) != 0) strftime(stamp, sizeof stamp, "%H:%M:%S", &local);
    fprintf(stderr, "sshrelay %s %s\n", stamp, text.c_str());
    fflush(stderr);
}

void Relay::onSignal(int signal) {
    int saved = errno;
    if (signal != SIGCHLD) {
        gStop = 1;
        gStopSignal = signal;
    }
    if (gWake[1] >= 0) {
        ssize_t ignored = write(gWake[1], "x", 1);
        (void)ignored;
    }
    errno = saved;
}

Relay::Connection::Connection()
    : number(0), agent(-1), server(-1), serverPid(0), serverExited(false), accepted(0), greeted(false), keyed(false),
      waiting(false), maxMessage(0), closeWhenSent(false), cleaning(false), cleaned(false), killAt(0), hupGroups(0),
      bytesIn(0), bytesOut(0) {}

Relay::Relay(const Config& config)
    : config_(config), listener_(-1), hostKeyOk_(false), next_(1), stopping_(false), exitAt_(0), nextNote_(0),
      driverState_(kDriverNone), driverCommand_(0), driverCommandStatus_(-1), driverLookUntil_(0), driverNextLook_(0),
      driverKillAt_(0), driverFailedAt_(0), idleSince_(0), stateDirty_(false), stateFailed_(false), tidyKillAt_(0) {
    driverNames_ = namesOf(config_.driverPath);
    if (!config_.testServerName.empty()) {
        serverNames_.push_back(config_.testServerName);
    } else {
        serverNames_ = namesOf(config_.multiPath);
    }
}

Relay::~Relay() {
    if (listener_ >= 0) close(listener_);
}

void Relay::say(const Connection& c, const std::string& text) { logLine("#" + number(c.number) + " " + text); }

bool Relay::listen() {
    struct sockaddr_un address;
    const std::string& path = config_.socketPath;
    if (path.size() >= sizeof address.sun_path) {
        logLine("the socket's path is too long: " + path);
        return false;
    }
    unlink(path.c_str());
    listener_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener_ < 0) {
        logLine(std::string("cannot make a socket: ") + strerror(errno));
        return false;
    }
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (bind(listener_, (struct sockaddr*)&address, sizeof address) != 0 || ::listen(listener_, kListenBacklog) != 0) {
        logLine("cannot listen at " + path + ": " + strerror(errno));
        close(listener_);
        listener_ = -1;
        return false;
    }
    setNonBlocking(listener_);
    setCloseOnExec(listener_);
    return true;
}

// Made once, with the key tool, when it is not there; its public line read with the key tool for the answers. This
// runs before the loop and is the one thing that waits: the socket is listened on already, and connections that
// come meanwhile wait in its queue.
void Relay::makeHostKey() {
    const std::string& path = config_.hostKeyPath;
    std::string output, error;
    if (nbt::fs::kindOf(path) == nbt::fs::kNone) {
        std::string folder = nbt::fs::parentOf(path);
        if (!nbt::fs::makeDirs(folder, &error)) {
            logLine("no host key: " + error + "; connections are answered failed");
            return;
        }
        std::vector<std::string> make;
        make.push_back(config_.multiPath);
        make.push_back("dropbearkey");
        make.push_back("-t");
        make.push_back("ed25519");
        make.push_back("-f");
        make.push_back(path);
        if (!runToEnd(make, config_.env, kKeyToolWaitMs, &gStop, &output, &error)) {
            logLine("no host key: " + error + "; connections are answered failed");
            return;
        }
        chmod(path.c_str(), 0600);
        logLine("made the host key " + path);
    }
    std::vector<std::string> read;
    read.push_back(config_.multiPath);
    read.push_back("dropbearkey");
    read.push_back("-y");
    read.push_back("-f");
    read.push_back(path);
    if (!runToEnd(read, config_.env, kKeyToolWaitMs, &gStop, &output, &error)) {
        logLine("the host key cannot be read: " + error + "; connections are answered failed");
        return;
    }
    std::string line, fingerprint;
    size_t at = 0;
    while (at < output.size()) {
        size_t end = output.find('\n', at);
        if (end == std::string::npos) end = output.size();
        std::string text = output.substr(at, end - at);
        if (!text.empty() && text[text.size() - 1] == '\r') text.erase(text.size() - 1);
        if (text.compare(0, 12, "ssh-ed25519 ") == 0 && line.empty()) {
            // The type and the key; the key tool's comment (user@host) is not the phone's business.
            size_t keyEnd = text.find(' ', 12);
            line = text.substr(0, keyEnd);
        } else if (text.compare(0, 13, "Fingerprint: ") == 0) {
            fingerprint = text.substr(13);
        }
        at = end + 1;
    }
    std::string why;
    if (line.empty() || !checkKeyLine(line, &why)) {
        logLine("the key tool gave no ssh-ed25519 public line for " + path + "; connections are answered failed");
        return;
    }
    hostKeyLine_ = line;
    hostKeyOk_ = true;
    logLine("host key ssh-ed25519 " + shown(fingerprint));
}

int Relay::run() {
    if (pipe(gWake) != 0) {
        logLine(std::string("cannot make a pipe: ") + strerror(errno));
        return 2;
    }
    setNonBlocking(gWake[0]);
    setNonBlocking(gWake[1]);
    setCloseOnExec(gWake[0]);
    setCloseOnExec(gWake[1]);

    // Before listening: an older relay removes the socket file when it ends.
    self_.pid = (long)getpid();
    ProcessEntry own;
    if (readProcess(self_.pid, &own)) self_.start = own.start;
    selfName_ = nbt::programName(nbt::exePathOf(self_.pid).c_str());
    State old;
    bool haveOld = loadState(&old);
    if (haveOld && old.relay.pid != self_.pid && endOlderRelay(old.relay)) {
        // What it wrote last, or nothing if it ended cleanly and removed the file.
        old = State();
        haveOld = loadState(&old);
    }

    if (!listen()) return 2;
    logLine("started, pid " + number(getpid()) + ", listening at " + config_.socketPath);
    stateDirty_ = true;
    saveState();
    if (haveOld) tidy(old, nowMs());
    // Every keys folder an earlier run left: the named folder auth, removed whole and made again.
    std::string error;
    if (nbt::fs::kindOf(config_.authDir) != nbt::fs::kNone) {
        std::vector<std::string> names;
        nbt::fs::listDir(config_.authDir, &names, &error);
        if (!names.empty()) logLine("removed " + number((long long)names.size()) + " keys folder(s) left in " + config_.authDir);
        if (!nbt::fs::removeTree(config_.authDir, &error)) logLine("cannot remove " + config_.authDir + ": " + error);
    }
    if (!nbt::fs::makeDirs(config_.authDir, &error)) logLine("cannot make " + config_.authDir + ": " + error);
    chmod(config_.authDir.c_str(), 0700);
    if (config_.driverPath.empty() && config_.ptyDevDir.empty()) logLine("no pty driver to start here: ptys are the system's");
    makeHostKey();
    loop();
    // What is still there at the exit: a cleanup whose SIGKILL is due, the driver, a keys folder.
    for (size_t i = 0; i < connections_.size(); ++i) {
        Connection& c = connections_[i];
        if (c.cleaning && !c.cleaned) finishCleanup(c);
        if (c.agent >= 0) close(c.agent);
        if (c.server >= 0) close(c.server);
        removeKeysDir(c);
    }
    connections_.clear();
    // A driver still looked for is looked for once more, and ended at once if it is there.
    if (driverState_ == kDriverLooking) lookForDriver(nowMs(), true);
    if (driverState_ == kDriverEnding) finishDriver(nowMs());
    if (tidyKillAt_ != 0) finishTidy();
    nbt::fs::removeEmptyDir(config_.authDir);
    unlink(config_.statePath.c_str());
    logLine("exiting");
    return 0;
}

void Relay::stop(long long now) {
    stopping_ = true;
    exitAt_ = now + kStopExitAfterMs;
    if (listener_ >= 0) {
        close(listener_);
        listener_ = -1;
    }
    unlink(config_.socketPath.c_str());
    const char* name = gStopSignal == SIGINT ? "SIGINT" : gStopSignal == SIGHUP ? "SIGHUP" : "SIGTERM";
    logLine(std::string(name) + ": ending " + number((long long)connections_.size()) + " connection(s)");
    for (size_t i = 0; i < connections_.size(); ++i) {
        Connection& c = connections_[i];
        if (c.ending.empty()) {
            end(c, "the relay was ended", false, now);
        } else if (c.agent >= 0) {
            close(c.agent);
            c.agent = -1;
        }
        if (c.cleaning && !c.cleaned && c.killAt > now + kStopKillAfterMs) c.killAt = now + kStopKillAfterMs;
    }
    // One that is still looked for is ended when it is found (driverDecided).
    if (driverState_ == kDriverRunning) {
        endDriver(now, kStopKillAfterMs, "the relay was ended");
    } else if (driverState_ == kDriverEnding && driverKillAt_ > now + kStopKillAfterMs) {
        driverKillAt_ = now + kStopKillAfterMs;
    }
    if (tidyKillAt_ != 0 && tidyKillAt_ > now + kStopKillAfterMs) tidyKillAt_ = now + kStopKillAfterMs;
}

// How many connections hold a slot: they have, or wait for, a server, and have not ended.
size_t Relay::taken() const {
    size_t n = 0;
    for (size_t i = 0; i < connections_.size(); ++i) {
        const Connection& c = connections_[i];
        if (c.ending.empty() && (c.waiting || c.serverPid != 0)) ++n;
    }
    return n;
}

void Relay::loop() {
    for (;;) {
        long long now = nowMs();
        reap(now);
        if (gStop && !stopping_) stop(now);
        for (size_t i = 0; i < connections_.size(); ++i) {
            Connection& c = connections_[i];
            if (c.ending.empty() && !c.keyed && now >= c.accepted + kKeysWaitMs) {
                end(c, "no keys message within 10 s", false, now);
            }
            // The server has exited: what its end still holds goes on, and then the connection ends.
            if (c.ending.empty() && c.serverExited && c.server >= 0 && c.toAgent.empty()) readServer(c);
            if (c.cleaning && !c.cleaned && now >= c.killAt) finishCleanup(c);
        }
        if (now >= nextNote_) noteSessions(now, false);
        for (size_t i = 0; i < connections_.size(); ++i) {
            Connection& c = connections_[i];
            if (c.agent < 0 && c.server < 0 && (c.serverPid == 0 || c.cleaned)) {
                removeKeysDir(c);
                connections_.erase(connections_.begin() + (long)i);
                --i;
            }
        }
        if (tidyKillAt_ != 0 && now >= tidyKillAt_) finishTidy();
        // The driver's time with no connection runs from the end of the last one.
        bool live = false;
        for (size_t i = 0; i < connections_.size() && !live; ++i) live = connections_[i].ending.empty();
        if (live) {
            idleSince_ = 0;
        } else if (idleSince_ == 0) {
            idleSince_ = now;
        }
        long long wait = 60000;
        serveDriver(now, &wait);
        saveState();
        if (stopping_ && ((connections_.empty() && driverState_ == kDriverNone && tidyKillAt_ == 0) || now >= exitAt_)) {
            return;
        }

        fd_set readable, writable;
        FD_ZERO(&readable);
        FD_ZERO(&writable);
        FD_SET(gWake[0], &readable);
        int top = gWake[0];
        if (listener_ >= 0) {
            FD_SET(listener_, &readable);
            if (listener_ > top) top = listener_;
        }
        bool anyServer = false;
        for (size_t i = 0; i < connections_.size(); ++i) {
            Connection& c = connections_[i];
            if (c.agent >= 0) {
                size_t limit = c.greeted ? 2 + c.maxMessage : kMaxAgentLine;
                if (c.ending.empty() && c.in.size() < limit) FD_SET(c.agent, &readable);
                if (!c.toAgent.empty()) FD_SET(c.agent, &writable);
                if (c.agent > top) top = c.agent;
            }
            if (c.server >= 0) {
                if (c.toAgent.empty()) FD_SET(c.server, &readable);
                if (!c.toServer.empty()) FD_SET(c.server, &writable);
                if (c.server > top) top = c.server;
            }
            if (c.ending.empty() && !c.keyed && c.accepted + kKeysWaitMs - now < wait) {
                wait = c.accepted + kKeysWaitMs - now;
            }
            if (c.cleaning && !c.cleaned && c.killAt - now < wait) wait = c.killAt - now;
            if (c.serverPid != 0 && !c.serverExited && !c.cleaning) anyServer = true;
        }
        if (anyServer && nextNote_ - now < wait) wait = nextNote_ - now;
        if (tidyKillAt_ != 0 && tidyKillAt_ - now < wait) wait = tidyKillAt_ - now;
        if (stopping_ && exitAt_ - now < wait) wait = exitAt_ - now;
        if (wait < 0) wait = 0;
        struct timeval tv;
        tv.tv_sec = (long)(wait / 1000);
        tv.tv_usec = (long)(wait % 1000) * 1000;
        int r = select(top + 1, &readable, &writable, 0, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            logLine(std::string("select failed: ") + strerror(errno));
            gStop = 1;
            continue;
        }
        now = nowMs();
        if (FD_ISSET(gWake[0], &readable)) {
            char buffer[64];
            while (read(gWake[0], buffer, sizeof buffer) > 0) {
            }
        }
        if (listener_ >= 0 && FD_ISSET(listener_, &readable)) accept(now);
        for (size_t i = 0; i < connections_.size(); ++i) {
            Connection& c = connections_[i];
            bool agentReadable = c.agent >= 0 && FD_ISSET(c.agent, &readable);
            bool agentWritable = c.agent >= 0 && FD_ISSET(c.agent, &writable);
            bool serverReadable = c.server >= 0 && FD_ISSET(c.server, &readable);
            bool serverWritable = c.server >= 0 && FD_ISSET(c.server, &writable);
            serve(c, agentReadable, agentWritable, serverReadable, serverWritable, now);
        }
    }
}

void Relay::accept(long long now) {
    for (;;) {
        int fd = ::accept(listener_, 0, 0);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNABORTED) {
                logLine(std::string("accept failed: ") + strerror(errno));
            }
            return;
        }
        if (fd >= FD_SETSIZE) {
            logLine("a connection over the number of files select takes: closed");
            close(fd);
            continue;
        }
        setNonBlocking(fd);
        setCloseOnExec(fd);
        Connection c;
        c.number = next_++;
        c.agent = fd;
        c.accepted = now;
        connections_.push_back(c);
        say(connections_.back(), "connected");
    }
}

void Relay::serve(Connection& c, bool readable, bool writable, bool serverReadable, bool serverWritable,
                  long long now) {
    if (writable && !c.toAgent.empty()) {
        ssize_t n = write(c.agent, c.toAgent.data(), c.toAgent.size());
        if (n > 0) {
            c.toAgent.erase(0, (size_t)n);
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            c.toAgent.clear();
            if (c.ending.empty()) {
                end(c, "the agent's side closed", false, now);
            } else {
                close(c.agent);
                c.agent = -1;
            }
        }
    }
    if (c.agent >= 0 && c.closeWhenSent && c.toAgent.empty()) {
        close(c.agent);
        c.agent = -1;
    }
    if (serverWritable && !c.toServer.empty()) {
        ssize_t n = write(c.server, c.toServer.data(), c.toServer.size());
        if (n > 0) {
            c.toServer.erase(0, (size_t)n);
            c.bytesIn += (unsigned long long)n;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            // The server's end is gone: what it sent before is still to be read, and its close after that.
            c.toServer.clear();
        }
    }
    if (serverReadable && c.ending.empty() && c.toAgent.empty()) readServer(c);
    if (readable && c.ending.empty() && c.agent >= 0) {
        size_t limit = c.greeted ? 2 + c.maxMessage : kMaxAgentLine;
        if (c.in.size() < limit) {
            char buffer[8192];
            size_t want = limit - c.in.size();
            if (want > sizeof buffer) want = sizeof buffer;
            ssize_t n = read(c.agent, buffer, want);
            if (n > 0) {
                c.in.append(buffer, (size_t)n);
            } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                end(c, "the agent's side closed", false, now);
                return;
            }
        }
    }
    if (!c.ending.empty()) return;
    if (!c.keyed) greet(c, now);
    // While it waits for the driver, what the phone sends behind the keys message stays in `in`.
    if (!c.ending.empty() || c.server < 0) return;
    // The bridge, from the phone: whole messages, while the server has taken what came before.
    while (c.toServer.size() < c.maxMessage && c.in.size() >= 2) {
        size_t length = ((size_t)(unsigned char)c.in[0] << 8) | (unsigned char)c.in[1];
        if (length == 0 || length > c.maxMessage) {
            end(c, "a message of " + number((long long)length) + " bytes from the agent", false, now);
            return;
        }
        if (c.in.size() < 2 + length) break;
        c.toServer.append(c.in, 2, length);
        c.in.erase(0, 2 + length);
    }
}

void Relay::greet(Connection& c, long long now) {
    if (!c.greeted) {
        size_t end = c.in.find('\n');
        if (end == std::string::npos) {
            if (c.in.size() >= kMaxAgentLine) this->end(c, "the agent's line is too long", false, now);
            return;
        }
        nbt::json::Document doc;
        std::string error;
        bool ok = doc.parse(c.in.data(), end, &error);
        nbt::json::Value max = doc.root().find("maxMessage");
        if (!ok || !max.isInteger() || max.integer() < (int64_t)kMinMaxMessage || max.integer() > 65535) {
            this->end(c, "the agent's line cannot be read", false, now);
            return;
        }
        c.maxMessage = (size_t)max.integer();
        nbt::json::Value caller = doc.root().find("caller");
        nbt::json::Value phone = doc.root().find("phone");
        c.caller = caller.isString() ? caller.string() : "?";
        if (phone.isString()) c.phone = phone.string();
        c.in.erase(0, end + 1);
        c.greeted = true;
    }
    if (c.in.size() < 2) return;
    size_t length = ((size_t)(unsigned char)c.in[0] << 8) | (unsigned char)c.in[1];
    if (length == 0 || length > c.maxMessage) {
        this->end(c, "a message of " + number((long long)length) + " bytes from the agent", false, now);
        return;
    }
    if (c.in.size() < 2 + length) return;
    std::string message = c.in.substr(2, length);
    c.in.erase(0, 2 + length);
    handleKeys(c, message, now);
}

void Relay::answerError(Connection& c, const char* code, const std::string& why) {
    nbt::json::Writer w;
    w.beginObject().key("error").string(code).endObject();
    frame(w.text(), &c.toAgent);
    c.keyed = true;
    c.waiting = false;
    c.keyLines.clear();
    end(c, std::string("refused, ") + code + ": " + why, true, nowMs());
}

void Relay::handleKeys(Connection& c, const std::string& message, long long now) {
    std::vector<std::string> lines;
    std::string why;
    std::string who = "caller " + shown(c.caller) + (c.phone.empty() ? std::string() : ", phone '" + shown(c.phone) + "'");
    if (!readKeysMessage(message, &lines, &why)) {
        say(c, who);
        answerError(c, "bad-keys", why);
        return;
    }
    std::string keys;
    for (size_t i = 0; i < lines.size(); ++i) keys += (i ? ", " : "") + describeKeyLine(lines[i]);
    say(c, who + ", " + number((long long)lines.size()) + " key(s): " + keys);
    // Before anything is made for it.
    if (taken() >= kMaxConnections) {
        answerError(c, "too-many", number((long long)kMaxConnections) + " connections have or wait for a server");
        return;
    }
    if (!hostKeyOk_) {
        answerError(c, "failed", "there is no host key");
        return;
    }
    c.keyed = true;
    c.keyLines = lines;
    c.waiting = true;
    if (driverWait(now)) {
        say(c, "waits for the pty driver");
        return;
    }
    proceed(c);
}

// The keys folder, the server and the answer, for a connection whose keys have been taken.
void Relay::proceed(Connection& c) {
    c.waiting = false;
    std::vector<std::string> lines;
    lines.swap(c.keyLines);
    std::string error;
    std::string dir = config_.authDir + "/" + number(c.number);
    // A folder of that number is one a relay that ended before its cleanup left: this run's numbers are its own.
    if (nbt::fs::kindOf(dir) != nbt::fs::kNone && !nbt::fs::removeTree(dir, &error)) {
        answerError(c, "failed", error);
        return;
    }
    if (mkdir(dir.c_str(), 0700) != 0) {
        answerError(c, "failed", "cannot make " + dir + ": " + strerror(errno));
        return;
    }
    c.keysDir = dir;
    chmod(dir.c_str(), 0700);
    std::string text;
    for (size_t i = 0; i < lines.size(); ++i) text += lines[i] + "\n";
    if (!nbt::fs::writeFile(dir + "/authorized_keys", text, 0600, &error)) {
        removeKeysDir(c);
        answerError(c, "failed", error);
        return;
    }
    if (!startServer(c, &error)) {
        removeKeysDir(c);
        answerError(c, "failed", error);
        return;
    }
    bool pty = hasPty();
    nbt::json::Writer w;
    w.beginObject().key("v").integer(1).key("hostKeys").beginArray().string(hostKeyLine_).endArray();
    w.key("pty").boolean(pty).endObject();
    frame(w.text(), &c.toAgent);
    say(c, "server started, pid " + number(c.serverPid) + (pty ? ", pty yes" : ", pty no"));
}

bool Relay::startServer(Connection& c, std::string* error) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        *error = std::string("cannot make a socket pair: ") + strerror(errno);
        return false;
    }
    std::vector<std::string> args;
    if (!config_.testServer.empty()) {
        args.push_back(config_.testServer);
    } else {
        args.push_back(config_.multiPath);
        args.push_back("dropbear");
    }
    const char* const options[] = { "-i", "-E", "-s", "-e", "-j", "-k", "-I", "1800", "-r" };
    for (size_t i = 0; i < sizeof options / sizeof options[0]; ++i) args.push_back(options[i]);
    args.push_back(config_.hostKeyPath);
    args.push_back("-D");
    args.push_back(c.keysDir);
    // The server's end goes to it as its standard input and output; no flag that closes it on exec, which QNX's
    // spawn() would honour for a file of its list.
    pid_t pid = startChild(args, config_.env, pair[1], pair[1], 2, true, error);
    close(pair[1]);
    if (pid < 0) {
        close(pair[0]);
        return false;
    }
    setNonBlocking(pair[0]);
    setCloseOnExec(pair[0]);
    c.server = pair[0];
    c.serverPid = pid;
    // Its start time, for the state file: it is the relay's child and not collected yet, so it is there.
    ProcessEntry entry;
    c.sessions.push_back(pid);
    c.starts.push_back(readProcess(pid, &entry) ? entry.start : 0);
    stateDirty_ = true;
    return true;
}

void Relay::readServer(Connection& c) {
    char buffer[65536];
    size_t want = c.maxMessage < sizeof buffer ? c.maxMessage : sizeof buffer;
    ssize_t n = read(c.server, buffer, want);
    if (n > 0) {
        frame(std::string(buffer, (size_t)n), &c.toAgent);
        c.bytesOut += (unsigned long long)n;
        return;
    }
    if (n == 0) {
        end(c, "the server closed its end", true, nowMs());
    } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        if (c.serverExited) end(c, "the server exited", true, nowMs());
    } else {
        end(c, std::string("the server's end failed: ") + strerror(errno), true, nowMs());
    }
}

// The connection's end: logged once; the agent's side closed at once, or once what waits for it has gone; then
// the cleanup.
void Relay::end(Connection& c, const std::string& why, bool flush, long long now) {
    if (c.ending.empty()) {
        c.ending = why;
        say(c, "ended: " + why + " (" + number((long long)c.bytesIn) + " bytes in, " + number((long long)c.bytesOut) +
                   " out, " + number((now - c.accepted) / 1000) + " s)");
    }
    c.waiting = false;
    c.keyLines.clear();
    if (flush && c.agent >= 0) {
        c.closeWhenSent = true;
    } else if (c.agent >= 0) {
        close(c.agent);
        c.agent = -1;
        c.toAgent.clear();
    }
    c.in.clear();
    c.toServer.clear();
    beginCleanup(c, now);
}

void Relay::beginCleanup(Connection& c, long long now) {
    if (c.cleaning || c.cleaned) return;
    // The server's children, once more, while it is still their parent.
    if (c.serverPid != 0 && !c.serverExited) {
        std::vector<ProcessEntry> list;
        if (listProcesses(&list)) noteLeaders(c, list);
    }
    if (c.server >= 0) {
        close(c.server);
        c.server = -1;
    }
    removeKeysDir(c);
    if (c.serverPid == 0) {
        c.cleaned = true;
        return;
    }
    std::vector<long> groups;
    c.hupGroups = signalSessions(c, SIGHUP, &groups);
    c.cleaning = true;
    c.killAt = now + (stopping_ ? kStopKillAfterMs : kKillAfterMs);
}

void Relay::finishCleanup(Connection& c) {
    std::vector<long> groups;
    unsigned killed = signalSessions(c, SIGKILL, &groups);
    c.cleaned = true;
    stateDirty_ = true;
    std::string text = "what the server left: SIGHUP to " + number(c.hupGroups) + " process group(s)";
    if (killed == 0) {
        text += ", none left for SIGKILL";
    } else {
        text += ", SIGKILL to " + number(killed) + " still there (" + groupList(groups) + ")";
    }
    say(c, text);
}

// The process groups of the server's sessions, signalled. Never the relay's own group or session.
unsigned Relay::signalSessions(Connection& c, int signal, std::vector<long>* groups) {
    groups->clear();
    std::vector<ProcessEntry> list;
    if (!listProcesses(&list)) {
        say(c, "the process list cannot be read: only the server's own group is signalled");
        groups->push_back(c.serverPid);
    } else {
        *groups = groupsInSessions(list, c.sessions);
    }
    long ownGroup = (long)getpgrp(), ownSession = (long)getsid(0);
    unsigned sent = 0;
    for (size_t i = 0; i < groups->size(); ++i) {
        long g = (*groups)[i];
        if (g <= 1 || g == ownGroup || g == ownSession) continue;
        if (kill(-(pid_t)g, signal) == 0) ++sent;
    }
    return sent;
}

// The server's children that lead a session, the login shells, added to what the connection notes.
void Relay::noteLeaders(Connection& c, const std::vector<ProcessEntry>& list) {
    for (size_t i = 0; i < list.size() && c.sessions.size() < kMaxNoted; ++i) {
        const ProcessEntry& e = list[i];
        if (c.serverPid > 1 && e.parent == (long)c.serverPid && e.session == e.pid && e.pid != (long)c.serverPid &&
            !contains(c.sessions, e.pid)) {
            c.sessions.push_back(e.pid);
            c.starts.push_back(e.start);
            stateDirty_ = true;
        }
    }
}

// About once a second while a server runs: its children that lead a session, the login shells; and whether the
// driver is still there.
void Relay::noteSessions(long long now, bool force) {
    nextNote_ = now + kNoteEveryMs;
    bool any = force;
    for (size_t i = 0; i < connections_.size() && !any; ++i) {
        const Connection& c = connections_[i];
        any = c.serverPid != 0 && !c.serverExited && !c.cleaning;
    }
    if (!any) return;
    if (driverState_ == kDriverRunning) driverAlive();
    std::vector<ProcessEntry> list;
    if (!listProcesses(&list)) return;
    for (size_t i = 0; i < connections_.size(); ++i) {
        Connection& c = connections_[i];
        if (c.serverPid == 0 || c.serverExited || c.cleaning) continue;
        noteLeaders(c, list);
    }
}

void Relay::reap(long long now) {
    (void)now;
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) return;
        std::string how = WIFEXITED(status) ? "exited with " + number(WEXITSTATUS(status))
                                            : "was ended by signal " + number(WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        if (pid == driverCommand_) {
            // While the driver is looked for: its command has ended, or has detached and runs on (driver.cpp). Once
            // it has been taken as the driver, its end is told as the driver's.
            driverCommand_ = 0;
            driverCommandStatus_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
            if (driverCommandStatus_ != 0 && driverState_ == kDriverLooking) logLine("the pty driver's command " + how);
            continue;
        }
        bool found = false;
        for (size_t i = 0; i < connections_.size(); ++i) {
            Connection& c = connections_[i];
            if (c.serverPid == pid) {
                c.serverExited = true;
                say(c, "the server " + how);
                found = true;
            }
        }
        if (!found) logLine("pid " + number(pid) + " " + how);
    }
}

void Relay::removeKeysDir(Connection& c) {
    if (c.keysDir.empty()) return;
    std::string error;
    // The path is the one this connection made: ${tmp}/auth/<its number>.
    if (!nbt::fs::removeTree(c.keysDir, &error)) say(c, "cannot remove the keys folder: " + error);
    c.keysDir.clear();
}

}  // namespace sshrelay
