// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_AGENT_HPP
#define NBT_AGENT_AGENT_HPP

// The agent: what a controller talks to (plan document, "Phone and agent protocol"; every method is in the
// "Message reference" tab). It owns the install engine, the one plan and the one job, and every link.
//
//   hello                           first on every link; anything else before it gets bad-state
//   auth                            next on every link but the local socket: the phone proves it holds the key
//                                   it was paired with. Until then only hello, ping, auth and pairing.issue are
//                                   answered, and everything else gets unauthenticated
//   pairing.open, pairing.close     the local socket only: opens the pairing window, with a code for the car's
//                                   screen, and closes it
//   pairing.issue                   a phone, before auth: a pairing key for the code on the car's screen
//   phones.list, phones.forget      the local socket only: the paired phones, and the end of a pairing
//   ping                            answered with an empty result; the agent asks it too, on a serial link
//   info.get, packages.list         the head unit's profile and the installed packages
//   sources.list                    the package keys the agent trusts
//   source.add, source.remove       need the prompt on the car's screen, which does not exist yet: a new key gets
//                                   `unsupported`, and the embedded keys cannot be removed
//   job.plan                        works out a plan; the manifests it needs are pulled with blob.fetch first
//   job.start, job.cancel, job.status
//   system.restart                  answers, then restarts the HMI or the head unit
//   blob.fetch                      the one request the agent makes: a range of an archive, or a manifest
//   blob.error                      the controller's event that a stream cannot go on
//   job.progress, job.log, job.done the agent's events, to every controller
//   link.closing                    the agent's event to a phone whose link it is about to end; the last thing it
//                                   says on that link
//
// The links are the local socket and, on a head unit, Bluetooth serial (serial_link.hpp); the test bench has a
// TCP link too (TcpListener in link.hpp). Who is on the local socket is settled by who can open it: the head
// unit's own programs. Who is on any other link is settled by `auth`:
//
//   phone:  auth { "nonce": A }                  A: 16 random bytes, as hex
//   agent:  { "nonce": B, "proof": P1 }          B: the agent's 16
//   phone:  auth { "proof": P2 }
//   agent:  { }
//
//   T  = A || B || SHA-256(payload of the hello request) || SHA-256(payload of the hello reply)
//   P1 = HMAC-SHA256(K, "nbt-auth-1" || T)       K: the phone's pairing key (phones.hpp)
//   P2 = HMAC-SHA256(K, "nbt-auth-2" || T)
//
// A and B go in as bytes, not as hex. A payload is the whole of the frame's payload: the message as it travelled,
// from its first brace to its last. A phone the agent has no key for, and a wrong proof, get auth-failed, and the
// link is closed. The session key of the plan document, which a car's TCP link would need for its frame MAC, is
// not made: no link has that MAC yet, and frames keep their CRC32.
//
// The pairing window is how a phone comes by its key. A controller on the local socket opens it and is given a
// code of six digits, which it shows; for kPairingWindowSeconds a phone that sends that code with pairing.issue
// is given a key, which is filed under its phoneId, and the window closes. Three wrong codes close it too, and
// for kPairingPauseSeconds no new one opens. The controller that opened the window hears how it closed, in the
// event pairing.closed; its reason is issued, wrong-code, expired, or replaced when another controller opened a
// window in its place. The window also closes, silently, with pairing.close and when its controller's link ends.
// Codes, keys and the agent's nonces come from /dev/urandom alone (RandomSource::secret): where that cannot be
// read, pairing and auth are refused. Neither a code nor a key is ever written to the log.
//
// The test bench's TCP link is held to the same as a serial one: auth first, and pairing.issue is allowed on it,
// so that a phone app on an emulator can be paired. A key that is issued there crosses the link in the clear,
// which is one more reason the link is for the bench alone: a car's TCP link has to refuse pairing.issue.
//
// A link of kind "serial" is slow, and nothing says when the phone at its other end has gone. On such a link the
// agent gives 4,096 bytes as its largest frame in `hello`, so that no one frame holds the link for seconds; it
// sends `ping` when it has heard nothing for 10 seconds, and ends the link when it has heard nothing for 30. A
// serial link on which `hello` has not been done 30 seconds after it opened is ended as well, whatever came over
// it: until then the agent cannot tell that the link is a controller's at all. The numbers are the phone app's
// too (protocol_constants.hpp).
//
// A part of an install step can keep the agent from its links for a while. So silence is judged only when the
// links have just been read, and a link is ended for it only when the agent has also sent the two pings that fit
// into the 30 seconds, ten seconds apart, and has waited ten more: time the agent was away is not taken for the
// phone's silence. The phone, though, hears nothing from the agent for as long as such a part takes.
//
// How the agent ends a link on purpose. The phone made the link and is to end it: a Bluetooth serial link that the
// agent closed under a phone which went on holding it left the car's Bluetooth giving later links nothing
// (serial_link.hpp). So the agent says so first, on a link that is not the local socket's and on which `hello` is
// done:
//
//   { "event": "link.closing", "params": { "reason": "restarting" } }
//
// and closes its end once that has been written out, or two seconds later if the link does not take it. Nothing
// follows the event, and nothing the phone still sends is read. A phone app closes its side as soon as the event
// arrives. The reasons:
//
//   restarting   the agent's process ends for a newer version of itself, which the launcher starts: the phone
//                connects again, and its job.status is what confirms that version
//   stopping     the agent's process ends, and nothing says that it starts again
//   forgotten    the phone's pairing was ended on the head unit: its key is worth nothing now
//   replaced     a newer link from the same phone has taken this one's place
//   silent       the phone did not answer the agent's pings, or a blob.fetch
//
// A link that ends for what the phone did wrong (a broken frame, a failed `auth`, no protocol version in common)
// gets no event: where there is an answer, the error in it says why, and the phone that gets it closes its side.
// A link on which `hello` was never done is closed without a word, as before.
//
// The agent's own update (the plan document, "Agent updates"; engine.hpp). A job whose one step is the agent's
// own package ends with this agent handing the new version to the launcher: the job is `restarting`, the links
// are written out, and restartDue() tells the program to end with the status that has the launcher start the
// agent again. The version that starts then is on trial. It answers as any agent does, and refuses job.plan and
// system.restart with `busy`; the first job.status from a phone or the command-line controller is what confirms
// it. With that it becomes the version the launcher starts, the step is finished, and the job has succeeded,
// which that same job.status already says. If it is never asked, or falls over first, the launcher starts the
// old version again, and that one undoes the step in init() and reports the job as failed.
//
// One thread: nothing in the agent or the engine is locked. turn() waits on every link at once and does what there
// is to do. An install step can take minutes; while it runs the engine calls back, and the agent serves its links
// from inside that call: a controller can connect and ask for the job's status. What would disturb the step is
// refused with `busy` for as long: job.plan, job.start for another plan, packages.list and system.restart.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdint.h>
#include <stdio.h>

