// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// A job: see job.hpp.

#include "job.hpp"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <set>

#include "agent_home.hpp"
#include "fsutil.hpp"
#include "hex.hpp"
#include "json.hpp"
#include "peer.hpp"
#include "sha256.hpp"
#include "utf8.hpp"

namespace nbt {

namespace {

const int kJobVersion = 1;
const size_t kMaxJobFile = 4u << 20;
const size_t kMaxEventsFile = 16u << 20;
const size_t kKeptEvents = 256;              // at least this many of the newest are kept
const size_t kMaxLogText = 4096;             // bytes of text in one job.log
const int64_t kProgressGapMs = 250;          // at most four job.progress a second between changes
const uint64_t kFetchChunk = 1u << 20;       // the most asked for in one blob.fetch
const unsigned kFetchTries = 3;              // failed ranges in a row before the job gives up
// After a restart the event numbers go on from well above the last one written down. job.progress events are not
// written down, so a controller may have seen higher numbers than the files show.
const int64_t kSeqJump = 1 << 20;

std::string hashOfFile(const std::string& path) {
    uint8_t digest[kSha256Bytes];
    if (!fs::hashFile(path, digest, 0, 0)) return std::string();
    return toHex(digest, sizeof digest);
}

std::string stringOf(json::Value object, const char* name) {
    json::Value v = object.find(name);
    return v.isString() ? v.string() : std::string();
}

}  // namespace

Job::Job(Engine* engine, JobHost* host)
    : engine_(engine), host_(host), step_(0), hasBytes_(false), bytes_(0), total_(0), seq_(0), restart_("none"),
      lastProgressMs_(0), fetchChunk_(kFetchChunk), partFd_(-1), fetchFailures_(0), settled_(false), handOver_(false) {}

Job::~Job() { closePart(); }

std::string Job::dir() const { return engine_->platform()->stateDir() + "/job"; }

std::string Job::blobPath(const Step& step) const { return engine_->store().blobPath(step.plan.sha256); }

void Job::closePart() {
    if (partFd_ >= 0) close(partFd_);
    partFd_ = -1;
}

// ---- what is kept on disk -----------------------------------------------------------------------------------------

bool Job::persist(std::string* error) const {
    json::Writer w;
    w.beginObject().key("job").integer(kJobVersion).key("planId").string(planId_).key("state").string(state_);
    w.key("step").integer((int64_t)step_).key("seq").integer(seq_).key("restart").string(restart_);
    w.key("steps").beginArray();
    for (size_t i = 0; i < steps_.size(); ++i) {
        const Step& s = steps_[i];
        w.beginObject().key("action").string(s.plan.action).key("id").string(s.plan.id);
        w.key("version").string(s.plan.version);
        if (!s.plan.oldVersion.empty()) w.key("oldVersion").string(s.plan.oldVersion);
        if (s.plan.action == "remove") {
            w.key("deleteData").boolean(s.plan.deleteData);
        } else {
            w.key("sha256").string(s.plan.sha256).key("size").integer((int64_t)s.plan.size);
            w.key("manifestSha256").string(s.plan.manifestSha256);
        }
        if (!s.result.empty()) w.key("result").string(s.result);
        if (!s.error.code.empty()) {
            w.key("error").beginObject().key("code").string(s.error.code);
            w.key("message").string(sanitizeUtf8(s.error.message, 0)).endObject();
        }
        w.endObject();
    }
    w.endArray().endObject();
    return fs::makeDirs(dir(), error) && fs::writeFile(dir() + "/job.json", w.text() + "\n", 0600, error);
}

bool Job::load(bool recovered, const StepReport& report, std::string* error) {
    planId_.clear();
    steps_.clear();
    events_.clear();
    std::string path = dir() + "/job.json";
    if (fs::kindOf(path) != fs::kFile) return true;

    std::string text, why;
    json::Document doc;
    if (!fs::readFile(path, kMaxJobFile, &text, &why) || !doc.parse(text, &why)) {
        // A record that cannot be read tells nothing. The installed packages have records of their own, so the
        // only loss is the account of the last job.
        fs::removeTree(dir(), 0);
        return true;
    }
    json::Value root = doc.root();
    json::Value steps = root.find("steps");
    if (!root.find("job").isInteger() || root.find("job").integer() != kJobVersion || !steps.isArray() || steps.size() == 0 ||
        stringOf(root, "planId").empty() || !root.find("step").isInteger() || root.find("step").integer() < 0 ||
        (size_t)root.find("step").integer() >= steps.size()) {
        fs::removeTree(dir(), 0);
        return true;
    }
    planId_ = stringOf(root, "planId");
    state_ = stringOf(root, "state");
    step_ = (size_t)root.find("step").integer();
    seq_ = root.find("seq").isInteger() ? root.find("seq").integer() : 0;
    restart_ = stringOf(root, "restart");
    if (restart_ != "hmi-restart" && restart_ != "required") restart_ = "none";
    for (size_t i = 0; i < steps.size(); ++i) {
        json::Value v = steps.at(i);
        Step s;
        s.plan.action = stringOf(v, "action");
        s.plan.id = stringOf(v, "id");
        s.plan.version = stringOf(v, "version");
        s.plan.oldVersion = stringOf(v, "oldVersion");
        s.plan.sha256 = stringOf(v, "sha256");
        s.plan.manifestSha256 = stringOf(v, "manifestSha256");
        s.plan.size = v.find("size").isInteger() ? (uint64_t)v.find("size").integer() : 0;
        s.plan.deleteData = v.find("deleteData").isBool() && v.find("deleteData").boolean();
        s.result = stringOf(v, "result");
        s.error.code = stringOf(v.find("error"), "code");
        s.error.message = stringOf(v.find("error"), "message");
        steps_.push_back(s);
    }

    bool doneKept = false;
    if (fs::readFile(dir() + "/events", kMaxEventsFile, &text, 0)) {
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) break;   // a line cut off by a power cut
            std::string line = text.substr(at, end - at);
            at = end + 1;
            json::Document event;
            if (!event.parse(line, 0) || !event.root().find("seq").isInteger()) continue;
            KeptEvent kept;
            kept.seq = event.root().find("seq").integer();
            kept.text = line;
            events_.push_back(kept);
            if (kept.seq > seq_) seq_ = kept.seq;
            doneKept = stringOf(event.root(), "event") == "job.done";
        }
    }

    if (ended()) {
        // The record is whole, as it is at nearly every start: it is read, and not written again.
        if (doneKept) return true;
        // Written down as ended, but the agent stopped before job.done was: it is sent now.
        emit("job.done", doneParams(), true);
        return persist(error);
    }

    const Step& running = steps_[step_];
    bool installPhase = state_ == "installing" || state_ == "restarting";
    if (recovered && report.outcome == kStepRestart && installPhase && running.result.empty() &&
        running.plan.action != "remove" && running.plan.id == report.id && running.plan.version == report.version) {
        // This agent is the version the job has placed, and it is on trial. The job waits, in memory only: the
        // record stays as the version before left it, for that version to read if this one is not confirmed.
        state_ = "restarting";
        seq_ += kSeqJump;
        return true;
    }

    // The job was running when the agent stopped. It does not go on: staging is emptied after a power cut, and a
    // controller has to be there to serve the archives again.
    settled_ = true;
    seq_ += kSeqJump;
    bool wasRestarting = state_ == "restarting";
    Step& s = steps_[step_];
    if (!installPhase) {
        s.result = "failed";
        s.error.fail("interrupted", "the agent stopped while the job was fetching; nothing was changed");
    } else if (s.result.empty()) {
        settleStep(recovered, report, wasRestarting);
    }
    bool allDone = true;
    for (size_t i = 0; i < steps_.size(); ++i) allDone = allDone && steps_[i].result == "done";
    if (wasRestarting) {
        say(allDone ? "the update of the agent is complete: this is the new version"
                    : "the update of the agent was undone: this is the version from before");
    } else {
        say("the agent stopped while this job was running" + std::string(allDone ? "" : "; the steps that were left have not run"));
    }
    finish(allDone ? "succeeded" : "failed");
    return true;
}

