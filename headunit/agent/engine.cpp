// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The install engine: see engine.hpp for what a step does, and journal.hpp for how it survives a power cut.

#include "engine.hpp"

#include <stdio.h>
#include <sys/stat.h>

#include <map>
#include <set>

#include "ed25519.hpp"
#include "fsutil.hpp"
#include "hex.hpp"
#include "hmi_ids.hpp"
#include "journal.hpp"
#include "json.hpp"
#include "json_writer.hpp"
#include "log_files.hpp"
#include "process.hpp"
#include "semver.hpp"
#include "sha256.hpp"
#include "utf8.hpp"

namespace nbt {

namespace {

const size_t kMaxStepLog = 256u << 10;      // of one step, kept in memory and in the record
const size_t kMaxLogFile = 256u << 10;      // <state>/install.log is rotated past this
const size_t kMaxPluginFile = 4u << 20;     // a plugin manifest or UI description larger than this is not read
const unsigned kVersionTimeoutSeconds = 20;  // for a new version of the agent to say which version it is
const size_t kMaxVersionOutput = 1024;      // of what it prints, this much is read

bool startsWith(const std::string& text, const std::string& prefix) { return text.compare(0, prefix.size(), prefix) == 0; }
bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}
bool under(const std::string& path, const std::string& dir) { return path == dir || startsWith(path, dir + "/"); }

const InstalledPackage* findInstalled(const std::vector<InstalledPackage>& all, const std::string& id) {
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].manifest.id == id) return &all[i];
    }
    return 0;
}

std::string number(uint64_t value) {
    char text[32];
    snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    return text;
}

// The folders above a destination, as destinations, parents first. ${app} and ${data} themselves are among them,
// because the package owns them; ${plugins} and / are not.
std::vector<std::string> ancestorsOf(const std::string& dest) {
    size_t root = 0;
    bool includeRoot = false;
    if (startsWith(dest, "${app}")) {
        root = 6;
        includeRoot = true;
    } else if (startsWith(dest, "${data}")) {
        root = 7;
        includeRoot = true;
    } else if (startsWith(dest, "${plugins}")) {
        root = 10;
    }
    std::vector<std::string> out;
    if (includeRoot) out.push_back(dest.substr(0, root));
    for (size_t i = root + 1; i < dest.size(); ++i) {
        if (dest[i] == '/') out.push_back(dest.substr(0, i));
    }
    return out;
}

// One line of the journal, read back.
struct Op {
    std::string op, path, to, target, action, id, version, oldVersion;
    bool deleteData;
    std::vector<std::string> rmdirs;

    Op() : deleteData(false) {}
};

bool parseOp(const std::string& line, Op* out) {
    json::Document doc;
    if (!doc.parse(line, 0)) return false;
    json::Value root = doc.root();
    if (!root.find("op").isString()) return false;
    *out = Op();
    out->op = root.find("op").string();
    struct Field {
        const char* name;
        std::string* into;
    } fields[] = {{"path", &out->path},       {"to", &out->to},           {"target", &out->target},
                  {"action", &out->action},   {"id", &out->id},           {"version", &out->version},
                  {"oldVersion", &out->oldVersion}};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
        json::Value v = root.find(fields[i].name);
        if (v.isString()) *fields[i].into = v.string();
    }
    out->deleteData = root.find("deleteData").isBool() && root.find("deleteData").boolean();
    json::Value rmdirs = root.find("rmdirs");
    for (size_t i = 0; rmdirs.isArray() && i < rmdirs.size(); ++i) {
        if (rmdirs.at(i).isString()) out->rmdirs.push_back(rmdirs.at(i).string());
    }
    return true;
}

bool readOps(const std::string& path, std::vector<Op>* ops, std::string* error) {
    std::vector<std::string> lines;
    ops->clear();
    if (!Journal::read(path, &lines, error)) return false;
    for (size_t i = 0; i < lines.size(); ++i) {
        Op op;
        if (!parseOp(lines[i], &op)) {
            // A last line that does not read was not on disk whole when the power went. The change it announces is
            // made only after the line is synced, so it never started: the line is dropped, like a cut-off one.
            if (i + 1 == lines.size()) break;
            if (error != 0) *error = "line " + number(i + 1) + " of the journal is damaged";
            return false;
        }
        ops->push_back(op);
    }
    return true;
}

}  // namespace

// ---- checks on the installed set ----------------------------------------------------------------------------------

void checkInstall(const Manifest& m, const std::string& keyHex, bool keyEmbedded,
                  const std::vector<InstalledPackage>& installed, const std::string& agentVersion,
                  std::vector<Problem>* problems, std::vector<MissingDependency>* missing) {
    Problem p;
    p.id = m.id;
    if (startsWith(m.id, "org.nbtapp.") && !keyEmbedded) {
        p.code = "reserved-id";
        p.message = "ids that start with org.nbtapp. are reserved for the keys built into the agent";
        problems->push_back(p);
    }
    const InstalledPackage* old = findInstalled(installed, m.id);
    // The agent's own package. Any built-in key may sign it, whichever signed the version before: there is one
    // such key (embedded_keys.hpp), and a second, should there ever be one, could not take its place otherwise.
    // reserved-id above has kept every other key off. And the version that counts as installed is the one that
    // runs: the first install is a script's and leaves no record.
    bool self = m.id == kAgentPackageId;
    if (old != 0 && !self && old->key != keyHex) {
        p.code = "key-mismatch";
        p.message = "the installed version " + old->manifest.version + " was signed by another key";
        problems->push_back(p);
    }
    std::string have = self ? agentVersion : old != 0 ? old->manifest.version : std::string();
    int order = 0;
    if (!have.empty() && compareVersionStrings(m.version, have, &order) && order <= 0) {
        p.code = order < 0 ? "downgrade" : "already-installed";
        p.message = self ? "version " + have + " of the agent is the one that runs" : "version " + have + " is installed";
        problems->push_back(p);
    }
    if (!m.agentRange.empty() && !versionInRange(agentVersion, m.agentRange)) {
        p.code = "agent-too-old";
        p.message = "needs agent " + m.agentRange + "; this is " + agentVersion;
        problems->push_back(p);
    }
    for (size_t i = 0; i < m.depends.size(); ++i) {
        const ManifestDependency& d = m.depends[i];
        if (d.id == m.id) continue;
        const InstalledPackage* have = findInstalled(installed, d.id);
        if (have == 0 || !versionInRange(have->manifest.version, d.range)) {
            MissingDependency miss;
            miss.id = d.id;
            miss.range = d.range;
            miss.requiredBy = m.id;
            missing->push_back(miss);
        }
    }
    // A conflict counts whichever of the two packages declares it.
    for (size_t i = 0; i < m.conflicts.size(); ++i) {
        const ManifestDependency& c = m.conflicts[i];
        const InstalledPackage* have = c.id == m.id ? 0 : findInstalled(installed, c.id);
        if (have != 0 && versionInRange(have->manifest.version, c.range)) {
            p.code = "conflict";
            p.message = "conflicts with the installed " + c.id + " " + have->manifest.version;
            problems->push_back(p);
        }
    }
    for (size_t i = 0; i < installed.size(); ++i) {
        const Manifest& other = installed[i].manifest;
        if (other.id == m.id) continue;
        for (size_t k = 0; k < other.conflicts.size(); ++k) {
            if (other.conflicts[k].id == m.id && versionInRange(m.version, other.conflicts[k].range)) {
                p.code = "conflict";
                p.message = "the installed " + other.id + " " + other.version + " conflicts with it";
                problems->push_back(p);
            }
        }
    }
    // One program answers for a serial service's registration: two packages cannot both declare the name.
    for (size_t i = 0; i < m.serialServices.size(); ++i) {
        const std::string& name = m.serialServices[i].name;
        for (size_t k = 0; k < installed.size(); ++k) {
            const Manifest& other = installed[k].manifest;
            if (other.id == m.id) continue;
            for (size_t s = 0; s < other.serialServices.size(); ++s) {
                if (other.serialServices[s].name != name) continue;
                p.code = "conflict";
                p.message = "declares the serial service " + name + ", which the installed " + other.id + " " +
                            other.version + " declares";
                problems->push_back(p);
            }
        }
    }
}

void checkRemove(const std::string& id, const std::vector<InstalledPackage>& installed, std::vector<Problem>* problems) {
    Problem p;
    p.id = id;
    if (findInstalled(installed, id) == 0) {
        p.code = "not-installed";
        p.message = id + " is not installed";
        problems->push_back(p);
        return;
    }
    if (id == kAgentPackageId) {
        p.code = "required-by";
        p.message = "the agent does not remove itself: everything installed here depends on it";
        problems->push_back(p);
    }
    for (size_t i = 0; i < installed.size(); ++i) {
        const Manifest& other = installed[i].manifest;
        if (other.id == id) continue;
        for (size_t k = 0; k < other.depends.size(); ++k) {
            if (other.depends[k].id == id) {
                p.code = "required-by";
                p.message = "the installed " + other.id + " depends on it";
                problems->push_back(p);
            }
        }
    }
}

// ---- the log of a step --------------------------------------------------------------------------------------------

// What a step says: to the listener, to <state>/install.log, and into the text that the record keeps.
class StepLog : public OutputSink {
public:
    // With `quiet`, nothing is written anywhere: a plan's checks are not a step.
    StepLog(Engine* engine, StepListener* listener, bool quiet = false)
        : engine_(engine), listener_(listener), hook_("agent"), quiet_(quiet) {}

