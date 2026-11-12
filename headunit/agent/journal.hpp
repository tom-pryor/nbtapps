// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_JOURNAL_HPP
#define NBT_AGENT_JOURNAL_HPP

// The journal of the step in progress: <state>/journal/log, one JSON object per line. The engine writes down what
// it is about to change, syncs, and only then changes it, so after a power cut the file says everything that may
// have happened, and each line carries what is needed to undo it.
//
//   {"op":"begin","action":"upgrade","id":"org.nbtapp.hello","version":"1.2.0","oldVersion":"1.1.0"}
//   {"op":"backup","path":"/real/path","to":"<state>/journal/old/0"}     move a file aside     undo: move it back
//   {"op":"unlink","path":"/real/path","target":"hello"}                 remove a link         undo: make it again
//   {"op":"mkdir","path":"/real/path"}                                   make a folder         undo: remove if empty
//   {"op":"place","path":"/real/path"}                                   write a new file      undo: remove it
//   {"op":"link","path":"/real/path"}                                    make a link           undo: remove it
//   {"op":"commit","rmdirs":["/real/path"]}                              the step is decided: finish it, never undo
//
// A removal writes only "begin" (with "action":"remove"): it is finished after a power cut, not undone.
// A last line with no newline was cut off while being written. What it announced never started, so it is dropped.
// Beside the log, for the length of a step: journal/record (the new record being built), journal/old (files moved
// aside that the commit deletes) and journal/old-record (the previous record, moved aside at the commit).
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <string>
#include <vector>

namespace nbt {

class Journal {
public:
    Journal() : fd_(-1) {}
    ~Journal() { close(); }

    // Starts a new journal. Fails if one is there: a step that was interrupted has to be recovered first.
    bool create(const std::string& path, std::string* error);
    // Opens the journal that is there, to add to it: for the step that one agent begins and the next one decides.
    // Fails if its last line is not whole, because a line added after that one would be read as part of it.
    bool reopen(const std::string& path, std::string* error);
    // Writes one line and syncs it. Only after this returns true may the change it announces be made.
    bool append(const std::string& line, std::string* error);
    void close();

    // The complete lines of a journal, in order.
    static bool read(const std::string& path, std::vector<std::string>* lines, std::string* error);

private:
    Journal(const Journal&);
    Journal& operator=(const Journal&);
    int fd_;
    std::string path_;
};

}  // namespace nbt

#endif
