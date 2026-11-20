// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_SSH_RELAY_RELAY_HPP
#define NBT_SSH_RELAY_RELAY_HPP

// sshrelay: the program of the channel "ssh" of the package org.nbtapp.ssh (ssh/README, "The relay"). The agent
// connects to it once for each channel a phone opens; for each connection the relay starts one Dropbear server on a
// socket pair (dropbear -i) and carries the bytes between the channel and the server. One thread, one select loop;
// nothing in the loop blocks.
//
// One connection:
//   1. the agent's line of JSON, then two-byte-length messages either way (agent/agent.hpp, "Channels");
//   2. the phone's first message, the keys message (key_lines.hpp), within kKeysWaitMs of the connection, or the
//      connection is dropped;
//   3. too-many when kMaxConnections connections have, or wait for, a server; failed when there is no host key;
//   4. when the pty driver is to be started, or is being looked for, the connection waits for that (below);
//   5. the lines written to ${tmp}/auth/<n>/authorized_keys (folder 0700, file 0600, on the RAM disk);
//   6. the server started on a socket pair, and the answer { "v": 1, "hostKeys": [ "ssh-ed25519 AAAA..." ],
//      "pty": true | false }, or { "error": "bad-keys" | "too-many" | "failed" } and the connection closed;
//   7. the bridge: the server's bytes go out as messages of up to the agent's maxMessage, the phone's messages go
//      in as bytes. Each direction holds about one message, and a side is not read while what it sent on has not
//      been taken, so a slow reader on either side holds the other back.
//
// Ends. When the agent's side closes, the server's end is closed and the server exits. When the server's end
// closes (or the server has exited and its end has nothing more), what waits for the agent goes first, then the
// agent's connection is closed. Then what the server left behind is ended: its login shells lead sessions of their
// own, and their background jobs, or without a pty everything the shell ran, stay after it. So while a server
// runs the relay notes, every kNoteEveryMs, its children that lead a session; at the end it walks the process
// list and sends SIGHUP to every process group whose session is the server's or a noted one, and SIGKILL to each
// such group still there kKillAfterMs later, without waiting meanwhile. It signals no other group. A login shell
// started in the last kNoteEveryMs before a server ends by itself can be missed: accepted. When the agent's side
// closes, the server's children are noted once more first, while the server is still their parent. A connection's
// slot (step 3) is free once it has ended: its server's end closed and the SIGHUP sent, not when the SIGKILL is.
//
// The pty driver (QNX: /bin/devc-pty, which may not be there; elsewhere none, for ptys are always there). At a keys
// message, with no driver of the relay's own running, it runs `devc-pty -n 4`, which returns at once while the
// driver runs on under a new pid that is not the relay's child. So the process list is taken first, and every
// kDriverLookEveryMs for kDriverLookMs the relay looks for a process that was not in it and runs a program of the
// driver's file name; connections whose keys come meanwhile wait with the first, and all go on when it is found or
// the time is over. The relay signals only a driver it found so, and only while that pid still has the start time
// and the program name it had. kDriverIdleMs after the last connection has ended (no connection is left that has
// not ended) it sends the driver SIGTERM, and SIGKILL kKillAfterMs later if it is still there; a connection
// meanwhile keeps it, and one whose keys come while it is being ended sends the SIGKILL at once and starts another.
// A driver that has gone by itself is noticed (at a keys message, and with the notes of the sessions) and started
// again at the next connection. A start that found none is not tried again for kDriverRetryMs.
//
// The state file, ${tmp}/sshrelay.state, written whole (a new file renamed over it) whenever it changes: the
// relay's pid, its driver's, and each server's with its noted sessions, each with its start time. Removed at a
// clean exit. At a start, before listening: an older relay that the file names and that still runs (same pid,
// start time and program name) is sent SIGTERM, waited for kOlderRelayWaitMs, and sent SIGKILL then, and waited
// for kOlderRelayGoneMs at most; the file is read again after it. Then what the file names is ended as a
// connection's leftovers are (SIGHUP, SIGKILL kKillAfterMs later, in the loop), but only a session, a server or a
// driver that is still what was recorded (state.cpp, "What is checked"), and every keys folder under ${tmp}/auth
// is removed. A file that cannot be read or does not hold what it should is ignored, signals nothing, and is
// replaced.
//
// SIGTERM, SIGINT or SIGHUP: every connection is ended so, with the SIGKILL kStopKillAfterMs after the SIGHUP, and
// the driver too; the socket file, the keys folders and the state file are removed, and the relay exits, all well
// within the 2 s the agent waits before its SIGKILL to the relay's group, which would not reach the servers' or the
// driver's sessions. The relay does not exit by itself: with no connection only the driver goes.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <sys/types.h>

