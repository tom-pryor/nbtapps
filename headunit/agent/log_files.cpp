// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The files a controller may pull: see log_files.hpp.

#include "log_files.hpp"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <set>

#include "agent_home.hpp"
#include "fsutil.hpp"
#include "utf8.hpp"

namespace nbt {

namespace {

bool startsWith(const std::string& text, const char* prefix) { return text.compare(0, strlen(prefix), prefix) == 0; }

// `path` is `dir` or inside it.
bool under(const std::string& path, const std::string& dir) {
    return !dir.empty() && path.compare(0, dir.size(), dir) == 0 && (path.size() == dir.size() || path[dir.size()] == '/');
}

// The real path of a folder or file that is there, or empty.
std::string realPath(const std::string& path) {
    char buffer[PATH_MAX + 1];
    if (realpath(path.c_str(), buffer) == 0) return std::string();
    return buffer;
}

// The bytes of one UTF-8 character that starts at `at`.
size_t charLength(const std::string& text, size_t at) {
    size_t n = 1;
    while (at + n < text.size() && ((unsigned char)text[at + n] & 0xC0) == 0x80) ++n;
    return n;
}

bool newerFirst(const LogFile& a, const LogFile& b) {
    if (a.modified != b.modified) return a.modified > b.modified;
    return a.path < b.path;
}

// Adds the file at `real` if it is a regular file, and not a link.
bool addIfFile(const std::string& path, const std::string& real, std::vector<LogFile>* out) {
    struct stat st;
    if (lstat(real.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    LogFile f;
    f.path = path;
    f.real = real;
    f.size = (uint64_t)st.st_size;
    f.modified = (int64_t)st.st_mtime;
    out->push_back(f);
    return true;
}

void newestFirst(std::vector<LogFile>* files) {
    std::sort(files->begin(), files->end(), newerFirst);
    if (files->size() > kMaxLogFiles) files->resize(kMaxLogFiles);
}

}  // namespace

bool matchName(const std::string& pattern, const std::string& name) {
    if (!name.empty() && name[0] == '.' && (pattern.empty() || pattern[0] != '.')) {
        // Only a pattern that names the dot itself, or has no wildcard at all, reaches a hidden file.
        if (pattern.find_first_of("*?") != std::string::npos) return false;
    }
    // The usual walk: on a mismatch, go back to the last * and let it take one more character.
    size_t p = 0, n = 0, starP = std::string::npos, starN = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starN = n;
        } else if (p < pattern.size() && pattern[p] == '?') {
            ++p;
            n += charLength(name, n);
        } else if (p < pattern.size() && pattern[p] == name[n]) {
            ++p;
            ++n;
        } else if (starP != std::string::npos) {
            p = starP + 1;
            starN += charLength(name, starN);
            n = starN;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool namesTmp(const Manifest& m) {
    for (size_t i = 0; i < m.logs.size(); ++i) {
        if (startsWith(m.logs[i], "${tmp}/")) return true;
    }
    return false;
}

void listPackageLogs(const Manifest& m, const Store& store, const Platform& platform, std::vector<LogFile>* out) {
    out->clear();
    // Where an absolute entry may not lead, whatever its text: the agent's own folders.
    const std::string agentPlaces[] = {platform.stateDir(), platform.stagingDir(), platform.runDir(), platform.agentHome()};
    std::vector<std::string> forbidden;
    for (size_t i = 0; i < sizeof agentPlaces / sizeof agentPlaces[0]; ++i) {
        std::string real = realPath(agentPlaces[i]);
        if (!real.empty()) forbidden.push_back(real);
    }
    std::set<std::string> seen;
    for (size_t e = 0; e < m.logs.size(); ++e) {
        const std::string& entry = m.logs[e];
        if (!validLogPath(entry)) continue;   // a record from before the field was checked
        // The entry's text on this machine, and the folder it starts from: the one that holds every package's
        // folder of that kind, or /tmp or /var.
        std::string full, anchor;
        bool absolute = false;
        if (startsWith(entry, "${app}/")) {
            std::string base = store.appDir(m);
            full = base + entry.substr(6);
            anchor = fs::parentOf(base);
        } else if (startsWith(entry, "${data}/")) {
            std::string base = platform.dataDir(m.id);
            full = base + entry.substr(7);
            anchor = fs::parentOf(base);
        } else if (startsWith(entry, "${tmp}/")) {
            std::string base = platform.tmpDir(m.id);
            full = base + entry.substr(6);
            anchor = fs::parentOf(base);
        } else {
            full = platform.systemPath(entry);
            anchor = platform.systemPath(entry.substr(0, 4));   // "/tmp" or "/var"
            absolute = true;
        }
        size_t slash = full.rfind('/');
        std::string folder = full.substr(0, slash), pattern = full.substr(slash + 1);
        std::string realAnchor = realPath(anchor), realFolder = realPath(folder);
        if (realAnchor.empty() || realFolder.empty() || folder.compare(0, anchor.size(), anchor) != 0) continue;
        // Nothing on the way from the anchor to the folder may be a link.
        if (realFolder != realAnchor + folder.substr(anchor.size())) continue;
        // A folder above the agent's own is fine: only the files directly in it are listed, and the agent's
        // folders are folders.
        bool inside = false;
        for (size_t i = 0; absolute && i < forbidden.size(); ++i) inside = inside || under(realFolder, forbidden[i]);
        if (inside) continue;
        std::string shown = entry.substr(0, entry.rfind('/') + 1);
        std::vector<std::string> names;
        if (pattern.find_first_of("*?") == std::string::npos) {
            names.push_back(pattern);
        } else if (!fs::listDir(realFolder, &names, 0)) {
            continue;
        }
        for (size_t i = 0; i < names.size(); ++i) {
            // A name that is not text could not be asked for again as it is listed.
            if (!validUtf8(names[i]) || !matchName(pattern, names[i])) continue;
            std::string real = realFolder + "/" + names[i];
            if (seen.count(real) != 0) continue;
            if (addIfFile(shown + names[i], real, out)) seen.insert(real);
        }
    }
    newestFirst(out);
}

void listAgentLogs(const std::string& ownLog, const Platform& platform, std::vector<LogFile>* out) {
    out->clear();
    if (!ownLog.empty()) {
        addIfFile("agent.log", ownLog, out);
        addIfFile("agent.log.1", ownLog + ".1", out);
        addIfFile("agent.log.2", ownLog + ".2", out);
    }
    addIfFile("trial.log", platform.agentHome() + "/log/trial.log", out);
    newestFirst(out);
}

bool openLogFile(const LogFile& file, int* fd, uint64_t* size, std::string* error) {
    struct stat named, opened;
    if (lstat(file.real.c_str(), &named) != 0 || !S_ISREG(named.st_mode)) {
        if (error != 0) *error = file.path + " is no longer there";
        return false;
    }
    // Not blocking: should a FIFO have taken the file's place since lstat, open must not wait for a writer.
    int f = open(file.real.c_str(), O_RDONLY | O_NONBLOCK);
    if (f < 0) {
        if (error != 0) *error = file.path + " cannot be opened: " + strerror(errno);
        return false;
    }
    if (fstat(f, &opened) != 0 || !S_ISREG(opened.st_mode) || opened.st_dev != named.st_dev || opened.st_ino != named.st_ino) {
        close(f);
        if (error != 0) *error = file.path + " changed while it was opened";
        return false;
    }
    fcntl(f, F_SETFL, fcntl(f, F_GETFL, 0) & ~O_NONBLOCK);
    *fd = f;
    *size = (uint64_t)opened.st_size;
    return true;
}

}  // namespace nbt