#include <map>
#include <string>
#include <vector>

#include "engine.hpp"
#include "job.hpp"
#include "link.hpp"
#include "peer.hpp"
#include "phones.hpp"
#include "planner.hpp"
#include "platform.hpp"
#include "sha256.hpp"

namespace nbt {

// Milliseconds that only go forward. A test supplies its own, so that what depends on time can be repeated.
class Clock {
public:
    virtual ~Clock() {}
    virtual int64_t nowMs() = 0;
    // Seconds since 1970, as the machine has them: only for the date a phone was paired on.
    virtual int64_t unixSeconds();
};

class SystemClock : public Clock {
public:
    virtual int64_t nowMs();
};

// Random bytes, of two kinds.
class RandomSource {
public:
    virtual ~RandomSource() {}
    // To tell things apart: the head-unit id and plan ids. Always answers.
    virtual void bytes(uint8_t* out, size_t len) = 0;
    // Bytes nobody can guess: pairing codes, pairing keys and the nonces of `auth`. False, with nothing usable in
    // `out`, where the machine cannot give them.
    virtual bool secret(uint8_t* out, size_t len) = 0;
};

// /dev/urandom. bytes(), where that cannot be read, takes its bytes from the time and the process id: enough to
// tell head units and plans apart. secret() has no such way out, and does not wait for a device that has nothing
// to give yet.
class SystemRandom : public RandomSource {
public:
    virtual void bytes(uint8_t* out, size_t len);
    virtual bool secret(uint8_t* out, size_t len);
};

// The `reason` of link.closing: see the top of this file.
const char* const kClosingRestarting = "restarting";
const char* const kClosingStopping = "stopping";
const char* const kClosingForgotten = "forgotten";
const char* const kClosingReplaced = "replaced";
const char* const kClosingSilent = "silent";

// One line of the agent's log: the time of day, "nbtagent: " and the line, with anything in it that could start a
// line of its own, or steer a terminal, replaced. Nothing if `to` is 0.
void logLine(FILE* to, const std::string& line);

// The log's size, for an agent whose standard error goes to a file. The launcher (nbtlaunch.sh) sends the agent's
// output to a file and names that file in this variable of the agent's environment; the file is on the RAM disk
// behind /tmp, which is small.
extern const char kLogPathVariable[];              // "NBTAGENT_LOG"
const uint64_t kLogCapBytes = 512 * 1024;
// From here on, a logLine to standard error that leaves the file longer than `capBytes` renames it to
// `path` + ".1", over the one that was there, and goes on in a new file at `path`; standard output goes with it if
// it went to the same file. A file that cannot be set aside is emptied instead. False, with nothing changed, if
// `path` is not the regular file that standard error is written to.
bool capLog(const std::string& path, uint64_t capBytes);

class Agent : public PeerHandler, public JobHost {
public:
    // Nothing passed in is owned; all of it must outlive the agent.
    Agent(Platform* platform, const std::vector<TrustedKey>& keys, const std::string& agentVersion, Clock* clock,
          RandomSource* random);
    virtual ~Agent();