#include <string>
#include <vector>

#include "processes.hpp"

namespace sshrelay {

const long kKeysWaitMs = 10000;
const long kNoteEveryMs = 1000;
const long kKillAfterMs = 2000;
const long kStopKillAfterMs = 500;
const long kStopExitAfterMs = 1500;   // the relay exits by then, whatever is still waiting
const long kKeyToolWaitMs = 20000;
const size_t kMaxAgentLine = 2048;
// The smallest maxMessage the relay takes: its answer has to fit one message.
const size_t kMinMaxMessage = 256;
const size_t kMaxNoted = 64;          // noted session leaders, per connection
const int kListenBacklog = 64;        // on QNX a full queue refuses a connect
const size_t kMaxConnections = 4;     // with, or waiting for, a server
const long kDriverLookMs = 2000;
const long kDriverLookEveryMs = 100;
const long kDriverIdleMs = 30000;
const long kDriverRetryMs = 30000;
const long kOlderRelayWaitMs = 2000;
const long kOlderRelayGoneMs = 300;

struct Config {
    std::string socketPath;    // ${tmp}/channels/<channel>
    std::string authDir;       // ${tmp}/auth
    std::string statePath;     // ${tmp}/sshrelay.state
    std::string hostKeyPath;   // ${data}/hostkey_ed25519
    std::string multiPath;     // ${app}/bin/dropbearmulti
    std::string driverPath;    // /bin/devc-pty on QNX; empty where no driver is started
    std::string ptyDevDir;     // where pty names (ptyp*) are looked for: /dev on QNX; empty: there are always ptys
    long driverIdleMs;
    long driverRetryMs;
    std::string testServer;    // test builds only: run in place of `dropbearmulti dropbear`, with its arguments
    std::string testServerName;   // test builds only: the program name the test server's processes have
    std::vector<std::string> env;   // the server's, the key tool's and the driver's environment
    Config() : driverIdleMs(kDriverIdleMs), driverRetryMs(kDriverRetryMs) {}
};

// A process the relay may signal later, as the state file records it: its pid and start time.
struct Recorded {
    long pid;
    unsigned long long start;
    Recorded() : pid(0), start(0) {}
    Recorded(long p, unsigned long long s) : pid(p), start(s) {}
};

// What the state file holds (state.cpp).
struct State {
    Recorded relay;
    Recorded driver;                          // pid 0: none
    struct Server {
        Recorded server;
        std::vector<Recorded> logins;         // the noted session leaders
    };
    std::vector<Server> servers;
};

class Relay {
public:
    explicit Relay(const Config& config);
    ~Relay();

    // Ends an older relay, listens, tidies what a relay that died left, makes or reads the host key, and serves
    // until SIGTERM, SIGINT or SIGHUP. The exit status: 0, or 2 when the socket cannot be listened on.
    int run();

    // Called by the signal handlers.
    static void onSignal(int signal);

private:
    struct Connection {
        unsigned number;
        int agent;               // -1 once closed
        int server;              // the relay's end of the socket pair; -1 before and after
        pid_t serverPid;         // 0 when none was started
        bool serverExited;
        long long accepted;
        bool greeted;            // the agent's line has come
        bool keyed;              // the keys message has come
        bool waiting;            // keyed and taken, waiting for the pty driver
        size_t maxMessage;
        std::string caller, phone;
        std::vector<std::string> keyLines;   // while it waits
        std::string in;          // from the agent, not yet taken: at most one message and its length
        std::string toServer;    // the phone's bytes for the server: about one message
        std::string toAgent;     // framed, for the agent: one message, or the answer
        bool closeWhenSent;      // close the agent's side once toAgent has gone
        std::string keysDir;     // made for this connection, or empty
        std::vector<long> sessions;   // the server's pid and its noted session leaders
        std::vector<unsigned long long> starts;   // the start time of each of those, 0 when not known
        bool cleaning;           // the SIGHUP has gone; the SIGKILL is due at killAt
        bool cleaned;
        long long killAt;
        unsigned hupGroups;
        unsigned long long bytesIn, bytesOut;
        std::string ending;      // how the connection ended, for the log
        Connection();
    };

