// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The agent's own folder: see agent_home.hpp.

#include "agent_home.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include "fsutil.hpp"

namespace nbt {

const char kAgentPackageId[] = "org.nbtapp.agent";
const char kAgentProgram[] = "nbtagent";
const char kAgentLauncher[] = "nbtlaunch.sh";
const char kAgentStarter[] = "nbtstart.sh";
const char kStarterInPlace[] = "launcher";

namespace {

const size_t kMaxNameFile = 4096;   // a file that names a version is one short line
const size_t kMaxStarter = 256 * 1024;   // a starter is a shell script of a few kilobytes

bool isFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// The version a file names: its first line, as the shell's `read -r` takes one, if that is a version's name.
std::string nameIn(const std::string& path) {
    std::string text;
    if (!isFile(path) || !fs::readFile(path, kMaxNameFile, &text, 0)) return std::string();
    size_t end = text.find('\n');
    if (end != std::string::npos) text.erase(end);
    size_t first = text.find_first_not_of(" \t");
    size_t last = text.find_last_not_of(" \t");
    if (first == std::string::npos) return std::string();
    text = text.substr(first, last - first + 1);
    return validVersionName(text) ? text : std::string();
}

}  // namespace

bool validVersionName(const std::string& name) {
    if (name.empty() || name.size() > 64 || name[0] < '0' || name[0] > '9') return false;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '+' || c == '-';
        if (!ok) return false;
    }
    return true;
}

AgentHome::Files AgentHome::read() const {
    Files files;
    files.current = nameIn(dir_ + "/current");
    files.trial = isFile(dir_ + "/trial");
    files.started = isFile(dir_ + "/trial.started");
    if (files.started) files.pending = nameIn(dir_ + "/trial.started");
    else if (files.trial) files.pending = nameIn(dir_ + "/trial");
    return files;
}

TrialState AgentHome::stateOf(const Files& files, const std::string& self) {
    if (!files.any()) return kNoTrial;
    if (!files.pending.empty() && files.pending == files.current) return kConfirmed;
    if (files.pending == self) return kOnTrial;
    if (files.current == self) return kWayBack;
    return kNotOurs;
}

bool AgentHome::hasProgram(const std::string& version) const {
    if (!validVersionName(version)) return false;
    std::string program = versionDir(version) + "/" + kAgentProgram;
    return isFile(program) && access(program.c_str(), X_OK) == 0 && isFile(versionDir(version) + "/" + kAgentLauncher);
}

void AgentHome::versions(std::vector<std::string>* out) const {
    std::vector<std::string> names;
    out->clear();
    if (!fs::listDir(dir_, &names, 0)) return;
    for (size_t i = 0; i < names.size(); ++i) {
        if (validVersionName(names[i]) && fs::kindOf(dir_ + "/" + names[i]) == fs::kDir) out->push_back(names[i]);
    }
}

bool AgentHome::writeCurrent(const std::string& version, std::string* error) const {
    if (!validVersionName(version)) {
        if (error != 0) *error = version + " cannot name a version of the agent";
        return false;
    }
    return fs::writeFile(dir_ + "/current", version + "\n", 0644, error);
}

bool AgentHome::writeTrial(const std::string& version, std::string* error) const {
    if (!validVersionName(version)) {
        if (error != 0) *error = version + " cannot name a version of the agent";
        return false;
    }
    return fs::writeFile(dir_ + "/trial", version + "\n", 0644, error);
}

bool AgentHome::removeTrial(std::string* error) const {
    bool ok = fs::removeFile(dir_ + "/trial.started", error) && fs::removeFile(dir_ + "/trial", error);
    fs::syncDir(dir_);
    return ok;
}

void AgentHome::removeTemp() const {
    std::string temp = fs::tempIn(dir_);
    if (fs::kindOf(temp) != fs::kNone) fs::removeFile(temp, 0);
}

bool AgentHome::hasStarter() const { return isFile(dir_ + "/" + kStarterInPlace); }

bool AgentHome::autostart() const { return isFile(dir_ + "/" + kAutostartSwitch); }

bool AgentHome::setAutostart(bool on, std::string* error) const {
    const std::string path = dir_ + "/" + kAutostartSwitch;
    if (on) return isFile(path) || fs::writeFile(path, "on\n", 0644, error);
    if (fs::kindOf(path) == fs::kNone) return true;
    bool ok = fs::removeFile(path, error);
    fs::syncDir(dir_);
    return ok;
}

size_t AgentHome::autostartStarts() const {
    std::string text;
    const std::string record = dir_ + "/" + kAutostartBoots;
    if (!isFile(record) || !fs::readFile(record, kMaxNameFile, &text, 0)) return 0;
    // A line for each start, as the plugin writes them: digits only. Anything else is passed over, as there.
    size_t count = 0, at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        bool digits = end > at;
        for (size_t i = at; digits && i < end; ++i) digits = text[i] >= '0' && text[i] <= '9';
        if (digits) ++count;
        at = end + 1;
    }
    return count;
}

bool AgentHome::forgetAutostarts() const {
    const std::string record = dir_ + "/" + kAutostartBoots;
    if (fs::kindOf(record) == fs::kNone) return false;
    bool ok = fs::removeFile(record, 0);
    fs::syncDir(dir_);
    return ok;
}

AgentHome::Starter AgentHome::starterOf(const std::string& version) const {
    std::string mine, inPlace;
    if (!validVersionName(version)) return kStarterNone;
    const std::string source = versionDir(version) + "/" + kAgentStarter;
    if (!isFile(source) || !fs::readFile(source, kMaxStarter, &mine, 0) || mine.empty()) return kStarterNone;
    const std::string placed = dir_ + "/" + kStarterInPlace;
    if (!isFile(placed) || !fs::readFile(placed, kMaxStarter, &inPlace, 0)) return kStarterDiffers;
    return inPlace == mine ? kStarterSame : kStarterDiffers;
}

bool AgentHome::placeStarter(const std::string& version, std::string* error) const {
    std::string mine;
    if (!validVersionName(version)) {
        if (error != 0) *error = version + " cannot name a version of the agent";
        return false;
    }
    if (!fs::readFile(versionDir(version) + "/" + kAgentStarter, kMaxStarter, &mine, error)) return false;
    if (mine.empty()) {
        if (error != 0) *error = "the starter of version " + version + " is an empty file";
        return false;
    }
    return fs::writeFile(dir_ + "/" + kStarterInPlace, mine, 0755, error);
}

bool AgentHome::removeVersion(const std::string& version, std::string* error) const {
    if (!validVersionName(version)) {
        if (error != 0) *error = version + " cannot name a version of the agent";
        return false;
    }
    bool ok = fs::removeTree(versionDir(version), error);
    fs::syncDir(dir_);
    return ok;
}

}  // namespace nbt
