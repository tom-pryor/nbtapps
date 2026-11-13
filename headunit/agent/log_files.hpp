// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_LOG_FILES_HPP
#define NBT_AGENT_LOG_FILES_HPP

// The files a controller may pull with files.list and files.fetch (the plan document, "Pulling files", and the
// Files section of its Message reference): the logs a package's manifest names in `logs`, and the agent's own.
//
// A `logs` entry names one file, or the files of one folder that a wildcard in its last part matches: * any
// characters, ? one. It is under ${app}, ${data} or ${tmp}, the package's own folders, or an absolute path under
// /tmp or /var outside the agent's folders (manifest.hpp checks the text). Here the text meets the disk:
//
//   - Only regular files are listed. A symbolic link is left out, whatever it leads to, and so is a file whose
//     name is not UTF-8: the protocol's text could not name it.
//   - The folder an entry names has to be where its text says, with nothing on the way that is a link: its real
//     path is the real path of the folder the entry starts from (the folder that holds every ${app}, ${data} or
//     ${tmp}, or /tmp or /var) and then the entry's own text. So a link in a package's folder, or a package's
//     folder that is a link itself, leads nowhere.
//   - An absolute entry whose real folder is in the agent's state, staging, run folder or own folder is left out,
//     whatever the manifest's text said: the pairing keys are in the state.
//   - A file is opened only as the listing of that moment has it: the same path, still a regular file, and the
//     same file (device and inode) that lstat saw just before. QNX 6.5 has no O_NOFOLLOW to say that in one call.
//
// The agent's own logs are package org.nbtapp.agent's: agent.log, the file the launcher names in NBTAGENT_LOG
// (agent.hpp), agent.log.1 and agent.log.2 beside it, and trial.log in the agent's folder's log/ (nbtstart.sh).
// Each is listed while it is there.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdint.h>

#include <string>
#include <vector>

#include "manifest.hpp"
#include "platform.hpp"
#include "store.hpp"

namespace nbt {

const size_t kMaxLogFiles = 64;   // files in one listing, the newest

// One file a controller may pull.
struct LogFile {
    std::string path;     // as files.list names it: the entry's text with its last part filled in, or a bare name
    std::string real;     // where it is on this machine
    uint64_t size;
    int64_t modified;     // seconds since 1970, by this machine's clock

    LogFile() : size(0), modified(0) {}
};

// Whether a name matches the last part of an entry: * any characters, ? one character (UTF-8), anything else
// itself. A wildcard does not match a name that starts with a dot unless the pattern does too.
bool matchName(const std::string& pattern, const std::string& name);

// Whether a manifest's logs name ${tmp}: the agent then makes that folder.
bool namesTmp(const Manifest& m);

// The files a package's logs name, as they are now: newest first, at most kMaxLogFiles.
void listPackageLogs(const Manifest& m, const Store& store, const Platform& platform, std::vector<LogFile>* out);

// The agent's own logs. `ownLog` is the file NBTAGENT_LOG named, or empty when there was none.
void listAgentLogs(const std::string& ownLog, const Platform& platform, std::vector<LogFile>* out);

// Opens a file of a listing, to read. *size is the file's size now. False, with *error saying why, if it is no
// longer the regular file the listing saw.
bool openLogFile(const LogFile& file, int* fd, uint64_t* size, std::string* error);

}  // namespace nbt

#endif
