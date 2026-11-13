// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_ENGINE_HPP
#define NBT_AGENT_ENGINE_HPP

// The install engine: one step at a time, each step one package installed, upgraded or removed. The plan document
// ("Install instructions") sets the order; a job (planning, fetching, several steps) is built on top of this.
//
// An install or upgrade:
//   verify        the archive, as package.hpp describes, and the checks below; unpack it into staging
//   pre-install   the preInstall hook. Until here nothing on the head unit has changed
//   backup        every file about to be replaced or removed is moved aside, a stock file into the new record
//   files         folders, files and links are put in place
//   post-install  the postInstall hook
//   record        the new record is written, and the journal's commit line decides the step
// A failure before the commit undoes everything, from the journal, last change first ("rollback"). After a power
// cut recover() does the same from the journal file: undo before the commit, finish after it.
//
// A removal runs preRemove, removes the package's files, links and folders, puts stock files back, runs
// postRemove and deletes the record. It is finished after a power cut, not undone.
//
// The agent's own package, org.nbtapp.agent (the plan document, "Agent updates"; agent_home.hpp), is an install
// like any other up to the record, with these differences:
//   - ${app} is the folder of that version in the agent's own folder. The version that runs stays where it is:
//     nothing of it is moved aside, because the launcher starts it again if the new one does not prove itself
//   - it places files in ${app} and ${data} only, ${app}/nbtagent and the version's launcher,
//     ${app}/nbtlaunch.sh, among them. Before the record is written the program is asked for its version and
//     the shell reads the launcher: a version that does not start here, or whose launcher the shell cannot
//     read, never gets a try
//   - the step ends without the journal's commit line, as kStepRestart. The caller hands the new version to
//     the launcher (beginTrial) and ends the process
//   - recover() decides it, by what the files in the agent's folder say to the agent that calls it: the new
//     version leaves the journal alone until a controller has reached it (confirmTrial), then writes the commit
//     line and finishes the step; the old version, started again after a try that was not confirmed, undoes
//     the step and removes the new version's folder
//   - what counts as installed is the version that runs, with or without a record, and any built-in key may
//     sign it. It cannot be removed
//
// What an install leaves alone or keeps:
//   - a file marked `config` that was changed on the head unit since it was installed (or was there before)
//   - a file that already is what the package would place
//   - ${data}: never removed on upgrade, and on removal only when asked
//   - outside the package's own folder, a file that is no longer the one the package placed (a firmware update
//     may have replaced it): a removal leaves it, and an upgrade treats it as a stock file again
//
// The system partition is writable only while the engine itself changes something on it. It is read-only again
// before every hook and at the end of the step; what comes after a hook, a rollback included, makes it writable
// anew. A hook therefore finds it as it is when no step runs, whatever the package places.
//
// Hooks are started without fork() (process.hpp), so the process the engine runs in may have other threads. The
// engine itself is not made for more than one: every call into it comes from the same thread.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdint.h>

#include <set>
#include <string>
#include <vector>

#include "agent_home.hpp"
#include "error.hpp"
#include "package.hpp"
#include "platform.hpp"
#include "store.hpp"