void Job::settleStep(bool recovered, const StepReport& report, bool wasRestarting) {
    Step& s = steps_[step_];
    if (recovered && report.id == s.plan.id) {
        s.result = report.outcome == kStepDone ? "done" : report.outcome == kStepFailed ? "failed" : "broken";
        s.error = report.error;
        if (report.outcome == kStepDone) restart_ = strongerRestart(restart_, report.restart);
        return;
    }
    // No journal: the step had not begun to change anything, or had finished to the last line.
    InstalledPackage now;
    bool installed = engine_->store().isInstalled(s.plan.id) && engine_->store().load(s.plan.id, &now, 0);
    bool done = s.plan.action == "remove" ? !engine_->store().isInstalled(s.plan.id)
                                          : installed && now.manifest.version == s.plan.version;
    if (done) {
        s.result = "done";
        if (installed) restart_ = strongerRestart(restart_, now.manifest.reboot);
    } else if (wasRestarting) {
        // The step was undone by an agent that stopped before it had said so here.
        s.result = "failed";
        s.error.fail("not-confirmed", "the new version of the agent was not confirmed, so the one before it is back");
    } else {
        s.result = "failed";
        s.error.fail("interrupted", "the agent stopped before the step had changed anything");
    }
}

void Job::settleRestart(bool recovered, const StepReport& report) {
    if (!restarting() || (recovered && report.outcome == kStepRestart)) return;   // not decided yet
    settleStep(recovered, report, true);
    bool done = steps_[step_].result == "done";
    say(done ? "a controller has reached the new version of the agent: the update is complete"
             : "the update of the agent could not be finished");
    finish(done ? "succeeded" : "failed");
}