    // Makes the store, finishes or undoes an interrupted step, settles the job that step belonged to, and reads or
    // makes the head-unit id. Call once, first.
    bool init(std::string* error);

    // Where controllers connect.
    void addListener(Listener* listener);
    // A link on which a controller has connected: takes it over. The controller dialled, so it speaks first.
    void adopt(Link* link);

    // One turn of the loop: waits up to `timeoutMs` for a link to have something, serves every link, and runs at
    // most one install step.
    void turn(int timeoutMs);

    // True while stopping the process would cut into an install step or a restart that was promised.
    bool busy() const { return job_.installing() || !pendingRestart_.empty(); }
    // True when this agent has placed a newer version of itself and has told its controllers so, or has waited
    // long enough for their links to take it: the process is to end now, with kExitRestart (exit_status.hpp).
    bool restartDue();
    // True while this agent is a version on trial, which no controller has confirmed yet.
    bool onTrial() const { return onTrial_; }
    // For the end of the process: ends every link but the local socket's, each with link.closing and this reason
    // where `hello` is done on it, and comes back when every one of them has taken what the agent had to say, or
    // after `waitMs`. The links are closed by then, and the serial ones wait for their phones (serial_link.hpp).
    // The local socket's links are left: they end with the process.
    void endPhoneLinks(const char* reason, int waitMs);

    Engine& engine() { return engine_; }
    Job& job() { return job_; }
    PhoneStore& phones() { return phones_; }
    const std::string& headUnitId() const { return headUnitId_; }
    size_t sessions() const { return sessions_.size(); }
    // A blob.fetch whose reply or stream stays silent this long ends its link, of whatever kind. A serial link
    // that has gone silent altogether is ended sooner, by the keep-alive.
    void setFetchTimeoutMs(int64_t ms) { fetchTimeoutMs_ = ms; }
    // Where the agent's account of what it does goes, a line at a time: links that come and go, each request and
    // how it was answered, plans, what it fetches, and a job's steps, phases and log. 0 for nowhere. It says what
    // happened and is not part of the protocol: nothing may depend on its wording.
    void setLog(FILE* log) { log_ = log; }