namespace nbt {

// ---- checks on the installed set --------------------------------------------------------------------------------

struct Problem {
    std::string code, id, message;
};

struct MissingDependency {
    std::string id, range, requiredBy;
};

// May `manifest`, signed by `keyHex`, be installed beside `installed`? Adds what speaks against it:
// reserved-id, key-mismatch, downgrade, already-installed, agent-too-old, conflict. A dependency that no
// installed package satisfies goes to `missing`. `installed` may be the real set or one a planner has worked out.
// For the agent's own package the installed version is `agentVersion`, and no key-mismatch is looked for.
void checkInstall(const Manifest& manifest, const std::string& keyHex, bool keyEmbedded,
                  const std::vector<InstalledPackage>& installed, const std::string& agentVersion,
                  std::vector<Problem>* problems, std::vector<MissingDependency>* missing);

// May `id` be removed? not-installed, or required-by for each installed package that depends on it, and for
// the agent's own package, which everything installed here depends on.
void checkRemove(const std::string& id, const std::vector<InstalledPackage>& installed, std::vector<Problem>* problems);

// ---- steps ------------------------------------------------------------------------------------------------------

// Told what a step is doing, as it does it. The names are the protocol's: job.progress and job.log.
class StepListener {
public:
    virtual ~StepListener() {}
    virtual void phase(const char* /*name*/) {}
    virtual void progress(uint64_t /*bytes*/, uint64_t /*total*/) {}
    // `source` is "agent" or a hook's name.
    virtual void log(const char* /*source*/, const std::string& /*text*/) {}
    // Called between the parts of a step and about ten times a second while a hook runs. A step can take minutes,
    // and the agent has one thread: this is where it serves its links in the meantime. Whatever is called from
    // here must not start another step.
    virtual void idle() {}
};

enum StepOutcome {
    kStepDone,     // completed
    kStepFailed,   // failed and undone: nothing of it remains
    kStepBroken,   // failed, and undoing (or finishing) it did not complete. The install log has the detail
    kStepRestart   // the agent's own package: placed, and decided only by the new version, once the launcher has
                   // started it and a controller has reached it. Nothing of the old version has changed
};

struct StepReport {
    StepOutcome outcome;
    Error error;              // set unless the outcome is kStepDone
    std::string action;       // "install", "upgrade" or "remove"
    std::string id, version;  // the package; for a removal the version that was installed
    std::string oldVersion;   // upgrades only
    std::string restart;      // what the head unit needs now that the step is done: "none", "hmi-restart", "required"

    StepReport() : outcome(kStepFailed), restart("none") {}
};

// Told the two moments of a step that matter to whoever keeps something outside the engine in line with the
// installed packages (serial_services.hpp). Called on the engine's thread, inside the step.
class StepObserver {
public:
    virtual ~StepObserver() {}
    // The hook that stops a package's programs has had its turn: preInstall of an install or an upgrade, where
    // `manifest` is the new package's, or preRemove of a removal. Called whether or not the package has that hook,
    // and for a preRemove that failed too: either way the package's programs are not expected to run from here.
    virtual void programsStopped(const Manifest& manifest) = 0;
    // A step has ended, however it ended: the set of installed packages may be another now.
    virtual void stepEnded() = 0;
};

// Test only: called at every point where a power cut would leave a different state behind.
class CrashHook {
public:
    virtual ~CrashHook() {}
    virtual void point(const char* name) = 0;
};

class Engine {
public:
    // `keys` are the package keys the agent trusts. `agentVersion` is what requires.agent is checked against.
    Engine(Platform* platform, const std::vector<TrustedKey>& keys, const std::string& agentVersion);

    // Makes the store's folders. Call first.
    bool init(std::string* error);
    // Empties staging: the blob cache and the unpack folder. For the first start after a power cut.
    bool clearStaging(std::string* error) { return store_.clearStaging(error); }

    // Finishes or undoes a step that a power cut or a crash interrupted. Call once at start, before any step.
    // *found says whether there was one; *report is then its result. A step that was undone has the error code
    // "interrupted". Returns false only if the store itself cannot be read.
    //
    // A step of the agent's own package that has no commit line is decided here, by the agent's folder:
    //   current names the step's version        it was confirmed: the commit line is written and the step finished
    //   this agent is the step's version        it is on trial: nothing is touched, and the outcome is kStepRestart
    //   current names this agent's version      the try is over, or never began: the step is undone, with the
    //                                           error code "not-confirmed" if a trial file is there
    //   neither                                 not this agent's to decide: left alone, and reported as broken
    // The trial files left over from an update that is decided go as well, and with a version that was not
    // confirmed its folder. An agent on trial therefore calls this again after confirmTrial().
    bool recover(bool* found, StepReport* report, StepListener* listener);