    void begin(const std::string& title) {
        std::string path = engine_->store_.logPath();
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && (size_t)st.st_size > kMaxLogFile) rename(path.c_str(), (path + ".1").c_str());
        keep("== " + title + "\n");
    }
    void line(const std::string& text) {
        keep(text + "\n");
        if (listener_ != 0) listener_->log("agent", text + "\n");
    }
    void phase(const char* name) {
        keep(std::string("-- ") + name + "\n");
        if (listener_ != 0) listener_->phase(name);
    }
    void progress(uint64_t bytes, uint64_t total) {
        if (listener_ != 0) listener_->progress(bytes, total);
    }
    // While a hook runs, its output arrives here.
    void setHook(const char* name) { hook_ = name; }
    virtual void output(const char* data, size_t len) {
        std::string text(data, len);
        keep(text);
        if (listener_ != 0) listener_->log(hook_, text);
    }
    virtual void idle() {
        if (listener_ != 0) listener_->idle();
    }
    const std::string& text() const { return text_; }

private:
    void keep(const std::string& text) {
        if (quiet_) return;
        if (text_.size() + text.size() <= kMaxStepLog) text_ += text;
        fs::appendFile(engine_->store_.logPath(), text, 0);
    }

    Engine* engine_;
    StepListener* listener_;
    const char* hook_;
    bool quiet_;
    std::string text_;
};

// ---- the engine ---------------------------------------------------------------------------------------------------

Engine::Engine(Platform* platform, const std::vector<TrustedKey>& keys, const std::string& agentVersion)
    : platform_(platform), keys_(keys), agentVersion_(agentVersion), store_(platform), crashHook_(0), observer_(0) {}

bool Engine::init(std::string* error) {
    return store_.init(error) && fs::makeDirs(fs::parentOf(platform_->appDir("x")), error) &&
           fs::makeDirs(fs::parentOf(platform_->dataDir("x")), error) && fs::makeDirs(fs::parentOf(platform_->tmpDir("x")), error);
}

void Engine::clearJournalFolder() {
    fs::removeTree(store_.journalDir() + "/record", 0);
    fs::removeTree(store_.journalDir() + "/old", 0);
    fs::removeTree(store_.journalDir() + "/old-record", 0);
    // recover() comes here at every start of the agent. With nothing to clear, nothing is written.
    fs::emptyDir(store_.unpackDir(), 0);
}

bool Engine::runHook(const Manifest& m, HookKind kind, const std::string& cwd, const char* action,
                     const std::string& oldVersion, StepLog* log, Error* error) {
    const ManifestHook& hook = m.hooks[kind];
    if (!hook.present) return true;
    const char* name = hookName(kind);
    if (fs::kindOf(cwd + "/" + hook.script) != fs::kFile) {
        return error->fail("hook-failed", std::string(name) + ": the script " + hook.script + " is missing");
    }
    std::vector<std::string> argv, env;
    argv.push_back(platform_->shell());
    argv.push_back(hook.script);
    env.push_back("PATH=" + platform_->hookPath());
    env.push_back(std::string("NBT_ACTION=") + action);
    env.push_back("NBT_PKG_ID=" + m.id);
    env.push_back("NBT_PKG_VERSION=" + m.version);
    if (!oldVersion.empty()) env.push_back("NBT_OLD_VERSION=" + oldVersion);
    env.push_back("NBT_APP_DIR=" + store_.appDir(m));
    env.push_back("NBT_DATA_DIR=" + platform_->dataDir(m.id));
    env.push_back("NBT_TMP_DIR=" + platform_->tmpDir(m.id));

    putBackReadOnly(log);
    log->line(std::string("running ") + name + ": " + hook.script);
    log->setHook(name);
    ProcessResult result;
    std::string why;
    bool started = runProcess(platform_->shell(), argv, env, cwd, hook.timeout, log, &result, &why);
    log->setHook("agent");
    if (!started) return error->fail("hook-failed", std::string(name) + " could not be started: " + why);
    if (result.timedOut) return error->fail("hook-timeout", std::string(name) + " was killed after " + number(hook.timeout) + " s");
    if (!result.exited) return error->fail("hook-failed", std::string(name) + " was ended by signal " + number((uint64_t)result.signal));
    if (result.exitCode != 0) return error->fail("hook-failed", std::string(name) + " exited with " + number((uint64_t)result.exitCode));
    return true;
}

void Engine::putBackReadOnly(StepLog* log) {
    std::string why;
    if (!platform_->restoreReadOnly(&why)) log->line("the system partition could not be made read-only again: " + why);
}

bool Engine::rollBack(StepLog* log) {
    std::vector<Op> ops;
    std::string why;
    if (!readOps(journalPath(), &ops, &why)) {
        log->line("cannot undo: " + why);
        return false;
    }
    for (size_t i = ops.size(); i-- > 0;) {
        const Op& op = ops[i];
        bool ok = true;
        if (op.op == "place") {
            // If this undoing was itself interrupted and is being run again, the file that was moved aside for this
            // one may be back in place already. Its copy is gone then, and what is at the path must stay.
            bool restored = false;
            for (size_t k = 0; k < i; ++k) {
                if (ops[k].op == "backup" && ops[k].path == op.path && fs::kindOf(ops[k].to) == fs::kNone) restored = true;
            }
            // Otherwise the new file goes, and the temporary it may still have been written as.
            if (!restored) {
                ok = platform_->makeWritable(op.path, &why) && fs::removeFile(op.path, &why) &&
                     fs::removeFile(fs::tempIn(fs::parentOf(op.path)), &why);
            }
        } else if (op.op == "link") {
            if (fs::kindOf(op.path) == fs::kLink) ok = platform_->makeWritable(op.path, &why) && fs::removeFile(op.path, &why);
        } else if (op.op == "unlink") {
            if (fs::kindOf(op.path) == fs::kNone) ok = platform_->makeWritable(op.path, &why) && fs::makeLink(op.target, op.path, &why);
        } else if (op.op == "backup") {
            // No copy at `to` means the file never left its place.
            if (fs::kindOf(op.to) != fs::kNone) ok = platform_->makeWritable(op.path, &why) && fs::moveFile(op.to, op.path, &why);
        } else if (op.op == "mkdir") {
            // A folder that something else has put a file into since is left.
            if (platform_->makeWritable(op.path, &why)) fs::removeEmptyDir(op.path);
        }
        if (!ok) {
            log->line("cannot undo: " + why);
            return false;
        }
        crash("undo");
    }
    clearJournalFolder();
    if (!fs::removeFile(journalPath(), &why)) {
        log->line("cannot undo: " + why);
        return false;
    }
    fs::syncDir(store_.journalDir());
    crash("undone");
    return true;
}

bool Engine::finishCommitted(const std::string& id, const std::vector<std::string>& rmdirs, StepLog* log) {
    std::string why;
    std::string record = store_.journalDir() + "/record", oldRecord = store_.journalDir() + "/old-record";
    std::string target = store_.recordDir(id);
    if (fs::kindOf(record) == fs::kDir) {
        if (fs::kindOf(target) == fs::kDir) {
            if (!fs::removeTree(oldRecord, &why) || rename(target.c_str(), oldRecord.c_str()) != 0) {
                log->line("cannot move the old record aside: " + why);
                return false;
            }
            crash("finish");
        }
        if (rename(record.c_str(), target.c_str()) != 0) {
            log->line("cannot put the new record in place");
            return false;
        }
        fs::syncDir(store_.installedDir());
        fs::syncDir(store_.journalDir());
        crash("finish");
    }
    // Folders only the old version used. Best effort: one that still holds something stays.
    for (size_t i = 0; i < rmdirs.size(); ++i) {
        if (platform_->makeWritable(rmdirs[i], &why)) fs::removeEmptyDir(rmdirs[i]);
    }
    clearJournalFolder();
    crash("finish");
    if (!fs::removeFile(journalPath(), &why)) {
        log->line("cannot remove the journal: " + why);
        return false;
    }
    fs::syncDir(store_.journalDir());
    crash("finished");
    return true;
}