    enum DriverState {
        kDriverNone,      // none of the relay's own
        kDriverLooking,   // its command run, the new process looked for
        kDriverRunning,   // found: driverPid_
        kDriverEnding     // SIGTERM sent; SIGKILL due at driverKillAt_
    };

    bool listen();
    void makeHostKey();
    void loop();
    void accept(long long now);
    void serve(Connection& c, bool readable, bool writable, bool serverReadable, bool serverWritable, long long now);
    void greet(Connection& c, long long now);
    void handleKeys(Connection& c, const std::string& message, long long now);
    void proceed(Connection& c);
    void answerError(Connection& c, const char* code, const std::string& why);
    bool startServer(Connection& c, std::string* error);
    void readServer(Connection& c);
    void end(Connection& c, const std::string& why, bool flush, long long now);
    void beginCleanup(Connection& c, long long now);
    void finishCleanup(Connection& c);
    void noteSessions(long long now, bool force);
    void noteLeaders(Connection& c, const std::vector<ProcessEntry>& list);
    unsigned signalSessions(Connection& c, int signal, std::vector<long>* groups);
    void reap(long long now);
    void stop(long long now);
    void removeKeysDir(Connection& c);
    void say(const Connection& c, const std::string& text);
    size_t taken() const;

    // The pty driver (driver.cpp).
    bool driverWait(long long now);
    void startDriver(long long now);
    void lookForDriver(long long now);
    void driverDecided(long long now);
    bool driverAlive();
    bool isOurDriver(const Recorded& r);
    void endDriver(long long now, long killAfterMs, const char* why);
    void finishDriver(long long now);
    void serveDriver(long long now, long long* wait);
    bool hasPty();
    bool isDriverName(const std::string& name) const;

    // The state file and the start (state.cpp).
    bool loadState(State* out);
    bool endOlderRelay(const Recorded& older);
    void tidy(const State& old, long long now);
    void finishTidy();
    void saveState();
    bool sameProcess(const Recorded& r, ProcessEntry* entry);
    bool isServerName(const std::string& name) const;

    Config config_;
    int listener_;
    bool hostKeyOk_;
    std::string hostKeyLine_;
    unsigned next_;
    bool stopping_;
    long long exitAt_;
    long long nextNote_;
    std::vector<Connection> connections_;

    std::vector<std::string> driverNames_;   // the file names a driver's process may have
    std::vector<std::string> serverNames_;   // the same for a server's
    DriverState driverState_;
    Recorded driver_;
    pid_t driverCommand_;        // the command's process, the relay's child, while it has not been collected
    int driverCommandStatus_;    // -1 while it runs or was not run
    std::vector<long> driverBefore_;   // sorted: the pids there before the command
    long long driverLookUntil_, driverNextLook_, driverKillAt_, driverFailedAt_, idleSince_;

    Recorded self_;
    std::string selfName_;
    bool stateDirty_;
    bool stateFailed_;
    std::vector<Recorded> tidySessions_;   // what the start's SIGHUP went to, for the SIGKILL
    Recorded tidyDriver_;
    long long tidyKillAt_;                  // 0: nothing due
};

// One line on standard error: the channel's log.
void logLine(const std::string& text);

// The monotonic clock in milliseconds, and a number as text: for relay.cpp, driver.cpp and state.cpp.
long long nowMs();
std::string number(long long value);

// The state file's text, and back. readState is false for a text that is not exactly such a file, and then *out is
// to be ignored whole.
std::string stateText(const State& state);
bool readState(const std::string& text, State* out);

}  // namespace sshrelay

#endif