    // What the files in the agent's own folder say to this agent now.
    TrialState trialState() const;
    // After a step that ended as kStepRestart: hands `version` to the launcher, which starts it once when this
    // process has ended (exit_status.hpp, kExitRestart). False if nothing will start it.
    bool beginTrial(const std::string& version, std::string* error);
    // For an agent on trial: makes its version the one the launcher starts from now on. One rename decides it.
    bool confirmTrial(std::string* error);
    // The starter (agent_home.hpp), for the agent that current names, with no update under way: puts the one this
    // version came with in place of the one that is there, if the two differ and the shell can read the new one.
    // Call at a start, and again once this version is confirmed. *said is a line for the log when the starter
    // was put in place or could not be; it is empty when there was nothing to do, which writes nothing.
    void settleStarter(std::string* said);

    // Installs the archive at `archivePath`, as a new package or over an older version. With
    // `expectedManifestSha256` not empty, the archive must hold exactly that manifest (the one a plan was made
    // with), or the step fails with bad-archive. With `signatureHex` not empty (128 hex characters: the signature
    // the plan's offer gave and the planner checked), that signature over the manifest is the one checked, and the
    // archive's own manifest.sig is not read; this is how a package that its author signed is installed with a
    // catalogue's signature. Empty: manifest.sig is checked.
    StepReport install(const std::string& archivePath, const std::string& expectedManifestSha256, StepListener* listener,
                       const std::string& signatureHex = std::string());

    // Removes an installed package. `deleteData` also deletes its ${data}.
    StepReport remove(const std::string& id, bool deleteData, StepListener* listener);

    // For a plan: would install() of a package with this manifest be refused for what is on the head unit now?
    // Runs the file checks of the verify phase (file-owned, stock-mismatch, no-space) without the archive and
    // changes nothing. `installed` is the set the step will find, which a plan works out from the steps before it.
    // `undecided` holds real paths that those earlier steps touch: what is at them now says nothing about what the
    // step will find, so they are left to the step itself. So are the HMI ids, which are inside the archive.
    // False with *error set if the step would be refused.
    bool precheck(const Manifest& manifest, const std::vector<InstalledPackage>& installed,
                  const std::set<std::string>& undecided, Error* error);

    const Store& store() const { return store_; }
    Platform* platform() const { return platform_; }
    const std::vector<TrustedKey>& keys() const { return keys_; }
    const std::string& agentVersion() const { return agentVersion_; }

    void setCrashHook(CrashHook* hook) { crashHook_ = hook; }
    // 0 for none. It outlives the engine, or is taken away again first.
    void setStepObserver(StepObserver* observer) { observer_ = observer; }

private:
    friend class InstallStep;
    friend class StepLog;
    Engine(const Engine&);
    Engine& operator=(const Engine&);

    void crash(const char* name) {
        if (crashHook_ != 0) crashHook_->point(name);
    }
    std::string journalPath() const { return store_.journalDir() + "/log"; }
    // Undoes every change the journal lists, last first, and removes the journal. False if something could not be
    // undone; the journal then stays, and recover() tries again.
    bool rollBack(class StepLog* log);
    // After the commit line: puts the new record in place, clears up, and removes the journal.
    bool finishCommitted(const std::string& id, const std::vector<std::string>& rmdirs, class StepLog* log);
    // The removal itself, also used to finish one after a power cut.
    bool removeFiles(const InstalledPackage& package, bool deleteData, bool runPreRemove, class StepLog* log, Error* error);
    // Clears what a step leaves beside the journal's log. With no log there, none of it is needed.
    void clearJournalFolder();
    bool runHook(const Manifest& manifest, HookKind kind, const std::string& cwd, const char* action,
                 const std::string& oldVersion, class StepLog* log, Error* error);
    // Has the platform put back read-only what the step made writable, and says so in the log if it cannot.
    void putBackReadOnly(class StepLog* log);
    // remove() without the word to the observer that the step has ended.
    StepReport removeStep(const std::string& id, bool deleteData, StepListener* listener);
    // With no journal left: removes the trial files of an update of the agent that is decided, and the folder of
    // a version that was not confirmed.
    void settleTrial();
    // After an update of the agent is finished: removes the folders of every version but these two, the one
    // that runs and the one current names.
    void pruneVersions(const std::string& now, const std::string& before);

    Platform* platform_;
    std::vector<TrustedKey> keys_;
    std::string agentVersion_;
    Store store_;
    CrashHook* crashHook_;
    StepObserver* observer_;
};

}  // namespace nbt

#endif
