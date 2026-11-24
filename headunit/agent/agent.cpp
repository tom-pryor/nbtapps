// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The agent: see agent.hpp.

#include "agent.hpp"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "checks.hpp"
#include "fsutil.hpp"
#include "hex.hpp"
#include "inflate.hpp"
#include "json_writer.hpp"
#include "semver.hpp"
#include "settings.hpp"
#include "settings_file.hpp"
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
const size_t kFileReadBytes = 16384;          // read from a file that is streamed, at a time
const int64_t kProgramEndWaitMs = 3000;       // how long a job's first step waits for a channel's program to end

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
      engine_(platform, keys, agentVersion), planner_(&engine_), job_(&engine_, this), serialReset_(0), serialDial_(0),
      nextSession_(1), channels_(this, &socketEnds_), building_(0),
      havePlan_(false), jobOwner_(0), jobAccounted_(true), inStep_(false), restartDeadlineMs_(0), onTrial_(false),
      handOverDeadlineMs_(0), stopAsked_(false), stopDeadlineMs_(0), fetchTimeoutMs_(kFetchTimeoutMs), stepGate_(false),
      stepHeld_(false), stepGateUntilMs_(0), log_(stderr) {
    channels_.setPrograms(platform->runDir(), platform->shell());
}

Agent::~Agent() {
    delete building_;
    for (size_t i = 0; i < sessions_.size(); ++i) {
        endOutgoing(sessions_[i], "", "");
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

std::string cappedLog() {
    pthread_mutex_lock(&gLogLock);
    std::string path = gLogPath;
    pthread_mutex_unlock(&gLogLock);
    return path;
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

void Agent::refuse(Session* s, int64_t id, const char* code, const std::string& message, const char* name, const std::string& json,
                   const std::string& logged) {
    s->peer->respondError(id, code, message, name, json);
    noteAnswer(s, id, std::string(code) + ": " + message + logged);
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

    // ${tmp} is on the RAM disk, which a power down empties: the folders of the packages whose logs name it are
    // made again. A package's own program makes its folder too, since it may start before the agent.
    {
        std::vector<InstalledPackage> installed;
        std::string why;
        if (engine_.store().loadAll(&installed, &why)) {
            for (size_t i = 0; i < installed.size(); ++i) {
                const Manifest& m = installed[i].manifest;
                if (namesTmp(m) && !fs::makeDirs(platform_->tmpDir(m.id), &why)) note("the folder ${tmp} of " + m.id + " cannot be made: " + why);
                // Where the programs of its channels listen.
                if (!m.channels.empty() && !makeChannelsDir(platform_->tmpDir(m.id), &why)) {
                    note("the folder of the channels of " + m.id + " cannot be made: " + why);
                }
            }
            adoptPrograms(installed);
        }
    }
    sweepChannelMarks();

    // A record of the paired phones that cannot be read stops no start: the phones pair again.
    std::string problem;
    if (!phones_.load(platform_->stateDir() + "/phones.json", &problem)) {
        note("the record of paired phones cannot be used, so no phone is paired: " + problem);
    }
    note(countOf(phones_.all().size(), "phone") + " paired");
    shareAddresses();
    return true;
}

// The programs an agent before this one started for channels, by their marks in the run folder: one whose process
// still runs that program is this agent's from now on; any other mark is stale, and goes.
void Agent::adoptPrograms(const std::vector<InstalledPackage>& installed) {
    std::vector<std::string> names;
    if (!fs::listDir(platform_->runDir(), &names, 0)) return;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string prefix = "channel-pid.";
        if (names[i].compare(0, prefix.size(), prefix) != 0) continue;
        std::string path = platform_->runDir() + "/" + names[i], text;
        bool kept = false;
        for (size_t k = 0; k < installed.size() && !kept; ++k) {
            const Manifest& m = installed[k].manifest;
            for (size_t c = 0; c < m.channels.size() && !kept; ++c) {
                const ManifestChannel& channel = m.channels[c];
                if (!channel.hasStart || channels_.pidFile(m.id, channel.name) != path) continue;
                char* end = 0;
                long pid = fs::readFile(path, 32, &text, 0) ? strtol(text.c_str(), &end, 10) : 0;
                // Believed only while that process is still the channel's program, a script's by its interpreter: a
                // pid can be given out again.
                if (!runsProgram(pid, engine_.store().appDir(m) + "/" + channel.program)) continue;
                channels_.adoptProgram(m.id, channel.name, pid, channelsDir(platform_->tmpDir(m.id)) + "/" + channel.name + ".log");
                kept = true;
            }
        }
        if (!kept) fs::removeFile(path, 0);
    }
}

void Agent::addListener(Listener* listener) { listeners_.push_back(listener); }

void Agent::adopt(Link* link) {
    Session* s = new Session();
    s->number = nextSession_++;
    s->peer = new Peer(link, false, this);
    s->peer->user = s;
    s->serial = strcmp(s->peer->linkKind(), "serial") == 0;
    s->dialled = s->serial && s->peer->linkDialledByCar();
    s->local = strcmp(s->peer->linkKind(), "local") == 0;
    s->openedMs = s->heardMs = s->pingedMs = clock_->nowMs();
    sessions_.push_back(s);
    note(linkName(s->number) + " opened (" + s->peer->linkKind() + (s->dialled ? ", dialled by the car" : "") + ")");
    if (s->dialled) rememberDial(s);
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
    // The programs' ends of the channels: what a program sends wakes the loop at once. They are served after the
    // links, in turn() and jobIdle().
    top = channels_.addToSets(&readable, &writable, top);
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
    endOutgoing(s, "the link ended", "");
    channels_.closeLink(s->peer, "the link ended");
    // Nobody shows the code any more.
    if (pairing_.open && pairing_.opener == s) closePairing(0);
    if (jobOwner_ == s) {
        jobOwner_ = 0;
        // The archives came over this link. The job waits until a controller starts it again.
        job_.pause();
    }
}

void Agent::turn(int timeoutMs) {
    if ((job_.installing() && !stepHeld_) || streamsWaiting()) timeoutMs = 0;
    else if (stepHeld_ && timeoutMs > 50) timeoutMs = 50;
    else if ((!pendingRestart_.empty() || job_.handingOver() || stopAsked_) && timeoutMs > 20) timeoutMs = 20;
    else if ((channels_.connecting() || channels_.ending()) && timeoutMs > (int)kChannelStartRetryMs) timeoutMs = (int)kChannelStartRetryMs;
    serve(timeoutMs);
    forgetDials();
    channels_.serve();
    channels_.tick();
    pumpStreams();
    keepAlive();
    endOverdue();
    expirePairing();
    reap();
    channels_.reap();
    if (fetch_.active && clock_->nowMs() - fetch_.lastActivityMs > fetchTimeoutMs_) {
        endLink(fetch_.session, kClosingSilent, "no answer to blob.fetch");
        reap();
    }
    if (job_.installing()) {
        if (stepMayBegin()) {
            inStep_ = true;
            job_.runStep();
            inStep_ = false;
            reap();
            if (job_.handingOver()) handOverDeadlineMs_ = clock_->nowMs() + kRestartFlushMs;
        }
    } else {
        driveJob();
    }
    // A plan that had to wait for a stream nobody wants any more to run out.
    if (building_ != 0 && !fetch_.active) advancePlan();
    afterJob();
    holdChannels();
    if (!pendingRestart_.empty()) doRestart();
}

void Agent::jobIdle() {
    serve(0);
    forgetDials();
    channels_.serve();
    channels_.tick();
    pumpStreams();
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
    stepGate_ = false;
    raiseRestartNeeded(job_.restart());
    sweepChannelMarks();
}

// See agent.hpp, "Channels". A job that is not over holds every channel: neither a program's socket nor a channel's
// DATA moves, and nothing is opened. The links that have channels are told when that begins and when it ends.
void Agent::holdChannels() {
    bool hold = job_.exists() && !job_.ended();
    // An open that is still connecting when a job starts is answered busy at once, and a program it started ends.
    if (hold) channels_.closeConnecting("busy", "a job is running: nothing opens until it has ended");
    if (hold == channels_.paused()) return;
    channels_.setPaused(hold);
    unsigned told = 0;
    for (size_t i = 0; i < sessions_.size(); ++i) {
        Session* s = sessions_[i];
        if (!s->peer->open() || (hold ? channels_.on(s->peer).empty() : !s->toldPaused)) continue;
        s->toldPaused = hold;
        if (s->peer->event(hold ? "channel.paused" : "channel.resumed", -1, "{}")) ++told;
    }
    note(std::string(hold ? "channels are held while the job runs" : "the job is over: channels go on") + " (" +
         countOf(told, "link") + " told)");
}

// Before the first step of a job, every channel of a package the job installs, upgrades or removes is closed. The
// step waits until the programs the agent started for those packages have gone, or for kProgramEndWaitMs.
bool Agent::stepMayBegin() {
    std::vector<std::string> ids = job_.packages();
    if (!stepGate_) {
        stepGate_ = true;
        stepGateUntilMs_ = clock_->nowMs() + kProgramEndWaitMs;
        for (size_t i = 0; i < ids.size(); ++i) {
            channels_.closePackage(ids[i], "", "package-changed", "a job installs, upgrades or removes " + ids[i]);
            channels_.endPrograms(ids[i], "", "a job installs, upgrades or removes " + ids[i]);
        }
    }
    bool waited = stepHeld_;
    stepHeld_ = channels_.programsRunning(ids) && clock_->nowMs() < stepGateUntilMs_;
    if (waited && !stepHeld_ && channels_.programsRunning(ids)) {
        note("the job's first step begins with a program of a channel still running");
    }
    return !stepHeld_;
}

// ---- the agent's own update ---------------------------------------------------------------------------------------

bool Agent::restartDue() {
    if (!job_.handingOver()) return false;
    // The controllers are told that the job is restarting before the process goes, as far as their links take it.
    bool flushed = true;
    for (size_t i = 0; i < sessions_.size(); ++i) flushed = flushed && !sessions_[i]->peer->wantsWrite();
    return flushed || clock_->nowMs() >= handOverDeadlineMs_;
}

bool Agent::stopDue() {
    if (!stopAsked_ || busy()) return false;
    // The controller that asked has its answer before the process goes, as far as the links take it.
    bool flushed = true;
    for (size_t i = 0; i < sessions_.size(); ++i) flushed = flushed && !sessions_[i]->peer->wantsWrite();
    return flushed || clock_->nowMs() >= stopDeadlineMs_;
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
    // The car no longer dials the phones for the trial.
    if (serialDial_ != 0) serialDial_->agentOnTrial(false);
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

// ---- the compatibility checks -------------------------------------------------------------------------------------

namespace {

// A string as JSON, for a list that is filled to the room a frame has.
std::string jsonString(const std::string& text) {
    json::Writer w;
    w.string(text);
    return w.text();
}

// What checks.run says of one check. A problem gets `count` of its paths from `from` on, which are JSON strings
// already, and `more` if others follow them.
std::string checkObject(const Check& c, const CheckFinding& f, const std::vector<std::string>& paths, size_t from,
                        size_t count, bool more) {
    json::Writer w;
    w.beginObject().key("id").string(c.id).key("state").string(checkStateName(f.state));
    if (f.state == kCheckUnknown) w.key("why").string(sanitizeUtf8(f.why, 0));
    if (f.state == kCheckProblem) {
        std::string detail = c.detail;
        if (!f.why.empty()) detail += " " + f.why;
        w.key("title").string(c.title).key("detail").string(sanitizeUtf8(detail, 0)).key("count").integer((int64_t)paths.size());
        w.key("found").beginArray();
        for (size_t i = from; i < from + count && i < paths.size(); ++i) w.raw(paths[i]);
        // The moves are few (kCheckMaxMoves at most), and every page has them.
        w.endArray().key("more").boolean(more).key("moves").beginArray();
        for (size_t i = 0; i < f.moves.size(); ++i) {
            w.beginObject().key("from").string(shownPath(f.moves[i].from)).key("to").string(shownPath(f.moves[i].to)).endObject();
        }
        w.endArray();
        if (f.fixable) {
            w.key("fix").beginObject().key("label").string(c.fixLabel).key("detail").string(c.fixDetail);
            w.key("restart").string(c.restart).key("token").string(f.token).endObject();
        }
    }
    w.endObject();
    return w.text();
}

}  // namespace

CheckContext Agent::checkContext() {
    CheckContext context(platform_, &engine_.store());
    context.stepRunning = inStep_ || job_.installing();
    return context;
}

void Agent::checksRun(Session* s, int64_t id, const json::Value& params) {
    json::Value which = params.find("id"), after = params.find("after");
    if ((which.present() && !which.isString()) || (after.present() && (!after.isString() || !which.present()))) {
        refuse(s, id, "bad-request", "checks.run takes the id of a check, and with it the last path it was given as after");
        return;
    }
    if (inStep_) {
        refuse(s, id, "busy", "a job is installing; what the checks look at is changing");
        return;
    }
    std::vector<const Check*> asked;
    if (which.present()) {
        const Check* one = findCheck(which.string());
        if (one == 0) {
            refuse(s, id, "not-found", "this agent has no check " + sanitizeUtf8(which.string().substr(0, 64), 0));
            return;
        }
        asked.push_back(one);
    } else {
        for (size_t i = 0; i < checkCount(); ++i) asked.push_back(&checkAt(i));
    }
    CheckContext context = checkContext();
    std::vector<CheckFinding> found(asked.size());
    std::vector<std::vector<std::string> > paths(asked.size());
    std::vector<size_t> from(asked.size(), 0), count(asked.size(), 0);
    std::string said;
    for (size_t i = 0; i < asked.size(); ++i) {
        runCheck(*asked[i], context, &found[i]);
        for (size_t k = 0; k < found[i].entries.size(); ++k) {
            std::string shown = shownPath(found[i].entries[k].path);
            if (after.present() && shown <= after.string()) ++from[i];
            paths[i].push_back(jsonString(shown));
        }
        said += std::string(i == 0 ? "" : ", ") + asked[i]->id + " " + checkStateName(found[i].state);
        if (found[i].state == kCheckProblem) said += " (" + countOf(found[i].entries.size(), "path") + ")";
    }
    // What has to be said of each check comes first; the paths fill what room the link's frame has left, in the
    // order of the checks. `more` is written as false here, which is the longer of the two.
    size_t room = s->peer->maxFrame() > kEnvelopeRoom + 32 ? s->peer->maxFrame() - kEnvelopeRoom - 32 : 0;
    size_t fixed = 14;   // {"checks":[]}
    for (size_t i = 0; i < asked.size(); ++i) fixed += checkObject(*asked[i], found[i], paths[i], 0, 0, false).size() + 1;
    size_t left = room > fixed ? room - fixed : 0;
    for (size_t i = 0; i < asked.size(); ++i) {
        for (size_t k = from[i]; k < paths[i].size() && paths[i][k].size() + 1 <= left; ++k) {
            left -= paths[i][k].size() + 1;
            ++count[i];
        }
        // Once one list is cut short, the ones after it wait for their own pages: the order stays as it is.
        if (from[i] + count[i] < paths[i].size()) left = 0;
    }
    json::Writer w;
    w.beginObject().key("checks").beginArray();
    for (size_t i = 0; i < asked.size(); ++i) {
        w.raw(checkObject(*asked[i], found[i], paths[i], from[i], count[i], from[i] + count[i] < paths[i].size()));
    }
    w.endArray().endObject();
    note(linkName(s->number) + ": checks: " + said);
    answer(s, id, w.text());
}

void Agent::checksFix(Session* s, int64_t id, const json::Value& params) {
    json::Value which = params.find("id"), token = params.find("token");
    if (!which.isString() || !token.isString() || !isHex(token.string(), 2 * kSha256Bytes)) {
        refuse(s, id, "bad-request", "checks.fix takes the id of a check and the token of 64 hex characters that checks.run gave");
        return;
    }
    if (s->role == "plugin") {
        refuse(s, id, "forbidden", "a check's fix is asked for from a phone or with nbtctl, by a person who has seen what it removes");
        return;
    }
    if (onTrial_) {
        refuse(s, id, "busy", "this version of the agent is on trial, and changes nothing until it is confirmed");
        return;
    }
    if (job_.active() || inStep_ || !pendingRestart_.empty()) {
        refuse(s, id, "busy", pendingRestart_.empty() ? "a job is running" : "a restart is on its way");
        return;
    }
    const Check* check = findCheck(which.string());
    if (check == 0) {
        refuse(s, id, "not-found", "this agent has no check " + sanitizeUtf8(which.string().substr(0, 64), 0));
        return;
    }
    FixOutcome out;
    const char* code = 0;
    std::string message;
    if (!fixCheck(*check, checkContext(), token.string(), &out, &code, &message)) {
        refuse(s, id, code, message);
        return;
    }
    std::string stopped;
    for (size_t i = 0; i < out.stopped.size(); ++i) stopped += (i == 0 ? "" : ", ") + out.stopped[i];
    // The moves that were done: few, and every one of them is said.
    json::Writer moved;
    moved.beginArray();
    for (size_t i = 0; i < out.moved.size(); ++i) {
        moved.beginObject().key("from").string(shownPath(out.moved[i].from)).key("to").string(shownPath(out.moved[i].to)).endObject();
    }
    moved.endArray();
    note(linkName(s->number) + ": the fix of the check " + check->id + ": " + countOf(out.moved.size(), "folder") + " moved, " +
         countOf(out.removed, "path") + " removed, " +
         number(out.left.size()) + " left" + (stopped.empty() ? std::string() : ", and stopped " + stopped) +
         "; the check now finds: " + checkStateName(out.state) + ", restart needed: " + out.restart);
    // The paths that are left, as many as the frame takes, and how many more there are.
    size_t room = s->peer->maxFrame() > kEnvelopeRoom + 32 ? s->peer->maxFrame() - kEnvelopeRoom - 32 : 0;
    json::Writer tail;
    tail.beginObject().key("state").string(checkStateName(out.state)).key("removed").integer((int64_t)out.removed);
    tail.key("moved").raw(moved.text()).key("leftMore").integer((int64_t)out.left.size()).key("stopped").beginArray();
    for (size_t i = 0; i < out.stopped.size(); ++i) tail.string(sanitizeUtf8(out.stopped[i], 0));
    tail.endArray().key("restart").string(out.restart).key("left").beginArray().endArray().endObject();
    size_t free = room > tail.text().size() ? room - tail.text().size() : 0, shown = 0;
    std::vector<std::string> left;
    for (size_t i = 0; i < out.left.size(); ++i) {
        std::string one = jsonString(out.left[i]);
        if (one.size() + 1 > free) break;
        free -= one.size() + 1;
        left.push_back(one);
        ++shown;
    }
    json::Writer w;
    w.beginObject().key("state").string(checkStateName(out.state)).key("removed").integer((int64_t)out.removed);
    w.key("moved").raw(moved.text()).key("left").beginArray();
    for (size_t i = 0; i < left.size(); ++i) w.raw(left[i]);
    w.endArray().key("leftMore").integer((int64_t)(out.left.size() - shown)).key("stopped").beginArray();
    for (size_t i = 0; i < out.stopped.size(); ++i) w.string(sanitizeUtf8(out.stopped[i], 0));
    w.endArray().key("restart").string(out.restart).endObject();
    answer(s, id, w.text());
}

// ---- serial.reset -------------------------------------------------------------------------------------------------

namespace {

// A value of serial.reset's: 1 to 32 printable characters, nothing that steers a log or a terminal.
bool resetValue(const std::string& text) {
    if (text.empty() || text.size() > 32) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] < 0x20 || text[i] > 0x7e) return false;
    }
    return true;
}

}  // namespace

