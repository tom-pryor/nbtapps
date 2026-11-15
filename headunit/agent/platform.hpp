// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_PLATFORM_HPP
#define NBT_AGENT_PLATFORM_HPP

// Everything the install engine needs to know or do that depends on the machine it runs on: where things live, how
// a read-only partition is made writable, how the HMI is restarted. The engine reaches the car through this
// interface only, so the same engine runs on a build host against a folder (HostPlatform) and on the head unit
// (HeadUnitPlatform, hu_platform.hpp). That one takes its folders and its commands from a configuration; what
// hu_probe.sh, beside this file, reads off a car is where the car's comes from. Nothing in this file touches one.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>

#include <string>
#include <vector>

#include "programs.hpp"

namespace nbt {

class Platform {
public:
    virtual ~Platform() {}

    // Persistent and writable: installed records, the journal, backups. /var/opt/nbtapp on the head unit.
    virtual std::string stateDir() const = 0;
    // Writable, with room for archives: the blob cache and unpacked packages. Nothing in it is needed after a power
    // cut, and it need not be lost in one: the agent empties it the first time it starts after one. On the head
    // unit it is on the persistent partition, because the RAM disk behind /tmp is too small for archives.
    virtual std::string stagingDir() const = 0;
    // Writable, and lost when the head unit powers down: small marks that have to go when it does.
    virtual std::string runDir() const = 0;
    // ${app} and ${data} of one package, and ${plugins}, the HMI's plugin folder.
    virtual std::string appDir(const std::string& id) const = 0;
    virtual std::string dataDir(const std::string& id) const = 0;
    // ${tmp} of one package: its own folder on the RAM disk, for logs that are to spare the flash and may go at a
    // power down. Only a manifest's `logs` names it (log_files.hpp). /tmp/nbtapp/apps/<id> on the head unit.
    virtual std::string tmpDir(const std::string& id) const = 0;
    virtual std::string pluginsDir() const = 0;
    // The agent's own folder (agent_home.hpp): a folder for each version of it, and the files that say which one
    // the launcher starts. /var/opt/nbtapp/agent on the head unit. It is there only where the agent was put
    // onto the machine that way: the platform does not make it.
    virtual std::string agentHome() const = 0;
    // Every folder the HMI loads plugins from: pluginsDir(), and any other, such as the stock, read-only one on a
    // head unit whose ${plugins} is the writable one. The ids already in use are read from all of them.
    virtual std::vector<std::string> pluginScanDirs() const = 0;
    // Where an absolute destination from a manifest really is: the path itself on the head unit, the same path
    // under a folder on a build host.
    virtual std::string systemPath(const std::string& absolute) const = 0;

    // Called before the first change beneath `path` in a step. Remounts the partition that holds it if that
    // partition is read-only, and remembers having done so.
    virtual bool makeWritable(const std::string& path, std::string* error) = 0;
    // Puts back read-only whatever makeWritable changed. Called at the end of every step, however it ended, and
    // before every hook: a hook finds the system partition as it is when no step runs.
    virtual bool restoreReadOnly(std::string* error) = 0;

    // Free bytes on the file system that holds `path`, or the nearest folder above it that exists.
    virtual bool freeBytes(const std::string& path, uint64_t* out) const = 0;
    // Bytes to leave free on the file system that holds `path`, beyond what a job is known to need there. More
    // where the agent shares a partition with the head unit's own settings.
    virtual uint64_t reserveBytes(const std::string& path) const = 0;

    // The shell hooks run under: /bin/ksh on the head unit.
    virtual std::string shell() const = 0;
    // PATH for hooks.
    virtual std::string hookPath() const = 0;

    // Whether this machine has a way to do each restart. system.restart is answered before the restart is tried,
    // so what can be known beforehand is asked here; false with *error saying why.
    virtual bool canRestartHmi(std::string* error) const = 0;
    virtual bool canReboot(std::string* error) const = 0;
    virtual bool restartHmi(std::string* error) = 0;
    virtual bool reboot(std::string* error) = 0;

    // Whether a program with this file name is running on the machine (programs.hpp). The same on every platform
    // the agent has, the folder that stands in for a head unit included: it is the machine's processes that are
    // asked about. A test gives its own answer.
    virtual ProgramState programState(const std::string& name) const { return nbt::programState(name); }
    // The processes whose executable is in the folder `dir` or below it (programs.hpp), asked of the machine's
    // processes in the same way.
    virtual ProgramState programsUnder(const std::string& dir, std::vector<RunningProgram>* out) const {
        return nbt::programsUnder(dir, out);
    }
    // Sends signal `number` to process `pid`. True if it was sent, or there is no such process any more. A test
    // that gives its own answer to programsUnder gives its own here too, so that it signals no process it did not
    // start.
    virtual bool signalProcess(long pid, int number) { return kill((pid_t)pid, number) == 0 || errno == ESRCH; }

    // The system partition as it is now, for the check that finds it left writable (checks.hpp).
    enum SystemState {
        kSystemNone,       // the question does not apply: the machine has no system partition the agent remounts
        kSystemReadOnly,
        kSystemWritable,
        kSystemUnknown     // it could not be told; *error says why
    };
    virtual SystemState systemState(std::string* /*error*/) const { return kSystemNone; }
    // True while the system partition is writable because makeWritable made it so and restoreReadOnly has not put
    // it back yet, or because an agent that ended in the middle of that left its marker.
    virtual bool remountedForStep() const { return false; }
    // Puts the system partition back read-only now, whoever made it writable, and then asks the file system
    // whether it is. False with *error saying why if it is not.
    virtual bool putSystemReadOnly(std::string* error) {
        if (error != 0) *error = "this machine has no system partition to remount";
        return false;
    }
};

}  // namespace nbt

#endif
