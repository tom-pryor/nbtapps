// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The process list: see processes.hpp.

#include "processes.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __QNXNTO__
#include <devctl.h>
#include <errno.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#endif

namespace sshrelay {

namespace {

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

bool contains(const std::vector<long>& list, long value) {
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i] == value) return true;
    }
    return false;
}

#ifdef __QNXNTO__
// One process's parent, group and session, from the process manager. The relay runs as root, which may open any
// process's address space for reading.
bool readEntry(const char* procDir, long pid, ProcessEntry* out) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%ld/as", procDir, pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    procfs_info info;
    memset(&info, 0, sizeof info);
    int r = devctl(fd, DCMD_PROC_INFO, &info, sizeof info, 0);
    close(fd);
    if (r != EOK) return false;
    out->pid = pid;
    out->parent = info.parent;
    out->group = info.pgrp;
    out->session = info.sid;
    out->start = info.start_time;
    out->zombie = (info.flags & _NTO_PF_ZOMBIE) != 0;
    return true;
}
#else
#ifdef NBT_SSHRELAY_TEST_HOOK
unsigned long long gStartOffset = 0;
#endif

bool readEntry(const char* procDir, long pid, ProcessEntry* out) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%ld/stat", procDir, pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    char text[1024];
    ssize_t n = read(fd, text, sizeof text - 1);
    close(fd);
    if (n <= 0) return false;
    if (!parseStat(std::string(text, (size_t)n), out) || out->pid != pid) return false;
#ifdef NBT_SSHRELAY_TEST_HOOK
    if (out->start != 0) out->start += gStartOffset;
#endif
    return true;
}
#endif

}  // namespace

#if defined(NBT_SSHRELAY_TEST_HOOK) && !defined(__QNXNTO__)
void setStartOffset(unsigned long long offset) { gStartOffset = offset; }
#endif

bool parseStat(const std::string& text, ProcessEntry* out) {
    // "pid (name) state parent group session ...": the name may hold spaces and parentheses, so the fields are
    // counted from its last ')'.
    size_t open = text.find(" (");
    size_t close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    long pid = 0;
    if (!isPid(text.substr(0, open).c_str(), &pid)) return false;
    long parent = 0, group = 0, session = 0;
    unsigned long long start = 0;
    char state = 0;
    // After the session: tty, its group, flags, four fault counts, four times, priority, nice, threads, the old
    // interval timer, and then the start time.
    int n = sscanf(text.c_str() + close + 1,
                   " %c %ld %ld %ld %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %llu", &state, &parent,
                   &group, &session, &start);
    if (n < 4) return false;
    out->pid = pid;
    out->parent = parent;
    out->group = group;
    out->session = session;
    out->start = n == 5 ? start : 0;
    out->zombie = state == 'Z';
    return true;
}

bool readProcess(long pid, ProcessEntry* out, const char* procDir) {
    if (pid <= 0) return false;
    return readEntry(procDir, pid, out);
}

bool listProcesses(std::vector<ProcessEntry>* out, const char* procDir) {
    out->clear();
    DIR* dir = opendir(procDir);
    if (dir == 0) return false;
    struct dirent* entry;
    while ((entry = readdir(dir)) != 0) {
        long pid = 0;
        if (!isPid(entry->d_name, &pid)) continue;
        ProcessEntry e;
        if (readEntry(procDir, pid, &e)) out->push_back(e);
    }
    closedir(dir);
    return true;
}

std::vector<long> sessionLeadersStartedBy(const std::vector<ProcessEntry>& list, long parent) {
    std::vector<long> leaders;
    for (size_t i = 0; i < list.size(); ++i) {
        const ProcessEntry& e = list[i];
        if (parent > 1 && e.parent == parent && e.session == e.pid && e.pid != parent) leaders.push_back(e.pid);
    }
    return leaders;
}

std::vector<long> groupsInSessions(const std::vector<ProcessEntry>& list, const std::vector<long>& sessions) {
    std::vector<long> groups;
    for (size_t i = 0; i < list.size(); ++i) {
        const ProcessEntry& e = list[i];
        if (e.group > 1 && e.session > 1 && contains(sessions, e.session) && !contains(groups, e.group)) {
            groups.push_back(e.group);
        }
    }
    return groups;
}

}  // namespace sshrelay