void Agent::serialReset(Session* s, int64_t id, const json::Value& params) {
    if (serialReset_ == 0) {
        refuse(s, id, "unsupported", "this agent has no Bluetooth serial listener");
        return;
    }
    static const char* const kNames[3] = {"device", "instance", "service"};
    std::string values[3];
    for (int i = 0; i < 3; ++i) {
        json::Value v = params.find(kNames[i]);
        if (!v.present()) continue;
        if (!v.isString() || !resetValue(v.string())) {
            refuse(s, id, "bad-request", "serial.reset takes device, instance and service, each 1 to 32 printable characters");
            return;
        }
        values[i] = v.string();
    }
    if (job_.active() || inStep_) {
        refuse(s, id, "busy", "a job is running");
        return;
    }
    int64_t forMs = 0;
    std::string why;
    SerialReset::Outcome outcome = serialReset_->resetSerialLink(values[0], values[1], values[2], &forMs, &why);
    if (outcome == SerialReset::kResetFailed) {
        refuse(s, id, "io-error", why);
        return;
    }
    if (outcome == SerialReset::kResetDialled) {
        note(linkName(s->number) + ": serial.reset: the link the car dialled is ended");
        answer(s, id, "{\"state\":\"sent\"}");
        for (size_t i = 0; i < sessions_.size(); ++i) {
            Session* dialled = sessions_[i];
            if (dialled->peer->open() && dialled->peer->linkDialledByCar())
                endLink(dialled, kClosingReset, "serial.reset ended the link the car dialled");
        }
        return;
    }
    std::string given;
    for (int i = 0; i < 3; ++i) {
        if (!values[i].empty()) given += std::string(given.empty() ? " (" : ", ") + kNames[i] + " '" + values[i] + "'";
    }
    if (!given.empty()) given += ")";
    json::Writer w;
    w.beginObject();
    if (outcome == SerialReset::kResetSent) {
        note(linkName(s->number) + ": serial.reset: the car is asked to end the phone's serial link" + given);
        w.key("state").string("sent");
    } else {
        note(linkName(s->number) + ": serial.reset: the next serial link within " + number((uint64_t)(forMs / 1000)) +
             " s is ended at its open" + given);
        w.key("state").string("armed").key("forMs").integer(forMs);
    }
    w.endObject();
    answer(s, id, w.text());
}

