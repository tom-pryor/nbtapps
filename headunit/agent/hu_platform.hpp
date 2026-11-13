// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_HU_PLATFORM_HPP
#define NBT_AGENT_HU_PLATFORM_HPP

// The platform for the head unit: real paths, the system partition remounted for a write and put back after it,
// and the HMI restarted by the car's own program.
//
// Everything that differs from car to car, or that the repo does not know yet, is in HeadUnitConfig: the folders,
// and the command lines that remount and restart. So the class itself holds no fact about a car, and the bench
// runs it whole against a folder, with scripts standing in for `mount` and the restart program
// (tests/agent/test_hu_platform.cpp). The head unit's build of nbtagent uses it when it is started with no
// --root, with HeadUnitConfig::car(). It has not run on a car.
//
// The system partition:
//   - makeWritable(path) does nothing for a path on a writable file system. For one on a read-only file system it
//     runs the remount command, once per step however many paths follow, and checks that it helped.
//   - A partition that is writable when the agent comes to it (somebody remounted it by hand) is used as it is
//     and left as it is: restoreReadOnly puts back only what makeWritable changed.
//   - Before the remount a marker file goes into the run folder. An agent that dies while the partition is
//     writable leaves the marker, and the next one puts the partition back when it starts (prepare). A power cut
//     clears the run folder, and the head unit mounts the partition read-only again by itself.
//   - If the partition cannot be put back (something holds a file open for writing), restoreReadOnly says so and
//     the next call tries again. Until then it stays writable; a reboot ends that too.
//
// ${app} on the system partition is not supported: prepare() makes the folder that holds every ${app}, so it has
// to be on a file system that is writable without a remount.
//
// Two folders for what does not last. The run folder is on the RAM disk behind /tmp, which a power down empties
// and which is small: it holds the marks. Staging holds the archives and is on the persistent partition, beside
// the state, where there is room; the agent empties it after a power cut (Agent::init). That partition also
// holds the head unit's own settings, so a job has to leave stateReserveBytes free on it.
//
// The commands run through runProcess, under the configured shell (process.hpp).
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdint.h>

#include <string>
#include <vector>

#include "platform.hpp"

namespace nbt {

struct HeadUnitConfig {
    std::string stateDir;      // persistent: records, journal, backups
    std::string stagingDir;    // blobs and unpacked packages: needs room, and is emptied after a power cut
    std::string runDir;        // lost at power down: the marker, and the agent's own marks
    std::string appsDir;       // ${app} of a package is <appsDir>/<id>
    std::string dataDir;       // ${data} of a package is <dataDir>/<id>
    std::string tmpDir;        // ${tmp} of a package is <tmpDir>/<id>: on the RAM disk, lost at power down
    std::string pluginsDir;    // ${plugins}
    std::string agentDir;      // the agent's own folder: its versions, and the files the launcher reads
    std::vector<std::string> otherPluginDirs;   // further folders the HMI loads plugins from
    std::string systemMount;   // where the system partition is mounted; its read-only flag is the one that counts

    // Command lines, the program by its absolute path. They run as the agent, with PATH set to hookPath and
    // LD_LIBRARY_PATH to libraryPath, and nothing else in their environment.
    std::vector<std::string> remountWritable, remountReadOnly;
    std::vector<std::string> restartHmi;
    std::vector<std::string> reboot;   // may be empty: the agent then refuses to reboot

    std::string shell;         // hooks run under it, and it starts the commands above
    std::string hookPath;      // PATH for hooks and for the commands above
    std::string libraryPath;   // LD_LIBRARY_PATH for the commands above, not for hooks. May be empty: the loader
                               // then looks where it does by itself

    unsigned commandTimeoutSeconds;   // a command that takes longer is killed
    unsigned restoreAttempts;         // how often restoreReadOnly tries before it gives up for now
    unsigned restoreRetryMs;          // and how long it waits in between

    uint64_t stateReserveBytes;       // kept free on the file system that holds stateDir, whatever a job needs
    uint64_t otherReserveBytes;       // and on every other file system

    HeadUnitConfig()
        : commandTimeoutSeconds(20), restoreAttempts(5), restoreRetryMs(200), stateReserveBytes(32u << 20),
          otherReserveBytes(64u << 10) {}

    // What the repo knows about an NBT Evo head unit, and nothing it would have to guess. See the .cpp file for
    // where each value comes from and for what is left open.
    static HeadUnitConfig car();

    // The names of the values that are not set, or not usable as they are, separated by ", ". Empty if the
    // configuration can be used.
    std::string missing() const;
};

class HeadUnitPlatform : public Platform {
public:
    explicit HeadUnitPlatform(const HeadUnitConfig& config);

    // Makes the agent's own folders: state, staging, run, and the ones that hold ${app}, ${data} and ${tmp}. If an agent that
    // died left the system partition writable, puts it back; startNote() then says what was found and done. So
    // the caller has to know that no other agent is at work on this head unit: its marker would be taken for a
    // dead one's.
    // False with *error set, and nothing made, if the configuration is incomplete or this machine is not the one
    // it describes: systemMount and pluginsDir have to be folders already. False also if a folder cannot be made.
    bool prepare(std::string* error);
    const HeadUnitConfig& config() const { return config_; }
    // One line for the log after prepare(), or empty: nothing was out of the ordinary.
    const std::string& startNote() const { return startNote_; }

    virtual std::string stateDir() const { return config_.stateDir; }
    virtual std::string stagingDir() const { return config_.stagingDir; }
    virtual std::string runDir() const { return config_.runDir; }
    virtual std::string appDir(const std::string& id) const { return config_.appsDir + "/" + id; }
    virtual std::string dataDir(const std::string& id) const { return config_.dataDir + "/" + id; }
    virtual std::string tmpDir(const std::string& id) const { return config_.tmpDir + "/" + id; }
    virtual std::string pluginsDir() const { return config_.pluginsDir; }
    virtual std::string agentHome() const { return config_.agentDir; }
    virtual std::vector<std::string> pluginScanDirs() const;
    virtual std::string systemPath(const std::string& absolute) const { return absolute; }

    virtual bool makeWritable(const std::string& path, std::string* error);
    virtual bool restoreReadOnly(std::string* error);

    virtual bool freeBytes(const std::string& path, uint64_t* out) const;
    virtual uint64_t reserveBytes(const std::string& path) const;

    virtual std::string shell() const { return config_.shell; }
    virtual std::string hookPath() const { return config_.hookPath; }

    virtual bool canRestartHmi(std::string* error) const;
    virtual bool canReboot(std::string* error) const;
    virtual bool restartHmi(std::string* error);
    virtual bool reboot(std::string* error);

    // True from a remount by makeWritable until restoreReadOnly has put the partition back.
    bool remounted() const { return remounted_; }
    std::string markerPath() const { return config_.runDir + "/system-writable"; }

protected:
    // Whether the file system that holds `path` is read-only now. A test with no right to remount anything puts
    // its own answer here.
    virtual bool readOnly(const std::string& path, bool* out, std::string* error) const;

private:
    HeadUnitPlatform(const HeadUnitPlatform&);
    HeadUnitPlatform& operator=(const HeadUnitPlatform&);

    // Runs one of the configured commands. False with *error saying how it ended, and what it printed, unless it
    // exited with 0.
    bool run(const std::vector<std::string>& argv, std::string* error) const;
    bool canRun(const std::vector<std::string>& argv, const char* what, std::string* error) const;

    HeadUnitConfig config_;
    bool remounted_;
    std::string startNote_;
};

}  // namespace nbt

#endif
