// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/problem_log.cpp: the problem file of Bluetooth serial, and how little it is written.
//
// usage: test_problem_log [work folder]

#include "../../agent/problem_log.hpp"

#include <sys/stat.h>

#include "../../agent/fsutil.hpp"
#include "test_util.hpp"

using namespace nbt;

static size_t linesIn(const std::string& text) {
    size_t n = 0;
    for (size_t i = 0; i < text.size(); ++i) n += text[i] == '\n' ? 1 : 0;
    return n;
}

int main(int argc, char** argv) {
    std::string work = std::string(argc > 1 ? argv[1] : "build/agent-host") + "/work-problem-log";
    std::string why, path = work + "/serial-problems.log";
    CHECK_IN(why, fs::removeTree(work, &why) && fs::makeDirs(work, &why));
    fs::setSyncing(false);

    {   // a run with no problem: no file, not even after a flush
        ProblemLog log(path, "1.0.0 (build test)");
        CHECK(log.flush(&why) && fs::kindOf(path) == fs::kNone && log.recorded() == 0 && log.counted() == 0);
    }
    {   // the first of each kind, one short line each, with the time and the agent; the rest only counted
        ProblemLog log(path, "1.0.0 (build test)");
        log.note("refusal", "the car refused a phone's serial link");
        log.note("refusal", "the car refused a phone's serial link again");
        log.note("dial-BUSY", "phone 1: the car's dial answered 2 BUSY");
        CHECK(fs::kindOf(path) == fs::kNone);   // nothing before the flush
        CHECK(log.flush(&why) && log.recorded() == 2 && log.counted() == 1);
        std::string text = testutil::readFile(path);
        CHECK_IN(text, linesIn(text) == 2 && text.find("agent 1.0.0 (build test): the car refused a phone's serial link\n") != std::string::npos);
        CHECK(text.find("again") == std::string::npos && text.find("phone 1: the car's dial answered 2 BUSY") != std::string::npos);
        CHECK(text[4] == '-' && text[7] == '-' && text[13] == ':');   // 2026-11-18 12:34:56
        // A long text, or one that would break the line: one line, short, printable.
        log.note("reset", std::string(400, 'x') + "\nforged line");
        CHECK(log.flush(&why));
        text = testutil::readFile(path);
        CHECK(linesIn(text) == 3);
        std::string last = text.substr(text.rfind('\n', text.size() - 2) + 1);
        CHECK_IN(last, last.size() <= (size_t)ProblemLog::kMostLineBytes && last.find("...") != std::string::npos);
        // Flushing with nothing queued touches nothing.
        struct stat before, after;
        CHECK(stat(path.c_str(), &before) == 0 && log.flush(&why) && stat(path.c_str(), &after) == 0);
        CHECK(before.st_size == after.st_size && before.st_mtime == after.st_mtime && before.st_ino == after.st_ino);
    }
    {   // at most 16 lines a run, whatever the kinds; a later run adds its own
        ProblemLog log(path, "1.0.1 (build test)");
        for (int i = 0; i < 40; ++i) {
            char kind[16];
            snprintf(kind, sizeof kind, "kind-%d", i);
            log.note(kind, std::string("problem ") + kind);
        }
        CHECK(log.flush(&why) && log.recorded() == 16 && log.counted() == 24);
        std::string text = testutil::readFile(path);
        CHECK_IN(text, linesIn(text) == 3 + 16 && text.find("problem kind-15\n") != std::string::npos &&
                           text.find("problem kind-16") == std::string::npos);
    }
    {   // the file stays under 32 KB: a line that would take it over drops the older half, in one rewrite
        std::string text;
        for (int i = 0; i < 400; ++i) {
            char line[200];
            snprintf(line, sizeof line, "2026-01-01 00:00:00 agent 1.0.0 (build test): line %03d %s\n", i, std::string(60, '.').c_str());
            text = ProblemLog::withLine(text, line);
            CHECK(text.size() <= (size_t)ProblemLog::kMostBytes);
        }
        CHECK(text.find("line 399") != std::string::npos && text.find("line 000") == std::string::npos);
        CHECK(text.size() > (size_t)ProblemLog::kMostBytes / 4);
        // On disk: a full file and a run's line.
        std::string full;
        while (full.size() + 100 <= (size_t)ProblemLog::kMostBytes) full += std::string(99, 'o') + "\n";
        CHECK_IN(why, fs::writeFile(path, full, 0644, &why));
        ProblemLog log(path, "1.0.2 (build test)");
        log.note("reset", "serial.reset was asked for");
        CHECK(log.flush(&why));
        std::string now = testutil::readFile(path);
        CHECK(now.size() <= (size_t)ProblemLog::kMostBytes && now.size() < full.size() / 2 + 300);
        CHECK(now.find("serial.reset was asked for\n") == now.size() - 27);
        CHECK(fs::kindOf(fs::tempIn(work)) == fs::kNone);
    }
    {   // a file that cannot be written: the lines are counted, not lost without a word
        ProblemLog log(work + "/no-such-folder/serial-problems.log", "1.0.0 (build test)");
        log.note("refusal", "the car refused a phone's serial link");
        CHECK(!log.flush(&why) && !why.empty() && log.recorded() == 0 && log.counted() == 1);
    }
    fs::removeTree(work, 0);
    printf("test_problem_log: ok\n");
    return 0;
}