// ---- restartNeeded ------------------------------------------------------------------------------------------------

// Whether an installed package still waits for a restart is kept in the run folder: a power cut clears that
// folder, and it also is the restart.
std::string Agent::restartNeeded() const { return restartMark(platform_->runDir()); }

void Agent::raiseRestartNeeded(const std::string& need) { raiseRestartMark(platform_->runDir(), need); }

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
    else if (method == "agent.stop") agentStop(s, id, params);
    else if (method == "files.list") filesList(s, id, params);
    else if (method == "files.fetch") filesFetch(s, id, params);
    else if (method == "files.cancel") filesCancel(s, id, params);
    else if (method == "autostart.get") autostartGet(s, id);
    else if (method == "autostart.set") autostartSet(s, id, params);
    else if (method == "checks.run") checksRun(s, id, params);
    else if (method == "checks.fix") checksFix(s, id, params);
    else if (method == "serial.reset") serialReset(s, id, params);
    else if (method == "serial.status") serialStatus(s, id);
    else if (method == "serial.dial") serialDialNow(s, id);
    else if (method == "settings.get") settingsGet(s, id, params);
    else if (method == "settings.set") settingsSet(s, id, params);
    else if (method == "channel.open") channelOpen(s, id, params);
    else if (method == "channel.get") channelGet(s, id, params);
    else if (method == "channel.set") channelSet(s, id, params);
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
    // What this agent has beyond the protocol's first version: autostart.get and autostart.set, installs checked
    // with the offer's signature, the files of a package, the compatibility checks, the settings of a package,
    // channels, and, with Bluetooth serial, serial.reset.
    w.key("maxFrame").integer(ours).key("capabilities").beginArray().string(kCapabilityAutostart);
    w.string(kCapabilityDetachedSignature).string(kCapabilityFiles).string(kCapabilityChecks).string(kCapabilitySettings);
    w.string(kCapabilityChannels);
    if (serialReset_ != 0) w.string(kCapabilitySerialReset);
    if (serialDial_ != 0 && serialDial_->dialOn()) w.string(kCapabilitySerialDial);
    // The car dials a phone when its app knocks: only with the dial, which the app's knock is for.
    if (serialDial_ != 0 && serialDial_->dialOn() && serialDial_->knockOn()) {
        w.string(kCapabilitySerialKnock);
        // And a phone's request for the car's dial-up service is a knock too: only with the knock.
        if (serialDial_->knockDialupOn()) w.string(kCapabilitySerialKnockDialup);
    }
    w.endArray().endObject();
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
    // A serial link of the other kind did `hello` first: that one stays.
    if (s->serial) {
        for (size_t i = 0; i < sessions_.size(); ++i) {
            Session* other = sessions_[i];
            if (other == s || !other->serial || other->dialled == s->dialled || !other->greeted || other->ending ||
                !other->peer->open())
                continue;
            note(linkName(s->number) + ": " + linkName(other->number) + ", " +
                 (other->dialled ? "which the car dialled" : "which the phone opened") + ", did hello first: it stays");
            endLink(s, kClosingOtherLink, std::string("a link ") + (other->dialled ? "the car dialled" : "the phone opened") +
                                              " did hello first");
            break;
        }
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
    rememberAddress(s);
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
    if (serialDial_ != 0) serialDial_->pairingWindow(false);
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
    if (serialDial_ != 0) serialDial_->pairingWindow(true);
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
    // The link's address, where it has one: written with the pairing. A phone paired again over a link with no
    // address keeps the one it had.
    const PairedPhone* before = phones_.find(s->phoneId);
    bool fromLink = colonAddress(s->peer->linkPhoneAddress(), &phone.address);
    if (!fromLink) phone.address = before != 0 ? before->address : "";
    // A phone that pairs on a link the car dialled has been reached by the dial at that address; and what the dial
    // has shown of an address holds for the phone paired again with it.
    phone.dialled = (fromLink && s->dialled) ||
                    (before != 0 && before->dialled && !phone.address.empty() && before->address == phone.address);
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
    rememberAddress(s);
    shareAddresses();
}

