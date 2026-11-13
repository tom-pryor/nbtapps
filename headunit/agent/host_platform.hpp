// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_HOST_PLATFORM_HPP
#define NBT_AGENT_HOST_PLATFORM_HPP

// The platform for a build host: everything under one folder, so the agent can be run and tested with no car. It
// is what the tests use, and what nbtagent uses when it is given --root, on a build host or on a head unit.
//
//   <root>/state      installed records, journal        <root>/apps/<id>   ${app}
//   <root>/staging    blob cache, unpacked packages     <root>/data/<id>   ${data}
//   <root>/plugins    ${plugins}                        <root>/system/...  absolute destinations
//   <root>/run        the marks a power down takes away: removing this folder is the power cut of a test
//   <root>/tmp/<id>   ${tmp}, which a power down would take away too
//   <root>/agent      the agent's own folder, if a test or a launcher has made one: prepare() does not
//
// Nothing is remounted and nothing restarts: those calls are counted, so a test can see that the engine made them.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <string>
#include <vector>

#include "fsutil.hpp"
#include "platform.hpp"

namespace nbt {

class HostPlatform : public Platform {
public:
    explicit HostPlatform(const std::string& root)
        : hmiRestarts(0), reboots(0), fakeFree(-1), reserve(64u << 10), root_(root), writable_(false) {}

    // Makes the folders a head unit would already have.
    bool prepare(std::string* error) const {
        return fs::makeDirs(stateDir(), error) && fs::makeDirs(stagingDir(), error) && fs::makeDirs(runDir(), error) &&
               fs::makeDirs(pluginsDir(), error) && fs::makeDirs(root_ + "/system", error);
    }

    virtual std::string stateDir() const { return root_ + "/state"; }
    virtual std::string stagingDir() const { return root_ + "/staging"; }
    virtual std::string runDir() const { return root_ + "/run"; }
    virtual std::string appDir(const std::string& id) const { return root_ + "/apps/" + id; }
    virtual std::string dataDir(const std::string& id) const { return root_ + "/data/" + id; }
    virtual std::string tmpDir(const std::string& id) const { return root_ + "/tmp/" + id; }
    virtual std::string pluginsDir() const { return root_ + "/plugins"; }
    virtual std::string agentHome() const { return root_ + "/agent"; }
    virtual std::vector<std::string> pluginScanDirs() const {
        std::vector<std::string> dirs(1, pluginsDir());
        dirs.insert(dirs.end(), morePluginDirs.begin(), morePluginDirs.end());
        return dirs;
    }
    virtual std::string systemPath(const std::string& absolute) const { return root_ + "/system" + absolute; }

    // Stands in for the read-only system partition: <root>/plugins and <root>/system count as being on it.
    virtual bool makeWritable(const std::string& path, std::string*) {
        if (onSystem(path)) {
            if (!writable_) remounts.push_back("rw");
            writable_ = true;
        }
        return true;
    }
    virtual bool restoreReadOnly(std::string*) {
        if (writable_) remounts.push_back("ro");
        writable_ = false;
        return true;
    }

    // The real figure, or `fakeFree` when a test has set one.
    virtual bool freeBytes(const std::string& path, uint64_t* out) const {
        if (fakeFree >= 0) {
            *out = (uint64_t)fakeFree;
            return true;
        }
        return fs::freeBytes(path, out);
    }
    virtual uint64_t reserveBytes(const std::string&) const { return reserve; }

    virtual std::string shell() const { return "/bin/sh"; }
    virtual std::string hookPath() const { return "/usr/bin:/bin"; }

    virtual bool canRestartHmi(std::string* error) const { return can(noHmiRestart, error); }
    virtual bool canReboot(std::string* error) const { return can(noReboot, error); }
    virtual bool restartHmi(std::string* error) {
        if (!can(noHmiRestart, error)) return false;
        ++hmiRestarts;
        return true;
    }
    virtual bool reboot(std::string* error) {
        if (!can(noReboot, error)) return false;
        ++reboots;
        return true;
    }

    bool onSystem(const std::string& path) const {
        return path.compare(0, root_.size() + 9, root_ + "/plugins/") == 0 || path == root_ + "/plugins" ||
               path.compare(0, root_.size() + 8, root_ + "/system/") == 0 || path == root_ + "/system";
    }
    bool writableNow() const { return writable_; }

    std::vector<std::string> remounts;   // "rw" and "ro", in the order they happened
    int hmiRestarts, reboots;
    long long fakeFree;                  // free bytes to report everywhere; below 0 for the real figure
    uint64_t reserve;                    // bytes kept free everywhere: 64 KiB unless a test sets another figure
    std::vector<std::string> morePluginDirs;   // further folders the HMI is taken to load plugins from
    std::string noHmiRestart, noReboot;  // not empty: this machine cannot do that restart, and this is why

private:
    static bool can(const std::string& refusal, std::string* error) {
        if (refusal.empty()) return true;
        if (error != 0) *error = refusal;
        return false;
    }

    std::string root_;
    bool writable_;
};

}  // namespace nbt

#endif