bool Engine::removeFiles(const InstalledPackage& p, bool deleteData, bool runPreRemove, StepLog* log, Error* error) {
    const Manifest& m = p.manifest;
    std::string recordDir = store_.recordDir(m.id);
    std::string hooksDir = recordDir + "/hooks";
    std::string why;
    Error failure;   // the first thing that could not be removed; the rest is still tried

    // A failing remove hook is logged, and the removal goes on.
    if (runPreRemove) {
        log->phase("pre-remove");
        Error hookError;
        if (!runHook(m, kPreRemove, hooksDir, "remove", "", log, &hookError)) log->line(hookError.message + "; removing anyway");
        crash("action");
    }
    if (observer_ != 0) observer_->programsStopped(m);

    log->phase("remove");
    for (size_t i = 0; i < p.files.size(); ++i) {
        const InstalledFile& f = p.files[i];
        if (startsWith(f.dest, "${data}")) continue;   // kept, or deleted with the folder below
        std::string real = store_.resolve(m, f.dest);
        fs::Kind kind = fs::kindOf(real);
        // Outside the package's own folder, a file that is no longer the one the package placed is not ours to
        // touch: a firmware update may have put its own there since. A config file is expected to have changed.
        bool ours = true;
        if (!startsWith(f.dest, "${app}/") && kind == fs::kFile && !f.config) {
            uint8_t digest[kSha256Bytes];
            ours = fs::hashFile(real, digest, 0, &why) && toHex(digest, sizeof digest) == f.sha256;
        }
        bool ok = true;
        if (!ours) {
            log->line(f.dest + " was changed since it was installed; it is left as it is");
        } else if (!f.backup.empty()) {
            // The stock file goes back. No backup left means it went back before a power cut: the file is stock.
            std::string backup = recordDir + "/" + f.backup;
            if (fs::kindOf(backup) == fs::kFile) ok = platform_->makeWritable(real, &why) && fs::moveFile(backup, real, &why);
        } else if (kind == fs::kFile || kind == fs::kLink) {
            ok = platform_->makeWritable(real, &why) && fs::removeFile(real, &why);
        }
        if (!ok) failure.fail("io-error", why);
        crash("action");
    }
    for (size_t i = 0; i < p.links.size(); ++i) {
        if (startsWith(p.links[i].path, "${data}")) continue;
        std::string real = store_.resolve(m, p.links[i].path);
        if (fs::kindOf(real) == fs::kLink && !(platform_->makeWritable(real, &why) && fs::removeFile(real, &why))) {
            failure.fail("io-error", why);
        }
        crash("action");
    }
    for (size_t i = p.dirs.size(); i-- > 0;) {
        if (startsWith(p.dirs[i], "${data}") || startsWith(p.dirs[i], "${app}")) continue;
        std::string real = store_.resolve(m, p.dirs[i]);
        if (platform_->makeWritable(real, &why)) fs::removeEmptyDir(real);
    }
    // ${app} is the package's own: whatever is left in it goes too.
    if (!fs::removeTree(store_.appDir(m), &why)) failure.fail("io-error", why);
    if (deleteData && !fs::removeTree(platform_->dataDir(m.id), &why)) failure.fail("io-error", why);
    // ${tmp} holds logs only, on the RAM disk: it goes with the package, data or not, and nothing hangs on it.
    fs::removeTree(platform_->tmpDir(m.id), 0);
    crash("action");

    if (!failure.code.empty()) {
        // The record stays, so the removal can be asked for again; the journal goes, so other steps can run.
        log->line("the removal did not complete: " + failure.message);
        fs::removeFile(journalPath(), 0);
        *error = failure;
        return false;
    }

    log->phase("post-remove");
    Error hookError;
    if (!runHook(m, kPostRemove, hooksDir, "remove", "", log, &hookError)) log->line(hookError.message);
    crash("action");

    // The record disappears in one rename, so it is never seen half deleted.
    log->phase("record");
    std::string oldRecord = store_.journalDir() + "/old-record";
    if (!fs::removeTree(oldRecord, &why) || rename(recordDir.c_str(), oldRecord.c_str()) != 0) {
        fs::removeFile(journalPath(), 0);
        return error->fail("io-error", "cannot remove the record of " + m.id);
    }
    fs::syncDir(store_.installedDir());
    crash("action");
    clearJournalFolder();
    fs::removeFile(journalPath(), 0);
    fs::syncDir(store_.journalDir());
    crash("finished");
    return true;
}

StepReport Engine::remove(const std::string& id, bool deleteData, StepListener* listener) {
    StepReport report = removeStep(id, deleteData, listener);
    if (observer_ != 0) observer_->stepEnded();
    return report;
}

StepReport Engine::removeStep(const std::string& id, bool deleteData, StepListener* listener) {
    StepLog log(this, listener);
    StepReport report;
    report.action = "remove";
    report.id = id;
    log.begin("remove " + id);
    std::string why;
    if (fs::kindOf(journalPath()) != fs::kNone) {
        report.error.fail("internal", "an interrupted step has to be recovered first");
        return report;
    }
    std::vector<InstalledPackage> installed;
    if (!store_.loadAll(&installed, &why)) {
        report.error.fail("internal", why);
        return report;
    }
    std::vector<Problem> problems;
    checkRemove(id, installed, &problems);
    if (!problems.empty()) {
        report.error.fail(problems[0].code.c_str(), problems[0].message);
        log.line("refused: " + problems[0].message);
        return report;
    }
    const InstalledPackage* package = findInstalled(installed, id);
    report.version = package->manifest.version;

    clearJournalFolder();
    Journal journal;
    json::Writer begin;
    begin.beginObject().key("op").string("begin").key("action").string("remove").key("id").string(id);
    begin.key("version").string(report.version).key("deleteData").boolean(deleteData).endObject();
    if (!journal.create(journalPath(), &why) || !journal.append(begin.text(), &why)) {
        journal.close();
        fs::removeFile(journalPath(), 0);
        report.error.fail("io-error", why);
        return report;
    }
    journal.close();
    crash("begin");

    bool ok = removeFiles(*package, deleteData, true, &log, &report.error);
    putBackReadOnly(&log);
    report.outcome = ok ? kStepDone : kStepBroken;
    log.line(ok ? "removed " + id + " " + report.version : "failed: " + report.error.message);
    return report;
}

bool Engine::recover(bool* found, StepReport* report, StepListener* listener) {
    *found = false;
    *report = StepReport();
    std::string why;
    if (fs::kindOf(journalPath()) == fs::kNone) {
        clearJournalFolder();
        settleTrial();
        return true;
    }
    std::vector<Op> ops;
    if (!readOps(journalPath(), &ops, &why)) {
        StepLog log(this, listener);
        log.begin("recover");
        *found = true;
        report->outcome = kStepBroken;
        report->error.fail("internal", why);
        log.line("cannot recover: " + why);
        return true;
    }
    if (ops.empty() || ops[0].op != "begin") {
        // The journal was cut off before its first line was whole: nothing had started.
        clearJournalFolder();
        fs::removeFile(journalPath(), 0);
        settleTrial();
        return true;
    }
    *found = true;
    const Op begin = ops[0];   // a copy: `ops` grows below
    report->action = begin.action;
    report->id = begin.id;
    report->version = begin.version;
    report->oldVersion = begin.oldVersion;
    bool committed = false;
    for (size_t i = 0; i < ops.size(); ++i) committed = committed || ops[i].op == "commit";

    // The agent's own package: placed by one agent, and decided by the next. See engine.hpp.
    bool own = begin.id == kAgentPackageId && begin.action != "remove";
    bool confirmed = false;
    const char* undoneCode = "interrupted";
    const char* undoneText = "the step was interrupted and has been undone";
    if (own && !committed) {
        AgentHome::Files files = AgentHome(platform_->agentHome()).read();
        if (files.current == begin.version) {
            confirmed = true;
        } else if (begin.version == agentVersion_) {
            // On trial. Nothing is written, the log included: until this version is confirmed, the version
            // before it has to find everything as it left it.
            report->outcome = kStepRestart;
            return true;
        } else if (files.current != agentVersion_) {
            StepLog log(this, listener);
            log.begin("recover");
            report->outcome = kStepBroken;
            report->error.fail("internal", "an update of the agent to " + begin.version + " is under way, and this agent, " +
                                               agentVersion_ + ", is neither that version nor the one the launcher starts");
            log.line("left alone: " + report->error.message);
            return true;
        } else if (files.any()) {
            undoneCode = "not-confirmed";
            undoneText = files.started ? "no controller reached the new version of the agent, so the one before it is back"
                                       : "the new version of the agent was never tried, so the one before it stays";
        }
    }

    StepLog log(this, listener);
    log.begin("recover");
    if (confirmed) {
        log.line("version " + begin.version + " of the agent is confirmed: finishing the step that placed it");
        // The line the agent that placed it left out. Nothing of the old version is among the folders to remove.
        Journal journal;
        json::Writer commit;
        commit.beginObject().key("op").string("commit").key("rmdirs").beginArray().endArray().endObject();
        if (!journal.reopen(journalPath(), &why) || !journal.append(commit.text(), &why)) {
            report->outcome = kStepBroken;
            report->error.fail("io-error", why);
            log.line("cannot finish: " + why);
            return true;
        }
        journal.close();
        crash("commit");
        Op line;
        line.op = "commit";
        ops.push_back(line);
    } else {
        log.line("an interrupted " + begin.action + " of " + begin.id + " " + begin.version + " was found");
    }
    const Op& first = begin;

    if (first.action == "remove") {
        // A removal is finished. With the record gone already, only the clearing up was left.
        bool ok = true;
        if (store_.isInstalled(first.id)) {
            InstalledPackage package;
            ok = store_.load(first.id, &package, &why) && removeFiles(package, first.deleteData, false, &log, &report->error);
            if (!ok && report->error.code.empty()) report->error.fail("internal", why);
        } else {
            clearJournalFolder();
            fs::removeFile(journalPath(), 0);
        }
        report->outcome = ok ? kStepDone : kStepBroken;
    } else {
        const Op* commit = 0;
        for (size_t i = 0; i < ops.size(); ++i) {
            if (ops[i].op == "commit") commit = &ops[i];
        }
        if (commit != 0) {
            log.phase("record");
            if (finishCommitted(first.id, commit->rmdirs, &log)) {
                report->outcome = kStepDone;
                InstalledPackage package;
                if (store_.load(first.id, &package, &why)) report->restart = package.manifest.reboot;
            } else {
                report->outcome = kStepBroken;
                report->error.fail("io-error", "the install was decided but could not be finished");
            }
        } else {
            log.phase("rollback");
            if (rollBack(&log)) {
                report->outcome = kStepFailed;
                report->error.fail(undoneCode, undoneText);
                // The folder the new version of the agent was in, if a hook or the program itself left
                // something in it that the journal does not know.
                if (own && first.version != agentVersion_) AgentHome(platform_->agentHome()).removeVersion(first.version, 0);
            } else {
                report->outcome = kStepBroken;
                report->error.fail("interrupted", "the step was interrupted and could not be undone");
            }
        }
    }
    putBackReadOnly(&log);
    log.line(report->outcome == kStepDone ? "recovered: the step is complete"
                                          : "recovered: " + report->error.message);
    if (fs::kindOf(journalPath()) == fs::kNone) {
        settleTrial();
        if (own && report->outcome == kStepDone) pruneVersions(first.version, first.oldVersion);
    }
    return true;
}