void Agent::rememberAddress(Session* s) {
    std::string address = s->peer->linkPhoneAddress(), why;
    if (address.empty() || s->phoneId.empty()) return;
    bool changed = false;
    if (!phones_.setAddress(s->phoneId, address, &changed, &why)) {
        note(linkName(s->number) + ": the phone's Bluetooth address could not be kept: " + why);
        return;
    }
    if (!changed) return;
    note(linkName(s->number) + ": the phone's Bluetooth address is kept, for the car to dial the phone's app");
    shareAddresses();
}

void Agent::rememberDial(Session* s) {
    std::string why;
    bool changed = false;
    if (!phones_.setDialled(s->peer->linkPhoneAddress(), &changed, &why)) {
        note(linkName(s->number) + ": that the car's dial reached the phone could not be kept: " + why);
        return;
    }
    if (!changed) return;
    note(linkName(s->number) + ": the car's dial has reached this phone's app, which is kept: a link the phone opens "
         "itself is now left to another program while one that is declared for a serial service runs, or may");
    shareAddresses();
}

void Agent::forgetDials() {
    if (serialDial_ == 0) return;
    std::vector<std::string> gone = serialDial_->takeUndialled();
    bool any = false;
    for (size_t i = 0; i < gone.size(); ++i) {
        std::string why;
        bool changed = false;
        if (!phones_.clearDialled(gone[i], &changed, &why)) {
            note("that the car's dials no longer reach a phone's app could not be kept: " + why);
            continue;
        }
        any = any || changed;
    }
    // The listener's log has said it, with the phone's number.
    if (any) shareAddresses();
}

void Agent::shareAddresses() { addresses_.set(phones_.addresses(), phones_.dialledAddresses()); }

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
    shareAddresses();
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
    w.endObject().key("restartNeeded").string(restartNeeded()).key("serial");
    writeSerial(w, s);
    w.endObject();
    answer(s, id, w.text());
}

// ---- serial.status, serial.dial -----------------------------------------------------------------------------------

void Agent::writeSerial(json::Writer& w, Session* s) {
    w.beginObject();
    if (serialDial_ == 0) {
        w.key("listener").boolean(false).endObject();
        return;
    }
    SerialStatus st = serialDial_->serialStatus();
    w.key("listener").boolean(true).key("registered").boolean(st.registered);
    if (st.checkedMsAgo >= 0) w.key("checkedMsAgo").integer(st.checkedMsAgo);
    w.key("stack").beginArray();
    for (size_t i = 0; i < st.stack.size(); ++i) w.string(sanitizeUtf8(st.stack[i].substr(0, 64), 0));
    w.endArray();
    w.key("phonesPresent").integer(st.phonesPresent).key("phonesEligible").integer(st.phonesEligible);
    w.key("dial").string(st.dialOn ? "on" : "off").key("knock").boolean(st.knock).key("dialState").string(st.dialState);
    w.key("lastResult").string(st.lastResult).key("link").string(st.link);
    w.key("recordDoubtful").boolean(st.recordDoubtful).key("repointStopped").boolean(st.repointStopped);
    // For the phone on this link; a link that knows no address (the bench's TCP) is never left.
    if (!s->local) w.key("phoneOpenedLeft").boolean(serialDial_->phoneOpenedLeft(s->peer->linkPhoneAddress()));
    w.key("counts").beginObject();
    w.key("dials").integer(st.dials).key("answeredOk").integer(st.answeredOk).key("notListening").integer(st.notListening);
    w.key("notInReach").integer(st.notInReach).key("busy").integer(st.busy).key("otherAnswers").integer(st.otherAnswers);
    w.key("noAnswer").integer(st.noAnswer).key("notTheApp").integer(st.notTheApp).key("leftBehind").integer(st.leftBehind);
    w.key("asks").integer(st.asks).key("asksListed").integer(st.asksListed).key("asksNotListed").integer(st.asksNotListed);
    w.key("asksNoAnswer").integer(st.asksNoAnswer).key("askErrors").integer(st.askErrors);
    w.key("repoints").integer(st.repoints).key("repointsExpected").integer(st.repointsExpected);
    w.key("repointsUnexpected").integer(st.repointsUnexpected).key("repointsNotSent").integer(st.repointsNotSent);
    w.key("checks").integer(st.checks).key("checksRestored").integer(st.checksRestored).key("refusals").integer(st.refusals);
    w.key("leftUnread").integer(st.leftUnread).key("putBack").integer(st.putBack);
    w.key("knocksByMarker").integer(st.knocksByMarker);
    w.key("knocksIgnoredQuiet").integer(st.knocksIgnoredQuiet).key("knocksIgnoredBusy").integer(st.knocksIgnoredBusy);
    w.key("knocksIgnoredNotDialable").integer(st.knocksIgnoredNotDialable);
    w.key("knockDials").integer(st.knockDials).key("knockDialsFailed").integer(st.knockDialsFailed);
    w.key("refusalsNoAudio").integer(st.refusalsNoAudio);
    w.endObject();
    w.key("problems").beginObject().key("recorded").integer(st.problemsRecorded);
    w.key("counted").integer(st.problemsCounted).endObject();
    w.endObject();
}

void Agent::serialStatus(Session* s, int64_t id) {
    if (serialDial_ == 0) {
        refuse(s, id, "unsupported", "this agent has no Bluetooth serial listener");
        return;
    }
    json::Writer w;
    writeSerial(w, s);
    answer(s, id, w.text());
}

