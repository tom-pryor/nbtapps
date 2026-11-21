// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Which programs are running: see programs.hpp.
#include "programs.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __QNXNTO__
#include <devctl.h>
#include <errno.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#endif

namespace nbt {

namespace {

// A process's entry in the folder of processes is its number.
bool isPid(const char* name, long* pid) {
    if (name[0] == '\0') return false;
    long value = 0;
    for (size_t i = 0; name[i] != '\0'; ++i) {
        if (name[i] < '0' || name[i] > '9' || i >= 10) return false;
        value = value * 10 + (name[i] - '0');
    }
    *pid = value;
    return true;
}

// A path the system reports, from the root: QNX gives the path a process was started by without its leading '/'
// ("bin/sh" for /bin/sh), and the folders it is compared with begin with one. A path that it gives as "./prog" or
// "../bin/prog" was a relative one, of a program started from a folder that is not known here, and stays as it is.
std::string fromRoot(const std::string& path) {
    if (path.empty() || path[0] == '/' || path.compare(0, 2, "./") == 0 || path.compare(0, 3, "../") == 0) return path;
    return "/" + path;
}

}  // namespace

std::string exePathOf(long pid, const char* procDir) {
    char p[PATH_MAX];
    char text[PATH_MAX + 1];
    snprintf(p, sizeof(p), "%s/%ld/exefile", procDir, pid);
    int fd = open(p, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, text, sizeof(text) - 1);
        close(fd);
        if (n > 0) {
            text[n] = 0;
            return fromRoot(text);
        }
    }
    snprintf(p, sizeof(p), "%s/%ld/exe", procDir, pid);
    ssize_t n = readlink(p, text, sizeof(text) - 1);
    if (n > 0) {
        std::string path(text, (size_t)n);
        // Linux says so when the file has been replaced or removed since the process started.
        const std::string gone = " (deleted)";
        if (path.size() > gone.size() && path.compare(path.size() - gone.size(), gone.size(), gone) == 0) {
            path.erase(path.size() - gone.size());
        }
        return fromRoot(path);
    }
#ifdef __QNXNTO__
    snprintf(p, sizeof(p), "%s/%ld/as", procDir, pid);
    fd = open(p, O_RDONLY);
    if (fd < 0) return std::string();
    struct {
        procfs_debuginfo info;
        char more[PATH_MAX];
    } map;
    memset(&map, 0, sizeof(map));
    int r = devctl(fd, DCMD_PROC_MAPDEBUG_BASE, &map, sizeof(map), NULL);
    close(fd);
    if (r != EOK) return std::string();
    map.more[sizeof(map.more) - 1] = 0;
    return fromRoot(map.info.path);
#else
    return std::string();
#endif
}

namespace {

// The interpreter a script's first line names, without its arguments: "#!/bin/sh -e" is /bin/sh. Empty for a file
// that is not a script, or names none by an absolute path.
std::string interpreterOf(const std::string& path) {
    char text[256];
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return std::string();
    ssize_t n = read(fd, text, sizeof(text));
    close(fd);
    if (n < 3 || text[0] != '#' || text[1] != '!') return std::string();
    size_t at = 2;
    while (at < (size_t)n && (text[at] == ' ' || text[at] == '\t')) ++at;
    size_t end = at;
    while (end < (size_t)n && text[end] != ' ' && text[end] != '\t' && text[end] != '\n' && text[end] != '\r') ++end;
    // A line longer than what was read names no interpreter that can be trusted.
    if (end == (size_t)n || end == at || text[at] != '/') return std::string();
    return std::string(text + at, end - at);
}

}  // namespace

bool runsProgram(long pid, const std::string& programPath, const char* procDir) {
    if (pid <= 0) return false;
    std::string running = programName(exePathOf(pid, procDir).c_str());
    if (running.empty()) return false;
    if (running == programName(programPath.c_str())) return true;
    std::string interpreter = interpreterOf(programPath);
    if (interpreter.empty()) return false;
    bool same = running == programName(interpreter.c_str());
    char real[PATH_MAX + 1];
    if (!same && realpath(interpreter.c_str(), real) != 0) same = running == programName(real);
    // The agent starts each program as the leader of a group of its own, which a script's shell stays.
    return same && getpgid((pid_t)pid) == (pid_t)pid;
}

ProgramState programState(const std::string& name, const char* procDir, unsigned* unnamed) {
    if (unnamed != 0) *unnamed = 0;
    DIR* dir = opendir(procDir);
    if (dir == 0) return kProgramUnknown;
    unsigned named = 0, noName = 0;
    bool found = false;
    struct dirent* entry;
    while (!found && (entry = readdir(dir)) != 0) {
        long pid = 0;
        if (!isPid(entry->d_name, &pid)) continue;
        std::string path = exePathOf(pid, procDir);
        if (path.empty()) {
            // Gone since it was listed, or a process the system gives no executable for.
            ++noName;
            continue;
        }
        ++named;
        found = programName(path.c_str()) == name;
    }
    closedir(dir);
    if (unnamed != 0) *unnamed = noName;
    if (found) return kProgramRunning;
    return named != 0 ? kProgramNotRunning : kProgramUnknown;
}

namespace {

// The path as the system reports it, without what ends the line of a text file.
std::string trimmedPath(const std::string& path) {
    size_t n = path.size();
    while (n > 0 && (path[n - 1] == '\n' || path[n - 1] == '\r' || path[n - 1] == ' ')) --n;
    return path.substr(0, n);
}

bool inside(const std::string& path, const std::string& dir) {
    return !dir.empty() && path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/';
}

}  // namespace

ProgramState programsUnder(const std::string& dir, std::vector<RunningProgram>* out, const char* procDir, unsigned* unnamed) {
    out->clear();
    if (unnamed != 0) *unnamed = 0;
    std::string given = dir;
    while (given.size() > 1 && given[given.size() - 1] == '/') given.erase(given.size() - 1);
    std::string real;
    char buffer[PATH_MAX + 1];
    if (realpath(given.c_str(), buffer) != 0) real = buffer;
    DIR* list = opendir(procDir);
    if (list == 0) return kProgramUnknown;
    unsigned named = 0, noName = 0;
    struct dirent* entry;
    while ((entry = readdir(list)) != 0) {
        long pid = 0;
        if (!isPid(entry->d_name, &pid)) continue;
        std::string path = trimmedPath(exePathOf(pid, procDir));
        if (path.empty()) {
            ++noName;
            continue;
        }
        ++named;
        if (inside(path, given) || (!real.empty() && inside(path, real))) {
            RunningProgram p;
            p.pid = pid;
            p.path = path;
            out->push_back(p);
        }
    }
    closedir(list);
    if (unnamed != 0) *unnamed = noName;
    if (!out->empty()) return kProgramRunning;
    return named != 0 ? kProgramNotRunning : kProgramUnknown;
}

}  // namespace nbt
