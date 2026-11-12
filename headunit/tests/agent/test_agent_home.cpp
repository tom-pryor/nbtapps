// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/agent_home.cpp: the agent's own folder as the agent reads and writes it. The starter
// (agent/nbtstart.sh) reads the same files by the same rules; tests/e2e/run_launcher.py runs the two together.
//
// usage: test_agent_home [work folder]     (default build/agent-host/work)

#include "../../agent/agent_home.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "../../agent/fsutil.hpp"
#include "test_util.hpp"

using namespace nbt;

static void put(const std::string& path, const std::string& text, unsigned mode = 0644) {
    std::string why;
    CHECK_IN(why, fs::makeDirs(fs::parentOf(path), &why) && fs::writeFile(path, text, mode, &why));
}

static TrialState state(const char* current, const char* trial, const char* started, const char* self) {
    AgentHome::Files files;
    files.current = current;
    files.trial = trial != 0;
    files.started = started != 0;
    files.pending = started != 0 ? started : trial != 0 ? trial : "";
    return AgentHome::stateOf(files, self);
}

int main(int argc, char** argv) {
    std::string work = std::string(argc > 1 ? argv[1] : "build/agent-host/work") + "/agent-home";
    std::string why;
    CHECK_IN(why, fs::removeTree(work, &why));

    // 1. A version's name, as the launcher's name_of takes one.
    const char* good[] = {"1.0.0", "2.0.0-rc.1+b7", "0", "10.20.30+build.5", "1A.z"};
    for (size_t i = 0; i < sizeof good / sizeof good[0]; ++i) CHECK_IN(good[i], validVersionName(good[i]));
    const char* bad[] = {"", "v1.0.0", ".1", "-1", "1/0", "1.0.0/..", "../1.0.0", "1 0", "1.0.0\n", "1_0", "1\\0", "1:0", "log", "current"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) CHECK_IN(bad[i], !validVersionName(bad[i]));
    CHECK(validVersionName("1" + std::string(63, '0')) && !validVersionName("1" + std::string(64, '0')));

    // 2. What the files say to an agent of one version.
    CHECK(state("1.0.0", 0, 0, "1.0.0") == kNoTrial && state("", 0, 0, "1.0.0") == kNoTrial);
    CHECK(state("1.0.0", "1.1.0", 0, "1.1.0") == kOnTrial && state("1.0.0", 0, "1.1.0", "1.1.0") == kOnTrial);
    CHECK(state("", 0, "1.1.0", "1.1.0") == kOnTrial);                       // no current at all
    CHECK(state("1.1.0", 0, "1.1.0", "1.1.0") == kConfirmed && state("1.1.0", 0, "1.1.0", "1.0.0") == kConfirmed);
    CHECK(state("1.0.0", 0, "1.1.0", "1.0.0") == kWayBack && state("1.0.0", "1.1.0", 0, "1.0.0") == kWayBack);
    CHECK(state("1.0.0", 0, "", "1.0.0") == kWayBack);                       // a trial file that names nothing
    CHECK(state("1.0.0", 0, "1.1.0", "0.9.0") == kNotOurs && state("", 0, "", "1.0.0") == kNotOurs);

    // 3. A folder that is not there reads as three missing files, and nothing is made.
    AgentHome home(work + "/agent");
    AgentHome::Files files = home.read();
    CHECK(files.current.empty() && !files.trial && !files.started && files.pending.empty() && !files.any());
    CHECK(!home.hasProgram("1.0.0") && fs::kindOf(home.dir()) == fs::kNone);
    std::vector<std::string> versions;
    home.versions(&versions);
    CHECK(versions.empty());

    // 4. The files, read as the launcher reads them: the first line, with blanks around it dropped.
    put(home.dir() + "/current", "1.0.0\n");
    put(home.dir() + "/trial", "  1.1.0\t\nand a second line\n");
    files = home.read();
    CHECK(files.current == "1.0.0" && files.trial && !files.started && files.pending == "1.1.0");
    put(home.dir() + "/trial.started", "1.2.0");                            // no line end; and it goes before trial
    files = home.read();
    CHECK(files.started && files.pending == "1.2.0");
    // A file that names no version names nothing, and is still there.
    const char* nothing[] = {"", "\n", "../1.0.0\n", "1.0.0/nbtagent\n", "1.0.0 1.1.0\n", "\n1.0.0\n"};
    for (size_t i = 0; i < sizeof nothing / sizeof nothing[0]; ++i) {
        put(home.dir() + "/trial.started", nothing[i]);
        files = home.read();
        CHECK_IN(nothing[i], files.started && files.pending.empty() && files.any());
    }
    put(home.dir() + "/current", std::string(5000, '1') + "\n");            // not one short line
    CHECK(home.read().current.empty());
    // A folder of that name is not the file.
    CHECK(fs::removeFile(home.dir() + "/trial", &why) && fs::removeFile(home.dir() + "/trial.started", &why));
    CHECK(fs::makeDirs(home.dir() + "/trial", &why));
    files = home.read();
    CHECK(!files.trial && !files.any());
    CHECK(fs::removeTree(home.dir() + "/trial", &why));

    // 5. What the agent writes: whole files, each one line.
    CHECK_IN(why, home.writeCurrent("1.0.0", &why) && home.writeTrial("1.1.0", &why));
    CHECK(testutil::readFile(home.dir() + "/current") == "1.0.0\n" && testutil::readFile(home.dir() + "/trial") == "1.1.0\n");
    CHECK(fs::kindOf(fs::tempIn(home.dir())) == fs::kNone);
    CHECK(!home.writeCurrent("../x", &why) && !home.writeTrial("", &why) && testutil::readFile(home.dir() + "/current") == "1.0.0\n");
    CHECK(rename((home.dir() + "/trial").c_str(), (home.dir() + "/trial.started").c_str()) == 0);   // the launcher
    CHECK(home.read().started && AgentHome::stateOf(home.read(), "1.1.0") == kOnTrial);
    CHECK(home.writeTrial("1.3.0", &why) && home.removeTrial(&why) && !home.read().any());
    CHECK(home.removeTrial(&why));                                           // nothing there: nothing to do
    put(fs::tempIn(home.dir()), "1.1");                                      // from an agent that stopped mid-write
    home.removeTemp();
    home.removeTemp();
    CHECK(fs::kindOf(fs::tempIn(home.dir())) == fs::kNone);

    // 6. Versions: a folder to each, and in it the program and the version's launcher.
    put(home.dir() + "/1.0.0/nbtagent", "#!/bin/sh\n", 0755);
    CHECK(!home.hasProgram("1.0.0"));                                       // no launcher: the starter has nothing to run
    put(home.dir() + "/1.0.0/nbtlaunch.sh", "#!/bin/sh\n", 0755);
    put(home.dir() + "/1.1.0/nbtagent", "#!/bin/sh\n", 0644);               // there, and not a program
    put(home.dir() + "/1.1.0/nbtlaunch.sh", "#!/bin/sh\n", 0755);
    put(home.dir() + "/1.1.0/share/notes.txt", "x\n");
    put(home.dir() + "/log/trial.log", "what a try said\n");
    put(home.dir() + "/launcher", "#!/bin/sh\n", 0755);
    put(home.dir() + "/9.9.9", "a file, not a folder\n");
    CHECK(fs::makeDirs(home.dir() + "/2.0.0", &why));
    CHECK(home.hasProgram("1.0.0") && !home.hasProgram("1.1.0") && !home.hasProgram("2.0.0") && !home.hasProgram("3.0.0"));
    CHECK(!home.hasProgram("") && !home.hasProgram("../agent/1.0.0") && !home.hasProgram("log"));
    home.versions(&versions);
    CHECK(versions.size() == 3 && versions[0] == "1.0.0" && versions[1] == "1.1.0" && versions[2] == "2.0.0");

    // 7. Removing a version takes its folder and nothing beside it; a name that is not a version's is refused.
    CHECK(!home.removeVersion("log", &why) && !home.removeVersion("..", &why) && !home.removeVersion("", &why));
    CHECK(!home.removeVersion("1.0.0/../log", &why) && !home.removeVersion("launcher", &why));
    CHECK(home.removeVersion("1.1.0", &why) && home.removeVersion("1.1.0", &why) && home.removeVersion("4.0.0", &why));
    home.versions(&versions);
    CHECK(versions.size() == 2 && fs::kindOf(home.dir() + "/1.1.0") == fs::kNone);
    CHECK(testutil::readFile(home.dir() + "/log/trial.log") == "what a try said\n" && fs::kindOf(home.dir() + "/launcher") == fs::kFile);
    CHECK(home.hasProgram("1.0.0") && testutil::readFile(home.dir() + "/current") == "1.0.0\n");

    // 8. The record of starts with the head unit: removed when it is there, and with no record nothing in the
    // folder is touched. The file that asks for those starts is not the agent's to remove.
    struct stat before, after;
    CHECK(stat(home.dir().c_str(), &before) == 0);
    CHECK(!home.forgetAutostarts());
    CHECK(stat(home.dir().c_str(), &after) == 0 && before.st_mtime == after.st_mtime && before.st_ctime == after.st_ctime);
    put(home.dir() + "/" + kAutostartSwitch, "on\n");
    put(home.dir() + "/" + kAutostartBoots, "315532800\n315532900\n");
    CHECK(home.forgetAutostarts() && fs::kindOf(home.dir() + "/" + kAutostartBoots) == fs::kNone);
    CHECK(!home.forgetAutostarts() && fs::kindOf(home.dir() + "/" + kAutostartSwitch) == fs::kFile);
    home.versions(&versions);
    CHECK(versions.size() == 2 && !validVersionName(kAutostartSwitch) && !validVersionName(kAutostartBoots));
    CHECK(kAutostartSettledS == 300);

    // 9. The starter: the one a version came with, against the one in place.
    const std::string newer = "#!/bin/sh\n# the starter 1.0.0 came with\n";
    CHECK(home.starterOf("1.0.0") == AgentHome::kStarterNone && home.starterOf("4.0.0") == AgentHome::kStarterNone);
    CHECK(home.starterOf("../agent/1.0.0") == AgentHome::kStarterNone && !home.placeStarter("1.0.0", &why));
    put(home.dir() + "/1.0.0/nbtstart.sh", newer, 0644);
    CHECK(home.starterOf("1.0.0") == AgentHome::kStarterDiffers);          // `launcher` holds another one
    CHECK_IN(why, home.placeStarter("1.0.0", &why) && home.starterOf("1.0.0") == AgentHome::kStarterSame);
    struct stat placed;
    CHECK(testutil::readFile(home.dir() + "/launcher") == newer && stat((home.dir() + "/launcher").c_str(), &placed) == 0);
    CHECK((placed.st_mode & 0777) == 0755 && fs::kindOf(fs::tempIn(home.dir())) == fs::kNone);
    CHECK(fs::removeFile(home.dir() + "/launcher", &why) && home.starterOf("1.0.0") == AgentHome::kStarterDiffers);
    put(home.dir() + "/1.0.0/nbtstart.sh", "");                             // an empty file is no starter
    CHECK(home.starterOf("1.0.0") == AgentHome::kStarterNone && !home.placeStarter("1.0.0", &why));
    CHECK(!home.placeStarter("../agent/1.0.0", &why) && fs::kindOf(home.dir() + "/launcher") == fs::kNone);
    CHECK(!validVersionName(kStarterInPlace) && std::string(kAgentLauncher) == "nbtlaunch.sh" && std::string(kAgentStarter) == "nbtstart.sh");

    CHECK(fs::removeTree(work, &why));
    printf("test_agent_home: ok\n");
    return 0;
}