void Agent::serialDialNow(Session* s, int64_t id) {
    if (!s->local) {
        refuse(s, id, "forbidden", "a dial is asked for on the head unit itself");
        return;
    }
    if (serialDial_ == 0 || !serialDial_->dialOn()) {
        refuse(s, id, "unsupported", serialDial_ == 0 ? "this agent has no Bluetooth serial listener"
                                                       : "the car does not dial phones in this run (--dial off)");
        return;
    }
    std::string why;
    const char* code = "internal";
    unsigned phones = serialDial_->dialNow(&why, &code);
    if (phones == 0) {
        refuse(s, id, code, why);
        return;
    }
    note(linkName(s->number) + ": serial.dial: a dial of " + countOf(phones, "phone") + " is asked for");
    json::Writer w;
    w.beginObject().key("phones").integer(phones).endObject();
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
        one.endArray();
        // Whether files.list has anything to say about it: logs its manifest names in folders this agent knows, or
        // the agent's own.
        if (namesLogs(p.manifest) || p.manifest.id == kAgentPackageId) one.key("logs").boolean(true);
        // Whether settings.get has anything to say about it: an option of a type this agent knows.
        if (!p.manifest.settings.empty()) one.key("settings").boolean(true);
        one.endObject();
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
    bool fetched = fetch_.active && fetch_.session == s && fetch_.streaming && fetch_.stream == stream;
    // A channel's number: the table's, which drops what comes for one that is not open. The fetch's own stream goes
    // first, for a controller that numbers its blob streams past kStreamLast as the ones before channels did.
    if (!fetched && stream >= kChannelFirstStream) {
        if (s->trusted) channels_.data(peer, stream, bytes, last);
        return;
    }
    if (!fetched) {
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
    if (!s->trusted) return;
    if (event == "channel.credit") {
        channels_.credit(peer, params);
        return;
    }
    if (event == "channel.close") {
        channels_.closedByPhone(peer, params);
        return;
    }
    if (event != "blob.error") return;   // unknown events are ignored
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
            checkOfferedManifest(offer, building_->bytes, keys_, agentVersion_, &into);
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
    stepGate_ = false;
    note("job " + job_.planId() + " started, over " + linkName(s->number));
    answer(s, id, "{\"job\":" + job_.describe() + "}");
    holdChannels();
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

void Agent::agentStop(Session* s, int64_t id, const json::Value& params) {
    if (params.size() != 0) {
        refuse(s, id, "bad-request", "agent.stop takes no params");
        return;
    }
    if (!s->local) {
        refuse(s, id, "forbidden", "the agent is stopped on the head unit itself");
        return;
    }
    if (stopAsked_) {
        answer(s, id, "{}");
        return;
    }
    // An agent on trial has the job that placed it, which waits for a controller to confirm this version: nothing of
    // it runs, and a stop ends the try.
    if ((job_.active() && !onTrial_) || inStep_ || !pendingRestart_.empty()) {
        refuse(s, id, "busy", pendingRestart_.empty() ? "a job is running" : "a restart is on its way");
        return;
    }
    answer(s, id, "{}");
    note(linkName(s->number) + ": agent.stop: the agent ends once its answer is written out" +
         (onTrial_ ? std::string(", and this version's try ends with it") : std::string()));
    stopAsked_ = true;
    stopDeadlineMs_ = clock_->nowMs() + kRestartFlushMs;
}

// ---- files --------------------------------------------------------------------------------------------------------

namespace {

// The package and area every files method takes. False with the error's code and message if they are not usable.
bool packageAndArea(const json::Value& params, std::string* package, const char** code, std::string* message) {
    json::Value id = params.find("package"), area = params.find("area");
    if (!id.isString() || !validPackageId(id.string()) || !area.isString()) {
        *code = "bad-request";
        *message = "a package id and an area";
        return false;
    }
    if (area.string() != "logs") {
        *code = "unsupported";
        *message = "this agent has the area logs only";
        return false;
    }
    *package = id.string();
    return true;
}

// "1234 bytes as 321 (26 %)".
std::string ratio(uint64_t size, uint64_t sent) {
    return number(size) + " bytes as " + number(sent) + (size == 0 ? std::string() : " (" + number(sent * 100 / size) + " %)");
}

}  // namespace

bool Agent::logsOf(const std::string& package, std::vector<LogFile>* out, const char** code, std::string* message) {
    if (package == kAgentPackageId) {
        listAgentLogs(ownLog_.empty() ? cappedLog() : ownLog_, *platform_, out);
        return true;
    }
    InstalledPackage installed;
    std::string why;
    if (!engine_.store().isInstalled(package)) {
        *code = "not-found";
        *message = "no such package is installed";
        return false;
    }
    if (!engine_.store().load(package, &installed, &why)) {
        *code = "internal";
        *message = "the record of " + package + " cannot be read: " + why;
        return false;
    }
    listPackageLogs(installed.manifest, engine_.store(), *platform_, out);
    return true;
}

void Agent::filesList(Session* s, int64_t id, const json::Value& params) {
    std::string package, message;
    const char* code = 0;
    if (!packageAndArea(params, &package, &code, &message)) {
        refuse(s, id, code, message);
        return;
    }
    if (inStep_) {
        refuse(s, id, "busy", "a job is installing; the packages are changing");
        return;
    }
    std::vector<LogFile> files;
    if (!logsOf(package, &files, &code, &message)) {
        refuse(s, id, code, message);
        return;
    }
    // Newest first, as many as fit in the link's frame: the rest are older, and `more` says that there are some.
    size_t room = s->peer->maxFrame() > kEnvelopeRoom + 32 ? s->peer->maxFrame() - kEnvelopeRoom - 32 : 0;
    json::Writer w;
    w.beginObject().key("files").beginArray();
    bool more = false;
    for (size_t i = 0; i < files.size(); ++i) {
        json::Writer one;
        one.beginObject().key("path").string(files[i].path).key("size").integer((int64_t)files[i].size);
        one.key("modified").integer(files[i].modified).endObject();
        if (w.text().size() + one.text().size() + 1 > room) {
            more = true;
            break;
        }
        w.raw(one.text());
    }
    w.endArray().key("more").boolean(more).endObject();
    answer(s, id, w.text());
}

void Agent::filesFetch(Session* s, int64_t id, const json::Value& params) {
    std::string package, message;
    const char* code = 0;
    if (!packageAndArea(params, &package, &code, &message)) {
        refuse(s, id, code, message);
        return;
    }
    json::Value path = params.find("path"), offset = params.find("offset"), length = params.find("length");
    json::Value compress = params.find("compress");
    if (!path.isString() || (offset.present() && (!offset.isInteger() || offset.integer() < 0)) ||
        (length.present() && (!length.isInteger() || length.integer() < 1)) || !compress.isString() ||
        (compress.string() != "deflate" && compress.string() != "none")) {
        refuse(s, id, "bad-request", "files.fetch takes a path, an offset from 0, a length from 1, and compress: deflate or none");
        return;
    }
    if (s->outgoing != 0) {
        refuse(s, id, "busy", "a files stream is open on this link");
        return;
    }
    if (job_.active() || inStep_) {
        refuse(s, id, "busy", "a job is running");
        return;
    }
    std::vector<LogFile> files;
    if (!logsOf(package, &files, &code, &message)) {
        refuse(s, id, code, message);
        return;
    }
    // Only a file the listing of this moment has.
    const LogFile* file = 0;
    for (size_t i = 0; i < files.size() && file == 0; ++i) {
        if (files[i].path == path.string()) file = &files[i];
    }
    int fd = -1;
    uint64_t size = 0;
    std::string why;
    if (file == 0 || !openLogFile(*file, &fd, &size, &why)) {
        refuse(s, id, "not-found", file == 0 ? "files.list has no such file for " + package : why);
        return;
    }
    uint64_t from = offset.present() ? (uint64_t)offset.integer() : 0;
    if (from > size) {
        close(fd);
        refuse(s, id, "out-of-range", "the file has " + countOf(size, "byte"));
        return;
    }
    uint64_t count = size - from;
    if (length.present() && (uint64_t)length.integer() < count) count = (uint64_t)length.integer();
    if (count != 0 && lseek(fd, (off_t)from, SEEK_SET) != (off_t)from) {
        close(fd);
        refuse(s, id, "io-error", file->path + " cannot be read from byte " + number(from));
        return;
    }
    json::Writer w;
    w.beginObject();
    if (count == 0) {
        // Nothing to send, so no stream.
        close(fd);
        w.key("size").integer(0).key("compress").string(compress.string()).endObject();
        answer(s, id, w.text());
        return;
    }
    Outgoing* o = new Outgoing();
    o->fd = fd;
    o->stream = s->nextStream;
    // The numbers above kStreamLast are the channels'.
    s->nextStream = s->nextStream >= kStreamLast ? 1 : (uint16_t)(s->nextStream + 1);
    o->size = o->left = count;
    if (compress.string() == "deflate") o->deflater = new Deflater();
    o->package = package;
    o->path = file->path;
    o->startMs = clock_->nowMs();
    s->outgoing = o;
    note(linkName(s->number) + ": files.fetch " + package + " " + sanitizeUtf8(file->path, 0) + ": stream " + number(o->stream) +
         ", " + countOf(count, "byte") + " from " + number(from) + ", " + compress.string());
    w.key("stream").integer(o->stream).key("size").integer((int64_t)count).key("compress").string(compress.string()).endObject();
    // The DATA frames follow from the loop (pumpStreams), behind this answer.
    answer(s, id, w.text());
}

void Agent::filesCancel(Session* s, int64_t id, const json::Value& params) {
    json::Value stream = params.find("stream");
    if (!stream.isInteger() || stream.integer() < 1 || stream.integer() > 65535) {
        refuse(s, id, "bad-request", "files.cancel takes a stream from 1 to 65535");
        return;
    }
    // The frames already queued go first, and this answer after them: nothing more comes on the stream once it is
    // there. A stream that has ended already is answered the same.
    if (s->outgoing != 0 && s->outgoing->stream == stream.integer()) {
        endOutgoing(s, "cancelled after " + countOf(s->outgoing->sent, "byte") + " in DATA frames", "");
    }
    answer(s, id, "{}");
}

bool Agent::streamsWaiting() const {
    for (size_t i = 0; i < sessions_.size(); ++i) {
        const Session* s = sessions_[i];
        if (s->outgoing != 0 && s->peer->open() && !s->peer->closing() && s->peer->queued() < 2 * (size_t)s->peer->maxFrame()) return true;
    }
    return false;
}

void Agent::pumpStreams() {
    for (size_t i = 0; i < sessions_.size(); ++i) pumpLink(sessions_[i]);
}

// The link's channels and its files stream take turns, a frame each, round and round, while less than two frames'
// worth waits to be written on the link: the link's other messages, `ping` and its answer among them, wait behind
// two frames at most, and a channel that floods holds up neither a quiet one nor the files stream.
void Agent::pumpLink(Session* s) {
    Peer* peer = s->peer;
    // A link that is closing sends nothing more; its streams go when the link does.
    if (!peer->open() || peer->closing()) return;
    std::vector<Channel*> channels = channels_.on(peer);
    size_t sources = channels.size() + 1, idle = 0, frame = peer->maxFrame();
    while (idle < sources && peer->open() && !peer->closing() && peer->queued() < 2 * frame) {
        size_t k = s->turn++ % sources;
        bool sent = k < channels.size() ? channels_.sendOne(channels[k]) : sendFileFrame(s);
        idle = sent ? 0 : idle + 1;
    }
}

bool Agent::sendFileFrame(Session* s) {
    Peer* peer = s->peer;
    size_t frame = peer->maxFrame();
    while (s->outgoing != 0) {
        Outgoing* o = s->outgoing;
        if (!o->pending.empty() && (o->pending.size() >= frame || o->read)) {
            size_t n = o->pending.size() < frame ? o->pending.size() : frame;
            bool last = o->read && n == o->pending.size();
            peer->data(o->stream, o->pending.data(), n, last);
            o->sent += n;
            o->pending.erase(0, n);
            if (last) endOutgoing(s, ratio(o->size, o->sent), "");
            return true;
        }
        char buffer[kFileReadBytes];
        size_t want = o->left < sizeof buffer ? (size_t)o->left : sizeof buffer;
        ssize_t got = read(o->fd, buffer, want);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            // The file was cut short since it was opened, or cannot be read.
            std::string why = got == 0 ? "the file ended " + countOf(o->left, "byte") + " early" : std::string("read failed: ") + strerror(errno);
            endOutgoing(s, "io-error: " + why, why);
            return false;
        }
        o->left -= (uint64_t)got;
        if (o->left == 0) o->read = true;
        if (o->deflater == 0) {
            o->pending.append(buffer, (size_t)got);
        } else if (!o->deflater->deflate(buffer, (size_t)got, o->read, &o->pending)) {
            endOutgoing(s, "io-error: the compression failed", "the compression failed");
            return false;
        }
    }
    return false;
}

void Agent::endOutgoing(Session* s, const std::string& how, const std::string& error) {
    Outgoing* o = s->outgoing;
    if (o == 0) return;
    s->outgoing = 0;
    if (!error.empty() && s->peer->open()) {
        json::Writer w;
        w.beginObject().key("stream").integer(o->stream).key("code").string("io-error").key("message").string(error).endObject();
        s->peer->event("files.error", -1, w.text());
    }
    if (!how.empty()) {
        note(linkName(s->number) + ": files stream " + number(o->stream) + " -> " + how + " (" +
             number((uint64_t)(clock_->nowMs() - o->startMs)) + " ms)");
    }
    close(o->fd);
    delete o->deflater;
    delete o;
}

// ---- settings -----------------------------------------------------------------------------------------------------

namespace {

// A key as a controller sent it, for the log: short, and text.
std::string shownKey(const std::string& key) { return sanitizeUtf8(key.substr(0, 64), 0); }

}  // namespace

bool Agent::settingsOf(const std::string& package, InstalledPackage* out, const char** code, std::string* message) {
    std::string why;
    if (!engine_.store().isInstalled(package)) {
        *code = "not-found";
        *message = "no such package is installed";
        return false;
    }
    if (!engine_.store().load(package, out, &why)) {
        *code = "internal";
        *message = "the record of " + package + " cannot be read: " + why;
        return false;
    }
    if (out->manifest.settings.empty()) {
        *code = "not-found";
        *message = package + " names no settings this agent knows";
        return false;
    }
    return true;
}

void Agent::settingsGet(Session* s, int64_t id, const json::Value& params) {
    if (s->peer->maxFrame() < kSettingsMinFrame) {
        refuse(s, id, "unsupported", "settings need a link whose frame holds " + number(kSettingsMinFrame) + " bytes");
        return;
    }
    json::Value package = params.find("package"), from = params.find("from");
    if (!package.isString() || !validPackageId(package.string()) || (from.present() && (!from.isInteger() || from.integer() < 0))) {
        refuse(s, id, "bad-request", "settings.get takes a package id, and a from of 0 or more");
        return;
    }
    if (inStep_) {
        refuse(s, id, "busy", "a job is installing; the packages are changing");
        return;
    }
    InstalledPackage installed;
    const char* code = 0;
    std::string message;
    if (!settingsOf(package.string(), &installed, &code, &message)) {
        refuse(s, id, code, message);
        return;
    }
    const std::vector<ManifestSetting>& options = installed.manifest.settings;
    uint64_t first = from.present() ? (uint64_t)from.integer() : 0;
    if (first >= options.size()) {
        refuse(s, id, "bad-request", "from is past the last option: " + package.string() + " has " + countOf(options.size(), "option"));
        return;
    }
    // The file once, for the whole page.
    std::string text, why;
    bool exists = false;
    if (!readSettingsFile(installed.manifest, *platform_, &text, &exists, &why)) {
        refuse(s, id, "io-error", "the settings file cannot be read: " + why);
        return;
    }
    SettingsFile file(text);
    // From `first` on, as many as the link's frame takes, and the first whatever it takes: an option fits a frame
    // of kSettingsMinFrame however long its strings are (tests/agent/test_agent.cpp).
    size_t room = s->peer->maxFrame() > kEnvelopeRoom + 32 ? s->peer->maxFrame() - kEnvelopeRoom - 32 : 0;
    json::Writer w;
    w.beginObject().key("options").beginArray();
    size_t next = (size_t)first;
    for (; next < options.size(); ++next) {
        std::string one = settingJson(options[next], file);
        if (next > first && w.text().size() + one.size() + 1 > room) break;
        w.raw(one);
    }
    w.endArray();
    if (next < options.size()) w.key("next").integer((int64_t)next);
    w.endObject();
    answer(s, id, w.text());
}

void Agent::settingsSet(Session* s, int64_t id, const json::Value& params) {
    if (s->peer->maxFrame() < kSettingsMinFrame) {
        refuse(s, id, "unsupported", "settings need a link whose frame holds " + number(kSettingsMinFrame) + " bytes");
        return;
    }
    json::Value package = params.find("package"), set = params.find("set"), reset = params.find("reset");
    bool usable = package.isString() && validPackageId(package.string()) && (!set.present() || set.isObject()) &&
                  (!reset.present() || reset.isArray());
    for (size_t i = 0; usable && i < reset.size(); ++i) usable = reset.at(i).isString();
    if (!usable || set.size() + reset.size() == 0) {
        refuse(s, id, "bad-request", "settings.set takes a package id, and keys to set as an object or to reset as an array, one at least");
        return;
    }
    for (size_t i = 0; i < reset.size(); ++i) {
        if (set.find(reset.at(i).string()).present()) {
            refuse(s, id, "bad-request", shownKey(reset.at(i).string()) + " is both set and reset");
            return;
        }
    }
    if (job_.active() || inStep_) {
        refuse(s, id, "busy", "a job is running");
        return;
    }
    if (onTrial_) {
        refuse(s, id, "busy", "this version of the agent is on trial, and changes nothing until it is confirmed");
        return;
    }
    InstalledPackage installed;
    const char* code = 0;
    std::string message;
    if (!settingsOf(package.string(), &installed, &code, &message)) {
        refuse(s, id, code, message);
        return;
    }
    const Manifest& m = installed.manifest;
    std::map<std::string, const ManifestSetting*> byKey;
    for (size_t i = 0; i < m.settings.size(); ++i) byKey[m.settings[i].key] = &m.settings[i];

    // Every key and value before anything is written. An option of a type this agent does not know is not in
    // m.settings, and so is unknown here.
    std::vector<std::pair<std::string, std::string> > lines;    // key, the text after KEY=
    std::vector<std::pair<std::string, const char*> > faults;   // key, reason
    for (size_t i = 0; i < set.size(); ++i) {
        std::map<std::string, const ManifestSetting*>::const_iterator option = byKey.find(set.key(i));
        std::string line;
        const char* reason = option == byKey.end() ? "unknown-key" : checkSettingValue(*option->second, set.at(i), &line);
        if (reason != 0) faults.push_back(std::make_pair(set.key(i), reason));
        else lines.push_back(std::make_pair(set.key(i), line));
    }
    for (size_t i = 0; i < reset.size(); ++i) {
        if (byKey.count(reset.at(i).string()) == 0) faults.push_back(std::make_pair(reset.at(i).string(), "unknown-key"));
    }
    if (!faults.empty()) {
        std::string said = countOf(faults.size(), "setting") + (faults.size() == 1 ? " is" : " are") + " not allowed";
        // The keys at fault, as many as the frame takes beside the rest of the error.
        size_t room = s->peer->maxFrame() > kEnvelopeRoom + 96 ? s->peer->maxFrame() - kEnvelopeRoom - 96 : 0;
        json::Writer keys;
        keys.beginArray();
        std::string logged;
        for (size_t i = 0; i < faults.size(); ++i) {
            json::Writer one;
            one.beginObject().key("key").string(faults[i].first).key("reason").string(faults[i].second).endObject();
            if (keys.text().size() + one.text().size() + 2 > room) break;
            keys.raw(one.text());
            logged += std::string(i == 0 ? " (" : ", ") + shownKey(faults[i].first) + " " + faults[i].second;
        }
        keys.endArray();
        refuse(s, id, "invalid", said, "keys", keys.text(), logged.empty() ? logged : logged + ")");
        return;
    }

    std::string before, why;
    bool exists = false;
    if (!readSettingsFile(m, *platform_, &before, &exists, &why)) {
        refuse(s, id, "io-error", "the settings file cannot be read: " + why);
        return;
    }
    SettingsFile file(before);
    std::string changed;
    for (size_t i = 0; i < lines.size(); ++i) {
        file.set(lines[i].first, lines[i].second);
        changed += (i == 0 ? "set " : ", ") + lines[i].first;
    }
    for (size_t i = 0; i < reset.size(); ++i) {
        file.remove(reset.at(i).string());
        changed += (i == 0 ? std::string(lines.empty() ? "" : "; ") + "reset " : ", ") + reset.at(i).string();
    }
    // Written once, whole, and only if the text has changed: a reset of a key that has no line writes nothing, and
    // neither makes a file.
    std::string after = file.text();
    if (after != before && !writeSettingsFile(m, *platform_, after, &why)) {
        refuse(s, id, "io-error", "the settings file cannot be written: " + why);
        return;
    }
    note(linkName(s->number) + ": settings of " + m.id + ": " + changed + (after == before ? " (the file was so already)" : ""));
    answer(s, id, "{}");
}

// ---- channels -----------------------------------------------------------------------------------------------------

namespace {

// "org.nbtapp.ssh ssh, stream 32768": a channel, for the log.
std::string channelText(const Channel* c) { return c->package + " " + c->name + ", stream " + number(c->stream); }

}  // namespace

bool Agent::channelOf(const std::string& package, const std::string& name, InstalledPackage* out, const ManifestChannel** channel,
                      const char** code, std::string* message) {
    std::string why;
    if (!engine_.store().isInstalled(package)) {
        *code = "not-found";
        *message = "no such package is installed";
        return false;
    }
    if (!engine_.store().load(package, out, &why)) {
        *code = "internal";
        *message = "the record of " + package + " cannot be read: " + why;
        return false;
    }
    *channel = findChannel(out->manifest, name);
    if (*channel == 0) {
        *code = "not-found";
        *message = package + " has no channel " + sanitizeUtf8(name.substr(0, 64), 0);
        return false;
    }
    return true;
}

void Agent::channelOpen(Session* s, int64_t id, const json::Value& params) {
    json::Value package = params.find("package"), name = params.find("channel"), credit = params.find("credit");
    if (!package.isString() || !validPackageId(package.string()) || !name.isString() ||
        (credit.present() && (!credit.isInteger() || credit.integer() < 1 || credit.integer() > 65535))) {
        refuse(s, id, "bad-request", "channel.open takes a package id, a channel's name, and a credit from 1 to 65535");
        return;
    }
    // The checks, in this order: who asks, for whom, whether the agent is free, what is installed, how many.
    if (s->role == "plugin") {
        refuse(s, id, "forbidden", "a channel is opened from a phone or with nbtctl, not by the iDrive plugin");
        return;
    }
    if (params.find("app").present()) {
        refuse(s, id, "unsupported", "this agent opens channels for NBTApps itself only, and for no other phone app");
        return;
    }
    if (s->peer->maxFrame() < kChannelMaxMessage) {
        refuse(s, id, "unsupported", "channels need a link whose frame holds " + number(kChannelMaxMessage) + " bytes");
        return;
    }
    if ((job_.exists() && !job_.ended()) || inStep_ || onTrial_) {
        refuse(s, id, "busy", onTrial_ ? "this version of the agent is on trial" : "a job is running");
        return;
    }
    InstalledPackage installed;
    const ManifestChannel* channel = 0;
    const char* code = 0;
    std::string message;
    if (!channelOf(package.string(), name.string(), &installed, &channel, &code, &message)) {
        refuse(s, id, code, message);
        return;
    }
    // Before anything is connected or started.
    if (channel->switched && fs::kindOf(switchFile(platform_->runDir(), installed.manifest.id, channel->name)) != fs::kFile) {
        refuse(s, id, "off", installed.manifest.id + " " + channel->name + " is switched off");
        return;
    }
    if (channels_.on(s->peer).size() >= kChannelPerLink) {
        refuse(s, id, "too-many", number(kChannelPerLink) + " channels are open on this link");
        return;
    }
    Channel* c = new Channel();
    c->peer = s->peer;
    c->link = s->number;
    c->package = installed.manifest.id;
    c->name = channel->name;
    c->access = channel->access;
    c->version = installed.manifest.version;
    c->requestId = id;
    c->socketPath = platform_->tmpDir(c->package) + "/channels/" + c->name;
    c->toPhone = credit.present() ? (unsigned)credit.integer() : kChannelCredit;
    c->fromPhone = kChannelCredit;
    // What starts the program when nothing listens: in ${app}, with a hook's environment and the channel's name.
    c->canStart = channel->hasStart && platform_->canStartPrograms();
    if (c->canStart) {
        const Manifest& m = installed.manifest;
        c->cwd = engine_.store().appDir(m);
        c->program = c->cwd + "/" + channel->program;
        c->args = channel->args;
        c->env.push_back("PATH=" + platform_->hookPath());
        c->env.push_back("NBT_PKG_ID=" + m.id);
        c->env.push_back("NBT_PKG_VERSION=" + m.version);
        c->env.push_back("NBT_APP_DIR=" + c->cwd);
        c->env.push_back("NBT_DATA_DIR=" + platform_->dataDir(m.id));
        c->env.push_back("NBT_TMP_DIR=" + platform_->tmpDir(m.id));
        c->env.push_back("NBT_CHANNEL=" + channel->name);
        c->log = channelsDir(platform_->tmpDir(m.id)) + "/" + channel->name + ".log";
    }
    // A number counting up from the last one given out on this link, and not one in use.
    for (unsigned tries = 0; tries <= 65535u - kChannelFirstStream; ++tries) {
        c->stream = s->nextChannel;
        s->nextChannel = s->nextChannel == 65535 ? kChannelFirstStream : (uint16_t)(s->nextChannel + 1);
        if (!channels_.inUse(s->peer, c->stream)) break;
    }
    // What the program is told first, on a line of its own.
    json::Writer line;
    line.beginObject().key("v").integer(1).key("channel").string(c->name).key("access").string(c->access);
    line.key("caller").string(s->role == "phone" ? "nbtapps" : "cli");
    if (s->role == "phone") line.key("phone").string(s->name);
    line.key("maxMessage").integer((int64_t)kChannelMaxMessage).endObject();
    note(linkName(s->number) + ": channel.open " + channelText(c) + ": connecting to " + c->socketPath);
    channels_.add(c, line.text());
}

// What channel.get and channel.set answer.
std::string Agent::channelState(const Manifest& m, const ManifestChannel& channel) const {
    bool on = !channel.switched || fs::kindOf(switchFile(platform_->runDir(), m.id, channel.name)) == fs::kFile;
    json::Writer w;
    w.beginObject().key("switched").boolean(channel.switched).key("on").boolean(on);
    w.key("open").integer(channels_.count(m.id, channel.name)).endObject();
    return w.text();
}

void Agent::channelGet(Session* s, int64_t id, const json::Value& params) {
    json::Value package = params.find("package"), name = params.find("channel");
    if (!package.isString() || !validPackageId(package.string()) || !name.isString()) {
        refuse(s, id, "bad-request", "channel.get takes a package id and a channel's name");
        return;
    }
    if (inStep_) {
        refuse(s, id, "busy", "a job is installing; the packages are changing");
        return;
    }
    InstalledPackage installed;
    const ManifestChannel* channel = 0;
    const char* code = 0;
    std::string message;
    if (!channelOf(package.string(), name.string(), &installed, &channel, &code, &message)) {
        refuse(s, id, code, message);
        return;
    }
    answer(s, id, channelState(installed.manifest, *channel));
}

void Agent::channelSet(Session* s, int64_t id, const json::Value& params) {
    json::Value package = params.find("package"), name = params.find("channel"), on = params.find("on");
    if (!package.isString() || !validPackageId(package.string()) || !name.isString() || !on.isBool()) {
        refuse(s, id, "bad-request", "channel.set takes a package id, a channel's name and on: true or false");
        return;
    }
    if (s->role == "plugin") {
        refuse(s, id, "forbidden", "a channel is switched from a phone or with nbtctl, not from the car's screen");
        return;
    }
    if ((job_.exists() && !job_.ended()) || inStep_ || onTrial_) {
        refuse(s, id, "busy", onTrial_ ? "this version of the agent is on trial, and changes nothing until it is confirmed" : "a job is running");
        return;
    }
    InstalledPackage installed;
    const ManifestChannel* channel = 0;
    const char* code = 0;
    std::string message;
    if (!channelOf(package.string(), name.string(), &installed, &channel, &code, &message)) {
        refuse(s, id, code, message);
        return;
    }
    if (!channel->switched) {
        refuse(s, id, "bad-request", installed.manifest.id + " " + channel->name + " has no switch: it is always on");
        return;
    }
    const std::string& pkg = installed.manifest.id;
    std::string file = switchFile(platform_->runDir(), pkg, channel->name), why;
    if (on.boolean()) {
        if (!fs::writeFile(file, "on\n", 0600, &why)) {
            refuse(s, id, "io-error", "the switch could not be written: " + why);
            return;
        }
    } else {
        if (!fs::removeFile(file, &why)) {
            refuse(s, id, "io-error", "the switch could not be removed: " + why);
            return;
        }
        channels_.closePackage(pkg, channel->name, "off", "switched off");
        channels_.endPrograms(pkg, channel->name, "its channel was switched off");
    }
    note(linkName(s->number) + ": " + pkg + " " + channel->name + " is switched " + (on.boolean() ? "on" : "off"));
    answer(s, id, channelState(installed.manifest, *channel));
}

// The switches and the marks of programs in the run folder whose package or channel is no longer installed go: at a
// start, and after a job.
void Agent::sweepChannelMarks() {
    std::vector<InstalledPackage> installed;
    std::vector<std::string> names;
    if (!engine_.store().loadAll(&installed, 0) || !fs::listDir(platform_->runDir(), &names, 0)) return;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string prefix = "channel-on.";
        if (names[i].compare(0, prefix.size(), prefix) != 0) continue;
        std::string path = platform_->runDir() + "/" + names[i];
        bool kept = false;
        for (size_t k = 0; k < installed.size() && !kept; ++k) {
            const Manifest& m = installed[k].manifest;
            for (size_t c = 0; c < m.channels.size() && !kept; ++c) {
                kept = m.channels[c].switched && switchFile(platform_->runDir(), m.id, m.channels[c].name) == path;
            }
        }
        if (!kept) {
            fs::removeFile(path, 0);
            note("the switch " + names[i] + " goes with its channel");
        }
    }
}

void Agent::channelConnected(Channel* c) {
    Session* s = (Session*)c->peer->user;
    json::Writer w;
    w.beginObject().key("stream").integer(c->stream).key("maxMessage").integer((int64_t)kChannelMaxMessage);
    w.key("credit").integer(c->fromPhone).key("version").string(c->version).endObject();
    if (c->peer->open()) answer(s, c->requestId, w.text());
}

void Agent::channelRefused(Channel* c, const char* code, const std::string& why) {
    Session* s = (Session*)c->peer->user;
    if (c->peer->open()) refuse(s, c->requestId, code, c->package + " " + c->name + ": " + why);
}

void Agent::channelNote(const std::string& line) { note(line); }

void Agent::channelClosed(Channel* c, const char* reason, const std::string& why) {
    // One that never opened: its link has gone with the request, and nobody is left to answer.
    if (c->connecting) {
        note(linkName(c->link) + ": channel.open of " + channelText(c) + " goes unanswered: " + why);
        return;
    }
    note(linkName(c->link) + ": channel " + channelText(c) + " closed" + (reason != 0 ? std::string(", ") + reason : std::string()) +
         ": " + why);
}

}  // namespace nbt
