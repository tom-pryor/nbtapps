// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// See hu_platform.hpp. QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include "hu_platform.hpp"

#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include "fsutil.hpp"
#include "process.hpp"

namespace nbt {

namespace {

const size_t kMaxCommandOutput = 1024;   // of what a command prints, this much reaches an error message

bool fail(std::string* error, const std::string& message) {
    if (error != 0) *error = message;
    return false;
}

std::string number(unsigned long value) {
    char text[32];
    snprintf(text, sizeof text, "%lu", value);
    return text;
}

std::string joined(const std::vector<std::string>& argv) {
    std::string line;
    for (size_t i = 0; i < argv.size(); ++i) line += (i == 0 ? "" : " ") + argv[i];
    return line;
}

// What a command printed, as part of one line: line ends become "; ", and the end is trimmed.
std::string oneLine(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n') out += "; ";
        else out += c;
    }
    while (!out.empty() && (out[out.size() - 1] == ' ' || out[out.size() - 1] == ';')) out.erase(out.size() - 1);
    return out;
}

class Collect : public OutputSink {
public:
    virtual void output(const char* data, size_t len) {
        if (text.size() < kMaxCommandOutput) text.append(data, len < kMaxCommandOutput - text.size() ? len : kMaxCommandOutput - text.size());
    }
    std::string text;
};

bool absolute(const std::string& path) { return !path.empty() && path[0] == '/'; }

}  // namespace

// ---- the configuration --------------------------------------------------------------------------------------------

// Each value from the place named. "The probe" is what hu_probe.sh reads off a head unit, on the node the HMI runs
// on, which is the node the agent runs on. What no source gives stays unset, so nothing runs on a guess:
//   reboot            not recorded anywhere. It may stay empty: the agent then refuses to reboot
//   otherPluginDirs   no second folder is known that the HMI loads plugins from
//
// The two remount commands are in the form the head unit's own partition script uses, `mount -u -o rw|ro` and the
// mount point, with mount where the probe finds it. Neither is taken at its word: after each the platform asks the
// file system whether it is read-only. A command that makes nothing writable fails the step before a file is
// written (makeWritable), and one that does not put the partition back leaves it writable until the head unit is
// turned off, which the install log says (restoreReadOnly).
HeadUnitConfig HeadUnitConfig::car() {
    HeadUnitConfig c;
    c.stateDir = "/var/opt/nbtapp";                    // plan document, "Target platform": writable and persistent
    c.stagingDir = "/var/opt/nbtapp/staging";          // beside the state: the RAM disk behind /tmp is too small
    c.runDir = "/tmp/nbtapp";                          // plan document, "Target platform": /tmp is cleared on reboot
    c.appsDir = "/var/opt/nbtapp/apps";                // the probe's --exec-test: a program starts from /var/opt
    c.dataDir = "/var/opt/nbtapp/data";                // plan document, "Files": ${data} sits under /var/opt/nbtapp
    c.tmpDir = "/tmp/nbtapp/apps";                     // plan document, "Pulling files": ${tmp} on the RAM disk
    c.pluginsDir = "/opt/hmi/ID5/data/ro/Plugins";     // README, "Installing"
    c.agentDir = "/var/opt/nbtapp/agent";              // plan document, "Agent updates"; hu_install.sh puts it there
    c.systemMount = "/fs/sda0";                        // the probe: /opt is a link into it
    const char* writable[] = {"/bin/mount", "-u", "-o", "rw", "/fs/sda0"};
    const char* readOnly[] = {"/bin/mount", "-u", "-o", "ro", "/fs/sda0"};
    const char* restart[] = {"/opt/core/bin/OnOffDSICommander", "appreset"};   // README, "Installing"; the probe
    c.remountWritable.assign(writable, writable + sizeof writable / sizeof writable[0]);
    c.remountReadOnly.assign(readOnly, readOnly + sizeof readOnly / sizeof readOnly[0]);
    c.restartHmi.assign(restart, restart + sizeof restart / sizeof restart[0]);
    c.shell = "/bin/ksh";                              // plan document, "Target platform"
    // The probe: what a shell on the head unit has, less the "." at the end of its PATH. The program that restarts
    // the HMI asks for libraries from these folders, and the loader looks in none of them but /lib by itself. A
    // hook gets the PATH only.
    c.hookPath = "/bin:/opt/os/bin:/opt/sys/bin:/opt/core/bin:/opt/car/bin";
    c.libraryPath = "/lib:/usr/lib:/opt/os/lib:/opt/sys/lib:/opt/core/lib:/opt/car/lib";
    return c;
}

