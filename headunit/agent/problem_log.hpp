// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_PROBLEM_LOG_HPP
#define NBT_AGENT_PROBLEM_LOG_HPP

// The problem file of Bluetooth serial, serial-problems.log in the agent's state folder: a few short lines that
// outlast a power down, which the agent's own log on the RAM disk does not, for what went wrong with Bluetooth
// serial. The state folder is on flash, so it is written seldom:
//
//   * only when a problem comes: a registration that the car had dropped and the check restored, the car's
//     Bluetooth service that never attached, a refusal of a phone's link, a dial that went wrong, a reset;
//   * only the first problem of each kind in a run of the agent, and kMostLines lines a run at most. What is not
//     written is counted (serial.status says how many);
//   * a line is short (kMostLineBytes at most): the time, the agent's version and build, and what happened. Never a
//     phone's address or name;
//   * the file is kept under kMostBytes: when a line would take it over, the older half of its lines goes, in one
//     rewrite of the whole file that is renamed into place (fsutil.hpp);
//   * a run with no problem does not create the file, open it or touch it at all.
//
// note() may come from any thread and only queues the line; flush() writes what is queued, on the thread that can
// wait for the flash (the listener's).
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <pthread.h>
#include <stdint.h>

#include <set>
#include <string>
#include <vector>

namespace nbt {

class ProblemLog {
public:
    enum { kMostLines = 16, kMostBytes = 32768, kMostLineBytes = 200 };

    // `path`: the file. `who`: the agent's version and build, for every line ("1.5.1 (build ...)").
    ProblemLog(const std::string& path, const std::string& who);
    ~ProblemLog();

    // A problem of `kind`, said by `text`. Queued if it is the first of its kind in this run and there is room; else
    // only counted. From any thread.
    void note(const std::string& kind, const std::string& text);
    // Writes what is queued. False, with *why, if the file could not be written: those lines are lost, and counted.
    bool flush(std::string* why);

    unsigned recorded();   // lines written or queued in this run
    unsigned counted();    // problems not written

    // The file's text with `line` added, kept under kMostBytes as said above. For flush(), and for a test.
    static std::string withLine(const std::string& text, const std::string& line);

private:
    ProblemLog(const ProblemLog&);
    ProblemLog& operator=(const ProblemLog&);

    std::string path_, who_;
    pthread_mutex_t mu_;   // everything below
    std::set<std::string> kinds_;
    std::vector<std::string> queued_;
    unsigned recorded_, counted_;
};

}  // namespace nbt

#endif