// ---- the agent's own folder ---------------------------------------------------------------------------------------

TrialState Engine::trialState() const {
    return AgentHome::stateOf(AgentHome(platform_->agentHome()).read(), agentVersion_);
}

bool Engine::beginTrial(const std::string& version, std::string* error) {
    AgentHome home(platform_->agentHome());
    if (!home.hasProgram(version)) {
        if (error != 0) *error = "version " + version + " of the agent is not in " + home.versionDir(version);
        return false;
    }
    bool ok = home.writeTrial(version, error);
    crash("trial");
    return ok;
}

bool Engine::confirmTrial(std::string* error) {
    AgentHome home(platform_->agentHome());
    if (AgentHome::stateOf(home.read(), agentVersion_) != kOnTrial) {
        if (error != 0) *error = "this agent is not on trial";
        return false;
    }
    bool ok = home.writeCurrent(agentVersion_, error);
    crash("confirm");
    return ok;
}

void Engine::settleTrial() {
    AgentHome home(platform_->agentHome());
    AgentHome::Files files = home.read();
    TrialState state = AgentHome::stateOf(files, agentVersion_);
    // An update that is not this agent's to decide, or not decided yet, is left as it is.
    if (state == kOnTrial || state == kNotOurs) return;
    home.removeTemp();
    if (state == kWayBack) {
        // The version that was tried, or was to be, goes before the file that names it: an agent that stops in
        // between finds the file again and comes back here.
        if (!files.pending.empty() && files.pending != files.current) home.removeVersion(files.pending, 0);
        crash("settle");
    }
    if (state == kWayBack || state == kConfirmed) home.removeTrial(0);
}

void Engine::pruneVersions(const std::string& now, const std::string& before) {
    AgentHome home(platform_->agentHome());
    AgentHome::Files files = home.read();
    if (files.any()) return;
    std::vector<std::string> all;
    home.versions(&all);
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i] == now || all[i] == before || all[i] == agentVersion_ || all[i] == files.current) continue;
        home.removeVersion(all[i], 0);
    }
}

// ---- an install or upgrade ----------------------------------------------------------------------------------------

class InstallStep {
public:
    InstallStep(Engine* engine, StepListener* listener, bool quiet = false)
        : engine_(engine), log_(engine, listener, quiet), manifest_(0), undecided_(0), old_(0), keyEmbedded_(false),
          self_(false), backups_(0), moved_(0) {}

    StepReport run(const std::string& archivePath, const std::string& expectedManifestSha256,
                   const std::string& signatureHex);
    // The checks of plan() and checkSpace() alone, for Engine::precheck.
    bool check(const Manifest& manifest, const std::vector<InstalledPackage>& installed,
               const std::set<std::string>& undecided, Error* error);

private:
    // What happens at one destination of the new package.
    struct FileAction {
        std::string dest, real, unpacked;
        size_t file, content;
        enum What { kPlace, kKeep, kUnchanged } what;
        bool moveAside;             // what is there now goes to journal/old first
        bool backupStock;           // what is there now is a stock file: it goes into the new record first
        std::string carryBackup;    // the old record's backup of the stock file, to carry into the new record
        std::string recordBackup;   // where the new record has the stock file, if it has one

        FileAction() : file(0), content(0), what(kPlace), moveAside(false), backupStock(false) {}
    };
    // A file of the old version that the new one does not have.
    struct Removal {
        std::string real;
        bool exists;
        std::string restoreFrom;   // the old record's backup of the stock file that goes back, if any
    };
    struct LinkAction {
        std::string dest, real, target;
        bool make;                 // false: the right link is there already
        bool unlinkFirst;          // a link with another target is there
        std::string oldTarget;
        bool moveAside;            // a file is there, in the package's own folders
        LinkAction() : make(true), unlinkFirst(false), moveAside(false) {}
    };

    const Manifest& m() const { return manifest_ != 0 ? *manifest_ : package_.manifest(); }
    bool undecided(const std::string& real) const { return undecided_ != 0 && undecided_->count(real) != 0; }
    const Store& store() const { return engine_->store_; }
    Platform* platform() const { return engine_->platform_; }

    bool fail(const char* code, const std::string& message) { return report_.error.fail(code, message); }
    bool ioFail(const std::string& why) { return fail("io-error", why); }
    bool writable(const std::string& path) {
        std::string why;
        return platform()->makeWritable(path, &why) || ioFail(why);
    }

    bool unpack();
    bool plan();
    bool planFile(const Placement& placement, const std::map<std::string, std::string>& others,
                  const std::map<std::string, size_t>& oldFiles, const std::map<std::string, std::string>& oldLinks);
    bool planLink(const ManifestLink& link, const std::map<std::string, std::string>& others,
                  const std::map<std::string, size_t>& oldFiles, const std::map<std::string, std::string>& oldLinks);
    bool checkHmiIds();
    bool checkSpace();
    // The agent's own package only: what plan() refuses of it, and the look at the program it has placed.
    bool checkSelf();
    bool checkProgram();
    bool begin();
    bool apply();
    bool writeRecord();

    // Each of these writes its line to the journal first and makes the change second.
    bool note(const json::Writer& line);
    bool moveAway(const std::string& real, const std::string& to);
    bool placeFile(const std::string& from, const std::string& real, int mode);
    bool ensureDir(const std::string& dest);
    bool ensureParents(const std::string& dest);
    bool removeLink(const std::string& real, const std::string& target);
    std::string nextMoved() { return store().journalDir() + "/old/" + number(moved_++); }

    Engine* engine_;
    StepLog log_;
    StepReport report_;
    FileSource source_;
    Package package_;
    const Manifest* manifest_;                  // precheck only: the manifest, with no archive behind it
    const std::set<std::string>* undecided_;    // precheck only
    std::vector<InstalledPackage> installed_;
    const InstalledPackage* old_;
    std::string keyHex_;
    bool keyEmbedded_;
    bool self_;                                 // the package is the agent's own
    std::string unpackDir_;
    Journal journal_;

    std::vector<FileAction> files_;
    std::vector<Removal> removals_;
    std::vector<LinkAction> links_;
    std::vector<std::pair<std::string, std::string> > linkRemovals_;   // real path, target
    std::vector<std::string> createdDirs_;                             // as destinations
    std::vector<std::string> rmdirs_;                                  // real paths
    unsigned backups_, moved_;
};

StepReport Engine::install(const std::string& archivePath, const std::string& expectedManifestSha256, StepListener* listener,
                           const std::string& signatureHex) {
    StepReport report;
    {
        InstallStep step(this, listener);
        report = step.run(archivePath, expectedManifestSha256, signatureHex);
    }
    if (observer_ != 0) observer_->stepEnded();
    return report;
}

bool Engine::precheck(const Manifest& manifest, const std::vector<InstalledPackage>& installed,
                      const std::set<std::string>& undecided, Error* error) {
    InstallStep step(this, 0, true);
    return step.check(manifest, installed, undecided, error);
}

bool InstallStep::check(const Manifest& manifest, const std::vector<InstalledPackage>& installed,
                        const std::set<std::string>& undecided, Error* error) {
    manifest_ = &manifest;
    undecided_ = &undecided;
    installed_ = installed;
    old_ = findInstalled(installed_, manifest.id);
    self_ = manifest.id == kAgentPackageId;
    bool ok = plan() && checkSpace();
    *error = report_.error;
    return ok;
}