// ---- events -------------------------------------------------------------------------------------------------------

void Job::emit(const char* name, const std::string& params, bool keep) {
    ++seq_;
    if (keep) {
        KeptEvent kept;
        kept.seq = seq_;
        kept.text = eventText(name, seq_, params);
        events_.push_back(kept);
        fs::appendFile(dir() + "/events", kept.text + "\n", 0);
        trimEvents();
    }
    host_->jobEvent(name, seq_, params);
}

void Job::trimEvents() {
    if (events_.size() <= kKeptEvents * 2) return;
    events_.erase(events_.begin(), events_.end() - (long)kKeptEvents);
    std::string text;
    for (size_t i = 0; i < events_.size(); ++i) text += events_[i].text + "\n";
    fs::writeFile(dir() + "/events", text, 0600, 0);
}

void Job::emitProgress(bool force) {
    std::string key = state_ + "|" + phase_ + "|";
    key += (char)('0' + step_ % 10);
    key += (char)('0' + step_ / 10 % 10);
    key += (char)('0' + step_ / 100 % 10);
    int64_t now = host_->nowMs();
    if (!force && key == lastProgressKey_ && now - lastProgressMs_ < kProgressGapMs) return;
    lastProgressKey_ = key;
    lastProgressMs_ = now;

    json::Writer w;
    w.beginObject().key("planId").string(planId_).key("state").string(state_);
    w.key("step").integer((int64_t)step_ + 1).key("steps").integer((int64_t)steps_.size());
    w.key("id").string(steps_[step_].plan.id);
    if (!phase_.empty()) w.key("phase").string(phase_);
    if (hasBytes_) w.key("bytes").integer((int64_t)bytes_).key("total").integer((int64_t)total_);
    w.endObject();
    emit("job.progress", w.text(), false);
}

void Job::emitLog(const char* source, const std::string& text) {
    // A hook writes bytes, and a piece can end inside a character: that part waits for the next piece.
    std::string& tail = tails_[source];
    std::string whole = tail + text;
    std::string clean = sanitizeUtf8(whole, &tail);
    size_t at = 0;
    while (at < clean.size()) {
        size_t len = clean.size() - at;
        if (len > kMaxLogText) {
            len = kMaxLogText;
            while (len > 1 && ((unsigned char)clean[at + len] & 0xC0) == 0x80) --len;   // not inside a character
        }
        json::Writer w;
        w.beginObject().key("planId").string(planId_).key("step").integer((int64_t)step_ + 1);
        w.key("source").string(source).key("text").string(clean.substr(at, len)).endObject();
        emit("job.log", w.text(), true);
        at += len;
    }
}

std::string Job::doneParams() const {
    json::Writer w;
    w.beginObject().key("planId").string(planId_).key("state").string(state_).key("steps").beginArray();
    for (size_t i = 0; i < steps_.size(); ++i) {
        const Step& s = steps_[i];
        w.beginObject().key("action").string(s.plan.action).key("id").string(s.plan.id);
        w.key("version").string(s.plan.version).key("result").string(s.result);
        if (!s.error.code.empty()) {
            w.key("error").beginObject().key("code").string(s.error.code);
            w.key("message").string(sanitizeUtf8(s.error.message, 0)).endObject();
        }
        w.endObject();
    }
    w.endArray().key("restart").string(restart_).endObject();
    return w.text();
}

std::string Job::describe() const {
    json::Writer w;
    w.beginObject().key("planId").string(planId_).key("state").string(state_);
    w.key("step").integer((int64_t)step_ + 1).key("steps").integer((int64_t)steps_.size());
    w.key("id").string(steps_[step_].plan.id);
    if (!phase_.empty()) w.key("phase").string(phase_);
    if (hasBytes_) w.key("bytes").integer((int64_t)bytes_).key("total").integer((int64_t)total_);
    w.key("seq").integer(seq_);
    if (ended()) w.key("result").raw(doneParams());
    w.endObject();
    return w.text();
}

