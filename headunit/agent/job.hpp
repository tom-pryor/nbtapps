// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_JOB_HPP
#define NBT_AGENT_JOB_HPP

// A job: one plan being carried out (plan document, "Jobs and disconnects"; "Message reference", Jobs and Job
// events). The agent runs one at a time and remembers the last one.
//
//   fetching     the archives of the install steps are pulled into the blob cache, <staging>/blobs/<sha256>, in
//                ranges of at most 1 MiB. A partial archive is <sha256>.part, and a fetch goes on from its end.
//                Nothing on the head unit has changed yet, so the job can be cancelled
//   paused       the link it fetched over has gone. It waits, with no timeout, to be started again
//   installing   the steps run, one engine step each. The first that fails ends the job; steps done stay done
//   restarting   the one step of the job has placed a newer version of the agent itself (engine.hpp, the agent's
//                own package). This agent has handed that version to the launcher and ends; the job is decided
//                by the agent that runs next
//   succeeded, failed, cancelled
//
// A job writes nothing to a link itself. It hands its events to a JobHost, which sends them to every controller,
// and it is told what arrived for it. What it keeps on disk, so that a controller that connects later, or after
// the agent has restarted, still learns how the job went:
//
//   <state>/job/job.json   the plan's steps, the state, and each step's result so far; rewritten at every step
//   <state>/job/events     the job.log and job.done events, one per line; at least the newest 256
//
// A job that was running when the agent stopped does not go on afterwards: load() settles it from what the
// engine's recovery found, and reports the step that was interrupted.
//
// A job that is restarting is the one that outlasts its agent. The new version, on trial, reads it and leaves the
// record as it is: nothing is written or sent until a controller has reached that version and the engine has
// finished the step (settleRestart). The old version, started again after a try that was not confirmed, settles
// it in load() as failed, as it does a step that a power cut interrupted.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "engine.hpp"
#include "json_writer.hpp"
#include "planner.hpp"

namespace nbt {

class JobHost {
public:
    virtual ~JobHost() {}
    // An event for every connected controller.
    virtual void jobEvent(const std::string& name, int64_t seq, const std::string& params) = 0;
    // Called while a step runs: the host's chance to serve its links.
    virtual void jobIdle() = 0;
    // Milliseconds on a clock that only goes forward.
    virtual int64_t nowMs() = 0;
};

class Job : public StepListener {
public:
    Job(Engine* engine, JobHost* host);
    virtual ~Job();

    // Call once at start, after Engine::recover(): reads the last job back. If it was still running, it is settled
    // now; `recovered` and `report` are what recover() found.
    bool load(bool recovered, const StepReport& report, std::string* error);
    // True if load() found a job that was still running and ended it.
    bool settledAtLoad() const { return settled_; }
    // For the agent on trial, once the engine's recover() has run again after the confirmation: ends the job
    // that is restarting with what recover() found.
    void settleRestart(bool recovered, const StepReport& report);

    bool exists() const { return !planId_.empty(); }
    const std::string& planId() const { return planId_; }
    const std::string& state() const { return state_; }
    // Fetching, installing or restarting: no other job may start.
    bool active() const { return state_ == "fetching" || state_ == "installing" || state_ == "restarting"; }
    bool fetching() const { return state_ == "fetching"; }
    bool paused() const { return state_ == "paused"; }
    bool installing() const { return state_ == "installing"; }
    bool restarting() const { return state_ == "restarting"; }
    // True once a step of this process has handed a newer version of the agent to the launcher: the process is
    // to end, with the status that has the launcher start the agent again.
    bool handingOver() const { return handOver_; }
    bool ended() const { return state_ == "succeeded" || state_ == "failed" || state_ == "cancelled"; }

    // Begins a new job, in place of the last one. It starts as fetching, or as installing if no archive has to be
    // fetched. Nothing is sent from here: the first event comes with the first fetch or step.
    bool start(const std::string& planId, const Plan& plan, std::string* error);

    // ---- fetching
    // The next range to ask for. False when there is none: the job is not fetching, or it has just found every
    // archive complete and moved on to installing (or failed on a wrong hash).
    bool nextFetch(std::string* sha256, uint64_t* offset, uint64_t* length);
    // Bytes of the range asked for, in order.
    void fetchData(const std::string& bytes);
    // The range is complete (`ok`), or the stream ended early with a blob.error code or a fault of the link.
    void fetchEnded(bool ok, const std::string& code, const std::string& message);
    void pause();
    void resume();
    void cancel();

    // ---- installing
    // Runs the next step and returns when it is over. Call while installing().
    void runStep();

    // ---- reporting
    // The `job` object of job.start and job.status.
    std::string describe() const;
    // The kept events numbered above `after`, oldest first, as far as they fit in `room` bytes. Each is added to
    // `out` (inside an array) as the event message it was sent as.
    void replay(int64_t after, size_t room, json::Writer* out, bool* more) const;
    // What the steps that are done need now: "none", "hmi-restart" or "required".
    const std::string& restart() const { return restart_; }

    // Test only: the largest range asked for at a time.
    void setFetchChunk(uint64_t bytes) { fetchChunk_ = bytes; }

    // StepListener: the engine's account of the step that is running.
    virtual void phase(const char* name);
    virtual void progress(uint64_t bytes, uint64_t total);
    virtual void log(const char* source, const std::string& text);
    virtual void idle();

private:
    Job(const Job&);
    Job& operator=(const Job&);

    struct Step {
        PlanStep plan;
        std::string result;   // "", then "done", "failed", "broken" or "skipped"
        Error error;
    };
    struct KeptEvent {
        int64_t seq;
        std::string text;     // the whole event message
    };

    std::string dir() const;
    std::string blobPath(const Step& step) const;
    bool persist(std::string* error) const;
    void emit(const char* name, const std::string& params, bool keep);
    void emitProgress(bool force);
    void emitLog(const char* source, const std::string& text);
    void say(const std::string& line) { emitLog("agent", line + "\n"); }
    std::string doneParams() const;
    // Ends the job: every step with no result yet is skipped, job.done goes out, and the job is written down.
    void finish(const char* state);
    void failFetch(const char* code, const std::string& message);
    // Gives the step that was running when an agent stopped its result, from what the engine's recovery found.
    void settleStep(bool recovered, const StepReport& report, bool wasRestarting);
    void closePart();
    void trimEvents();

    Engine* engine_;
    JobHost* host_;

    std::string planId_, state_;
    std::vector<Step> steps_;
    size_t step_;              // the step being worked on
    std::string phase_;
    bool hasBytes_;
    uint64_t bytes_, total_;
    int64_t seq_;
    std::string restart_;

    std::vector<KeptEvent> events_;
    std::map<std::string, std::string> tails_;   // per log source: a character cut off at the end of a piece
    int64_t lastProgressMs_;
    std::string lastProgressKey_;

    uint64_t fetchChunk_;
    int partFd_;
    unsigned fetchFailures_;
    bool settled_;
    bool handOver_;
};

}  // namespace nbt

#endif
