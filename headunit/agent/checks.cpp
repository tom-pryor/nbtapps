// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The compatibility checks: see checks.hpp.

#include "checks.hpp"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>

#include "fsutil.hpp"
#include "hex.hpp"
#include "hmi_ids.hpp"
#include "planner.hpp"
#include "sha256.hpp"
#include "utf8.hpp"

namespace nbt {

namespace {

std::string number(uint64_t value) {
    char text[32];
    snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    return text;
}

bool under(const std::string& path, const std::string& dir) {
    return !dir.empty() && path.compare(0, dir.size(), dir) == 0 && (path.size() == dir.size() || path[dir.size()] == '/');
}

bool byPath(const CheckEntry& a, const CheckEntry& b) { return a.path < b.path; }

char kindLetter(EntryKind kind) {
    switch (kind) {
        case kEntryFile: return 'f';
        case kEntryLink: return 'l';
        case kEntryDir: return 'd';
        default: return 'o';
    }
}

EntryKind kindOfStat(const struct stat& st) {
    if (S_ISREG(st.st_mode)) return kEntryFile;
    if (S_ISLNK(st.st_mode)) return kEntryLink;
    if (S_ISDIR(st.st_mode)) return kEntryDir;
    return kEntryOther;
}

bool showable(const std::string& path) {
    if (!validUtf8(path)) return false;
    for (size_t i = 0; i < path.size(); ++i) {
        unsigned char c = (unsigned char)path[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

// One listing's walk. `depth` is the folder's own: 0 for a root.
class Walk {
public:
    Walk(const Fence& fence, Listing* out) : fence_(fence), out_(out) {}

    // False once the listing has to stop: too many entries, or a folder that cannot be listed.
    bool add(const std::string& path, const struct stat& st) {
        if (out_->entries.size() >= kCheckMaxEntries) {
            out_->tooMany = true;
            return false;
        }
        out_->entries.push_back(CheckEntry(path, kindOfStat(st)));
        if (!showable(path)) out_->badName = true;
        return true;
    }

    bool folder(const std::string& dir, uint64_t rootDevice, unsigned depth) {
        std::vector<std::string> names;
        std::string why;
        if (!fs::listDir(dir, &names, &why)) {
            out_->error = why;
            return false;
        }
        for (size_t i = 0; i < names.size(); ++i) {
            std::string path = dir + "/" + names[i];
            if (fenced(fence_, path)) continue;
            struct stat st;
            if (lstat(path.c_str(), &st) != 0) {
                if (errno == ENOENT) continue;   // gone since the folder was read
                out_->error = "cannot look at " + path;
                return false;
            }
            if (!add(path, st)) return false;
            if (!S_ISDIR(st.st_mode)) continue;
            FolderStep step = folderStep(rootDevice, (uint64_t)st.st_dev, depth + 1);
            if (step == kFolderOtherDevice) {
                out_->otherDevice = true;
            } else if (step == kFolderLast) {
                std::vector<std::string> inside;
                if (!fs::listDir(path, &inside, &why)) {
                    out_->error = why;
                    return false;
                }
                if (!inside.empty()) out_->tooDeep = true;
            } else if (!folder(path, rootDevice, depth + 1)) {
                return false;
            }
        }
        return true;
    }

private:
    const Fence& fence_;
    Listing* out_;
};

void unknown(CheckFinding* out, const std::string& why) {
    out->state = kCheckUnknown;
    out->why = why;
    out->fixable = false;
    out->entries.clear();
}

void problem(CheckFinding* out, const std::vector<CheckEntry>& entries, bool fixable, const std::string& why) {
    out->state = kCheckProblem;
    out->entries = entries;
    std::sort(out->entries.begin(), out->entries.end(), byPath);
    out->fixable = fixable;
    out->why = why;
}

// ---- system-writable ----------------------------------------------------------------------------------------------

void detectSystemWritable(const CheckContext& context, CheckFinding* out) {
    if (context.stepRunning) {
        return unknown(out, "an install step is running, which makes the system partition writable while it places files");
    }
    std::string why;
    Platform::SystemState state = context.platform->systemState(&why);
    if (state == Platform::kSystemUnknown) return unknown(out, "it cannot be told whether the system partition is read-only: " + why);
    if (state != Platform::kSystemWritable || context.platform->remountedForStep()) return;
    problem(out, std::vector<CheckEntry>(), true, "");
}

// ---- aa-fix -------------------------------------------------------------------------------------------------------

// An app that was installed by hand, outside this system: what is known of it is where it is. Its programs and
// files are in the first folder, what it keeps in the second, and its iDrive plugin claims the connector id (the
// README has it among the ids that are taken).
const char kHandAppDir[] = "/opt/aa";
const char kHandAppDataDir[] = "/var/opt/aa";
const long kHandAppConnector = 210;

// The plugin folders whose <Name>.manifest claims the app's connector id, and in which no installed package's
// record owns anything.
void handAppPlugins(const CheckContext& context, const Fence& fence, std::vector<std::string>* out) {
    std::vector<PluginFolder> folders;
    scanPluginFolders(context.platform->pluginScanDirs(), std::set<std::string>(), &folders);
    for (size_t i = 0; i < folders.size(); ++i) {
        const PluginFolder& folder = folders[i];
        if (fenced(fence, folder.dir) || ownsUnder(fence, folder.dir)) continue;
        bool claims = false;
        for (size_t k = 0; k < folder.manifests.size() && !claims; ++k) {
            const PluginManifest& m = folder.manifests[k];
            if (m.path != folder.dir + "/" + folder.name + ".manifest") continue;
            for (size_t n = 0; n < m.connectorIds.size(); ++n) claims = claims || m.connectorIds[n] == kHandAppConnector;
        }
        if (claims) out->push_back(folder.dir);
    }
}

// A folder directly in the app's folder that a fix has moved there, or that looks so: a real folder, not a link,
// whose name ends in one of the two endings a move gives.
bool parkedName(const std::string& name) {
    static const char* const endings[] = {".old", ".nbtappsold"};
    for (size_t i = 0; i < sizeof endings / sizeof endings[0]; ++i) {
        size_t n = strlen(endings[i]);
        if (name.size() > n && name.compare(name.size() - n, n, endings[i]) == 0) return true;
    }
    return false;
}

void parkedIn(const std::string& app, std::set<std::string>* out) {
    std::vector<std::string> names;
    if (!fs::listDir(app, &names, 0)) return;
    for (size_t i = 0; i < names.size(); ++i) {
        if (parkedName(names[i]) && fs::kindOf(app + "/" + names[i]) == fs::kDir) out->insert(app + "/" + names[i]);
    }
}

// The device of `path`, or of the nearest folder above it that is there. False if none can be asked.
bool deviceOf(std::string path, uint64_t* out) {
    for (;;) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            *out = (uint64_t)st.st_dev;
            return true;
        }
        if (errno != ENOENT || path == "/" || path.empty()) return false;
        path = fs::parentOf(path);
    }
}

void detectHandApp(const CheckContext& context, CheckFinding* out) {
    Fence fence;
    std::string why;
    if (!fenceOf(context, &fence, &why)) return unknown(out, "the records of the installed packages cannot be read: " + why);
    std::string app = context.platform->systemPath(kHandAppDir);
    std::vector<std::string> plugins;
    handAppPlugins(context, fence, &plugins);
    fs::Kind appKind = fenced(fence, app) ? fs::kNone : fs::kindOf(app);
    // The folder of its data is listed with the rest, and is not enough by itself to say that the app is there.
    if (plugins.empty() && appKind == fs::kNone) return;
    std::string held;

    // Each plugin folder is moved into the app's folder, whole, under a name that is free now.
    std::vector<CheckMove> moves;
    std::set<std::string> taken;
    for (size_t i = 0; i < plugins.size() && moves.size() < kCheckMaxMoves; ++i) {
        std::string name = plugins[i].substr(plugins[i].rfind('/') + 1), first = app + "/" + name + ".old", to = first;
        if (fs::kindOf(to) != fs::kNone || taken.count(to) != 0) to = app + "/" + name + ".nbtappsold";
        if ((fs::kindOf(to) != fs::kNone || taken.count(to) != 0) && held.empty()) {
            held = "Its plugin's folder, " + plugins[i] + ", would be moved to " + first + " or " + to +
                   ", and both are taken. No fix is offered.";
        }
        taken.insert(to);
        moves.push_back(CheckMove(plugins[i], to));
        uint64_t from = 0, into = 0;
        if (held.empty() && (!deviceOf(fs::parentOf(plugins[i]), &from) || !deviceOf(app, &into) || from != into)) {
            held = "Its plugin's folder, " + plugins[i] + ", is on another file system than " + app +
                   ", and a fix moves it only by renaming it. No fix is offered.";
        }
        if (held.empty() && (!showable(plugins[i]) || !showable(to))) {
            held = "A name in it cannot be shown as text, so the list cannot be shown as it is. No fix is offered.";
        }
    }
    if (plugins.size() > kCheckMaxMoves && held.empty()) {
        held = "There is too much here to move safely: more than " + number(kCheckMaxMoves) + " plugin folders. No fix is offered.";
    }
    if (!moves.empty() && appKind != fs::kNone && appKind != fs::kDir && held.empty()) {
        held = app + " is not a folder, and nothing is moved through a link or onto a file. No fix is offered.";
    }

    // What a fix moved there before stays there, and is not listed.
    if (appKind == fs::kDir) parkedIn(app, &fence.parked);
    std::vector<std::string> roots;
    roots.push_back(app);
    roots.push_back(context.platform->systemPath(kHandAppDataDir));
    Listing listing;
    listBounded(roots, fence, &listing);
    if (!listing.error.empty()) return unknown(out, "what the app left cannot be listed: " + listing.error);
    // The app's folder stays where it receives a plugin or holds one already: it is not listed to be removed.
    if ((!moves.empty() || !fence.parked.empty()) && appKind == fs::kDir) {
        std::vector<CheckEntry> kept;
        for (size_t i = 0; i < listing.entries.size(); ++i) {
            if (listing.entries[i].path != app) kept.push_back(listing.entries[i]);
        }
        listing.entries.swap(kept);
    }
    if (listing.entries.empty() && moves.empty()) return;
    if (held.empty()) {
        if (listing.tooMany) {
            held = "There is too much here to remove safely: more than " + number(kCheckMaxEntries) +
                   " entries. No fix is offered; this has to be removed by hand.";
        } else if (listing.tooDeep) {
            held = "There is too much here to remove safely: folders more than " + number(kCheckMaxDepth) +
                   " levels deep. No fix is offered; this has to be removed by hand.";
        } else if (listing.otherDevice) {
            held = "Part of it is on another file system, which the fix does not go into. No fix is offered.";
        } else if (listing.badName) {
            held = "A name in it cannot be shown as text, so the list cannot be shown as it is. No fix is offered.";
        }
    }
    problem(out, listing.entries, held.empty(), held);
    out->moves = moves;
}

// ---- the table ----------------------------------------------------------------------------------------------------

const Check kChecks[] = {
    {"system-writable",
     "The system partition was left writable",
     "The partition that holds the head unit's programs is writable, and the agent did not make it so: it was "
     "remounted by hand and not put back. Until it is read-only again, anything can change the files on it.",
     kFixRemount,
     "Make it read-only again",
     "Remounts the system partition read-only, as the agent does after each install step. Nothing is removed. "
     "If a program holds a file on it open for writing, this fails; a restart of the head unit puts it back too.",
     "none", 0, detectSystemWritable},
    {"aa-fix",
     "An app that was installed by hand",
     "Files of an app that was installed by hand, outside this system, are on the head unit: in /opt/aa, in "
     "/var/opt/aa, or in a plugin folder whose manifest takes the connector id 210. Its programs may start with "
     "the head unit, and the HMI loads its plugin.",
     kFixRemove,
     "Remove that app",
     "Stops the programs that run from /opt/aa. Then moves the plugin's folder out of the plugin folder into "
     "/opt/aa, under a name that ends in .old, and leaves it there, where the HMI no longer loads it: it is moved "
     "and not removed because the HMI may have it loaded. Then removes the rest as listed and nothing else; a "
     "folder goes only when it is empty. The configuration of the packet filter is not touched: what that app "
     "changed there stays as it is. The HMI needs a restart afterwards.",
     "hmi-restart", kHandAppDir, detectHandApp},
};

// ---- the fix ------------------------------------------------------------------------------------------------------

const size_t kMaxInstallLog = 256u << 10;   // <state>/install.log is rotated past this, as the engine does
const unsigned kStopPollMs = 50;

// The install log, as a step writes it: a line that names what follows, and a line for each thing done.
class FixLog {
public:
    explicit FixLog(const Store& store) : path_(store.logPath()) {}
    void begin(const std::string& title) {
        struct stat st;
        if (stat(path_.c_str(), &st) == 0 && (size_t)st.st_size > kMaxInstallLog) rename(path_.c_str(), (path_ + ".1").c_str());
        line("== " + title);
    }
    void line(const std::string& text) { fs::appendFile(path_, shownPath(text) + "\n", 0); }

private:
    std::string path_;
};

void crashPoint(const CheckContext& context, const char* name) {
    if (context.crash != 0) context.crash->point(name);
}

bool refuse(const char** code, std::string* message, const char* what, const std::string& why) {
    *code = what;
    *message = why;
    return false;
}

std::string processText(const RunningProgram& p) {
    return programName(p.path.c_str()) + " (process " + number((uint64_t)p.pid) + ")";
}

// Waits, a poll at a time, until no process runs from `dir` or `ms` have passed. False with *error where the
// processes cannot be read.
bool waitForNone(const CheckContext& context, const std::string& dir, unsigned ms, std::vector<RunningProgram>* running,
                 std::string* error) {
    for (unsigned waited = 0;; waited += kStopPollMs) {
        ProgramState state = context.platform->programsUnder(dir, running);
        if (state == kProgramUnknown) {
            *error = "the head unit's processes can no longer be read, so it cannot be told whether a program still runs from " + dir;
            return false;
        }
        if (state == kProgramNotRunning || waited >= ms) return true;
        poll(0, 0, (int)kStopPollMs);
    }
}

// Stops every program that runs from `dir`: SIGTERM, and SIGKILL to what is still there. False with *error, before
// anything is removed, if one cannot be stopped or the processes cannot be read.
bool stopPrograms(const CheckContext& context, const std::string& dir, std::vector<std::string>* stopped, FixLog* log,
                  std::string* error) {
    std::vector<RunningProgram> running;
    ProgramState state = context.platform->programsUnder(dir, &running);
    if (state == kProgramUnknown) {
        *error = "the head unit's processes cannot be read, so it cannot be told whether a program runs from " + dir +
                 ", and its files are not removed under it";
        return false;
    }
    std::set<std::string> names;
    const int signals[] = {SIGTERM, SIGKILL};
    for (size_t round = 0; round < 2 && !running.empty(); ++round) {
        for (size_t i = 0; i < running.size(); ++i) {
            if (!context.platform->signalProcess(running[i].pid, signals[round])) {
                *error = processText(running[i]) + " cannot be sent " + (round == 0 ? "SIGTERM" : "SIGKILL");
                return false;
            }
            log->line(std::string(round == 0 ? "sent SIGTERM to " : "sent SIGKILL to ") + processText(running[i]));
            std::string name = programName(running[i].path.c_str());
            if (names.insert(name).second) stopped->push_back(name);
        }
        if (!waitForNone(context, dir, round == 0 ? context.stopWaitMs : context.killWaitMs, &running, error)) return false;
    }
    if (!running.empty()) {
        *error = processText(running[0]) + " is still running after SIGKILL";
        return false;
    }
    return true;
}

// Moves each folder of `moves`, as fixCheck says, and keeps those that were moved in *moved.
void moveListed(const CheckContext& context, const std::vector<CheckMove>& moves, std::vector<CheckMove>* moved, FixLog* log) {
    Platform& platform = *context.platform;
    std::string why;
    for (size_t i = 0; i < moves.size(); ++i) {
        const CheckMove& m = moves[i];
        std::string into = fs::parentOf(m.to);
        if (!platform.makeWritable(m.from, &why) || !platform.makeWritable(m.to, &why)) {
            log->line("left " + m.from + ": " + why);
        } else if (!fs::makeDirs(into, &why)) {
            log->line("left " + m.from + ": " + why);
        } else if (fs::kindOf(m.to) != fs::kNone) {
            log->line("left " + m.from + ": " + m.to + " is there already, and nothing is written over");
        } else if (rename(m.from.c_str(), m.to.c_str()) != 0) {
            log->line("left " + m.from + ": it cannot be moved to " + m.to + ": " + strerror(errno));
        } else {
            fs::syncDir(fs::parentOf(m.from));
            fs::syncDir(into);
            log->line("moved " + m.from + " to " + m.to);
            moved->push_back(m);
        }
        crashPoint(context, "check-moved");
    }
}

// Removes what `listed` holds, as fixCheck says. The entries are sorted by path, so a folder comes before what it
// holds, and from the end backwards the deepest come first: the files and links, then the folders.
void removeListed(const CheckContext& context, const std::vector<CheckEntry>& listed, FixLog* log) {
    Platform& platform = *context.platform;
    std::vector<const CheckEntry*> steps, folders;
    for (size_t i = listed.size(); i-- > 0;) {
        const CheckEntry& e = listed[i];
        if (e.kind == kEntryFile || e.kind == kEntryLink) steps.push_back(&e);
        else if (e.kind == kEntryDir) folders.push_back(&e);
        else log->line("left " + e.path + ": it is not a file, a link or a folder, and a fix removes none such");
    }
    steps.insert(steps.end(), folders.begin(), folders.end());
    std::string why;
    for (size_t i = 0; i < steps.size(); ++i) {
        const std::string& path = steps[i]->path;
        bool folder = steps[i]->kind == kEntryDir;
        std::string shown = folder ? path + "/" : path;
        if (!platform.makeWritable(path, &why)) {
            log->line("left " + shown + ": " + why);
        } else if (folder ? !fs::removeEmptyDir(path) : !fs::removeFile(path, &why)) {
            log->line("left " + shown + ": " + (folder ? std::string("it is not empty") : why));
        } else {
            log->line("removed " + shown);
        }
        crashPoint(context, folder ? "check-removed-folder" : "check-unlinked");
    }
}

}  // namespace

bool fixCheck(const Check& check, const CheckContext& context, const std::string& token, FixOutcome* out,
              const char** code, std::string* message) {
    *out = FixOutcome();
    if (check.fix == kFixNone) return refuse(code, message, "unsupported", std::string("the check ") + check.id + " has no fix");
    CheckFinding now;
    runCheck(check, context, &now);
    if (now.state == kCheckUnknown) return refuse(code, message, "io-error", "the check cannot be run now: " + now.why);
    if (now.token != token) {
        return refuse(code, message, "stale", "what the check finds is not what the token was given for: run the check again");
    }
    if (now.state == kCheckOk) return true;
    if (!now.fixable) return refuse(code, message, "unsupported", now.why.empty() ? "no fix is offered for what the check finds" : now.why);

    FixLog log(*context.store);
    log.begin(std::string("fix of the check ") + check.id);
    std::string why;
    if (check.fix == kFixRemount) {
        if (!context.platform->putSystemReadOnly(&why)) {
            log.line("the system partition could not be made read-only: " + why);
            return refuse(code, message, "io-error", "the system partition could not be made read-only: " + why);
        }
        log.line("the system partition is read-only again");
        out->restart = check.restart;
    } else {
        if (check.stopUnder != 0) {
            std::string dir = context.platform->systemPath(check.stopUnder);
            bool stopped = stopPrograms(context, dir, &out->stopped, &log, &why);
            if (!stopped) {
                log.line("nothing was removed: " + why);
                return refuse(code, message, "io-error", why);
            }
            crashPoint(context, "check-stopped");
        }
        // Before the first change: a fix that is cut off after its last change still leaves the mark.
        raiseRestartMark(context.platform->runDir(), check.restart);
        crashPoint(context, "check-marked");
        moveListed(context, now.moves, &out->moved, &log);
        removeListed(context, now.entries, &log);
        if (!context.platform->restoreReadOnly(&why)) log.line("the system partition could not be put back read-only: " + why);
        out->restart = check.restart;
        for (size_t i = 0; i < now.entries.size(); ++i) {
            if (fs::kindOf(now.entries[i].path) == fs::kNone) ++out->removed;
            else out->left.push_back(shownPath(now.entries[i].path));
        }
        for (size_t i = 0; i < now.moves.size(); ++i) {
            bool done = false;
            for (size_t k = 0; k < out->moved.size() && !done; ++k) done = out->moved[k].from == now.moves[i].from;
            if (!done) out->left.push_back(shownPath(now.moves[i].from));
        }
        std::sort(out->left.begin(), out->left.end());
    }
    CheckFinding after;
    runCheck(check, context, &after);
    out->state = after.state;
    log.line(number(out->moved.size()) + " moved, " + number(out->removed) + " removed, " + number(out->left.size()) +
             " left; the check now finds: " + checkStateName(after.state));
    return true;
}

size_t checkCount() { return sizeof kChecks / sizeof kChecks[0]; }
const Check& checkAt(size_t index) { return kChecks[index]; }

const Check* findCheck(const std::string& id) {
    for (size_t i = 0; i < checkCount(); ++i) {
        if (id == kChecks[i].id) return &kChecks[i];
    }
    return 0;
}

void runCheck(const Check& check, const CheckContext& context, CheckFinding* out) {
    *out = CheckFinding();
    check.detect(context, out);
    if (out->state != kCheckProblem) {
        out->entries.clear();
        out->moves.clear();
    }
    if (out->state != kCheckProblem || check.fix == kFixNone) out->fixable = false;
    out->token = checkToken(check.id, out->entries, out->moves);
}

const char* checkStateName(CheckState state) {
    return state == kCheckOk ? "ok" : state == kCheckProblem ? "problem" : "unknown";
}

std::string checkToken(const std::string& id, const std::vector<CheckEntry>& entries, const std::vector<CheckMove>& moves) {
    // Each entry is a line, and each move two, sorted by the path they start with.
    std::vector<std::pair<std::string, std::string> > lines;
    for (size_t i = 0; i < entries.size(); ++i) {
        lines.push_back(std::make_pair(entries[i].path, std::string(1, kindLetter(entries[i].kind)) + " " + entries[i].path + "\n"));
    }
    for (size_t i = 0; i < moves.size(); ++i) {
        lines.push_back(std::make_pair(moves[i].from, "m " + moves[i].from + "\nt " + moves[i].to + "\n"));
    }
    std::sort(lines.begin(), lines.end());
    Sha256 hash;
    std::string head = "nbt-check-1\n" + id + "\n";
    hash.update(head.data(), head.size());
    for (size_t i = 0; i < lines.size(); ++i) hash.update(lines[i].second.data(), lines[i].second.size());
    uint8_t digest[kSha256Bytes];
    hash.finish(digest);
    return toHex(digest, sizeof digest);
}

bool fenceOf(const CheckContext& context, Fence* out, std::string* error) {
    out->owned.clear();
    out->keep.clear();
    out->parked.clear();
    const Platform& p = *context.platform;
    out->keep.push_back(p.stateDir());
    out->keep.push_back(p.stagingDir());
    out->keep.push_back(p.runDir());
    out->keep.push_back(p.agentHome());
    // The folders that hold every package's ${app}, ${data} and ${tmp}.
    out->keep.push_back(fs::parentOf(p.appDir("x")));
    out->keep.push_back(fs::parentOf(p.dataDir("x")));
    out->keep.push_back(fs::parentOf(p.tmpDir("x")));
    std::vector<InstalledPackage> installed;
    if (!context.store->loadAll(&installed, error)) return false;
    for (size_t i = 0; i < installed.size(); ++i) {
        const InstalledPackage& one = installed[i];
        for (size_t k = 0; k < one.files.size(); ++k) out->owned.insert(context.store->resolve(one.manifest, one.files[k].dest));
        for (size_t k = 0; k < one.links.size(); ++k) out->owned.insert(context.store->resolve(one.manifest, one.links[k].path));
        for (size_t k = 0; k < one.dirs.size(); ++k) out->owned.insert(context.store->resolve(one.manifest, one.dirs[k]));
    }
    return true;
}

bool fenced(const Fence& fence, const std::string& path) {
    if (fence.owned.count(path) != 0) return true;
    for (std::set<std::string>::const_iterator it = fence.parked.begin(); it != fence.parked.end(); ++it) {
        if (under(path, *it)) return true;
    }
    for (size_t i = 0; i < fence.keep.size(); ++i) {
        if (under(path, fence.keep[i])) return true;
    }
    return false;
}

bool ownsUnder(const Fence& fence, const std::string& dir) {
    std::set<std::string>::const_iterator it = fence.owned.lower_bound(dir);
    for (; it != fence.owned.end() && it->compare(0, dir.size(), dir) == 0; ++it) {
        if (under(*it, dir)) return true;
    }
    return false;
}

FolderStep folderStep(uint64_t rootDevice, uint64_t folderDevice, unsigned depth) {
    if (folderDevice != rootDevice) return kFolderOtherDevice;
    return depth >= kCheckMaxDepth ? kFolderLast : kFolderEnter;
}

void listBounded(const std::vector<std::string>& roots, const Fence& fence, Listing* out) {
    *out = Listing();
    Walk walk(fence, out);
    std::vector<std::string> sorted = roots;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    for (size_t i = 0; i < sorted.size(); ++i) {
        const std::string& root = sorted[i];
        if (fenced(fence, root)) continue;
        // A root inside another is listed with that one.
        bool inside = false;
        for (size_t k = 0; k < i && !inside; ++k) inside = under(root, sorted[k]);
        if (inside) continue;
        struct stat st;
        if (lstat(root.c_str(), &st) != 0) {
            if (errno == ENOENT || errno == ENOTDIR) continue;
            out->error = "cannot look at " + root;
            break;
        }
        if (!walk.add(root, st)) break;
        if (S_ISDIR(st.st_mode) && !walk.folder(root, (uint64_t)st.st_dev, 0)) break;
    }
    std::sort(out->entries.begin(), out->entries.end(), byPath);
}

std::string shownPath(const std::string& path) {
    std::string text = sanitizeUtf8(path, 0);
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c == 0x7f) text[i] = '?';
    }
    return text;
}

std::string restartMark(const std::string& runDir) {
    std::string text;
    if (!fs::readFile(runDir + "/restart-needed", 64, &text, 0)) return "none";
    if (text.compare(0, 8, "required") == 0) return "required";
    if (text.compare(0, 11, "hmi-restart") == 0) return "hmi-restart";
    return "none";
}

void raiseRestartMark(const std::string& runDir, const std::string& need) {
    std::string now = restartMark(runDir);
    std::string both = strongerRestart(now, need);
    if (both != now) fs::writeFile(runDir + "/restart-needed", both + "\n", 0600, 0);
}

}  // namespace nbt