StepReport InstallStep::run(const std::string& archivePath, const std::string& expectedManifestSha256,
                            const std::string& signatureHex) {
    report_.action = "install";
    log_.begin("install " + archivePath);
    std::string why;
    if (fs::kindOf(engine_->journalPath()) != fs::kNone) {
        fail("internal", "an interrupted step has to be recovered first");
        return report_;
    }

    // Everything up to and including preInstall changes nothing outside staging, so a failure needs no undoing.
    bool ready = false;
    do {
        log_.phase("verify");
        if (!source_.open(archivePath, &why)) {
            ioFail(why);
            break;
        }
        // The signature the plan checked, if it gave one, is the one checked here too: for a package its author
        // signed, manifest.sig is the author's and the catalogue's signature comes with the offer.
        std::string signature;
        if (!signatureHex.empty()) {
            signature.assign(kEd25519SignatureBytes, '\0');
            if (!fromHex(signatureHex, (uint8_t*)&signature[0], signature.size())) {
                fail("bad-signature", "the planned signature is not 128 hex characters");
                break;
            }
        }
        if (!package_.open(&source_, engine_->keys_, signature, &report_.error)) break;
        report_.id = m().id;
        report_.version = m().version;
        if (!expectedManifestSha256.empty() && expectedManifestSha256 != package_.manifestSha256()) {
            fail("bad-archive", "the archive holds another manifest than the planned one");
            break;
        }
        if (!store().loadAll(&installed_, &why)) {
            fail("internal", why);
            break;
        }
        old_ = findInstalled(installed_, m().id);
        self_ = m().id == kAgentPackageId;
        if (self_) {
            // What it comes after is the version that runs, whatever a record says, and if there is none.
            report_.action = "upgrade";
            report_.oldVersion = engine_->agentVersion_;
        } else if (old_ != 0) {
            report_.action = "upgrade";
            report_.oldVersion = old_->manifest.version;
        }
        const TrustedKey& key = engine_->keys_[package_.keyIndex()];
        keyHex_ = toHex(key.key, sizeof key.key);
        keyEmbedded_ = key.embedded;

        std::vector<Problem> problems;
        std::vector<MissingDependency> missing;
        checkInstall(m(), keyHex_, keyEmbedded_, installed_, engine_->agentVersion_, &problems, &missing);
        if (!problems.empty()) {
            fail(problems[0].code.c_str(), problems[0].message);
            break;
        }
        if (!missing.empty()) {
            // A plan installs dependencies first, so this is a fault in whatever ordered the steps.
            fail("internal", "the dependency " + missing[0].id + " " + missing[0].range + " is not installed");
            break;
        }
        if (!unpack() || !plan() || !checkHmiIds() || !checkSpace()) break;

        log_.phase("pre-install");
        if (!engine_->runHook(m(), kPreInstall, unpackDir_, report_.action.c_str(), report_.oldVersion, &log_, &report_.error)) break;
        if (engine_->observer_ != 0) engine_->observer_->programsStopped(m());
        ready = true;
    } while (false);
    if (!ready) {
        log_.line("failed: " + report_.error.message);
        engine_->clearJournalFolder();
        return report_;
    }

    if (!begin()) {
        log_.line("failed: " + report_.error.message);
        engine_->clearJournalFolder();
        return report_;
    }
    if (!apply()) {
        journal_.close();
        log_.line("failed: " + report_.error.message);
        log_.phase("rollback");
        bool undone = engine_->rollBack(&log_);
        engine_->putBackReadOnly(&log_);
        report_.outcome = undone ? kStepFailed : kStepBroken;
        log_.line(undone ? "everything was put back" : "NOT everything could be put back");
        return report_;
    }
    journal_.close();

    if (self_) {
        // No commit line: the version that runs is as it was, and the new one is decided by itself, later.
        engine_->putBackReadOnly(&log_);
        report_.outcome = kStepRestart;
        report_.restart = m().reboot;
        log_.line("version " + m().version + " of the agent is placed. It takes over once the launcher has started it and a "
                  "controller has reached it");
        return report_;
    }

    // The commit line is written: the step is decided, and what is left is clearing up.
    bool finished = engine_->finishCommitted(m().id, rmdirs_, &log_);
    engine_->putBackReadOnly(&log_);
    if (!finished) {
        report_.outcome = kStepBroken;
        fail("io-error", "the install was decided but could not be finished");
        return report_;
    }
    report_.outcome = kStepDone;
    report_.restart = m().reboot;
    log_.line(report_.action + " of " + m().id + " " + m().version + " is complete");
    return report_;
}

bool InstallStep::unpack() {
    std::string why;
    unpackDir_ = store().unpackDir() + "/" + m().id + "-" + m().version;
    if (!fs::removeTree(store().unpackDir(), &why) || !fs::makeDirs(unpackDir_, &why)) return ioFail(why);
    uint64_t total = 0, free = 0;
    for (size_t i = 0; i < m().contents.size(); ++i) total += m().contents[i].size;
    if (platform()->freeBytes(unpackDir_, &free) && free < total + platform()->reserveBytes(unpackDir_)) {
        return fail("no-space", "unpacking needs " + number(total) + " bytes in staging, and " +
                                    number(platform()->reserveBytes(unpackDir_)) + " are kept free; " + number(free) + " are free");
    }
    for (size_t i = 0; i < m().contents.size(); ++i) {
        std::string path = unpackDir_ + "/" + m().contents[i].path;
        fs::FileWriter writer;
        if (!fs::makeDirs(fs::parentOf(path), &why) || !writer.open(path, &why)) return ioFail(why);
        if (!package_.extract(m().contents[i].path, &writer, &report_.error)) return false;
        if (!writer.commit(0644, false, &why)) return ioFail(why);
        log_.idle();
    }
    return true;
}

// Works out what the step will do at every destination, and refuses what it may not do. Nothing is changed here.
bool InstallStep::plan() {
    if (self_ && !checkSelf()) return false;
    // Every path another package owns, and every path the old version of this one owns.
    std::map<std::string, std::string> others;
    std::map<std::string, size_t> oldFiles;
    std::map<std::string, std::string> oldLinks;
    for (size_t i = 0; i < installed_.size(); ++i) {
        const InstalledPackage& p = installed_[i];
        bool own = &p == old_;
        for (size_t k = 0; k < p.files.size(); ++k) {
            std::string real = store().resolve(p.manifest, p.files[k].dest);
            if (own) oldFiles[real] = k;
            else others[real] = p.manifest.id;
        }
        for (size_t k = 0; k < p.links.size(); ++k) {
            std::string real = store().resolve(p.manifest, p.links[k].path);
            if (own) oldLinks[real] = p.links[k].target;
            else others[real] = p.manifest.id;
        }
    }

    std::set<std::string> placed, placedLinks;
    for (size_t i = 0; i < m().placements.size(); ++i) {
        if (!planFile(m().placements[i], others, oldFiles, oldLinks)) return false;
        placed.insert(files_.back().real);
    }
    for (size_t i = 0; i < m().links.size(); ++i) {
        if (!planLink(m().links[i], others, oldFiles, oldLinks)) return false;
        placed.insert(links_.back().real);
        placedLinks.insert(links_.back().real);
    }
    // Two destinations of one manifest can still be one real path: ${plugins}/x and the same folder written out.
    if (placed.size() != files_.size() + links_.size()) return fail("file-owned", "the package places one path twice");

    if (old_ != 0) {
        std::string oldDir = store().recordDir(m().id);
        for (size_t i = 0; i < old_->files.size(); ++i) {
            const InstalledFile& f = old_->files[i];
            std::string real = store().resolve(old_->manifest, f.dest);
            if (placed.count(real) != 0 || startsWith(f.dest, "${data}")) continue;   // ${data} is left alone
            // The agent's own package: the version that runs stays in its folder, whole.
            if (self_ && startsWith(f.dest, "${app}")) continue;
            Removal r;
            r.real = real;
            fs::Kind kind = fs::kindOf(real);
            r.exists = kind == fs::kFile || kind == fs::kLink;
            // As in a removal: outside ${app}, a file that is no longer what the old version placed is left alone.
            if (!startsWith(f.dest, "${app}/") && kind == fs::kFile && !f.config) {
                uint8_t digest[kSha256Bytes];
                std::string why;
                if (!fs::hashFile(real, digest, 0, &why)) return ioFail(why);
                if (toHex(digest, sizeof digest) != f.sha256) {
                    log_.line(f.dest + " was changed since it was installed; it is left as it is");
                    continue;
                }
            }
            if (!f.backup.empty() && fs::kindOf(oldDir + "/" + f.backup) == fs::kFile) r.restoreFrom = oldDir + "/" + f.backup;
            if (r.exists || !r.restoreFrom.empty()) removals_.push_back(r);
        }
        // An old link where the new version has a link is dealt with there. One where it has a file, or nothing,
        // is removed here, before the file is placed.
        for (size_t i = 0; i < old_->links.size(); ++i) {
            std::string real = store().resolve(old_->manifest, old_->links[i].path);
            if (placedLinks.count(real) != 0) continue;
            if (self_ && startsWith(old_->links[i].path, "${app}")) continue;
            if (placed.count(real) == 0 && startsWith(old_->links[i].path, "${data}")) continue;
            std::string target;
            if (fs::kindOf(real) == fs::kLink && fs::readLink(real, &target)) linkRemovals_.push_back(std::make_pair(real, target));
        }
    }
    return true;
}