std::string HeadUnitConfig::missing() const {
    std::string names;
    struct {
        const char* name;
        const std::string* value;
    } paths[] = {{"stateDir", &stateDir}, {"stagingDir", &stagingDir}, {"runDir", &runDir},           {"appsDir", &appsDir},
                 {"dataDir", &dataDir},   {"tmpDir", &tmpDir},   {"pluginsDir", &pluginsDir}, {"agentDir", &agentDir},
                 {"systemMount", &systemMount}, {"shell", &shell}};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
        if (!absolute(*paths[i].value)) names += std::string(names.empty() ? "" : ", ") + paths[i].name;
    }
    for (size_t i = 0; i < otherPluginDirs.size(); ++i) {
        if (!absolute(otherPluginDirs[i])) {
            names += std::string(names.empty() ? "" : ", ") + "otherPluginDirs";
            break;
        }
    }
    struct {
        const char* name;
        const std::vector<std::string>* argv;
        bool required;
    } commands[] = {{"remountWritable", &remountWritable, true},
                    {"remountReadOnly", &remountReadOnly, true},
                    {"restartHmi", &restartHmi, true},
                    {"reboot", &reboot, false}};
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; ++i) {
        const std::vector<std::string>& argv = *commands[i].argv;
        bool usable = argv.empty() ? !commands[i].required : absolute(argv[0]);
        if (!usable) names += std::string(names.empty() ? "" : ", ") + commands[i].name;
    }
    if (hookPath.empty()) names += std::string(names.empty() ? "" : ", ") + "hookPath";
    if (commandTimeoutSeconds == 0) names += std::string(names.empty() ? "" : ", ") + "commandTimeoutSeconds";
    if (restoreAttempts == 0) names += std::string(names.empty() ? "" : ", ") + "restoreAttempts";
    return names;
}

// ---- the platform -------------------------------------------------------------------------------------------------

HeadUnitPlatform::HeadUnitPlatform(const HeadUnitConfig& config) : config_(config), remounted_(false) {}

bool HeadUnitPlatform::prepare(std::string* error) {
    std::string missing = config_.missing();
    if (!missing.empty()) return fail(error, "the head unit's configuration is not complete: " + missing);
    // Before anything is made: a head unit has these two already, and a machine that lacks one is another machine,
    // or the configuration is wrong about this one.
    const std::string* stock[] = {&config_.systemMount, &config_.pluginsDir};
    for (size_t i = 0; i < sizeof stock / sizeof stock[0]; ++i) {
        if (!fs::isDir(*stock[i])) {
            return fail(error, "this is not the head unit the configuration describes: there is no folder " + *stock[i]);
        }
    }
    if (!fs::makeDirs(config_.stateDir, error) || !fs::makeDirs(config_.stagingDir, error) ||
        !fs::makeDirs(config_.runDir, error) || !fs::makeDirs(config_.appsDir, error) ||
        !fs::makeDirs(config_.dataDir, error) || !fs::makeDirs(config_.tmpDir, error)) {
        return false;
    }
    startNote_.clear();
    if (fs::kindOf(markerPath()) != fs::kNone) {
        // The run folder is cleared at power down, so this marker is from an agent that ended since the head unit
        // started.
        remounted_ = true;
        std::string why;
        if (restoreReadOnly(&why)) {
            startNote_ = "an agent that ended early had left the system partition writable; it is read-only again";
        } else {
            startNote_ = "an agent that ended early left the system partition writable, and it could not be made read-only again: " + why;
        }
    }
    return true;
}

std::vector<std::string> HeadUnitPlatform::pluginScanDirs() const {
    std::vector<std::string> dirs(1, config_.pluginsDir);
    dirs.insert(dirs.end(), config_.otherPluginDirs.begin(), config_.otherPluginDirs.end());
    return dirs;
}

bool HeadUnitPlatform::readOnly(const std::string& path, bool* out, std::string* error) const {
    if (fs::readOnly(path, out)) return true;
    return fail(error, "cannot tell whether " + path + " is on a read-only file system");
}

