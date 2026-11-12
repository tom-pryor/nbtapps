// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_AGENT_HOME_HPP
#define NBT_AGENT_AGENT_HOME_HPP

// The agent's own folder, as the agent reads and writes it: /var/opt/nbtapp/agent on a head unit (the plan
// document, "Agent updates"). The starter (nbtstart.sh, in the folder as `launcher`) reads the same files by the
// same rules:
//
//   <version>/nbtagent        one version of the agent. A version's folder is never written over
//   <version>/nbtlaunch.sh    that version's launcher, which the starter runs to have the version's agent started
//   <version>/nbtstart.sh     the starter that version came with
//   launcher                  the starter in place: what a person, or the iDrive plugin, starts
//   current                   one line: the version the starter has started
//   trial                     one line: a version that was placed and has not been tried
//   trial.started             the same file once the starter has begun its one try
//
// The starter renames trial and writes nothing else. The agent writes the rest: trial when it has placed a newer
// version of itself, current when a controller has reached that version, and it removes trial.started and the
// folders of versions that are no longer wanted.
//
// A version's launcher is tried with the version: the starter runs the launcher of whichever version it has
// chosen. The starter itself is tried with nothing, so it changes only once a version is decided: an agent that
// is the version current names, with no update under way, puts the starter it came with in place of `launcher`
// when the two differ (Engine::settleStarter). A starter that is running goes on as it was read, and the new
// one is what the next start of the head unit runs.
//
// A version's name starts with a digit and holds letters, digits, '.', '+' and '-' only, 64 at most, so no file's
// content can name a folder outside this one or one of the files beside the versions.
//
// Two more files are about the agent starting with the head unit, which the iDrive plugin sees to
// (src/agent_start.hpp):
//
//   autostart             there: the plugin starts the launcher when the HMI starts. Made and removed by the
//                         agent when a controller asks (autostart.set, agent.hpp), or by hand
//   autostart.boots       a line for each such start. Written by the plugin before the start, and removed by
//                         the agent once it has run for kAutostartSettledS, or has been stopped: what the file
//                         holds are the starts that no agent outlasted by that long
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>

#include <string>
#include <vector>

namespace nbt {

extern const char kAgentPackageId[];   // "org.nbtapp.agent": the package the agent updates itself with
extern const char kAgentProgram[];     // "nbtagent": the program that every version's folder holds
extern const char kAgentLauncher[];    // "nbtlaunch.sh": the version's launcher, which every version's folder holds
extern const char kAgentStarter[];     // "nbtstart.sh": the starter a version comes with, in its folder
extern const char kStarterInPlace[];   // "launcher": the starter that is started, in the agent's folder

const char kAutostartSwitch[] = "autostart";
const char kAutostartBoots[] = "autostart.boots";
// What starts the launcher with the head unit is the iDrive plugin, which is a package of its own: without it
// installed, in a version that does that, the file `autostart` asks nobody.
const char kPluginPackageId[] = "org.nbtapp.plugin";
const char kAutostartPluginRange[] = ">=1.0.0";
const int kAutostartSettledS = 5 * 60;

bool validVersionName(const std::string& name);

// What the files say to an agent of one version, when it starts.
enum TrialState {
    kNoTrial,     // neither trial file is there: no update of the agent is under way
    kConfirmed,   // a trial file names the version that current names: the update is decided, the file a leftover
    kOnTrial,     // a trial file names this agent's version, and current does not: this agent is being tried
    kWayBack,     // current names this agent's version, and a trial file names another one or none: the try is
                  // over without a confirmation, or never began. This agent undoes the update
    kNotOurs      // a trial file is there, and neither it nor current names this agent's version
};

class AgentHome {
public:
    struct Files {
        std::string current;    // the version that current names; empty if there is no such file or it names none
        bool trial, started;    // the file is there
        std::string pending;    // the version that trial.started names or, with no such file, trial; may be empty

        Files() : trial(false), started(false) {}
        bool any() const { return trial || started; }
    };

    explicit AgentHome(const std::string& dir) : dir_(dir) {}

    const std::string& dir() const { return dir_; }
    std::string versionDir(const std::string& version) const { return dir_ + "/" + version; }

    // Reads the three files. Nothing is written, and a folder that is not there reads as three missing files.
    Files read() const;
    static TrialState stateOf(const Files& files, const std::string& self);
    // Whether that version is there to be started, as the starter asks it: its program, and its launcher.
    bool hasProgram(const std::string& version) const;
    // The versions that have a folder here, sorted.
    void versions(std::vector<std::string>* out) const;

    // Each of the two is written whole: a temporary file, synced, and renamed into place.
    bool writeCurrent(const std::string& version, std::string* error) const;
    bool writeTrial(const std::string& version, std::string* error) const;
    // Removes trial and trial.started; true also when neither is there.
    bool removeTrial(std::string* error) const;
    // Removes the temporary file that an agent leaves which stopped while it wrote current or trial.
    void removeTemp() const;
    // Whether the starter is in place here, for something to start.
    bool hasStarter() const;
    // The file that asks for the agent to be started with the head unit: whether it is there; and making it, whole
    // and synced, or removing it. True also when there was nothing to do.
    bool autostart() const;
    bool setAutostart(bool on, std::string* error) const;
    // How many starts with the head unit autostart.boots has on record: with kMostStarts of them (src/agent_start.hpp)
    // the plugin starts nothing.
    size_t autostartStarts() const;
    // Removes autostart.boots: the starts with the head unit that it holds were followed by an agent that
    // stayed. True if the file was there and is gone. With no such file nothing is written.
    bool forgetAutostarts() const;
    // How the starter in place stands to the one `version` came with.
    enum Starter {
        kStarterNone,     // that version's folder holds no starter: there is nothing to put in place
        kStarterSame,     // the starter in place is that one, byte for byte
        kStarterDiffers   // it is another one, or there is none in place
    };
    Starter starterOf(const std::string& version) const;
    // Puts the starter `version` came with in place, as a program: written whole as current is, a temporary file,
    // synced, and renamed over the one that is there. What a starter that is running was read from is not touched.
    bool placeStarter(const std::string& version, std::string* error) const;
    // Removes a version's folder with what it holds; true also when it is not there. A name that is not a
    // version's is refused, so that nothing beside the versions can be named.
    bool removeVersion(const std::string& version, std::string* error) const;

private:
    std::string dir_;
};

}  // namespace nbt

#endif