void Job::replay(int64_t after, size_t room, json::Writer* out, bool* more) const {
    *more = false;
    size_t used = 0;
    for (size_t i = 0; i < events_.size(); ++i) {
        if (events_[i].seq <= after) continue;
        if (used + events_[i].text.size() + 1 > room) {
            *more = true;
            return;
        }
        out->raw(events_[i].text);
        used += events_[i].text.size() + 1;
    }
}

// ---- the life of a job --------------------------------------------------------------------------------------------

bool Job::start(const std::string& planId, const Plan& plan, std::string* error) {
    closePart();
    fs::removeTree(dir(), 0);
    planId_ = planId;
    steps_.clear();
    events_.clear();
    tails_.clear();
    seq_ = 0;
    restart_ = "none";
    phase_.clear();
    hasBytes_ = false;
    fetchFailures_ = 0;
    handOver_ = false;
    lastProgressKey_.clear();
    for (size_t i = 0; i < plan.steps.size(); ++i) {
        Step s;
        s.plan = plan.steps[i];
        steps_.push_back(s);
    }

    // The blob cache holds one job's archives. What this job does not name goes; what it names stays, a partial
    // archive too, so a fetch that was cut off goes on where it stopped.
    std::set<std::string> wanted;
    for (size_t i = 0; i < steps_.size(); ++i) {
        if (steps_[i].plan.action == "remove") continue;
        wanted.insert(steps_[i].plan.sha256);
        wanted.insert(steps_[i].plan.sha256 + ".part");
    }
    std::string blobs = engine_->store().blobDir();
    std::vector<std::string> names;
    fs::listDir(blobs, &names, 0);
    for (size_t i = 0; i < names.size(); ++i) {
        if (wanted.count(names[i]) == 0) fs::removeTree(blobs + "/" + names[i], 0);
    }
    // A whole archive in the cache was hashed when it was completed. That was before now, so it is hashed again.
    bool fetch = false;
    size_t first = 0;
    for (size_t i = steps_.size(); i-- > 0;) {
        if (steps_[i].plan.action == "remove") continue;
        std::string path = blobPath(steps_[i]);
        if (fs::kindOf(path) == fs::kFile && hashOfFile(path) == steps_[i].plan.sha256) continue;
        fs::removeTree(path, 0);
        fetch = true;
        first = i;
    }
    state_ = fetch ? "fetching" : "installing";
    step_ = fetch ? first : 0;
    if (fetch) phase_ = "fetch";
    if (!persist(error)) {
        planId_.clear();
        steps_.clear();
        return false;
    }
    return true;
}

bool Job::nextFetch(std::string* sha256, uint64_t* offset, uint64_t* length) {
    if (!fetching()) return false;
    for (size_t i = 0; i < steps_.size(); ++i) {
        const Step& s = steps_[i];
        if (s.plan.action == "remove" || fs::kindOf(blobPath(s)) == fs::kFile) continue;
        std::string part = blobPath(s) + ".part";
        struct stat st;
        uint64_t have = 0;
        if (stat(part.c_str(), &st) == 0) have = (uint64_t)st.st_size;
        if (have > s.plan.size) {
            fs::removeFile(part, 0);
            have = 0;
        }
        step_ = i;
        phase_ = "fetch";
        hasBytes_ = true;
        bytes_ = have;
        total_ = s.plan.size;
        if (have == s.plan.size) {
            // Every byte is here. The name it gets is the hash it must have.
            std::string why;
            if (hashOfFile(part) != s.plan.sha256) {
                fs::removeFile(part, 0);
                failFetch("fetch-failed", "the archive that arrived does not have the offered hash");
                return false;
            }
            if (!fs::moveFile(part, blobPath(s), &why)) {
                failFetch("io-error", why);
                return false;
            }
            emitProgress(true);
            continue;
        }
        emitProgress(false);
        *sha256 = s.plan.sha256;
        *offset = have;
        *length = s.plan.size - have < fetchChunk_ ? s.plan.size - have : fetchChunk_;
        return true;
    }
    // Every archive is staged: the install starts at once and needs no controller from here on.
    state_ = "installing";
    step_ = 0;
    phase_.clear();
    hasBytes_ = false;
    persist(0);
    return false;
}

void Job::fetchData(const std::string& bytes) {
    if (!fetching()) return;
    if (partFd_ < 0) {
        partFd_ = open((blobPath(steps_[step_]) + ".part").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (partFd_ < 0) {
            failFetch("io-error", std::string("cannot write to the blob cache: ") + strerror(errno));
            return;
        }
    }
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t n = write(partFd_, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            failFetch("io-error", std::string("cannot write to the blob cache: ") + strerror(errno));
            return;
        }
        done += (size_t)n;
    }
    bytes_ += bytes.size();
    emitProgress(false);
}