    // PeerHandler
    virtual void onRequest(Peer* peer, int64_t id, const std::string& method, const json::Value& params);
    virtual void onResponse(Peer* peer, int64_t id, bool ok, const json::Value& result, const std::string& code,
                            const std::string& message);
    virtual void onEvent(Peer* peer, const std::string& event, int64_t seq, const json::Value& params);
    virtual void onData(Peer* peer, uint16_t stream, const std::string& bytes, bool last);

    // JobHost
    virtual void jobEvent(const std::string& name, int64_t seq, const std::string& params);
    virtual void jobIdle();
    virtual int64_t nowMs() { return clock_->nowMs(); }

private:
    Agent(const Agent&);
    Agent& operator=(const Agent&);

    // One controller on one link.
    // A request that has no answer yet, for the log.
    struct Asked {
        std::string method;
        int64_t atMs;
    };
    struct Session {
        Peer* peer;
        unsigned number;       // for the log
        bool greeted;          // `hello` is done
        bool serial;           // a Bluetooth serial link: the smaller frame, and the keep-alive
        bool local;            // the local socket: no `auth`, and the one link the pairing window is opened on
        bool trusted;          // may use every method: a local link once `hello` is done, another once `auth` is
        bool challenged;       // the first call of `auth` is answered, and the second is due
        uint8_t helloHashes[2 * kSha256Bytes];   // SHA-256 of the hello request's payload, then of the reply's
        uint8_t proofDue[kSha256Bytes];          // with `challenged`: P2,
        uint8_t keyUsed[kPairingKeyBytes];       // and the pairing key it was made with
        int64_t openedMs;
        int64_t heardMs;       // when anything last came over the link
        int64_t pingedMs;      // when the agent last sent `ping`
        unsigned pings;        // how many it has sent since it last heard anything
        bool ending;           // the agent has said link.closing, and the link closes once that is written out
        int64_t endingMs;      // when it said so
        std::string endingWhy; // and why, for the log
        std::string role, phoneId, name;
        std::map<int64_t, Asked> asked;

        Session()
            : peer(0), number(0), greeted(false), serial(false), local(false), trusted(false), challenged(false),
              openedMs(0), heardMs(0), pingedMs(0), pings(0), ending(false), endingMs(0) {}
    };
    // The pairing window.
    struct Pairing {
        bool open;
        Session* opener;        // the controller that shows the code
        std::string code;
        int64_t untilMs;        // when the code runs out
        unsigned wrong;         // wrong codes so far
        int64_t pausedUntilMs;  // after the last of them: no new window before this

        Pairing() : open(false), opener(0), untilMs(0), wrong(0), pausedUntilMs(0) {}
    };
    // The one blob.fetch that can be open at a time.
    struct Fetch {
        bool active;
        Session* session;
        int64_t requestId;
        bool forPlan;          // a manifest for the plan being made; otherwise a range for the job
        bool streaming;        // the reply has come, and DATA frames are due
        bool discard;          // the bytes are no longer wanted; they are dropped until the stream ends
        bool orphan;           // and nobody waits for the end either: the job it was for is over
        uint16_t stream;
        uint64_t asked;        // the length asked for; 0: to the end of the blob
        uint64_t expect, got;
        int64_t lastActivityMs;
        std::string failCode, failMessage;   // with `discard`: why
        std::string sha256;    // these two for the log
        int64_t startMs;

        Fetch() : active(false), session(0), requestId(0), forPlan(false), streaming(false), discard(false),
                  orphan(false), stream(0), asked(0), expect(0), got(0), lastActivityMs(0), startMs(0) {}
    };
    // A job.plan that is waiting for manifests.
    struct PlanBuild {
        Session* session;
        int64_t requestId;
        PlanRequest request;
        std::map<std::string, OfferedManifest> fetched;   // by manifestSha256
        size_t offer;          // the offer whose manifest is being fetched
        std::string bytes;
    };

    void note(const std::string& line);
    // The answer to a request, and its line in the log. Every answer goes through one of these two.
    void answer(Session* s, int64_t id, const std::string& result);
    void refuse(Session* s, int64_t id, const char* code, const std::string& message);
    void noteAnswer(Session* s, int64_t id, const std::string& outcome);
    void noteJobEvent(const std::string& name, const std::string& params);
    void serve(int timeoutMs);
    void keepAlive();
    // Ends a link on purpose: with link.closing first, and `reason` in it, where the link is a phone's own and
    // `hello` is done on it. `why` is for the log.
    void endLink(Session* s, const char* reason, const std::string& why);
    void endOverdue();
    void reap();
    void sessionClosed(Session* session);

