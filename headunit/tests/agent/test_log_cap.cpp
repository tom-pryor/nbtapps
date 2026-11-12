// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for capLog (agent/agent.hpp): an agent whose standard error goes to a file keeps that file short. The
// test sends its own standard error and standard output to a file, as the launcher does with the agent's, logs
// more than the file may hold, and then reads what is on disk.
//
// usage: test_log_cap [work folder]     (default build/agent-host/work)

#include "../../agent/agent.hpp"

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "../../agent/fsutil.hpp"
#include "test_util.hpp"

using namespace nbt;

static const uint64_t kCap = 2000;
static const size_t kLine = 80;      // more than any line here is long, the time and the name in front included

static std::string read(const std::string& path) {
    std::string data, why;
    CHECK_IN(why, fs::readFile(path, 1u << 20, &data, &why));
    return data;
}

static std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        CHECK(end != std::string::npos);      // every line is whole
        out.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

static std::string number(int n) {
    char text[16];
    snprintf(text, sizeof text, "%04d", n);
    return text;
}

// Standard error and standard output go to `path` until back() is called. A check that fails in between would
// say so into that file, so nothing is checked in between.
struct Redirect {
    int err, out;
    explicit Redirect(const std::string& path) {
        fflush(stdout);
        fflush(stderr);
        err = dup(STDERR_FILENO);
        out = dup(STDOUT_FILENO);
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        CHECK(err >= 0 && out >= 0 && fd >= 0);
        dup2(fd, STDERR_FILENO);
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    void back() {
        fflush(stdout);
        fflush(stderr);
        dup2(err, STDERR_FILENO);
        dup2(out, STDOUT_FILENO);
        close(err);
        close(out);
    }
};

int main(int argc, char** argv) {
    std::string work = std::string(argc > 1 ? argv[1] : "build/agent-host/work") + "/log-cap";
    std::string why;
    CHECK_IN(why, fs::removeTree(work, &why) && fs::makeDirs(work, &why));
    std::string path = work + "/agent.log", older = path + ".1";

    // 1. A name that is not the file standard error goes to is not taken, and nothing is kept short.
    CHECK(!capLog(path, kCap));                                    // no such file
    CHECK(fs::appendFile(path, "another program's\n", &why));
    CHECK(!capLog(path, kCap) && !capLog(work, kCap) && !capLog("", kCap));
    CHECK(fs::removeFile(path, &why));

    // 2. The file is set aside when it has passed its size, and the log goes on in a new one.
    CHECK(fs::appendFile(older, "the start before\n", &why));
    {
        Redirect to(path);
        bool refusedNoSize = !capLog(path, 0);
        bool taken = capLog(path, kCap);
        for (int i = 0; i < 100; ++i) {
            logLine(stderr, "line " + number(i));
            if (i % 10 == 0) {
                printf("printed %s\n", number(i).c_str());     // as a library would: not through logLine
                fflush(stdout);
            }
        }
        to.back();
        CHECK(refusedNoSize && taken);
    }
    std::string now = read(path), before = read(older);
    std::vector<std::string> nowLines = lines(now), beforeLines = lines(before);
    // Neither file is longer than the size and the line that took it past it.
    CHECK(now.size() <= kCap + kLine && before.size() > kCap && before.size() <= kCap + kLine);
    CHECK(!has(before, "the start before"));                       // the one that was there is written over
    CHECK(has(nowLines[0], "nbtagent: the log had passed 1 KB and was begun again; what it held is in " + older));
    CHECK(has(nowLines.back(), "nbtagent: line 0099"));
    // Nothing is lost between the two: the line after the last one set aside is the first one kept.
    std::string last = beforeLines.back().substr(beforeLines.back().size() - 4);
    CHECK(has(beforeLines.back(), "nbtagent: line ") && has(nowLines[1], "line " + number(atoi(last.c_str()) + 1)));
    // Standard output went to the new file with standard error.
    CHECK(has(now, "\nprinted 0090\n") && has(now, "line 0090\nprinted 0090\n"));
    std::vector<std::string> names;
    CHECK(fs::listDir(work, &names, &why) && names.size() == 2);

    // 3. A log that is not standard error is nobody's to keep short.
    {
        std::string other = work + "/other.log";
        FILE* log = fopen(other.c_str(), "w");
        CHECK(log != 0);
        for (int i = 0; i < 100; ++i) logLine(log, "line " + number(i));
        fclose(log);
        CHECK(lines(read(other)).size() == 100 && fs::kindOf(other + ".1") == fs::kNone);
    }

    // 4. The name leads to another file by now, as after a second launcher has renamed the logs: the file that
    //    standard error goes to is emptied where it is, and the other one is left alone.
    std::string second = work + "/second.log", moved = work + "/second.log.2";
    {
        Redirect to(second);
        bool taken = capLog(second, kCap);
        logLine(stderr, "line 0000");
        bool renamed = rename(second.c_str(), moved.c_str()) == 0;
        bool written = fs::appendFile(second, "another start's\n", 0);
        for (int i = 1; i < 100; ++i) logLine(stderr, "line " + number(i));
        to.back();
        CHECK(taken && renamed && written);
    }
    CHECK(read(second) == "another start's\n" && fs::kindOf(second + ".1") == fs::kNone);
    std::string emptied = read(moved);
    CHECK(emptied.size() <= kCap + kLine && has(lines(emptied)[0], "was begun again; what it held could not be kept"));
    CHECK(has(lines(emptied).back(), "nbtagent: line 0099") && !has(emptied, "line 0000"));

    printf("test_log_cap: ok\n");
    return 0;
}