bool InstallStep::planFile(const Placement& placement, const std::map<std::string, std::string>& others,
                           const std::map<std::string, size_t>& oldFiles, const std::map<std::string, std::string>& oldLinks) {
    const ManifestFile& entry = m().files[placement.file];
    const ManifestContent& content = m().contents[placement.content];
    FileAction a;
    a.dest = placement.dest;
    a.real = store().resolve(m(), placement.dest);
    a.unpacked = unpackDir_ + "/" + placement.src;
    a.file = placement.file;
    a.content = placement.content;
    std::string why;
    if (undecided(a.real)) {
        a.what = FileAction::kUnchanged;   // counted nowhere; the step itself decides
        files_.push_back(a);
        return true;
    }

    bool own = startsWith(a.dest, "${app}/") || startsWith(a.dest, "${data}/");
    if (!own && (under(a.real, platform()->stateDir()) || under(a.real, platform()->stagingDir()))) {
        return fail("file-owned", a.dest + " is inside the agent's own folders");
    }
    std::map<std::string, std::string>::const_iterator other = others.find(a.real);
    if (other != others.end()) return fail("file-owned", a.dest + " belongs to " + other->second);

    fs::Kind kind = fs::kindOf(a.real);
    // A link of the old version at this place is removed before anything is placed.
    if (kind == fs::kLink && oldLinks.count(a.real) != 0) kind = fs::kNone;
    if (kind == fs::kDir || kind == fs::kOther || kind == fs::kLink) return ioFail(a.dest + ": something that is not a file is in the way");

    uint8_t digest[kSha256Bytes];
    std::string current;
    struct stat st;
    unsigned currentMode = 0;
    if (kind == fs::kFile) {
        if (!fs::hashFile(a.real, digest, 0, &why)) return ioFail(why);
        current = toHex(digest, sizeof digest);
        if (stat(a.real.c_str(), &st) == 0) currentMode = (unsigned)(st.st_mode & 0777);
    }

    std::map<std::string, size_t>::const_iterator oldIt = oldFiles.find(a.real);
    if (oldIt != oldFiles.end()) {
        // The old version of this package put the file there.
        const InstalledFile& of = old_->files[oldIt->second];
        bool stockAgain = false;
        if (!of.backup.empty()) {
            // It replaced a stock file. The new version must be allowed to replace that file too.
            if (!entry.hasReplaces) return fail("stock-mismatch", a.dest + " replaced a stock file, and this version does not say it may");
            std::string original;
            if (kind == fs::kFile && current != of.sha256 && !entry.config) {
                // What is there is no longer what the old version placed: a firmware update put a stock file
                // there again. That file is the one to check and to keep, and the older backup is dropped.
                original = current;
                stockAgain = true;
            } else {
                if (!fs::hashFile(store().recordDir(m().id) + "/" + of.backup, digest, 0, &why)) return ioFail(why);
                original = toHex(digest, sizeof digest);
                a.carryBackup = of.backup;
            }
            bool allowed = false;
            for (size_t i = 0; i < entry.replaces.size(); ++i) allowed = allowed || entry.replaces[i] == original;
            if (!allowed) return fail("stock-mismatch", a.dest + ": the stock file is not one this version replaces");
        }
        if (stockAgain) {
            a.what = FileAction::kPlace;
            a.backupStock = true;
        } else if (kind == fs::kNone) {
            a.what = FileAction::kPlace;
        } else if (entry.config && current != of.sha256) {
            a.what = FileAction::kKeep;        // changed on the head unit since it was installed
        } else if (current == content.sha256 && currentMode == entry.mode) {
            a.what = FileAction::kUnchanged;   // already what this version places
        } else {
            a.what = FileAction::kPlace;
            a.moveAside = true;
        }
    } else if (entry.hasReplaces) {
        // A stock file is expected there, and it must be one of the listed ones.
        bool allowed = false;
        for (size_t i = 0; kind == fs::kFile && i < entry.replaces.size(); ++i) allowed = allowed || entry.replaces[i] == current;
        if (!allowed) {
            return fail("stock-mismatch", a.dest + (kind == fs::kFile ? ": the file there is not one this package replaces"
                                                                        : ": there is no file to replace"));
        }
        a.what = FileAction::kPlace;
        a.backupStock = true;
    } else if (kind == fs::kNone) {
        a.what = FileAction::kPlace;
    } else if (own) {
        // Something is there that no record knows: left from before, or made by a hook. A config file is kept.
        if (entry.config) {
            a.what = FileAction::kKeep;
        } else {
            a.what = FileAction::kPlace;
            a.moveAside = true;
        }
    } else {
        return fail("stock-mismatch", a.dest + " exists, and the package does not say that it replaces it");
    }
    files_.push_back(a);
    return true;
}

bool InstallStep::planLink(const ManifestLink& link, const std::map<std::string, std::string>& others,
                           const std::map<std::string, size_t>& oldFiles, const std::map<std::string, std::string>& oldLinks) {
    LinkAction a;
    a.dest = link.path;
    a.real = store().resolve(m(), link.path);
    a.target = link.target;
    if (undecided(a.real)) {
        a.make = false;
        links_.push_back(a);
        return true;
    }
    bool own = startsWith(a.dest, "${app}/") || startsWith(a.dest, "${data}/");
    if (!own && (under(a.real, platform()->stateDir()) || under(a.real, platform()->stagingDir()))) {
        return fail("file-owned", a.dest + " is inside the agent's own folders");
    }
    std::map<std::string, std::string>::const_iterator other = others.find(a.real);
    if (other != others.end()) return fail("file-owned", a.dest + " belongs to " + other->second);

    fs::Kind kind = fs::kindOf(a.real);
    if (kind == fs::kLink) {
        std::string current;
        if (!fs::readLink(a.real, &current)) return ioFail("cannot read the link " + a.real);
        if (oldLinks.count(a.real) == 0 && !own) return fail("stock-mismatch", a.dest + " exists, and a link cannot replace it");
        if (current == a.target) {
            a.make = false;
        } else {
            a.unlinkFirst = true;
            a.oldTarget = current;
        }
    } else if (kind == fs::kFile) {
        // A file of the old version, or in the package's own folders a file nobody recorded: moved aside first.
        if (oldFiles.count(a.real) != 0 || own) {
            a.moveAside = true;
        } else {
            return fail("stock-mismatch", a.dest + " exists, and a link cannot replace it");
        }
    } else if (kind != fs::kNone) {
        return ioFail(a.dest + ": something that is not a file is in the way");
    }
    links_.push_back(a);
    return true;
}

// The connector id and the focus ids of a plugin this package places must not be in use by another app on the car.
bool InstallStep::checkHmiIds() {
    std::string plugins = platform()->pluginsDir();
    std::set<std::string> ours;   // what this package places now or placed before: not "another app"
    std::map<std::string, std::string> unpacked;
    for (size_t i = 0; i < files_.size(); ++i) {
        ours.insert(files_[i].real);
        unpacked[files_[i].real] = files_[i].unpacked;
    }
    if (old_ != 0) {
        for (size_t i = 0; i < old_->files.size(); ++i) ours.insert(store().resolve(old_->manifest, old_->files[i].dest));
    }

    std::vector<long> connectors, focus;
    for (size_t i = 0; i < files_.size(); ++i) {
        const FileAction& a = files_[i];
        if (!under(a.real, plugins) || !endsWith(a.real, ".manifest")) continue;
        std::string text, desc;
        if (!fs::readFile(a.unpacked, kMaxPluginFile, &text, 0)) continue;
        readPluginManifest(text, &connectors, &desc);
        if (!startsWith(desc, "{pluginDir}/")) continue;
        std::map<std::string, std::string>::const_iterator it = unpacked.find(fs::parentOf(a.real) + "/" + desc.substr(12));
        if (it != unpacked.end() && fs::readFile(it->second, kMaxPluginFile, &text, 0)) readPluginDescription(text, &focus);
    }
    if (connectors.empty() && focus.empty()) return true;

    // Every folder the HMI loads plugins from counts, not only the one this package goes into.
    std::set<long> usedConnectors, usedFocus;
    std::vector<std::string> folders;
    std::vector<std::string> scanned = platform()->pluginScanDirs();
    for (size_t s = 0; s < scanned.size(); ++s) {
        std::vector<std::string> names;
        if (!fs::listDir(scanned[s], &names, 0)) continue;
        for (size_t i = 0; i < names.size(); ++i) folders.push_back(scanned[s] + "/" + names[i]);
    }
    for (size_t i = 0; i < folders.size(); ++i) {
        const std::string& dir = folders[i];
        std::vector<std::string> names;
        if (!fs::isDir(dir) || !fs::listDir(dir, &names, 0)) continue;
        for (size_t k = 0; k < names.size(); ++k) {
            std::string path = dir + "/" + names[k];
            if (!endsWith(names[k], ".manifest") || ours.count(path) != 0) continue;
            std::string text, desc;
            std::vector<long> found;
            if (!fs::readFile(path, kMaxPluginFile, &text, 0)) continue;
            readPluginManifest(text, &found, &desc);
            usedConnectors.insert(found.begin(), found.end());
            found.clear();
            if (!startsWith(desc, "{pluginDir}/")) continue;
            std::string descPath = dir + "/" + desc.substr(12);
            if (ours.count(descPath) == 0 && fs::readFile(descPath, kMaxPluginFile, &text, 0)) readPluginDescription(text, &found);
            usedFocus.insert(found.begin(), found.end());
        }
    }
    for (size_t i = 0; i < connectors.size(); ++i) {
        if (builtInConnectorId(connectors[i]) || usedConnectors.count(connectors[i]) != 0) {
            return fail("hmi-id-taken", "the plugin's connector id " + number((uint64_t)connectors[i]) + " is in use");
        }
    }
    for (size_t i = 0; i < focus.size(); ++i) {
        if (usedFocus.count(focus[i]) != 0) {
            return fail("hmi-id-taken", "the plugin's entry-button focus id " + number((uint64_t)focus[i]) + " is in use");
        }
    }
    return true;
}