    void hello(Session* s, int64_t id, const json::Value& params);
    void auth(Session* s, int64_t id, const json::Value& params);
    void authFailed(Session* s, int64_t id, const std::string& message);
    void replaceOlderLinks(Session* s);
    void pairingOpen(Session* s, int64_t id);
    void pairingClose(Session* s, int64_t id);
    void pairingIssue(Session* s, int64_t id, const json::Value& params);
    void closePairing(const char* reason);
    void expirePairing();
    void phonesList(Session* s, int64_t id);
    void phonesForget(Session* s, int64_t id, const json::Value& params);
    void dropLinksOf(const std::string& phoneId);
    void infoGet(Session* s, int64_t id);
    void packagesList(Session* s, int64_t id, const json::Value& params);
    void sourcesList(Session* s, int64_t id);
    void sourceAdd(Session* s, int64_t id, const json::Value& params);
    void sourceRemove(Session* s, int64_t id, const json::Value& params);
    void jobPlan(Session* s, int64_t id, const json::Value& params);
    void jobStart(Session* s, int64_t id, const json::Value& params);
    void jobCancel(Session* s, int64_t id, const json::Value& params);
    void jobStatus(Session* s, int64_t id, const json::Value& params);
    void systemRestart(Session* s, int64_t id, const json::Value& params);
    // Starting with the head unit (agent_home.hpp, src/agent_start.hpp). autostart.get says whether the agent is
    // started with the head unit, and whether it can be here; autostart.set switches that on or off, which is
    // the file `autostart` in the agent's folder. A person asks: a phone, or nbtctl. The iDrive plugin, which
    // does the starting, is not who decides it.
    void autostartGet(Session* s, int64_t id);
    void autostartSet(Session* s, int64_t id, const json::Value& params);
    // Whether something would start the agent with the head unit here if it were asked to. If not: the error's
    // code, what to say, and whether it is the plugin's package that is wanting.
    bool autostartAvailable(std::string* code, std::string* message, bool* plugin) const;
    std::string autostartState() const;

    void advancePlan();
    void driveJob();
    void afterJob();
    void startFetch(Session* session, bool forPlan, const std::string& sha256, uint64_t offset, uint64_t length);
    void endFetch(bool ok, std::string code, std::string message);
    void doRestart();
    // For a version on trial, asked for the job's status by a controller that counts.
    void confirmTrial(Session* s);
    std::string restartNeeded() const;
    void raiseRestartNeeded(const std::string& need);
    const TrustedKey* findKey(const std::string& hex) const;

    Platform* platform_;
    std::vector<TrustedKey> keys_;
    std::string agentVersion_;
    Clock* clock_;
    RandomSource* random_;
    Engine engine_;
    Planner planner_;
    Job job_;

    std::string headUnitId_;
    PhoneStore phones_;
    Pairing pairing_;
    std::vector<Listener*> listeners_;
    std::vector<Session*> sessions_;
    unsigned nextSession_;

    PlanBuild* building_;
    bool havePlan_;
    std::string planId_;
    Plan plan_;

    Fetch fetch_;
    Session* jobOwner_;        // the link the job fetches over; 0 once it has gone
    bool jobAccounted_;        // the ended job's restart need has been added to restartNeeded
    bool inStep_;              // an engine step is running further up the stack
    std::string pendingRestart_;     // "hmi" or "reboot": promised by system.restart, not done yet
    int64_t restartDeadlineMs_;
    bool onTrial_;                   // this version waits to be confirmed
    int64_t handOverDeadlineMs_;     // 0, or when the process ends for a newer version whatever the links hold
    int64_t fetchTimeoutMs_;
    FILE* log_;
    std::string loggedProgress_;                     // the state, step and phase the log last named
    std::map<std::string, std::string> logTails_;    // by source: a job's log text that has no line end yet
};

}  // namespace nbt

#endif
