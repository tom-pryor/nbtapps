// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The agent: see agent.hpp.

#include "agent.hpp"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "fsutil.hpp"
#include "hex.hpp"
#include "json_writer.hpp"
#include "semver.hpp"
#include "sha256.hpp"
#include "utf8.hpp"

namespace nbt {

namespace {

const size_t kMaxListed = 100;                // packages in one packages.list reply
const size_t kEnvelopeRoom = 64;              // bytes of a response around its result
const int64_t kFetchTimeoutMs = 60000;
const int64_t kRestartFlushMs = 2000;         // how long a restart waits for its answer to be written out
const int64_t kClosingFlushMs = 2000;         // how long a link that is told link.closing has to take the event
const size_t kMaxPhoneName = 64;

std::string number(uint64_t value) {
    char text[32];
    snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    return text;
}

bool isHex(const std::string& text, size_t chars) {
    if (text.size() != chars) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

}  // namespace

int64_t SystemClock::nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t Clock::unixSeconds() { return (int64_t)time(0); }

bool SystemRandom::secret(uint8_t* out, size_t len) {
    // Not blocking: a device that has nothing to give yet must not hold the agent's one thread.
    int fd = open("/dev/urandom", O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, out + got, len - got);
        if (n > 0) got += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    close(fd);
    if (got != len) memset(out, 0, len);
    return got == len;
}

void SystemRandom::bytes(uint8_t* out, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    while (fd >= 0 && got < len) {
        ssize_t n = read(fd, out + got, len - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    if (fd >= 0) close(fd);
    static uint64_t counter = 0;
    while (got < len) {
        struct timespec now, up;
        clock_gettime(CLOCK_REALTIME, &now);
        clock_gettime(CLOCK_MONOTONIC, &up);
        Sha256 hash;
        pid_t pid = getpid();
        ++counter;
        hash.update(&now, sizeof now);
        hash.update(&up, sizeof up);
        hash.update(&pid, sizeof pid);
        hash.update(&counter, sizeof counter);
        uint8_t digest[kSha256Bytes];
        hash.finish(digest);
        size_t take = len - got < sizeof digest ? len - got : sizeof digest;
        memcpy(out + got, digest, take);
        got += take;
    }
}

Agent::Agent(Platform* platform, const std::vector<TrustedKey>& keys, const std::string& agentVersion, Clock* clock,
             RandomSource* random)
    : platform_(platform), keys_(keys), agentVersion_(agentVersion), clock_(clock), random_(random),
      engine_(platform, keys, agentVersion), planner_(&engine_), job_(&engine_, this), nextSession_(1), building_(0),
      havePlan_(false), jobOwner_(0), jobAccounted_(true), inStep_(false), restartDeadlineMs_(0), onTrial_(false),
      handOverDeadlineMs_(0), fetchTimeoutMs_(kFetchTimeoutMs), log_(stderr) {}

Agent::~Agent() {
    delete building_;
    for (size_t i = 0; i < sessions_.size(); ++i) {
        delete sessions_[i]->peer;
        delete sessions_[i];
    }
}

const char kLogPathVariable[] = "NBTAGENT_LOG";

namespace {

// logLine is called from the agent's thread and, in the head unit's build, from the serial links' threads.
pthread_mutex_t gLogLock = PTHREAD_MUTEX_INITIALIZER;
std::string gLogPath;            // the file standard error is written to, once capLog has taken it
uint64_t gLogCap = 0;            // 0: standard error is not a file that is kept short
bool gLogWithStdout = false;     // standard output goes to the same file

bool sameFile(const struct stat& a, const struct stat& b) { return a.st_dev == b.st_dev && a.st_ino == b.st_ino; }

void writeLine(FILE* to, const std::string& line) {
    struct timeval now;
    gettimeofday(&now, 0);
    time_t seconds = now.tv_sec;
    struct tm when;
    localtime_r(&seconds, &when);
    // A phone's name, a method's name and what a script prints all end up here.
    std::string clean = line;
    for (size_t i = 0; i < clean.size(); ++i) {
        unsigned char c = (unsigned char)clean[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) clean[i] = '?';
    }
    fprintf(to, "%02d:%02d:%02d.%03d nbtagent: %s\n", when.tm_hour, when.tm_min, when.tm_sec, (int)(now.tv_usec / 1000),
            clean.c_str());
    fflush(to);
}

// With the lock held, after a line went to standard error.
void keepLogShort() {
    struct stat file, named;
    if (gLogCap == 0 || fstat(STDERR_FILENO, &file) != 0 || (uint64_t)file.st_size <= gLogCap) return;
    if (gLogWithStdout) fflush(stdout);
    std::string older = gLogPath + ".1";
    bool setAside = false;
    // Only while the name still leads to the file: a launcher that was started beside this agent's own has
    // given the name to another file.
    if (stat(gLogPath.c_str(), &named) == 0 && sameFile(named, file) && rename(gLogPath.c_str(), older.c_str()) == 0) {
        int fd = open(gLogPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            if (gLogWithStdout) dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
            setAside = true;
        } else {
            rename(older.c_str(), gLogPath.c_str());
        }
    }
    if (!setAside) {
        // What it held is lost, and the file stays short.
        if (ftruncate(STDERR_FILENO, 0) != 0) return;
        lseek(STDERR_FILENO, 0, SEEK_SET);
    }
    char size[32];
    snprintf(size, sizeof size, "%u", (unsigned)(gLogCap / 1024));
    writeLine(stderr, std::string("the log had passed ") + size + " KB and was begun again; " +
                          (setAside ? "what it held is in " + older : "what it held could not be kept"));
}

}  // namespace

void logLine(FILE* to, const std::string& line) {
    if (to == 0) return;
    pthread_mutex_lock(&gLogLock);
    writeLine(to, line);
    if (to == stderr) keepLogShort();
    pthread_mutex_unlock(&gLogLock);
}

bool capLog(const std::string& path, uint64_t capBytes) {
    struct stat named, err, out;
    if (path.empty() || capBytes == 0 || stat(path.c_str(), &named) != 0 || !S_ISREG(named.st_mode) ||
        fstat(STDERR_FILENO, &err) != 0 || !sameFile(named, err)) {
        return false;
    }
    pthread_mutex_lock(&gLogLock);
    gLogPath = path;
    gLogCap = capBytes;
    gLogWithStdout = fstat(STDOUT_FILENO, &out) == 0 && sameFile(named, out);
    pthread_mutex_unlock(&gLogLock);
    return true;
}

void Agent::note(const std::string& line) { logLine(log_, line); }

namespace {

std::string linkName(unsigned link) { return "link " + number(link); }

// "1 step", "2 steps".
std::string countOf(uint64_t count, const char* what) { return number(count) + " " + what + (count == 1 ? "" : "s"); }

// The start of a hash: enough to tell one archive from another in the log.
std::string shortHash(const std::string& sha256) { return sha256.substr(0, 12) + "..."; }

std::string stepText(const std::string& action, const std::string& id, const std::string& version, const std::string& oldVersion) {
    return action + " " + id + " " + version + (oldVersion.empty() ? "" : " (from " + oldVersion + ")");
}

}  // namespace

void Agent::noteAnswer(Session* s, int64_t id, const std::string& outcome) {
    std::string method = "a request", took;
    std::map<int64_t, Asked>::iterator asked = s->asked.find(id);
    if (asked != s->asked.end()) {
        method = asked->second.method;
        took = " (" + number((uint64_t)(clock_->nowMs() - asked->second.atMs)) + " ms)";
        s->asked.erase(asked);
    }
    note(linkName(s->number) + ": " + method + " -> " + outcome + took);
}

void Agent::answer(Session* s, int64_t id, const std::string& result) {
    s->peer->respond(id, result);
    noteAnswer(s, id, "ok");
}

void Agent::refuse(Session* s, int64_t id, const char* code, const std::string& message) {
    s->peer->respondError(id, code, message);
    noteAnswer(s, id, std::string(code) + ": " + message);
}

// A job's events, as lines. They are made for controllers, as JSON; the log reads them back so that it shows what
// a controller was told, and the job needs no second way of saying it.
void Agent::noteJobEvent(const std::string& name, const std::string& params) {
    if (log_ == 0) return;
    json::Document doc;
    if (!doc.parse(params, 0)) return;
    json::Value p = doc.root();
    if (name == "job.log") {
        json::Value source = p.find("source"), text = p.find("text"), step = p.find("step");
        if (!source.isString() || !text.isString()) return;
        std::string prefix = "job, step " + (step.isInteger() ? number((uint64_t)step.integer()) : std::string("?")) + ": " +
                             source.string() + "| ";
        // Text comes in pieces that need not end where a line does.
        std::string& tail = logTails_[source.string()];
        tail += text.string();
        size_t at = 0;
        for (size_t end = tail.find('\n'); end != std::string::npos; end = tail.find('\n', at)) {
            note(prefix + tail.substr(at, end - at));
            at = end + 1;
        }
        tail.erase(0, at);
        if (tail.size() > 1024) {
            note(prefix + tail);
            tail.clear();
        }
    } else if (name == "job.progress") {
        // Many of these only count bytes. The log names a state, a step or a phase once, when it begins.
        json::Value state = p.find("state"), step = p.find("step"), steps = p.find("steps"), id = p.find("id"), phase = p.find("phase");
        if (!state.isString() || !step.isInteger() || !steps.isInteger() || !id.isString()) return;
        std::string line = state.string() + ", step " + number((uint64_t)step.integer()) + " of " +
                           number((uint64_t)steps.integer()) + " (" + id.string() + ")";
        if (phase.isString()) line += ": " + phase.string();
        if (line == loggedProgress_) return;
        loggedProgress_ = line;
        note("job " + line);
    } else if (name == "job.done") {
        for (std::map<std::string, std::string>::iterator it = logTails_.begin(); it != logTails_.end(); ++it) {
            if (!it->second.empty()) note("job: " + it->first + "| " + it->second);
        }
        logTails_.clear();
        loggedProgress_.clear();
        json::Value planId = p.find("planId"), state = p.find("state"), steps = p.find("steps"), restart = p.find("restart");
        if (!planId.isString() || !state.isString() || !steps.isArray()) return;
        note("job " + planId.string() + " " + state.string() + (restart.isString() ? ", restart needed: " + restart.string() : ""));
        for (size_t i = 0; i < steps.size(); ++i) {
            json::Value one = steps.at(i), error = one.find("error");
            if (!one.find("action").isString() || !one.find("id").isString() || !one.find("version").isString() ||
                !one.find("result").isString()) continue;
            std::string line = "  " + stepText(one.find("action").string(), one.find("id").string(), one.find("version").string(), "") +
                               ": " + one.find("result").string();
            if (error.find("code").isString() && error.find("message").isString()) {
                line += " (" + error.find("code").string() + ": " + error.find("message").string() + ")";
            }
            note(line);
        }
    }
}

bool Agent::init(std::string* error) {
    if (!engine_.init(error)) return false;

    // The run folder does not outlast a power cut, so a mark in it tells an agent that was started again from a
    // head unit that was.
    std::string bootMark = platform_->runDir() + "/agent-started";
    bool sameBoot = fs::kindOf(bootMark) == fs::kFile;
    if (!sameBoot) {
        // Staging may have kept its files through the power cut. They were not synced, and the job they belong to
        // does not go on, so they go: a fetch never continues from before a power down.
        std::string why;
        if (!engine_.clearStaging(&why)) {
            if (error != 0) *error = "staging cannot be emptied: " + why;
            return false;
        }
    }
    fs::writeFile(bootMark, "", 0600, 0);

    // What the agent's own folder says before anything is settled: recover() clears an update that is decided.
    TrialState trial = engine_.trialState();
    bool recovered = false;
    StepReport report;
    if (!engine_.recover(&recovered, &report, 0)) {
        if (error != 0) *error = "the store cannot be read: " + report.error.message;
        return false;
    }
    onTrial_ = engine_.trialState() == kOnTrial;
    if (onTrial_) {
        note("this version of the agent, " + agentVersion_ + ", is on trial. It stays once a phone has connected and asked for the "
             "job's status; if none does before the agent or the head unit stops, the launcher starts the version before it again");
    } else if (recovered && report.id == kAgentPackageId && report.action != "remove") {
        note("the update of the agent to " + report.version + " was " +
             (report.outcome == kStepDone ? "finished: it is confirmed"
                                          : report.outcome == kStepFailed ? "undone: " + report.error.message : "left BROKEN: " + report.error.message));
    } else if (recovered) {
        note("an interrupted " + report.action + " of " + report.id + " was " +
             (report.outcome == kStepDone ? "finished" : report.outcome == kStepFailed ? "undone" : "left BROKEN: " + report.error.message));
    } else if (trial == kWayBack || trial == kConfirmed) {
        note(trial == kWayBack ? "what an update of the agent left that was not confirmed has been cleared away"
                               : "what a confirmed update of the agent left has been cleared away");
    }
    if (engine_.trialState() == kNotOurs) note("an update of the agent is under way between other versions than this one, " + agentVersion_ + ": it is left alone");
    // The starter this version came with, if this is the version that is decided and another one is in place.
    std::string starter;
    engine_.settleStarter(&starter);
    if (!starter.empty()) note(starter);
    if (!job_.load(recovered, report, error)) return false;
    if (job_.exists()) note("the last job on record, " + job_.planId() + ": " + job_.state());
    // A job that waits for this version to be confirmed is still to end, and to be accounted for then.
    if (job_.restarting()) jobAccounted_ = false;
    // A job that was settled just now may have left packages that wait for a restart. After a power cut the head
    // unit has restarted since; after a crash of the agent alone it has not.
    if (job_.settledAtLoad() && sameBoot) raiseRestartNeeded(job_.restart());

    std::string idPath = platform_->stateDir() + "/head-unit-id";
    std::string text;
    if (fs::readFile(idPath, 64, &text, 0) && text.size() >= 32 && isHex(text.substr(0, 32), 32)) {
        headUnitId_ = text.substr(0, 32);
    } else {
        uint8_t raw[16];
        random_->bytes(raw, sizeof raw);
        headUnitId_ = toHex(raw, sizeof raw);
        if (!fs::writeFile(idPath, headUnitId_ + "\n", 0600, error)) return false;
    }

    // A record of the paired phones that cannot be read stops no start: the phones pair again.
    std::string problem;
    if (!phones_.load(platform_->stateDir() + "/phones.json", &problem)) {
        note("the record of paired phones cannot be used, so no phone is paired: " + problem);
    }
    note(countOf(phones_.all().size(), "phone") + " paired");
    return true;
}

void Agent::addListener(Listener* listener) { listeners_.push_back(listener); }

void Agent::adopt(Link* link) {
    Session* s = new Session();
    s->number = nextSession_++;
    s->peer = new Peer(link, false, this);
    s->peer->user = s;
    s->serial = strcmp(s->peer->linkKind(), "serial") == 0;
    s->local = strcmp(s->peer->linkKind(), "local") == 0;
    s->openedMs = s->heardMs = s->pingedMs = clock_->nowMs();
    sessions_.push_back(s);
    note(linkName(s->number) + " opened (" + s->peer->linkKind() + ")");
}

// ---- the loop -----------------------------------------------------------------------------------------------------

void Agent::serve(int timeoutMs) {
    fd_set readable, writable;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    int top = -1;
    for (size_t i = 0; i < listeners_.size(); ++i) {
        int fd = listeners_[i]->fd();
        if (fd < 0 || fd >= FD_SETSIZE) continue;
        FD_SET(fd, &readable);
        if (fd > top) top = fd;
    }
    for (size_t i = 0; i < sessions_.size(); ++i) {
        Peer* peer = sessions_[i]->peer;
        if (!peer->open()) continue;
        int fd = peer->fd();
        if (fd < 0 || fd >= FD_SETSIZE) continue;
        // A link that closes once its last words are out is not read any more, so it is not waited on for that.
        if (!peer->closing()) FD_SET(fd, &readable);
        if (peer->wantsWrite()) FD_SET(fd, &writable);
        if (fd > top) top = fd;
    }
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int ready = select(top + 1, &readable, &writable, 0, &tv);
    if (ready <= 0) return;

    for (size_t i = 0; i < listeners_.size(); ++i) {
        int fd = listeners_[i]->fd();
        if (fd < 0 || fd >= FD_SETSIZE || !FD_ISSET(fd, &readable)) continue;
        for (Link* link = listeners_[i]->accept(); link != 0; link = listeners_[i]->accept()) adopt(link);
    }
    // The sessions of this moment: a handler may add one (never remove one; that is reap()'s work).
    size_t count = sessions_.size();
    for (size_t i = 0; i < count; ++i) {
        Peer* peer = sessions_[i]->peer;
        if (!peer->open()) continue;
        int fd = peer->fd();
        if (fd < 0 || fd >= FD_SETSIZE) continue;
        if (FD_ISSET(fd, &writable)) peer->flush();
        if (peer->open() && FD_ISSET(fd, &readable) && peer->readable()) {
            sessions_[i]->heardMs = clock_->nowMs();
            sessions_[i]->pings = 0;
        }
    }
}

// What a serial link gets that the others do not: see agent.hpp. Called when the links have just been read, and
// only then, so that the agent does not take its own time away from them for a phone's silence. It closes links
// and leaves them for reap().
void Agent::keepAlive() {
    int64_t now = clock_->nowMs();
    for (size_t i = 0; i < sessions_.size(); ++i) {
        Session* s = sessions_[i];
        if (!s->serial || !s->peer->open() || s->ending) continue;
        if (!s->greeted) {
            if (now - s->openedMs >= kSerialSilenceMs) s->peer->close("no hello in " + number((uint64_t)(kSerialSilenceMs / 1000)) + " s");
            continue;
        }
        int64_t silent = now - s->heardMs;
        if (silent < kSerialPingAfterMs || now - s->pingedMs < kSerialPingAfterMs) continue;
        // Every ping that fits into the silence allowed has to have gone out first: an agent that comes back
        // from a long part of an install step asks before it takes the link for dead.
        if (silent >= kSerialSilenceMs && s->pings >= (unsigned)(kSerialSilenceMs / kSerialPingAfterMs) - 1) {
            endLink(s, kClosingSilent, "nothing heard for " + number((uint64_t)(silent / 1000)) + " s, and no answer to ping");
        } else {
            // Not in the log, and neither is its answer: an idle link would fill it.
            s->pingedMs = now;
            ++s->pings;
            s->peer->request("ping", "{}");
        }
    }
}

// See agent.hpp, "How the agent ends a link on purpose". It closes the link, or sets it to close, and leaves it for
// reap().
void Agent::endLink(Session* s, const char* reason, const std::string& why) {
    Peer* peer = s->peer;
    if (!peer->open()) return;
    // The local socket's links have no node behind them, and a link without `hello` has agreed no frame size and
    // may be no controller's at all.
    if (s->local || !s->greeted) {
        peer->close(why);
        return;
    }
    if (!s->ending) {
        json::Writer w;
        w.beginObject().key("reason").string(reason).endObject();
        // Not sent on a link that is closing already, after an error that said why.
        if (peer->event("link.closing", -1, w.text())) note(linkName(s->number) + ": told link.closing, " + reason);
        s->ending = true;
        s->endingMs = clock_->nowMs();
        s->endingWhy = why;
    }
    peer->closeWhenFlushed(why);
}

// A link that was told link.closing and has not taken the event in kClosingFlushMs is closed all the same.
void Agent::endOverdue() {
    int64_t now = clock_->nowMs();
    for (size_t i = 0; i < sessions_.size(); ++i) {
        Session* s = sessions_[i];
        if (s->ending && s->peer->open() && now - s->endingMs >= kClosingFlushMs)
            s->peer->close(s->endingWhy + "; the link did not take link.closing");
    }
}

void Agent::endPhoneLinks(const char* reason, int waitMs) {
    const std::string why = std::string("the agent is ") + reason;
    bool any = false;
    for (size_t i = 0; i < sessions_.size(); ++i) {
        Session* s = sessions_[i];
        if (s->local || !s->peer->open()) continue;
        endLink(s, reason, why);
        any = true;
    }
    if (!any) return;
    // Until each has taken what was queued for it. The time is counted in waits, so that it also passes under a
    // clock that a test holds still.
    const int kWaitMs = 20;
    for (int waited = 0; waited < waitMs; waited += kWaitMs) {
        fd_set writable;
        FD_ZERO(&writable);
        int top = -1;
        for (size_t i = 0; i < sessions_.size(); ++i) {
            Peer* peer = sessions_[i]->peer;
            if (sessions_[i]->local || !peer->open()) continue;
            int fd = peer->fd();
            if (fd < 0 || fd >= FD_SETSIZE) continue;
            FD_SET(fd, &writable);
            if (fd > top) top = fd;
        }
        if (top < 0) break;
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = kWaitMs * 1000;
        if (select(top + 1, 0, &writable, 0, &tv) <= 0) continue;
        for (size_t i = 0; i < sessions_.size(); ++i) {
            Peer* peer = sessions_[i]->peer;
            if (sessions_[i]->local || !peer->open()) continue;
            int fd = peer->fd();
            if (fd >= 0 && fd < FD_SETSIZE && FD_ISSET(fd, &writable)) peer->flush();
        }
    }
    for (size_t i = 0; i < sessions_.size(); ++i) {
        Session* s = sessions_[i];
        if (!s->local && s->peer->open()) s->peer->close(why + "; the link did not take link.closing");
    }
    reap();
}

void Agent::reap() {
    for (size_t i = 0; i < sessions_.size();) {
        Session* s = sessions_[i];
        if (s->peer->open()) {
            ++i;
            continue;
        }
        sessions_.erase(sessions_.begin() + (long)i);
        note(linkName(s->number) + " ended: " + s->peer->closeReason());
        sessionClosed(s);
        delete s->peer;
        delete s;
    }
}

void Agent::sessionClosed(Session* s) {
    if (building_ != 0 && building_->session == s) {
        delete building_;
        building_ = 0;
    }
    if (fetch_.active && fetch_.session == s) fetch_ = Fetch();
    // Nobody shows the code any more.
    if (pairing_.open && pairing_.opener == s) closePairing(0);
    if (jobOwner_ == s) {
        jobOwner_ = 0;
        // The archives came over this link. The job waits until a controller starts it again.
        job_.pause();
    }
}

void Agent::turn(int timeoutMs) {
    if (job_.installing()) timeoutMs = 0;
    else if ((!pendingRestart_.empty() || job_.handingOver()) && timeoutMs > 20) timeoutMs = 20;
    serve(timeoutMs);
    keepAlive();
    endOverdue();
    expirePairing();
    reap();
    if (fetch_.active && clock_->nowMs() - fetch_.lastActivityMs > fetchTimeoutMs_) {
        endLink(fetch_.session, kClosingSilent, "no answer to blob.fetch");
        reap();
    }
    if (job_.installing()) {
        inStep_ = true;
        job_.runStep();
        inStep_ = false;
        reap();
        if (job_.handingOver()) handOverDeadlineMs_ = clock_->nowMs() + kRestartFlushMs;
    } else {
        driveJob();
    }
    // A plan that had to wait for a stream nobody wants any more to run out.
    if (building_ != 0 && !fetch_.active) advancePlan();
    afterJob();
    if (!pendingRestart_.empty()) doRestart();
}

void Agent::jobIdle() {
    serve(0);
    keepAlive();
    endOverdue();
    expirePairing();
    // Not reap(): a link that ended is cleared away by turn(), once the step is over. Nothing fetches during a
    // step, so nothing waits for it.
}

void Agent::jobEvent(const std::string& name, int64_t seq, const std::string& params) {
    noteJobEvent(name, params);
    for (size_t i = 0; i < sessions_.size(); ++i) {
        Session* s = sessions_[i];
        // An event too large for a link's frame is not sent there; job.status still has what the job came to.
        // A link on which `auth` is not done hears nothing of a job.
        if (s->trusted && s->peer->open()) s->peer->event(name, seq, params);
    }
}

void Agent::afterJob() {
    if (jobAccounted_ || !job_.ended()) return;
    jobAccounted_ = true;
    jobOwner_ = 0;
    raiseRestartNeeded(job_.restart());
}

// ---- the agent's own update ---------------------------------------------------------------------------------------

bool Agent::restartDue() {
    if (!job_.handingOver()) return false;
    // The controllers are told that the job is restarting before the process goes, as far as their links take it.
    bool flushed = true;
    for (size_t i = 0; i < sessions_.size(); ++i) flushed = flushed && !sessions_[i]->peer->wantsWrite();
    return flushed || clock_->nowMs() >= handOverDeadlineMs_;
}

void Agent::confirmTrial(Session* s) {
    std::string why;
    if (!engine_.confirmTrial(&why)) {
        // Still on trial: the next job.status tries again.
        note(linkName(s->number) + ": this version of the agent could not be confirmed: " + why);
        return;
    }
    onTrial_ = false;
    note(linkName(s->number) + ": a controller has reached this version of the agent, " + agentVersion_ +
         ". It is confirmed, and is the one the launcher starts from now on");
    // The step that placed this version gets its commit line now, and is finished.
    bool recovered = false;
    StepReport report;
    engine_.recover(&recovered, &report, 0);
    if (recovered && report.outcome != kStepDone) {
        note("the step that placed this version could not be finished: " + report.error.message + ". The next start tries again");
    }
    // This version is the one that stays, so its starter is the one to have in place.
    std::string starter;
    engine_.settleStarter(&starter);
    if (!starter.empty()) note(starter);
    job_.settleRestart(recovered, report);
}

// ---- starting with the head unit ----------------------------------------------------------------------------------

bool Agent::autostartAvailable(std::string* code, std::string* message, bool* plugin) const {
    *plugin = false;
    AgentHome home(platform_->agentHome());
    if (!home.hasStarter()) {
        *code = "no-launcher";
        *message = "no starter is in " + home.dir() + ": this agent was not put here for one to start, with the head unit or otherwise";
        return false;
    }
    *plugin = true;
    if (!engine_.store().isInstalled(kPluginPackageId)) {
        *code = "plugin-missing";
        *message = std::string("the iDrive plugin, ") + kPluginPackageId + ", is what starts the agent with the head unit, and it is not installed";
        return false;
    }
    InstalledPackage installed;
    std::string why;
    if (!engine_.store().load(kPluginPackageId, &installed, &why)) {
        *plugin = false;
        *code = "internal";
        *message = "the record of the iDrive plugin cannot be read: " + why;
        return false;
    }
    if (!versionInRange(installed.manifest.version, kAutostartPluginRange)) {
        *code = "plugin-too-old";
        *message = "the iDrive plugin that is installed is version " + installed.manifest.version +
                   ", and starting the agent with the head unit takes " + kAutostartPluginRange;
        return false;
    }
    return true;
}

std::string Agent::autostartState() const {
    AgentHome home(platform_->agentHome());
    std::string code, message;
    bool plugin = false, available = autostartAvailable(&code, &message, &plugin);
    json::Writer w;
    w.beginObject().key("on").boolean(home.autostart()).key("available").boolean(available);
    if (!available) {
        w.key("reason").string(code);
        if (plugin) {
            w.key("needs").beginObject().key("id").string(kPluginPackageId).key("version").string(kAutostartPluginRange).endObject();
        }
    }
    w.key("starts").integer((int64_t)home.autostartStarts()).endObject();
    return w.text();
}

void Agent::autostartGet(Session* s, int64_t id) { answer(s, id, autostartState()); }

void Agent::autostartSet(Session* s, int64_t id, const json::Value& params) {
    json::Value on = params.find("on");
    if (!on.isBool() || params.size() != 1) {
        refuse(s, id, "bad-request", "autostart.set takes on: true or false");
        return;
    }
    if (s->role == "plugin") {
        refuse(s, id, "forbidden", "whether the agent is started with the head unit is decided from a phone or with nbtctl, not by the plugin that starts it");
        return;
    }
    if (onTrial_) {
        refuse(s, id, "busy", "this version of the agent is on trial, and changes nothing in its folder until it is confirmed");
        return;
    }
    std::string code, message, why;
    bool plugin = false;
    if (on.boolean() && !autostartAvailable(&code, &message, &plugin)) {
        refuse(s, id, code.c_str(), message);
        return;
    }
    AgentHome home(platform_->agentHome());
    if (!home.setAutostart(on.boolean(), &why)) {
        refuse(s, id, "io-error", std::string("the file ") + kAutostartSwitch + " could not be " + (on.boolean() ? "written: " : "removed: ") + why);
        return;
    }
    // Asked for anew: the starts on record are from before, and would keep the plugin from starting anything.
    if (on.boolean()) home.forgetAutostarts();
    note(linkName(s->number) + ": the agent is " + (on.boolean() ? "now" : "no longer") + " started with the head unit (" +
         home.dir() + "/" + kAutostartSwitch + ")");
    answer(s, id, autostartState());
}

// ---- restartNeeded ------------------------------------------------------------------------------------------------

// Whether an installed package still waits for a restart is kept in the run folder: a power cut clears that
// folder, and it also is the restart.
std::string Agent::restartNeeded() const {
    std::string text;
    if (!fs::readFile(platform_->runDir() + "/restart-needed", 64, &text, 0)) return "none";
    if (text.compare(0, 8, "required") == 0) return "required";
    if (text.compare(0, 11, "hmi-restart") == 0) return "hmi-restart";
    return "none";
}

void Agent::raiseRestartNeeded(const std::string& need) {
    std::string now = restartNeeded();
    std::string both = strongerRestart(now, need);
    if (both != now) fs::writeFile(platform_->runDir() + "/restart-needed", both + "\n", 0600, 0);
}

void Agent::doRestart() {
    bool flushed = true;
    for (size_t i = 0; i < sessions_.size(); ++i) flushed = flushed && !sessions_[i]->peer->wantsWrite();
    if (!flushed && clock_->nowMs() < restartDeadlineMs_) return;
    std::string kind = pendingRestart_;
    pendingRestart_.clear();
    std::string why;
    bool ok = kind == "reboot" ? platform_->reboot(&why) : platform_->restartHmi(&why);
    if (!ok) {
        note("the restart (" + kind + ") failed: " + why);
        return;
    }
    note(kind == "reboot" ? "the platform was told to reboot" : "the platform was told to restart the HMI");
    // If the agent is still running after this, the platform's restart did not take it down with it (the HMI
    // restarts without the agent; a build host restarts nothing).
    std::string need = restartNeeded();
    if (kind == "reboot" || need == "hmi-restart") fs::removeFile(platform_->runDir() + "/restart-needed", 0);
}

// ---- requests -----------------------------------------------------------------------------------------------------

void Agent::onRequest(Peer* peer, int64_t id, const std::string& method, const json::Value& params) {
    Session* s = (Session*)peer->user;
    if (method == "ping" && s->greeted) {
        // Not in the log: an idle link asks every few seconds.
        peer->respond(id, "{}");
        return;
    }
    Asked asked;
    asked.method = sanitizeUtf8(method.substr(0, 64), 0);
    asked.atMs = clock_->nowMs();
    s->asked[id] = asked;
    if (method == "hello") {
        if (s->greeted) refuse(s, id, "bad-state", "hello was already done on this link");
        else hello(s, id, params);
        return;
    }
    if (!s->greeted) {
        refuse(s, id, "bad-state", "hello comes first");
        return;
    }
    // The two that a link may ask before it has shown whose it is.
    if (method == "auth") {
        auth(s, id, params);
        return;
    }
    if (method == "pairing.issue") {
        pairingIssue(s, id, params);
        return;
    }
    if (!s->trusted) {
        refuse(s, id, "unauthenticated", "auth comes first on this link");
        return;
    }
    if (method == "info.get") infoGet(s, id);
    else if (method == "pairing.open") pairingOpen(s, id);
    else if (method == "pairing.close") pairingClose(s, id);
    else if (method == "phones.list") phonesList(s, id);
    else if (method == "phones.forget") phonesForget(s, id, params);
    else if (method == "packages.list") packagesList(s, id, params);
    else if (method == "sources.list") sourcesList(s, id);
    else if (method == "source.add") sourceAdd(s, id, params);
    else if (method == "source.remove") sourceRemove(s, id, params);
    else if (method == "job.plan") jobPlan(s, id, params);
    else if (method == "job.start") jobStart(s, id, params);
    else if (method == "job.cancel") jobCancel(s, id, params);
    else if (method == "job.status") jobStatus(s, id, params);
    else if (method == "system.restart") systemRestart(s, id, params);
    else if (method == "autostart.get") autostartGet(s, id);
    else if (method == "autostart.set") autostartSet(s, id, params);
    else refuse(s, id, "unsupported", "this agent has no method " + sanitizeUtf8(method.substr(0, 64), 0));
}

void Agent::hello(Session* s, int64_t id, const json::Value& params) {
    Peer* peer = s->peer;
    json::Value min = params.find("protocolMin"), max = params.find("protocolMax"), role = params.find("role");
    json::Value maxFrame = params.find("maxFrame"), capabilities = params.find("capabilities");
    json::Value phoneId = params.find("phoneId"), name = params.find("name");
    if (!min.isInteger() || !max.isInteger() || min.integer() < 1 || max.integer() < min.integer()) {
        refuse(s, id, "bad-request", "protocolMin and protocolMax are integers from 1, the first not above the second");
        return;
    }
    if (!role.isString() || (role.string() != "phone" && role.string() != "plugin" && role.string() != "cli")) {
        refuse(s, id, "bad-request", "role is phone, plugin or cli");
        return;
    }
    if (!maxFrame.isInteger() || maxFrame.integer() < (int64_t)kMinFramePayload || maxFrame.integer() > (int64_t)kMaxFramePayload) {
        refuse(s, id, "bad-request", "maxFrame is from 1024 to 65536");
        return;
    }
    if (!capabilities.isArray()) {
        refuse(s, id, "bad-request", "capabilities is an array");
        return;
    }
    for (size_t i = 0; i < capabilities.size(); ++i) {
        if (!capabilities.at(i).isString()) {
            refuse(s, id, "bad-request", "capabilities holds names");
            return;
        }
    }
    if (role.string() == "phone") {
        if (!phoneId.isString() || !isHex(phoneId.string(), 32)) {
            refuse(s, id, "bad-request", "a phone gives its phoneId, 32 lowercase hex characters");
            return;
        }
        if (!name.isString() || utf8Length(name.string()) < 1 || utf8Length(name.string()) > kMaxPhoneName) {
            refuse(s, id, "bad-request", "a phone gives its name, 1 to 64 characters");
            return;
        }
    }
    if (min.integer() > kProtocolVersion) {
        refuse(s, id, "unsupported-version", "this agent speaks protocol version 1 only");
        peer->closeWhenFlushed("no protocol version in common");
        return;
    }

    s->role = role.string();
    if (s->role == "phone") {
        s->phoneId = phoneId.string();
        s->name = name.string();
    }
    // Each side says the most it takes, and the lower of the two holds in both directions.
    uint32_t ours = s->serial ? kSerialMaxFrame : kMaxFramePayload;
    uint32_t agreed = (uint32_t)maxFrame.integer() < ours ? (uint32_t)maxFrame.integer() : ours;
    note(linkName(s->number) + ": hello from a " + s->role + (s->role == "phone" ? " called \"" + s->name + "\", id " + s->phoneId : "") +
         ", frames up to " + number(agreed) + " bytes");
    json::Writer w;
    w.beginObject().key("protocolMin").integer(kProtocolVersion).key("protocolMax").integer(kProtocolVersion);
    w.key("role").string("agent").key("headUnitId").string(headUnitId_).key("agentVersion").string(agentVersion_);
    // What this agent has beyond the protocol's first version: autostart.get and autostart.set, and installs
    // checked with the offer's signature.
    w.key("maxFrame").integer(ours).key("capabilities").beginArray().string(kCapabilityAutostart);
    w.string(kCapabilityDetachedSignature).endArray().endObject();
    // What `auth` signs: the two payloads as they travel. The reply's is what respond() sends for this result.
    const std::string& asked = peer->incoming();
    std::string reply = resultText(id, w.text());
    sha256(asked.data(), asked.size(), s->helloHashes);
    sha256(reply.data(), reply.size(), s->helloHashes + kSha256Bytes);
    answer(s, id, w.text());
    peer->setMaxFrame(agreed);
    s->greeted = true;
    if (s->local) {
        // Who may open the local socket is the head unit's own business: nothing more is asked.
        s->trusted = true;
        replaceOlderLinks(s);
    }
}

// A newer link from the same phone replaces the older one. Only a link that is trusted does that, and only to
// trusted ones: saying a phone's id in `hello` ends nobody's link.
void Agent::replaceOlderLinks(Session* s) {
    if (s->role != "phone") return;
    for (size_t i = 0; i < sessions_.size(); ++i) {
        Session* other = sessions_[i];
        if (other != s && other->trusted && other->phoneId == s->phoneId)
            endLink(other, kClosingReplaced, "replaced by a newer link from the same phone");
    }
}

// ---- auth ---------------------------------------------------------------------------------------------------------

namespace {

// HMAC-SHA256(key, label || transcript): see agent.hpp.
void proofOf(const uint8_t* key, const char* label, const uint8_t* transcript, size_t len, uint8_t out[kSha256Bytes]) {
    HmacSha256 mac(key, kPairingKeyBytes);
    mac.update(label, strlen(label));
    mac.update(transcript, len);
    mac.finish(out);
}

bool isDigits(const std::string& text, size_t chars) {
    if (text.size() != chars) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
    }
    return true;
}

}  // namespace

void Agent::authFailed(Session* s, int64_t id, const std::string& message) {
    s->challenged = false;
    memset(s->proofDue, 0, sizeof s->proofDue);
    memset(s->keyUsed, 0, sizeof s->keyUsed);
    refuse(s, id, "auth-failed", message);
    s->peer->closeWhenFlushed("auth failed");
}

void Agent::auth(Session* s, int64_t id, const json::Value& params) {
    if (s->local) {
        refuse(s, id, "forbidden", "the local socket has no auth");
        return;
    }
    if (s->trusted) {
        refuse(s, id, "bad-state", "auth is already done on this link");
        return;
    }
    json::Value nonce = params.find("nonce"), proof = params.find("proof");
    if (nonce.present() == proof.present() || (nonce.present() && (!nonce.isString() || !isHex(nonce.string(), 2 * kAuthNonceBytes))) ||
        (proof.present() && (!proof.isString() || !isHex(proof.string(), 2 * kSha256Bytes)))) {
        refuse(s, id, "bad-request", "auth takes a nonce of 32 hex characters, and then a proof of 64");
        return;
    }
    if (nonce.present()) {
        if (s->challenged) {
            refuse(s, id, "bad-state", "auth was given its nonce already; the proof is due");
            return;
        }
        const PairedPhone* phone = s->role == "phone" ? phones_.find(s->phoneId) : 0;
        if (phone == 0) {
            authFailed(s, id, s->role == "phone" ? "this head unit holds no pairing key for that phone" : "only a phone has a pairing key");
            return;
        }
        // T = A || B || the two hashes of hello.
        uint8_t transcript[2 * kAuthNonceBytes + 2 * kSha256Bytes];
        fromHex(nonce.string(), transcript, kAuthNonceBytes);
        if (!random_->secret(transcript + kAuthNonceBytes, kAuthNonceBytes)) {
            refuse(s, id, "internal", "this head unit has no random bytes to give");
            return;
        }
        memcpy(transcript + 2 * kAuthNonceBytes, s->helloHashes, sizeof s->helloHashes);
        uint8_t ours[kSha256Bytes];
        proofOf(phone->key, "nbt-auth-1", transcript, sizeof transcript, ours);
        proofOf(phone->key, "nbt-auth-2", transcript, sizeof transcript, s->proofDue);
        memcpy(s->keyUsed, phone->key, sizeof s->keyUsed);
        s->challenged = true;
        json::Writer w;
        w.beginObject().key("nonce").string(toHex(transcript + kAuthNonceBytes, kAuthNonceBytes));
        w.key("proof").string(toHex(ours, sizeof ours)).endObject();
        answer(s, id, w.text());
        return;
    }
    if (!s->challenged) {
        refuse(s, id, "bad-state", "auth takes its nonce first");
        return;
    }
    uint8_t given[kSha256Bytes];
    fromHex(proof.string(), given, sizeof given);
    // A proof made with a key that has since been replaced or forgotten is a wrong one.
    const PairedPhone* phone = phones_.find(s->phoneId);
    if (!constantTimeEqual(given, s->proofDue, sizeof given) || phone == 0 ||
        !constantTimeEqual(phone->key, s->keyUsed, sizeof s->keyUsed)) {
        authFailed(s, id, "the proof is not the one this phone's pairing key gives");
        return;
    }
    s->challenged = false;
    memset(s->proofDue, 0, sizeof s->proofDue);
    memset(s->keyUsed, 0, sizeof s->keyUsed);
    s->trusted = true;
    note(linkName(s->number) + ": \"" + s->name + "\" is the phone it was paired as");
    replaceOlderLinks(s);
    answer(s, id, "{}");
}

// ---- pairing ------------------------------------------------------------------------------------------------------

// Closes the window. With a reason the controller that opened it is told; without, it knows already or has gone.
void Agent::closePairing(const char* reason) {
    if (!pairing_.open) return;
    Session* opener = pairing_.opener;
    pairing_.open = false;
    pairing_.opener = 0;
    pairing_.code.clear();
    pairing_.wrong = 0;
    note(std::string("the pairing window closed") + (reason != 0 ? std::string(": ") + reason : std::string()));
    if (reason != 0 && opener != 0 && opener->peer->open()) {
        json::Writer w;
        w.beginObject().key("reason").string(reason).endObject();
        opener->peer->event("pairing.closed", -1, w.text());
    }
}

void Agent::expirePairing() {
    if (pairing_.open && clock_->nowMs() >= pairing_.untilMs) closePairing("expired");
}

void Agent::pairingOpen(Session* s, int64_t id) {
    if (!s->local) {
        refuse(s, id, "forbidden", "the pairing window is opened on the head unit itself");
        return;
    }
    expirePairing();
    int64_t now = clock_->nowMs();
    if (now < pairing_.pausedUntilMs) {
        refuse(s, id, "try-later", "wrong codes were given; no new code for another " +
                                       number((uint64_t)((pairing_.pausedUntilMs - now + 999) / 1000)) + " s");
        return;
    }
    // Every code is as likely as every other: a draw above the last whole run of codes is drawn again.
    uint32_t codes = 1;
    for (unsigned i = 0; i < kPairingCodeDigits; ++i) codes *= 10;
    const uint32_t limit = 0xFFFFFFFFu - 0xFFFFFFFFu % codes;
    uint32_t draw = limit;
    for (int tries = 0; tries < 16 && draw >= limit; ++tries) {
        uint8_t raw[4];
        if (!random_->secret(raw, sizeof raw)) break;
        draw = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
    }
    if (draw >= limit) {
        refuse(s, id, "internal", "this head unit has no random bytes to make a code of");
        return;
    }
    char code[16];
    snprintf(code, sizeof code, "%0*u", (int)kPairingCodeDigits, (unsigned)(draw % codes));
    // One window: a new one takes the place of the last, and another controller's screen is told its code is void.
    closePairing(pairing_.opener == s ? 0 : "replaced");
    pairing_.open = true;
    pairing_.opener = s;
    pairing_.code = code;
    pairing_.wrong = 0;
    pairing_.untilMs = now + kPairingWindowSeconds * 1000;
    note(linkName(s->number) + ": the pairing window is open for " + number((uint64_t)kPairingWindowSeconds) + " s");
    json::Writer w;
    w.beginObject().key("code").string(pairing_.code).key("expiresIn").integer(kPairingWindowSeconds).endObject();
    answer(s, id, w.text());
}

void Agent::pairingClose(Session* s, int64_t id) {
    if (!s->local) {
        refuse(s, id, "forbidden", "the pairing window is closed on the head unit itself");
        return;
    }
    // Another controller's window is left alone.
    if (pairing_.open && pairing_.opener == s) closePairing(0);
    answer(s, id, "{}");
}

void Agent::pairingIssue(Session* s, int64_t id, const json::Value& params) {
    if (s->local) {
        refuse(s, id, "forbidden", "a pairing key is issued to a phone, over its own link");
        return;
    }
    if (s->role != "phone") {
        refuse(s, id, "forbidden", "only a phone is given a pairing key");
        return;
    }
    json::Value code = params.find("code");
    if (!code.isString() || !isDigits(code.string(), kPairingCodeDigits)) {
        refuse(s, id, "bad-request", "pairing.issue takes a code of " + number(kPairingCodeDigits) + " digits");
        return;
    }
    expirePairing();
    if (!pairing_.open) {
        refuse(s, id, "forbidden", "no pairing window is open: it is opened on the car's screen");
        return;
    }
    if (!constantTimeEqual((const uint8_t*)code.string().data(), (const uint8_t*)pairing_.code.data(), kPairingCodeDigits)) {
        ++pairing_.wrong;
        note(linkName(s->number) + ": wrong pairing code, " + number(pairing_.wrong) + " of " + number(kPairingTries));
        refuse(s, id, "rejected", "that is not the code on the car's screen");
        if (pairing_.wrong >= kPairingTries) {
            pairing_.pausedUntilMs = clock_->nowMs() + kPairingPauseSeconds * 1000;
            closePairing("wrong-code");
        }
        return;
    }
    PairedPhone phone;
    phone.id = s->phoneId;
    phone.name = s->name;
    phone.pairedAt = clock_->unixSeconds();
    std::string why;
    if (!random_->secret(phone.key, sizeof phone.key)) {
        refuse(s, id, "internal", "this head unit has no random bytes to make a key of");
        return;
    }
    if (!phones_.put(phone, &why)) {
        refuse(s, id, "internal", "the pairing could not be recorded: " + why);
        return;
    }
    note(linkName(s->number) + ": paired the phone called \"" + s->name + "\", id " + s->phoneId);
    json::Writer w;
    w.beginObject().key("key").string(toHex(phone.key, sizeof phone.key)).endObject();
    answer(s, id, w.text());
    closePairing("issued");
}

// ---- the paired phones --------------------------------------------------------------------------------------------

void Agent::phonesList(Session* s, int64_t id) {
    if (!s->local) {
        refuse(s, id, "forbidden", "the paired phones are listed on the head unit itself");
        return;
    }
    // Never a key: no controller has a use for another phone's.
    json::Writer w;
    w.beginObject().key("phones").beginArray();
    const std::vector<PairedPhone>& all = phones_.all();
    for (size_t i = 0; i < all.size(); ++i) {
        w.beginObject().key("phoneId").string(all[i].id).key("name").string(all[i].name);
        w.key("pairedAt").integer(all[i].pairedAt).endObject();
    }
    w.endArray().endObject();
    answer(s, id, w.text());
}

// A phone that is no longer paired keeps no link.
void Agent::dropLinksOf(const std::string& phoneId) {
    for (size_t i = 0; i < sessions_.size(); ++i) {
        Session* other = sessions_[i];
        if (!other->local && other->trusted && other->phoneId == phoneId)
            endLink(other, kClosingForgotten, "its phone is no longer paired");
    }
}

void Agent::phonesForget(Session* s, int64_t id, const json::Value& params) {
    if (!s->local) {
        refuse(s, id, "forbidden", "a phone is forgotten on the head unit itself");
        return;
    }
    json::Value phoneId = params.find("phoneId"), all = params.find("all");
    bool everyone = all.isBool() && all.boolean();
    if ((phoneId.present() && (!phoneId.isString() || !isHex(phoneId.string(), 32))) || (all.present() && !all.isBool()) ||
        phoneId.present() == everyone) {
        refuse(s, id, "bad-request", "phones.forget takes a phoneId of 32 hex characters, or all: true");
        return;
    }
    std::vector<std::string> gone;
    if (everyone) {
        for (size_t i = 0; i < phones_.all().size(); ++i) gone.push_back(phones_.all()[i].id);
    } else if (phones_.find(phoneId.string()) != 0) {
        gone.push_back(phoneId.string());
    } else {
        refuse(s, id, "not-found", "no such phone is paired");
        return;
    }
    size_t count = 0;
    std::string why;
    if (!(everyone ? phones_.forgetAll(&count, &why) : phones_.forget(phoneId.string(), &count, &why))) {
        refuse(s, id, "internal", "the record of paired phones could not be written: " + why);
        return;
    }
    for (size_t i = 0; i < gone.size(); ++i) dropLinksOf(gone[i]);
    note(linkName(s->number) + ": " + countOf(count, "phone") + " forgotten");
    json::Writer w;
    w.beginObject().key("forgotten").integer((int64_t)count).endObject();
    answer(s, id, w.text());
}

void Agent::infoGet(Session* s, int64_t id) {
    struct {
        const char* name;
        std::string path;
    } places[] = {{"app", fs::parentOf(platform_->appDir("x"))},
                  {"data", fs::parentOf(platform_->dataDir("x"))},
                  {"plugins", platform_->pluginsDir()},
                  {"staging", platform_->stagingDir()}};
    json::Writer w;
    w.beginObject().key("agentVersion").string(agentVersion_).key("headUnitId").string(headUnitId_);
    w.key("manifestVersion").integer(kManifestVersion).key("free").beginObject();
    for (size_t i = 0; i < sizeof places / sizeof places[0]; ++i) {
        uint64_t free = 0;
        if (!platform_->freeBytes(places[i].path, &free)) free = 0;
        if (free > (uint64_t)json::kMaxInteger) free = (uint64_t)json::kMaxInteger;
        w.key(places[i].name).integer((int64_t)free);
    }
    w.endObject().key("restartNeeded").string(restartNeeded()).endObject();
    answer(s, id, w.text());
}

void Agent::packagesList(Session* s, int64_t id, const json::Value& params) {
    json::Value after = params.find("after");
    if (after.present() && !after.isString()) {
        refuse(s, id, "bad-request", "after is a package id");
        return;
    }
    if (inStep_) {
        refuse(s, id, "busy", "a job is installing; the list is changing");
        return;
    }
    std::vector<InstalledPackage> installed;
    std::string why;
    if (!engine_.store().loadAll(&installed, &why)) {
        refuse(s, id, "internal", why);
        return;
    }
    // Sorted by id, which is the order of the store's folder names.
    std::map<std::string, const InstalledPackage*> sorted;
    for (size_t i = 0; i < installed.size(); ++i) sorted[installed[i].manifest.id] = &installed[i];

    // A page ends at 100 packages, or sooner if the link's frame is full.
    size_t room = s->peer->maxFrame() > kEnvelopeRoom + 32 ? s->peer->maxFrame() - kEnvelopeRoom - 32 : 0;
    json::Writer w;
    w.beginObject().key("packages").beginArray();
    size_t listed = 0;
    bool more = false;
    for (std::map<std::string, const InstalledPackage*>::const_iterator it = sorted.begin(); it != sorted.end(); ++it) {
        if (after.present() && it->first <= after.string()) continue;
        const InstalledPackage& p = *it->second;
        json::Writer one;
        one.beginObject().key("id").string(p.manifest.id).key("name").string(p.manifest.name);
        one.key("version").string(p.manifest.version).key("key").string(p.key).key("flags").beginArray();
        std::vector<std::string> flags = riskFlags(p.manifest);
        for (size_t f = 0; f < flags.size(); ++f) one.string(flags[f]);
        one.endArray().endObject();
        if (listed == kMaxListed || (listed > 0 && w.text().size() + one.text().size() + 1 > room)) {
            more = true;
            break;
        }
        w.raw(one.text());
        ++listed;
    }
    w.endArray().key("more").boolean(more).endObject();
    answer(s, id, w.text());
}

const TrustedKey* Agent::findKey(const std::string& hex) const {
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (toHex(keys_[i].key, sizeof keys_[i].key) == hex) return &keys_[i];
    }
    return 0;
}

void Agent::sourcesList(Session* s, int64_t id) {
    json::Writer w;
    w.beginObject().key("sources").beginArray();
    for (size_t i = 0; i < keys_.size(); ++i) {
        w.beginObject().key("name").string(sanitizeUtf8(keys_[i].name, 0));
        w.key("key").string(toHex(keys_[i].key, sizeof keys_[i].key)).key("embedded").boolean(keys_[i].embedded).endObject();
    }
    w.endArray().endObject();
    answer(s, id, w.text());
}

void Agent::sourceAdd(Session* s, int64_t id, const json::Value& params) {
    json::Value name = params.find("name"), key = params.find("key");
    if (!name.isString() || !validDisplayName(name.string()) || !key.isString() || !isHex(key.string(), 64)) {
        refuse(s, id, "bad-request", "source.add takes a name of 1 to 64 characters and a key of 64 hex characters");
        return;
    }
    if (findKey(key.string()) != 0) {
        answer(s, id, "{}");   // trusted already: nothing to ask the user
        return;
    }
    refuse(s, id, "unsupported", "a new key needs the prompt on the car's screen, which this agent does not have yet");
}

void Agent::sourceRemove(Session* s, int64_t id, const json::Value& params) {
    json::Value key = params.find("key");
    if (!key.isString() || !isHex(key.string(), 64)) {
        refuse(s, id, "bad-request", "source.remove takes a key of 64 hex characters");
        return;
    }
    const TrustedKey* trusted = findKey(key.string());
    if (trusted == 0) {
        refuse(s, id, "not-found", "the agent does not trust that key");
    } else if (trusted->embedded) {
        refuse(s, id, "forbidden", "a key built into the agent cannot be removed");
    } else {
        refuse(s, id, "unsupported", "this agent cannot change its keys yet");
    }
}

// ---- job.plan -----------------------------------------------------------------------------------------------------

void Agent::jobPlan(Session* s, int64_t id, const json::Value& params) {
    if (building_ != 0) {
        refuse(s, id, "busy", "another plan is being made");
        return;
    }
    if (job_.active()) {
        refuse(s, id, "busy", "a job is running");
        return;
    }
    if (onTrial_) {
        refuse(s, id, "busy", "this version of the agent is on trial: ask for the job's status first, which confirms it");
        return;
    }
    PlanRequest request;
    std::string why;
    if (!parsePlanRequest(params, &request, &why)) {
        refuse(s, id, "bad-request", why);
        return;
    }
    {
        std::string wanted;
        for (size_t i = 0; i < request.install.size(); ++i) {
            wanted += (wanted.empty() ? "install " : ", install ") + request.install[i].id +
                      (request.install[i].version.empty() ? "" : " " + request.install[i].version);
        }
        for (size_t i = 0; i < request.remove.size(); ++i) {
            wanted += (wanted.empty() ? "remove " : ", remove ") + request.remove[i].id + (request.remove[i].deleteData ? " with its data" : "");
        }
        note(linkName(s->number) + ": job.plan asks to " + (wanted.empty() ? "do nothing" : wanted) + "; " + countOf(request.offers.size(), "package") + " on offer");
    }
    // The agent keeps one plan: an older one that was never started is gone from here on.
    havePlan_ = false;
    building_ = new PlanBuild();
    building_->session = s;
    building_->requestId = id;
    building_->request = request;
    building_->offer = 0;
    advancePlan();
}

void Agent::advancePlan() {
    if (building_ == 0 || fetch_.active) return;
    Plan plan;
    size_t need = 0;
    if (!planner_.resolve(building_->request, building_->fetched, &plan, &need)) {
        building_->offer = need;
        building_->bytes.clear();
        // The whole manifest: no length, because its size is not in the offer. The reply says how much comes.
        startFetch(building_->session, true, building_->request.offers[need].manifestSha256, 0, 0);
        return;
    }
    json::Writer w;
    w.beginObject();
    if (plan.runnable()) {
        uint8_t raw[8];
        random_->bytes(raw, sizeof raw);
        planId_ = toHex(raw, sizeof raw);
        plan_ = plan;
        havePlan_ = true;
        w.key("planId").string(planId_);
    }
    w.key("steps").beginArray();
    for (size_t i = 0; i < plan.steps.size(); ++i) {
        const PlanStep& step = plan.steps[i];
        w.beginObject().key("action").string(step.action).key("id").string(step.id).key("version").string(step.version);
        if (!step.oldVersion.empty()) w.key("oldVersion").string(step.oldVersion);
        if (step.action != "remove") w.key("sha256").string(step.sha256);
        w.endObject();
    }
    w.endArray().key("missing").beginArray();
    for (size_t i = 0; i < plan.missing.size(); ++i) {
        w.beginObject().key("id").string(plan.missing[i].id);
        if (!plan.missing[i].range.empty()) w.key("version").string(plan.missing[i].range);
        w.key("requiredBy").string(plan.missing[i].requiredBy).endObject();
    }
    w.endArray().key("problems").beginArray();
    for (size_t i = 0; i < plan.problems.size(); ++i) {
        w.beginObject().key("code").string(plan.problems[i].code).key("id").string(plan.problems[i].id);
        w.key("message").string(sanitizeUtf8(plan.problems[i].message, 0)).endObject();
    }
    w.endArray().key("flags").beginArray();
    for (size_t i = 0; i < plan.flags.size(); ++i) w.string(plan.flags[i]);
    w.endArray().key("fetchBytes").integer((int64_t)plan.fetchBytes).key("installBytes").integer((int64_t)plan.installBytes);
    w.key("restart").string(plan.restart).endObject();

    Session* s = building_->session;
    int64_t id = building_->requestId;
    delete building_;
    building_ = 0;
    if (plan.runnable()) {
        note("plan " + planId_ + ": " + countOf(plan.steps.size(), "step") + ", " + countOf(plan.fetchBytes, "byte") + " to fetch, restart: " + plan.restart);
    } else {
        note("no plan can be made of that:");
    }
    for (size_t i = 0; i < plan.steps.size(); ++i) {
        note("  " + stepText(plan.steps[i].action, plan.steps[i].id, plan.steps[i].version, plan.steps[i].oldVersion));
    }
    for (size_t i = 0; i < plan.missing.size(); ++i) {
        note("  missing: " + plan.missing[i].id + (plan.missing[i].range.empty() ? "" : " " + plan.missing[i].range) +
             ", which " + plan.missing[i].requiredBy + " needs");
    }
    for (size_t i = 0; i < plan.problems.size(); ++i) {
        note("  problem: " + plan.problems[i].code + ", " + plan.problems[i].id + ": " + sanitizeUtf8(plan.problems[i].message, 0));
    }
    answer(s, id, w.text());
}

// ---- blob.fetch ---------------------------------------------------------------------------------------------------

void Agent::startFetch(Session* session, bool forPlan, const std::string& sha256, uint64_t offset, uint64_t length) {
    json::Writer w;
    w.beginObject().key("sha256").string(sha256).key("offset").integer((int64_t)offset);
    if (length != 0) w.key("length").integer((int64_t)length);
    w.endObject();
    fetch_ = Fetch();
    fetch_.active = true;
    fetch_.session = session;
    fetch_.forPlan = forPlan;
    fetch_.asked = length;
    fetch_.lastActivityMs = fetch_.startMs = clock_->nowMs();
    fetch_.sha256 = sha256;
    note(linkName(session->number) + ": blob.fetch " + shortHash(sha256) +
         (forPlan ? ", a manifest" : ", " + countOf(length, "byte") + " from " + number(offset)));
    fetch_.requestId = session->peer->request("blob.fetch", w.text());
    if (fetch_.requestId == 0) session->peer->close("blob.fetch could not be sent");
}

void Agent::onResponse(Peer* peer, int64_t id, bool ok, const json::Value& result, const std::string& code,
                       const std::string& message) {
    Session* s = (Session*)peer->user;
    if (!fetch_.active || fetch_.session != s || fetch_.requestId != id || fetch_.streaming) return;
    fetch_.lastActivityMs = clock_->nowMs();
    if (!ok) {
        endFetch(false, code, "the controller answered blob.fetch with " + code + (message.empty() ? "" : ": " + sanitizeUtf8(message.substr(0, 200), 0)));
        return;
    }
    json::Value stream = result.find("stream"), size = result.find("size");
    if (!stream.isInteger() || stream.integer() < 1 || stream.integer() > 65535 || !size.isInteger() || size.integer() < 1) {
        peer->close("a blob.fetch reply with no usable stream or size");
        return;
    }
    fetch_.streaming = true;
    fetch_.stream = (uint16_t)stream.integer();
    fetch_.expect = (uint64_t)size.integer();
    if (fetch_.asked != 0 && fetch_.expect != fetch_.asked) {
        peer->close("a blob.fetch reply for another length than was asked for");
        return;
    }
    if (fetch_.forPlan && fetch_.expect > kMaxManifestBytes) {
        // The agent cannot stop a stream. It lets this one run out and keeps none of it.
        fetch_.discard = true;
        fetch_.failCode = "bad-manifest";
        fetch_.failMessage = "the offered manifest is larger than 1 MiB";
    }
}

void Agent::onData(Peer* peer, uint16_t stream, const std::string& bytes, bool last) {
    Session* s = (Session*)peer->user;
    if (!fetch_.active || fetch_.session != s || !fetch_.streaming || fetch_.stream != stream) {
        peer->close("a DATA frame for a stream that is not open");
        return;
    }
    fetch_.got += bytes.size();
    fetch_.lastActivityMs = clock_->nowMs();
    if (fetch_.got > fetch_.expect || (last && fetch_.got != fetch_.expect)) {
        peer->close("a stream that is not as long as its reply said");
        return;
    }
    if (!fetch_.discard) {
        if (fetch_.forPlan) {
            building_->bytes += bytes;
        } else {
            job_.fetchData(bytes);
            // The job may have ended on this very piece (the cache could not be written).
            if (!job_.fetching()) fetch_.discard = fetch_.orphan = true;
        }
    }
    if (last) endFetch(!fetch_.discard, fetch_.failCode, fetch_.failMessage);
}

void Agent::onEvent(Peer* peer, const std::string& event, int64_t, const json::Value& params) {
    Session* s = (Session*)peer->user;
    if (event != "blob.error" || !s->trusted) return;   // unknown events are ignored
    json::Value stream = params.find("stream"), code = params.find("code");
    if (!fetch_.active || fetch_.session != s || !fetch_.streaming || !stream.isInteger() || stream.integer() != fetch_.stream) return;
    std::string why = code.isString() ? code.string() : std::string("io-error");
    if (why != "not-found" && why != "cancelled") why = "io-error";
    if (fetch_.discard) endFetch(false, fetch_.failCode, fetch_.failMessage);
    else endFetch(false, why, "the controller ended the stream: " + why);
}

// By value: the callers pass strings that live in fetch_, which is cleared here.
void Agent::endFetch(bool ok, std::string code, std::string message) {
    bool forPlan = fetch_.forPlan, orphan = fetch_.orphan;
    {
        std::string line = linkName(fetch_.session->number) + ": blob.fetch " + shortHash(fetch_.sha256) + " -> ";
        if (ok) line += countOf(fetch_.got, "byte");
        else if (orphan) line += "ended; it was no longer wanted";
        else line += code + ": " + message;
        note(line + " (" + number((uint64_t)(clock_->nowMs() - fetch_.startMs)) + " ms)");
    }
    fetch_ = Fetch();
    if (orphan) return;   // the job it was for has ended or been replaced; the stream only had to run out
    if (forPlan) {
        if (building_ == 0) return;
        const Offer& offer = building_->request.offers[building_->offer];
        OfferedManifest& into = building_->fetched[offer.manifestSha256];
        if (ok) {
            checkOfferedManifest(offer, building_->bytes, keys_, &into);
        } else {
            into = OfferedManifest();
            into.error.fail(code == "bad-manifest" ? "bad-manifest" : "fetch-failed", message);
        }
        building_->bytes.clear();
        advancePlan();
        return;
    }
    job_.fetchEnded(ok, code, message);
}

// ---- jobs ---------------------------------------------------------------------------------------------------------

void Agent::driveJob() {
    if (!job_.fetching() || fetch_.active) return;
    if (jobOwner_ == 0) {
        job_.pause();
        return;
    }
    std::string sha256;
    uint64_t offset = 0, length = 0;
    if (job_.nextFetch(&sha256, &offset, &length)) startFetch(jobOwner_, false, sha256, offset, length);
}

void Agent::jobStart(Session* s, int64_t id, const json::Value& params) {
    json::Value planId = params.find("planId");
    if (!planId.isString() || !isHex(planId.string(), 16)) {
        refuse(s, id, "bad-request", "job.start takes a planId of 16 hex characters");
        return;
    }
    if (job_.exists() && job_.planId() == planId.string()) {
        // The running or the last job: nothing changes, so a lost reply can be asked for again. A paused job goes
        // on, over the link this came in on.
        if (job_.paused()) {
            jobOwner_ = s;
            job_.resume();
            note("job " + job_.planId() + " goes on, over " + linkName(s->number));
        }
        answer(s, id, "{\"job\":" + job_.describe() + "}");
        return;
    }
    if (!havePlan_ || planId_ != planId.string()) {
        refuse(s, id, "not-found", "there is no such plan");
        return;
    }
    if (job_.active()) {
        refuse(s, id, "busy", "another job is running");
        return;
    }
    std::vector<InstalledPackage> installed;
    std::string why;
    if (!engine_.store().loadAll(&installed, &why)) {
        refuse(s, id, "internal", why);
        return;
    }
    if (installedStateOf(installed) != plan_.installedState) {
        havePlan_ = false;
        refuse(s, id, "plan-stale", "the installed packages changed since the plan was made");
        return;
    }
    // A stream that an older job left running is no longer wanted, and its end says nothing about the new job.
    if (fetch_.active && !fetch_.forPlan) fetch_.discard = fetch_.orphan = true;
    if (!job_.start(planId_, plan_, &why)) {
        refuse(s, id, "internal", why);
        return;
    }
    havePlan_ = false;
    jobOwner_ = s;
    jobAccounted_ = false;
    note("job " + job_.planId() + " started, over " + linkName(s->number));
    answer(s, id, "{\"job\":" + job_.describe() + "}");
}

void Agent::jobCancel(Session* s, int64_t id, const json::Value& params) {
    json::Value planId = params.find("planId");
    if (!planId.isString() || !isHex(planId.string(), 16)) {
        refuse(s, id, "bad-request", "job.cancel takes a planId of 16 hex characters");
        return;
    }
    if (!job_.exists() || job_.planId() != planId.string()) {
        refuse(s, id, "not-found", "there is no such job");
        return;
    }
    if (job_.fetching() || job_.paused()) {
        if (fetch_.active && !fetch_.forPlan) fetch_.discard = fetch_.orphan = true;
        job_.cancel();
        answer(s, id, "{}");
    } else if (job_.state() == "cancelled") {
        answer(s, id, "{}");   // cancelled already: asking twice is harmless
    } else {
        refuse(s, id, "too-late", job_.ended() ? "the job has ended" : "the install has begun");
    }
}

void Agent::jobStatus(Session* s, int64_t id, const json::Value& params) {
    json::Value planId = params.find("planId"), after = params.find("after");
    if ((planId.present() && (!planId.isString() || !isHex(planId.string(), 16))) ||
        (after.present() && (!after.isInteger() || after.integer() < 0))) {
        refuse(s, id, "bad-request", "job.status takes a planId of 16 hex characters and an event number");
        return;
    }
    if (planId.present() && (!job_.exists() || job_.planId() != planId.string())) {
        refuse(s, id, "not-found", "there is no such job");
        return;
    }
    // A version of the agent on trial is confirmed by this: a controller from outside the head unit, or a person
    // at its console, has reached it and asks how the job went. The plugin is neither: it would reach an agent
    // that no phone can.
    if (onTrial_ && (s->role == "phone" || s->role == "cli")) confirmTrial(s);
    json::Writer w;
    w.beginObject();
    std::string job;
    if (job_.exists()) {
        job = job_.describe();
        w.key("job").raw(job);
    }
    w.key("events").beginArray();
    bool more = false;
    if (job_.exists() && after.present()) {
        size_t taken = job.size() + kEnvelopeRoom + 48;
        size_t room = s->peer->maxFrame() > taken ? s->peer->maxFrame() - taken : 0;
        job_.replay(after.integer(), room, &w, &more);
    }
    w.endArray().key("more").boolean(more).endObject();
    answer(s, id, w.text());
}

void Agent::systemRestart(Session* s, int64_t id, const json::Value& params) {
    json::Value kind = params.find("kind");
    if (!kind.isString() || (kind.string() != "hmi" && kind.string() != "reboot")) {
        refuse(s, id, "bad-request", "kind is hmi or reboot");
        return;
    }
    if (job_.active() || !pendingRestart_.empty() || onTrial_) {
        refuse(s, id, "busy", job_.active() ? "a job is running" : onTrial_ ? "this version of the agent is on trial" : "a restart is already on its way");
        return;
    }
    // The answer goes out before the restart is tried, since a reboot leaves nobody to answer. What can be known
    // now is asked now, so that a head unit with no way to do it does not say yes first.
    std::string why;
    if (!(kind.string() == "reboot" ? platform_->canReboot(&why) : platform_->canRestartHmi(&why))) {
        refuse(s, id, "unsupported", why);
        return;
    }
    answer(s, id, "{}");
    pendingRestart_ = kind.string();
    restartDeadlineMs_ = clock_->nowMs() + kRestartFlushMs;
}

}  // namespace nbt