void Job::fetchEnded(bool ok, const std::string& code, const std::string& message) {
    closePart();
    if (!fetching()) return;
    if (ok) {
        fetchFailures_ = 0;
        return;
    }
    if (code == "cancelled") {
        say("the controller stopped the job");
        cancel();
    } else if (code == "not-found") {
        failFetch("fetch-failed", "the controller no longer has the archive");
    } else if (++fetchFailures_ >= kFetchTries) {
        failFetch("fetch-failed", message.empty() ? "the archive could not be fetched" : message);
    } else {
        say("the fetch was cut off (" + code + "); asking again from where it stopped");
    }
}

void Job::failFetch(const char* code, const std::string& message) {
    closePart();
    steps_[step_].result = "failed";
    steps_[step_].error.fail(code, message);
    say("failed: " + message);
    finish("failed");
}

void Job::pause() {
    if (!fetching()) return;
    closePart();
    state_ = "paused";
    persist(0);
    emitProgress(true);
}

void Job::resume() {
    if (!paused()) return;
    state_ = "fetching";
    fetchFailures_ = 0;
    persist(0);
}

void Job::cancel() {
    if (!fetching() && !paused()) return;
    finish("cancelled");
}

void Job::finish(const char* state) {
    closePart();
    for (size_t i = 0; i < steps_.size(); ++i) {
        if (steps_[i].result.empty()) steps_[i].result = "skipped";
    }
    state_ = state;
    hasBytes_ = false;
    tails_.clear();
    if (state_ == "succeeded") {
        // "Record the package and delete staging": the archives are not needed again. They go before job.done is
        // sent, so that whoever hears it finds the job's work finished.
        for (size_t i = 0; i < steps_.size(); ++i) {
            if (steps_[i].plan.action != "remove") fs::removeFile(blobPath(steps_[i]), 0);
        }
    }
    // Written down first: if the agent stops between the two, load() sends the job.done that is missing.
    persist(0);
    emit("job.done", doneParams(), true);
    persist(0);
}

void Job::runStep() {
    if (!installing()) return;
    Step& s = steps_[step_];
    phase_.clear();
    hasBytes_ = false;
    tails_.clear();
    persist(0);
    emitProgress(true);

    StepReport report = s.plan.action == "remove" ? engine_->remove(s.plan.id, s.plan.deleteData, this)
                                                  : engine_->install(blobPath(s), s.plan.manifestSha256, this);
    if (report.outcome == kStepRestart) {
        // The step has placed a newer version of the agent, and only that version can decide it. Written down
        // first: whichever agent starts next finds a job that is restarting, and the files say the rest.
        state_ = "restarting";
        phase_.clear();
        hasBytes_ = false;
        persist(0);
        std::string why;
        if (engine_->beginTrial(s.plan.version, &why)) {
            say("version " + s.plan.version + " of the agent is placed. This agent ends now, and the launcher starts that version: "
                "connect again and ask for the job's status, which is what confirms it");
            emitProgress(true);
            handOver_ = true;
            return;
        }
        // Nothing will start the new version, so the step is undone, here and now.
        say("the new version could not be handed to the launcher: " + why);
        bool found = false;
        StepReport undone;
        engine_->recover(&found, &undone, this);
        s.result = found && undone.outcome == kStepBroken ? "broken" : "failed";
        s.error.fail("io-error", "the new version of the agent could not be handed to the launcher: " + why);
        finish("failed");
        return;
    }
    s.result = report.outcome == kStepDone ? "done" : report.outcome == kStepFailed ? "failed" : "broken";
    s.error = report.error;
    if (report.outcome == kStepDone) {
        restart_ = strongerRestart(restart_, report.restart);
        if (step_ + 1 < steps_.size()) {
            ++step_;
            phase_.clear();
            hasBytes_ = false;
            persist(0);
            return;
        }
        finish("succeeded");
        return;
    }
    finish("failed");
}

// ---- what the engine says while a step runs -----------------------------------------------------------------------

void Job::phase(const char* name) {
    phase_ = name;
    hasBytes_ = false;
    emitProgress(true);
    host_->jobIdle();
}

void Job::progress(uint64_t bytes, uint64_t total) {
    hasBytes_ = true;
    bytes_ = bytes;
    total_ = total;
    emitProgress(false);
}

void Job::log(const char* source, const std::string& text) { emitLog(source, text); }

void Job::idle() { host_->jobIdle(); }

}  // namespace nbt
