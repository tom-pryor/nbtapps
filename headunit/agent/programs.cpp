// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Which programs are running: see programs.hpp.
#include "programs.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
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
            return std::string(text);
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
        return path;
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
    return std::string(map.info.path);
#else
    return std::string();
#endif
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

}  // namespace nbt