bool HeadUnitPlatform::run(const std::vector<std::string>& argv, std::string* error) const {
    std::vector<std::string> env(1, "PATH=" + config_.hookPath);
    if (!config_.libraryPath.empty()) env.push_back("LD_LIBRARY_PATH=" + config_.libraryPath);
    Collect said;
    ProcessResult result;
    std::string why;
    std::string how;
    if (!runProcess(config_.shell, argv, env, "/", config_.commandTimeoutSeconds, &said, &result, &why)) {
        how = "could not be started: " + why;
    } else if (result.timedOut) {
        how = "did not end within " + number(config_.commandTimeoutSeconds) + " s";
    } else if (!result.exited) {
        how = "was ended by signal " + number((unsigned long)result.signal);
    } else if (result.exitCode != 0) {
        how = "exited with " + number((unsigned long)result.exitCode);
    } else {
        return true;
    }
    std::string printed = oneLine(said.text);
    return fail(error, "`" + joined(argv) + "` " + how + (printed.empty() ? "" : " (" + printed + ")"));
}

bool HeadUnitPlatform::makeWritable(const std::string& path, std::string* error) {
    bool ro = false;
    if (!readOnly(path, &ro, error)) return false;
    if (!ro) return true;

    bool systemRo = false;
    if (!readOnly(config_.systemMount, &systemRo, error)) return false;
    if (systemRo) {
        // The marker first: an agent that dies from here on leaves word that the partition may be writable.
        std::string why;
        if (!fs::writeFile(markerPath(), "", 0600, &why)) return fail(error, "the system partition was not remounted: " + why);
        bool ran = run(config_.remountWritable, &why);
        if (!readOnly(config_.systemMount, &systemRo, error)) {
            remounted_ = true;   // not known: put it back at the end of the step all the same
            return false;
        }
        if (systemRo) {
            fs::removeFile(markerPath(), 0);
            return fail(error, "the system partition could not be made writable: " +
                                   (ran ? "`" + joined(config_.remountWritable) + "` ran and changed nothing" : why));
        }
        remounted_ = true;
        if (!readOnly(path, &ro, error)) return false;
        if (!ro) return true;
    }
    // The system partition is writable, by this agent or by hand, and the path still is not.
    return fail(error, path + " is on a read-only file system that is not the system partition");
}

bool HeadUnitPlatform::restoreReadOnly(std::string* error) {
    if (!remounted_) return true;
    sync();
    std::string why;
    for (unsigned attempt = 0; attempt < config_.restoreAttempts; ++attempt) {
        if (attempt != 0) poll(0, 0, (int)config_.restoreRetryMs);
        bool ro = false;
        // Read-only already, by whoever's hand: nothing to run.
        if (!(readOnly(config_.systemMount, &ro, 0) && ro)) {
            std::string said;
            if (!run(config_.remountReadOnly, &said)) why = said;
            ro = false;
            if (!readOnly(config_.systemMount, &ro, 0)) ro = false;
        }
        if (ro) {
            remounted_ = false;
            fs::removeFile(markerPath(), 0);
            return true;
        }
    }
    return fail(error, "the system partition is still writable after " + number(config_.restoreAttempts) + " tries" +
                           (why.empty() ? "" : ": " + why));
}

bool HeadUnitPlatform::freeBytes(const std::string& path, uint64_t* out) const { return fs::freeBytes(path, out); }

uint64_t HeadUnitPlatform::reserveBytes(const std::string& path) const {
    return fs::sameDevice(path, config_.stateDir) ? config_.stateReserveBytes : config_.otherReserveBytes;
}

bool HeadUnitPlatform::canRun(const std::vector<std::string>& argv, const char* what, std::string* error) const {
    if (argv.empty()) return fail(error, std::string("no way to ") + what + " is known for this head unit");
    if (access(argv[0].c_str(), X_OK) != 0) return fail(error, "this head unit has no " + argv[0] + " to " + what + " with");
    return true;
}

bool HeadUnitPlatform::canRestartHmi(std::string* error) const { return canRun(config_.restartHmi, "restart the HMI", error); }
bool HeadUnitPlatform::canReboot(std::string* error) const { return canRun(config_.reboot, "reboot", error); }

bool HeadUnitPlatform::restartHmi(std::string* error) { return canRestartHmi(error) && run(config_.restartHmi, error); }
bool HeadUnitPlatform::reboot(std::string* error) { return canReboot(error) && run(config_.reboot, error); }

}  // namespace nbt
