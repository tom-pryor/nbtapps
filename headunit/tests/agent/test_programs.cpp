// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/programs.cpp: a program's name from a path, the three places a process's executable is read
// from, and whether a program is running, against a folder of processes that this test makes. All names and
// process numbers are invented. The last part asks this machine's own process list about the test itself.
//
// usage: test_programs [work folder]     (default build/agent-host/work/programs)

#include "../../agent/programs.hpp"

#include <unistd.h>

#include "../../agent/fsutil.hpp"
#include "test_util.hpp"

using namespace nbt;

static std::string root;

// A process as the head unit's system shows it: a text file with the path of its executable.
static void textProcess(const char* pid, const std::string& path) {
    std::string why;
    CHECK_IN(why, fs::makeDirs(root + "/" + pid, &why) && fs::writeFile(root + "/" + pid + "/exefile", path, 0644, &why));
}

// A process as Linux shows it: a link to its executable.
static void linkProcess(const char* pid, const std::string& path) {
    std::string why;
    CHECK_IN(why, fs::makeDirs(root + "/" + pid, &why) && fs::makeLink(path, root + "/" + pid + "/exe", &why));
}

// A process the system gives no executable for.
static void bareProcess(const char* pid) {
    std::string why;
    CHECK_IN(why, fs::makeDirs(root + "/" + pid, &why));
}

int main(int argc, char** argv) {
    root = argc > 1 ? argv[1] : "build/agent-host/work/programs";
    std::string why;
    CHECK_IN(why, fs::removeTree(root, &why) && fs::makeDirs(root, &why));
    const char* proc = root.c_str();

    // 1. A program's name from the path of its executable.
    CHECK(programName("/opt/thing/bin/SomeProgram") == "SomeProgram");
    CHECK(programName("/opt/thing/bin/SomeProgram\n") == "SomeProgram");      // as a text file gives it
    CHECK(programName("SomeProgram") == "SomeProgram" && programName("/opt/thing/bin/") == "" && programName(NULL) == "");

    // 2. No folder of processes, and one with no process in it: nothing is known.
    unsigned unnamed = 7;
    CHECK(programState("radiod", (root + "/none").c_str(), &unnamed) == kProgramUnknown && unnamed == 0);
    CHECK(programState("radiod", proc, &unnamed) == kProgramUnknown && unnamed == 0);
    // Entries that are not process numbers are not processes.
    CHECK_IN(why, fs::makeDirs(root + "/self", &why) && fs::writeFile(root + "/self/exefile", "/bin/radiod", 0644, &why));
    CHECK_IN(why, fs::makeDirs(root + "/12a", &why) && fs::writeFile(root + "/12a/exefile", "/bin/radiod", 0644, &why));
    CHECK_IN(why, fs::writeFile(root + "/version", "1", 0644, &why));
    CHECK(programState("radiod", proc) == kProgramUnknown);
    // Only processes without an executable: still nothing known, and they are counted.
    bareProcess("1");
    CHECK(programState("radiod", proc, &unnamed) == kProgramUnknown && unnamed == 1);

    // 3. The executable's path, from each place.
    textProcess("100", "/opt/thing/bin/SomeProgram\n");
    linkProcess("200", "/usr/bin/otherd");
    linkProcess("201", "/usr/bin/replaced (deleted)");
    CHECK(exePathOf(100, proc) == "/opt/thing/bin/SomeProgram\n" && exePathOf(200, proc) == "/usr/bin/otherd");
    CHECK(exePathOf(201, proc) == "/usr/bin/replaced");
    CHECK(exePathOf(1, proc) == "" && exePathOf(999, proc) == "");
    // The text file comes first where a process has both.
    textProcess("300", "/bin/first");
    linkProcess("300", "/bin/second");
    CHECK(exePathOf(300, proc) == "/bin/first");

    // 4. Running or not: the whole file name, in its own case, whatever folder it is in.
    CHECK(programState("SomeProgram", proc, &unnamed) == kProgramRunning);
    CHECK(programState("otherd", proc) == kProgramRunning && programState("replaced", proc) == kProgramRunning);
    CHECK(programState("first", proc) == kProgramRunning && programState("second", proc, &unnamed) == kProgramNotRunning);
    CHECK(unnamed == 1);                                                      // process 1, which could be anything
    CHECK(programState("someprogram", proc) == kProgramNotRunning && programState("SomeProgra", proc) == kProgramNotRunning);
    CHECK(programState("SomeProgram2", proc) == kProgramNotRunning && programState("bin", proc) == kProgramNotRunning);
    CHECK(programState("", proc) == kProgramNotRunning && programState("radiod", proc) == kProgramNotRunning);
    textProcess("400", "/elsewhere/radiod");
    CHECK(programState("radiod", proc) == kProgramRunning);
    CHECK_IN(why, fs::removeTree(root + "/400", &why));
    CHECK(programState("radiod", proc) == kProgramNotRunning);

    // 5. This machine's own list: the test finds itself, and not a program that does not exist.
    std::string self = programName(exePathOf((long)getpid()).c_str());
    CHECK(!self.empty());
    CHECK_IN(self, programState(self) == kProgramRunning);
    CHECK(programState("no-such-program-" + self) == kProgramNotRunning);

    CHECK_IN(why, fs::removeTree(root, &why));
    printf("test_programs: ok\n");
    return 0;
}