// The agent's own package (engine.hpp): what is refused of it, by the manifest and the agent's folder alone, so a
// plan makes these checks too.
bool InstallStep::checkSelf() {
    AgentHome home(platform()->agentHome());
    AgentHome::Files files = home.read();
    const std::string& running = engine_->agentVersion_;
    if (files.any()) return fail("conflict", "an update of the agent is under way and not decided yet");
    if (files.current != running || !home.hasProgram(running)) {
        return fail("no-launcher", "this agent, " + running + ", is not the version that " + home.dir() +
                                       "/current names, with its program and its launcher in a folder of that name: it "
                                       "was not put here for a starter to start, so nothing would start a newer version");
    }
    if (!validVersionName(m().version)) return fail("bad-manifest", "the version " + m().version + " cannot name a folder of the agent's");
    bool program = false, launcher = false;
    for (size_t i = 0; i < m().placements.size(); ++i) {
        const std::string& dest = m().placements[i].dest;
        if (!startsWith(dest, "${app}/") && !startsWith(dest, "${data}/")) {
            return fail("bad-manifest", "the agent's package places " + dest + ", which is outside ${app} and ${data}");
        }
        if (dest == std::string("${app}/") + kAgentProgram && (m().files[m().placements[i].file].mode & 0111) != 0) program = true;
        if (dest == std::string("${app}/") + kAgentLauncher) launcher = true;
    }
    for (size_t i = 0; i < m().links.size(); ++i) {
        const std::string& path = m().links[i].path;
        if (!startsWith(path, "${app}/") && !startsWith(path, "${data}/")) {
            return fail("bad-manifest", "the agent's package links " + path + ", which is outside ${app} and ${data}");
        }
    }
    if (!program) return fail("bad-manifest", std::string("the agent's package does not place ${app}/") + kAgentProgram + " as a program");
    // The starter runs a version's own launcher, so a version without one would never be started.
    if (!launcher) {
        return fail("bad-manifest", std::string("the agent's package does not place ${app}/") + kAgentLauncher + ", the launcher that starts this version");
    }
    // A version's folder is never written over, and one that is there was not put there by a step that ended.
    if (fs::kindOf(home.versionDir(m().version)) != fs::kNone) {
        return fail("already-installed", "version " + m().version + " of the agent is on the head unit already, in " +
                                             home.versionDir(m().version));
    }
    return true;
}

// What the new version of the agent printed when it was asked which version it is, or the shell when it was
// asked to read a script. `log` is the step's, or 0 outside a step.
class VersionOutput : public OutputSink {
public:
    explicit VersionOutput(StepLog* log) : log_(log) {}
    virtual void output(const char* data, size_t len) {
        size_t room = kMaxVersionOutput - text.size();
        text.append(data, len < room ? len : room);
    }
    virtual void idle() {
        if (log_ != 0) log_->idle();
    }
    std::string text;

private:
    StepLog* log_;
};

// Whether the platform's shell can read a script of the agent's: `sh -n`, which reads it to its end and runs
// none of it. A script the shell cannot read would start nothing. False with *why: what the shell said of it, or
// that the shell could not be asked.
static bool shellReads(Platform* platform, const std::string& script, StepLog* log, std::string* why) {
    std::vector<std::string> argv, env;
    argv.push_back(platform->shell());
    argv.push_back("-n");
    argv.push_back(script);
    env.push_back("PATH=" + platform->hookPath());
    VersionOutput said(log);
    ProcessResult result;
    if (!runProcess(platform->shell(), argv, env, fs::parentOf(script), kVersionTimeoutSeconds, &said, &result, why)) return false;
    if (result.timedOut || !result.exited || result.exitCode != 0) {
        std::string line = sanitizeUtf8(said.text.substr(0, said.text.find('\n')).substr(0, 200), 0);
        *why = line.empty() ? std::string("the shell refused it") : "the shell says \"" + line + "\"";
        return false;
    }
    return true;
}

void Engine::settleStarter(std::string* said) {
    said->clear();
    AgentHome home(platform_->agentHome());
    AgentHome::Files files = home.read();
    // Only the version that is decided: one on trial may yet go, and the starter of its time with it.
    if (files.any() || files.current != agentVersion_) return;
    if (home.starterOf(agentVersion_) != AgentHome::kStarterDiffers) return;
    const std::string mine = home.versionDir(agentVersion_) + "/" + kAgentStarter;
    const std::string placed = home.dir() + "/" + kStarterInPlace;
    std::string why;
    if (!shellReads(platform_, mine, 0, &why)) {
        *said = "the starter this version came with, " + mine + ", is not put in place of " + placed + ": " + why;
        return;
    }
    if (!home.placeStarter(agentVersion_, &why)) {
        *said = "the starter this version came with could not be put in place of " + placed + ": " + why;
        return;
    }
    *said = placed + " is now the starter that version " + agentVersion_ + " came with. A starter that is running goes on as "
            "it was read; the new one is what is started next";
}

// The agent's own package: does the new version start on this machine at all, and is it the version its package
// says it is? It is asked before it gets its try, as hu_install.sh asks the one it has copied. A version that
// took itself for another would not know that it is on trial.
bool InstallStep::checkProgram() {
    std::string dir = store().appDir(m());
    std::vector<std::string> argv, env;
    argv.push_back(dir + "/" + kAgentProgram);
    argv.push_back("--version");
    env.push_back("PATH=" + platform()->hookPath());
    engine_->putBackReadOnly(&log_);
    log_.line("asking the new version which one it is: " + argv[0] + " --version");
    VersionOutput said(&log_);
    ProcessResult result;
    std::string why;
    if (!runProcess(platform()->shell(), argv, env, dir, kVersionTimeoutSeconds, &said, &result, &why)) {
        return fail("not-confirmed", "the new version of the agent could not be started: " + why);
    }
    std::string line = sanitizeUtf8(said.text.substr(0, said.text.find('\n')).substr(0, 200), 0);
    if (result.timedOut || !result.exited || result.exitCode != 0) {
        return fail("not-confirmed", "the new version of the agent does not start here" + (line.empty() ? std::string() : ": " + line));
    }
    if (!startsWith(line + " ", std::string(kAgentProgram) + " " + m().version + " ")) {
        return fail("not-confirmed", "the new version of the agent says \"" + line + "\", and its package says it is " + m().version);
    }
    log_.line(line);
    // And its launcher, which the starter runs to give it its try: one the shell cannot read would only cost
    // the try.
    const std::string launcher = dir + "/" + kAgentLauncher;
    log_.line("having the shell read the new version's launcher: " + platform()->shell() + " -n " + launcher);
    if (!shellReads(platform(), launcher, &log_, &why)) {
        return fail("not-confirmed", "the new version's launcher, " + std::string(kAgentLauncher) + ", would not start it here: " + why);
    }
    engine_->crash("action");
    return true;
}

// Enough room for the new files where they go, and for what is moved aside where that goes, with the platform's
// reserve left over on each file system. A replaced file frees its room only at the commit, so it is not counted
// as free.
bool InstallStep::checkSpace() {
    std::vector<std::pair<std::string, uint64_t> > needs;   // a path on a file system, and the bytes needed there
    std::string journal = store().journalDir();
    for (size_t i = 0; i < files_.size(); ++i) {
        const FileAction& a = files_[i];
        std::string where;
        uint64_t bytes = 0;
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 0) {
                if (a.what != FileAction::kPlace) continue;
                where = a.real;
                bytes = m().contents[a.content].size;
            } else {
                // Moving aside within one file system is a rename and needs no room.
                struct stat st;
                if (!(a.moveAside || a.backupStock) || fs::sameDevice(a.real, journal) || stat(a.real.c_str(), &st) != 0) continue;
                where = journal;
                bytes = (uint64_t)st.st_size;
            }
            size_t g = 0;
            while (g < needs.size() && !fs::sameDevice(needs[g].first, where)) ++g;
            if (g == needs.size()) needs.push_back(std::make_pair(where, (uint64_t)0));
            needs[g].second += bytes;
        }
    }
    for (size_t g = 0; g < needs.size(); ++g) {
        uint64_t free = 0;
        uint64_t reserve = platform()->reserveBytes(needs[g].first);
        if (platform()->freeBytes(needs[g].first, &free) && free < needs[g].second + reserve) {
            return fail("no-space", "the install needs " + number(needs[g].second) + " bytes, and " + number(reserve) +
                                        " are kept free; " + number(free) + " are free");
        }
    }
    return true;
}

bool InstallStep::begin() {
    std::string why;
    std::string dir = store().journalDir();
    if (!fs::makeDirs(dir + "/record/backup", &why) || !fs::makeDirs(dir + "/old", &why)) return ioFail(why);
    json::Writer line;
    line.beginObject().key("op").string("begin").key("action").string(report_.action).key("id").string(m().id);
    line.key("version").string(m().version);
    if (!report_.oldVersion.empty()) line.key("oldVersion").string(report_.oldVersion);
    line.endObject();
    if (!journal_.create(engine_->journalPath(), &why) || !journal_.append(line.text(), &why)) {
        journal_.close();
        fs::removeFile(engine_->journalPath(), 0);
        return ioFail(why);
    }
    engine_->crash("begin");
    return true;
}

bool InstallStep::note(const json::Writer& line) {
    std::string why;
    if (!journal_.append(line.text(), &why)) return ioFail(why);
    engine_->crash("journal");
    return true;
}

