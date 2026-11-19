// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/programs.cpp: a program's name from a path, the three places a process's executable is read
// from, and whether a program is running, against a folder of processes that this test makes. All names and
// process numbers are invented. Also which processes run from a folder, with paths reported as QNX reports them,
// without their leading '/'. The last part asks this machine's own process list about the test itself.
//
// usage: test_programs [work folder]     (default build/agent-host/work/programs)

#include "../../agent/programs.hpp"

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include <vector>

#include "../../agent/fsutil.hpp"
#include "test_util.hpp"

using namespace nbt;

static std::string root;

// A process as a system with a text file for the path of its executable shows it. A path in it without a leading
// '/' stands for one as QNX reports it, which the head unit reads another way.
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

    // 5. The processes whose executable is in a folder: by the whole folder name, and below it, but not the
    // folder's own path or a folder whose name only starts the same.
    std::vector<RunningProgram> found;
    CHECK(programsUnder("/opt/thing", &found, proc, &unnamed) == kProgramRunning && found.size() == 1);
    CHECK(found[0].pid == 100 && found[0].path == "/opt/thing/bin/SomeProgram" && unnamed == 1);
    CHECK(programsUnder("/opt/thing/", &found, proc) == kProgramRunning && found.size() == 1);
    CHECK(programsUnder("/opt/thing/bin/SomeProgram", &found, proc) == kProgramNotRunning && found.empty());
    CHECK(programsUnder("/opt/thin", &found, proc) == kProgramNotRunning && found.empty());
    CHECK(programsUnder("/usr", &found, proc) == kProgramRunning && found.size() == 2 && found[0].pid + found[1].pid == 401);
    CHECK(programsUnder("/nowhere", &found, proc) == kProgramNotRunning);
    // And nothing is known where nothing could be named.
    CHECK(programsUnder("/opt/thing", &found, (root + "/none").c_str()) == kProgramUnknown && found.empty());

    // 6. A path that the system reports without its leading '/', as QNX does: from the root, by name and by folder.
    textProcess("500", "opt/other/bin/qprog\n");
    CHECK(exePathOf(500, proc) == "/opt/other/bin/qprog\n");
    CHECK(programState("qprog", proc) == kProgramRunning);
    CHECK(programsUnder("/opt/other", &found, proc) == kProgramRunning && found.size() == 1);
    CHECK(found[0].pid == 500 && found[0].path == "/opt/other/bin/qprog");
    CHECK(programsUnder("/opt", &found, proc) == kProgramRunning && found.size() == 2);
    CHECK(programsUnder("/opt/othe", &found, proc) == kProgramNotRunning);
    // One started by a relative path, which QNX reports as it was given: it stays relative, and is in no folder.
    textProcess("501", "./qprog");
    textProcess("502", "../bin/qprog");
    CHECK(exePathOf(501, proc) == "./qprog" && exePathOf(502, proc) == "../bin/qprog");
    CHECK(programsUnder("/opt/other", &found, proc) == kProgramRunning && found.size() == 1 && found[0].pid == 500);
    CHECK_IN(why, fs::removeTree(root + "/501", &why) && fs::removeTree(root + "/502", &why));
    // A folder given through a link, as the head unit's /tmp is one into a RAM disk: the process is found whether the
    // system reports the path through the link or the path it leads to.
    char resolved[PATH_MAX];
    CHECK(realpath(root.c_str(), resolved) != 0);
    std::string real = resolved;
    CHECK_IN(why, fs::makeDirs(root + "/ram/x/bin", &why) && fs::makeLink(real + "/ram", root + "/tmp", &why));
    textProcess("600", (real + "/ram/x/bin/held").substr(1));
    textProcess("601", (real + "/tmp/x/bin/given").substr(1));
    CHECK(programsUnder(real + "/tmp/x", &found, proc) == kProgramRunning && found.size() == 2);
    CHECK(found[0].pid + found[1].pid == 1201);
    CHECK(programsUnder(real + "/ram/x", &found, proc) == kProgramRunning && found.size() == 1 && found[0].pid == 600);
    CHECK_IN(why, fs::removeTree(root + "/600", &why) && fs::removeTree(root + "/601", &why));

    // 7. This machine's own list: the test finds itself, and not a program that does not exist.
    std::string self = programName(exePathOf((long)getpid()).c_str());
    CHECK(!self.empty());
    CHECK_IN(self, programState(self) == kProgramRunning);
    CHECK(programState("no-such-program-" + self) == kProgramNotRunning);
    // By its folder too, which may be given through a link.
    std::string exe = exePathOf((long)getpid());
    CHECK(programsUnder(fs::parentOf(exe), &found) == kProgramRunning);
    bool me = false;
    for (size_t i = 0; i < found.size(); ++i) me = me || found[i].pid == (long)getpid();
    CHECK(me);
    CHECK_IN(why, fs::makeLink(fs::parentOf(exe), root + "/through", &why));
    CHECK(programsUnder(root + "/through", &found) == kProgramRunning);

    CHECK_IN(why, fs::removeTree(root, &why));
    printf("test_programs: ok\n");
    return 0;
}
