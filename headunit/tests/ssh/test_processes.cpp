// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The relay's walk of the process list (ssh/relay/processes.hpp) on this machine's /proc: processes it starts in
// known places (a child that leads a session of its own, as a login shell does, with a grandchild in a group of its
// own in that session; a child in this process's own group and session) are found with their parent, group and
// session, and the two choices the relay makes from the list pick the right ones; each process's start time, and a
// child that has ended and is not collected. Every process it starts is ended before it exits. Prints one line per failure and a count; exits 1 on any failure.

#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "processes.hpp"

using namespace sshrelay;

namespace {

int failures = 0, checks = 0;

void expect(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        printf("FAIL  %s\n", what.c_str());
    }
}

bool has(const std::vector<long>& list, long value) {
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i] == value) return true;
    }
    return false;
}

const ProcessEntry* find(const std::vector<ProcessEntry>& list, long pid) {
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].pid == pid) return &list[i];
    }
    return 0;
}

void testParse() {
    ProcessEntry e;
    expect(parseStat("123 (sh) S 1 120 110 0 -1 4194560 100", &e) && e.pid == 123 && e.parent == 1 && e.group == 120 &&
               e.session == 110,
           "a plain stat line");
    expect(parseStat("77 (a) b) c (d) R 5 6 7 0", &e) && e.pid == 77 && e.parent == 5 && e.group == 6 && e.session == 7,
           "a name with spaces and parentheses");
    expect(!parseStat("", &e), "an empty text");
    expect(!parseStat("x (sh) S 1 2 3", &e), "a pid that is not a number");
    expect(!parseStat("12 (sh) S 1 2", &e), "a line cut short");
    expect(parseStat("123 (sh) S 1 120 110 0 -1 4194560 100", &e) && e.start == 0 && !e.zombie,
           "a line that ends before the start time");
    expect(parseStat("9 (x y) Z 1 9 9 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 1 0 987654 0 0", &e) && e.start == 987654 &&
               e.zombie && e.session == 9,
           "the start time, and a process that has ended");
}

}  // namespace

int main() {
    testParse();

    // The pids of the two the session leader starts come back through a pipe; each child then waits to be ended.
    int ready[2];
    if (pipe(ready) != 0) return 1;
    pid_t leader = fork();
    if (leader == 0) {
        setsid();
        pid_t inner = fork();
        if (inner == 0) {
            setpgid(0, 0);
            long self = (long)getpid();
            if (write(ready[1], &self, sizeof self) != (ssize_t)sizeof self) _exit(1);
            for (;;) pause();
        }
        for (;;) pause();
    }
    pid_t plain = fork();
    if (plain == 0) {
        for (;;) pause();
    }
    close(ready[1]);
    long inner = 0;
    bool started = leader > 0 && plain > 0 && read(ready[0], &inner, sizeof inner) == (ssize_t)sizeof inner;
    close(ready[0]);
    expect(started, "the test's processes started");

    if (started) {
        // The grandchild has written its pid after its setpgid; the leader's setsid came before its fork.
        std::vector<ProcessEntry> list;
        expect(listProcesses(&list), "the process list can be read");
        const ProcessEntry* self = find(list, (long)getpid());
        expect(self != 0 && self->parent == (long)getppid() && self->group == (long)getpgrp() &&
                   self->session == (long)getsid(0),
               "this process: its parent, group and session");
        const ProcessEntry* l = find(list, (long)leader);
        expect(l != 0 && l->parent == (long)getpid() && l->group == (long)leader && l->session == (long)leader,
               "the child that leads a session");
        const ProcessEntry* g = find(list, inner);
        expect(g != 0 && g->parent == (long)leader && g->group == inner && g->session == (long)leader,
               "the grandchild in a group of its own in that session");
        const ProcessEntry* p = find(list, (long)plain);
        expect(p != 0 && p->parent == (long)getpid() && p->group == (long)getpgrp() && p->session == (long)getsid(0),
               "the child in this process's group");

        std::vector<long> leaders = sessionLeadersStartedBy(list, (long)getpid());
        expect(leaders.size() == 1 && leaders[0] == (long)leader, "the one child that leads a session is found");
        expect(sessionLeadersStartedBy(list, (long)plain).empty(), "a process with no children leads none");
        std::vector<long> sessions;
        sessions.push_back((long)leader);
        std::vector<long> groups = groupsInSessions(list, sessions);
        expect(groups.size() == 2 && has(groups, (long)leader) && has(groups, inner),
               "the groups of that session: the leader's and the grandchild's");
        expect(!has(groups, (long)getpgrp()), "this process's group is not among them");
        sessions.push_back(0);
        sessions.push_back(1);
        groups = groupsInSessions(list, sessions);
        expect(!has(groups, 0) && !has(groups, 1) && groups.size() == 2, "sessions 0 and 1 give no group");

        // The start time, the same from the list and from one read, and later for a later process.
        ProcessEntry one;
        expect(self != 0 && self->start != 0 && !self->zombie, "this process has a start time and runs");
        expect(readProcess((long)getpid(), &one) && self != 0 && one.start == self->start && one.pid == (long)getpid(),
               "one read gives what the list gave");
        expect(l != 0 && self != 0 && l->start >= self->start, "a child started no earlier than its parent");
        expect(!readProcess(0, &one) && !readProcess(-5, &one), "no read of pid 0 or below");
    }

    // A child that has ended and is not collected yet.
    pid_t ended = fork();
    if (ended == 0) _exit(0);
    ProcessEntry z;
    bool seen = false;
    for (int i = 0; i < 200 && !seen; ++i) {
        seen = readProcess((long)ended, &z) && z.zombie;
        if (!seen) usleep(10000);
    }
    expect(seen, "a child that has ended is seen as ended");
    waitpid(ended, 0, 0);
    expect(!readProcess((long)ended, &z), "a collected child is not there");

    if (leader > 0) kill(-leader, SIGKILL);
    if (inner > 0) kill(-(pid_t)inner, SIGKILL);
    if (plain > 0) kill(plain, SIGKILL);
    if (leader > 0) waitpid(leader, 0, 0);
    if (plain > 0) waitpid(plain, 0, 0);
    printf("test_processes: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