bool InstallStep::moveAway(const std::string& real, const std::string& to) {
    json::Writer line;
    line.beginObject().key("op").string("backup").key("path").string(real).key("to").string(to).endObject();
    std::string why;
    if (!writable(real) || !note(line)) return false;
    if (!fs::moveFile(real, to, &why)) return ioFail(why);
    engine_->crash("action");
    return true;
}

bool InstallStep::placeFile(const std::string& from, const std::string& real, int mode) {
    json::Writer line;
    line.beginObject().key("op").string("place").key("path").string(real).endObject();
    std::string why;
    if (!writable(real) || !note(line)) return false;
    if (!fs::copyFile(from, real, mode, &why)) return ioFail(why);
    engine_->crash("action");
    return true;
}

bool InstallStep::removeLink(const std::string& real, const std::string& target) {
    // The journal is JSON, so it cannot hold a target that is not UTF-8, and without it the link could not be
    // made again. No package can declare such a link; one that is there was made by other means.
    if (!validUtf8(target)) return ioFail("the link " + real + " has a target that cannot be recorded");
    json::Writer line;
    line.beginObject().key("op").string("unlink").key("path").string(real).key("target").string(target).endObject();
    std::string why;
    if (!writable(real) || !note(line)) return false;
    if (!fs::removeFile(real, &why)) return ioFail(why);
    engine_->crash("action");
    return true;
}

bool InstallStep::ensureDir(const std::string& dest) {
    std::string real = store().resolve(m(), dest);
    if (fs::isDir(real)) return true;
    if (fs::kindOf(real) != fs::kNone) return ioFail(dest + ": something that is not a folder is in the way");
    json::Writer line;
    line.beginObject().key("op").string("mkdir").key("path").string(real).endObject();
    std::string why;
    if (!writable(real) || !note(line)) return false;
    if (!fs::makeDir(real, &why) && !why.empty()) return ioFail(why);
    engine_->crash("action");
    createdDirs_.push_back(dest);
    return true;
}

bool InstallStep::ensureParents(const std::string& dest) {
    std::vector<std::string> dirs = ancestorsOf(dest);
    for (size_t i = 0; i < dirs.size(); ++i) {
        if (!ensureDir(dirs[i])) return false;
    }
    return true;
}

bool InstallStep::apply() {
    std::string why;
    std::string recordDir = store().journalDir() + "/record";

    // Everything that goes is moved out before anything new is placed. For a plugin that is the rule the HMI
    // needs: the old folder's files leave Plugins first, and nothing of ours stays inside it as a second copy.
    log_.phase("backup");
    for (size_t i = 0; i < removals_.size(); ++i) {
        const Removal& r = removals_[i];
        if (r.exists && !moveAway(r.real, nextMoved())) return false;
        // The old version had replaced a stock file that the new one leaves alone: the stock file goes back.
        if (!r.restoreFrom.empty() && !placeFile(r.restoreFrom, r.real, -1)) return false;
    }
    for (size_t i = 0; i < linkRemovals_.size(); ++i) {
        if (!removeLink(linkRemovals_[i].first, linkRemovals_[i].second)) return false;
    }
    for (size_t i = 0; i < links_.size(); ++i) {
        const LinkAction& a = links_[i];
        if (a.unlinkFirst && !removeLink(a.real, a.oldTarget)) return false;
        if (a.moveAside && !moveAway(a.real, nextMoved())) return false;
    }
    for (size_t i = 0; i < files_.size(); ++i) {
        FileAction& a = files_[i];
        if (a.backupStock) {
            a.recordBackup = "backup/" + number(backups_++);
            if (!moveAway(a.real, recordDir + "/" + a.recordBackup)) return false;
        } else if (!a.carryBackup.empty()) {
            // A copy inside the new record: the old record keeps its own until the commit, so nothing to undo.
            a.recordBackup = "backup/" + number(backups_++);
            if (!fs::copyFile(store().recordDir(m().id) + "/" + a.carryBackup, recordDir + "/" + a.recordBackup, -1, &why)) return ioFail(why);
        }
        if (a.moveAside && !moveAway(a.real, nextMoved())) return false;
    }

    log_.phase("files");
    uint64_t total = 0, done = 0;
    for (size_t i = 0; i < files_.size(); ++i) {
        if (files_[i].what == FileAction::kPlace) total += m().contents[files_[i].content].size;
    }
    // ${app} and ${data} exist once a package is installed, whether or not it places anything there: hooks get
    // both as NBT_APP_DIR and NBT_DATA_DIR. ${tmp} exists for a package whose logs name it (NBT_TMP_DIR); it is
    // on the RAM disk, so an undo leaves it, and the agent makes it again at every start (Agent::init).
    if (!ensureDir("${app}") || !ensureDir("${data}")) return false;
    if (namesTmp(m()) && !fs::makeDirs(platform()->tmpDir(m().id), &why)) return ioFail(why);
    for (size_t i = 0; i < files_.size(); ++i) {
        const FileAction& a = files_[i];
        if (a.what != FileAction::kPlace) continue;
        if (!ensureParents(a.dest) || !placeFile(a.unpacked, a.real, (int)m().files[a.file].mode)) return false;
        done += m().contents[a.content].size;
        log_.progress(done, total);
        log_.idle();
    }
    for (size_t i = 0; i < links_.size(); ++i) {
        const LinkAction& a = links_[i];
        if (!a.make) continue;
        json::Writer line;
        line.beginObject().key("op").string("link").key("path").string(a.real).endObject();
        if (!ensureParents(a.dest) || !writable(a.real) || !note(line)) return false;
        if (!fs::makeLink(a.target, a.real, &why)) return ioFail(why);
        engine_->crash("action");
    }

    log_.phase("post-install");
    if (!engine_->runHook(m(), kPostInstall, unpackDir_, report_.action.c_str(), report_.oldVersion, &log_, &report_.error)) return false;
    engine_->crash("action");
    if (self_ && !checkProgram()) return false;

    log_.phase("record");
    return writeRecord();
}

bool InstallStep::writeRecord() {
    std::string why;
    std::string recordDir = store().journalDir() + "/record";
    InstalledPackage record;
    record.manifest = m();
    record.key = keyHex_;
    for (size_t i = 0; i < files_.size(); ++i) {
        const FileAction& a = files_[i];
        InstalledFile f;
        f.dest = a.dest;
        f.sha256 = m().contents[a.content].sha256;
        f.size = m().contents[a.content].size;
        f.mode = m().files[a.file].mode;
        f.config = m().files[a.file].config;
        f.kept = a.what == FileAction::kKeep;
        f.backup = a.recordBackup;
        record.files.push_back(f);
    }
    for (size_t i = 0; i < links_.size(); ++i) {
        InstalledLink l;
        l.path = links_[i].dest;
        l.target = links_[i].target;
        record.links.push_back(l);
    }
    // Folders: the ones the old version made that are still in use, then the ones made now. The rest of the old
    // version's folders are removed after the commit, if they are empty by then.
    if (old_ != 0) {
        for (size_t i = 0; i < old_->dirs.size(); ++i) {
            const std::string& dir = old_->dirs[i];
            // The agent's own package: the old version's ${app} is another folder, and stays as it is.
            if (self_ && startsWith(dir, "${app}")) continue;
            bool used = dir == "${app}" || startsWith(dir, "${data}");
            for (size_t k = 0; !used && k < record.files.size(); ++k) used = startsWith(record.files[k].dest, dir + "/");
            for (size_t k = 0; !used && k < record.links.size(); ++k) used = startsWith(record.links[k].path, dir + "/");
            if (used) record.dirs.push_back(dir);
            else rmdirs_.insert(rmdirs_.begin(), store().resolve(old_->manifest, dir));   // children before parents
        }
    }
    for (size_t i = 0; i < createdDirs_.size(); ++i) record.dirs.push_back(createdDirs_[i]);

    // The remove hooks are kept, under hooks/, so an uninstall needs neither the archive nor a phone.
    for (int kind = kPreRemove; kind <= kPostRemove; ++kind) {
        const ManifestHook& hook = m().hooks[kind];
        if (!hook.present) continue;
        std::string to = recordDir + "/hooks/" + hook.script;
        if (!fs::makeDirs(fs::parentOf(to), &why) || !fs::copyFile(unpackDir_ + "/" + hook.script, to, 0644, &why)) return ioFail(why);
    }
    if (!fs::writeFile(recordDir + "/manifest.json", package_.manifestBytes(), 0644, &why) ||
        !fs::writeFile(recordDir + "/manifest.sig", package_.signatureBytes(), 0644, &why) ||
        !fs::writeFile(recordDir + "/install.log", log_.text(), 0644, &why) ||
        !fs::writeFile(recordDir + "/record.json", Store::encodeRecord(record), 0644, &why)) {
        return ioFail(why);
    }
    fs::syncDir(recordDir);
    engine_->crash("action");

    // The agent's own package ends here: the new version writes the commit line, once it is confirmed.
    if (self_) return true;

    json::Writer commit;
    commit.beginObject().key("op").string("commit").key("rmdirs").beginArray();
    for (size_t i = 0; i < rmdirs_.size(); ++i) commit.string(rmdirs_[i]);
    commit.endArray().endObject();
    if (!journal_.append(commit.text(), &why)) return ioFail(why);
    engine_->crash("commit");
    return true;
}

}  // namespace nbt
