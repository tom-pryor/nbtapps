// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for src/agent_start.hpp: the iDrive plugin's start of the agent's launcher with the head unit, and by
// hand from its menu. The record of starts as plain functions; then the starts on a folder that stands in for the
// agent's, with
// agent/nbtstart.sh as the starter the plugin starts, agent/nbtlaunch.sh as the launcher of the one version there,
// and a few lines of shell as the agent.
//
// usage: test_agent_start [work folder [starter script [launcher script [install script]]]]
//        (default build/agent-host/work, agent/nbtstart.sh, agent/nbtlaunch.sh, agent/hu_install.sh)

#include "../../src/agent_start.hpp"

#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "../../agent/fsutil.hpp"
#include "test_util.hpp"

using namespace nbt;
using namespace nbt::agentstart;

static void put(const std::string& path, const std::string& text, unsigned mode = 0644) {
    std::string why;
    CHECK_IN(why, fs::makeDirs(fs::parentOf(path), &why) && fs::writeFile(path, text, mode, &why));
}

static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

static std::string fileOrNothing(const std::string& path) {
    return fs::kindOf(path) == fs::kFile ? testutil::readFile(path) : std::string();
}

static bool waitFor(bool (*what)(const std::string&), const std::string& path, int ms) {
    for (int waited = 0; waited < ms; waited += 10) {
        if (what(path)) return true;
        usleep(10000);
    }
    return what(path);
}

static bool isThere(const std::string& path) { return fs::kindOf(path) != fs::kNone; }
static bool isGone(const std::string& path) { return fs::kindOf(path) == fs::kNone; }

static std::vector<int64_t> times(int64_t a, int64_t b = -1, int64_t c = -1, int64_t d = -1) {
    std::vector<int64_t> v;
    v.push_back(a);
    if (b >= 0) v.push_back(b);
    if (c >= 0) v.push_back(c);
    if (d >= 0) v.push_back(d);
    return v;
}

// Stands in for nbtagent: writes down that it was started and with what, runs for a moment, ends as stopped.
static const char kAgent[] =
    "#!/bin/sh\n"
    "top=${0%/*}/../..\n"
    "{ echo \"delay=$NBT_LAUNCH_DELAY\"; echo \"path=$PATH\"; echo \"leak=$NBT_TEST_LEAK\"; echo \"args=$#\"; } >> \"$top/started\"\n"
    "if read -r line; then echo \"stdin=$line\" >> \"$top/started\"; else echo 'stdin=none' >> \"$top/started\"; fi\n"
    "sleep 1\n"
    "exit 0\n";

int main(int argc, char** argv) {
    const std::string work = std::string(argc > 1 ? argv[1] : "build/agent-host/work") + "/agent-start";
    const std::string starterScript = argc > 2 ? argv[2] : "agent/nbtstart.sh";
    const std::string launcherScript = argc > 3 ? argv[3] : "agent/nbtlaunch.sh";
    const std::string installScript = argc > 4 ? argv[4] : "agent/hu_install.sh";
    std::string why;
    CHECK_IN(why, fs::removeTree(work, &why));

    // 1. The record's text: a time to a line, and anything else passed over.
    CHECK(parseStarts("").empty() && parseStarts("\n\n").empty());
    CHECK(parseStarts("315532800\n1790000000\n") == times(315532800, 1790000000));
    CHECK(parseStarts("1790000000") == times(1790000000));                          // no line end
    CHECK(parseStarts("12x\n-5\n 7\n1790000000\n1234567890123\n") == times(1790000000));
    CHECK(formatStarts(times(315532800, 1790000000)) == "315532800\n1790000000\n" && formatStarts(std::vector<int64_t>()).empty());

    // 2. Which starts count. With the clock set, the ones of the last quarter of an hour.
    const int64_t now = 1790000000;
    CHECK(clockSet(now) && !clockSet(315532800) && clockSet(kClockSetFrom) && !clockSet(kClockSetFrom - 1));
    CHECK(counted(times(now - 60, now - kWindowS + 1), now) == times(now - 60, now - kWindowS + 1));
    CHECK(counted(times(now - kWindowS, now - 3600), now).empty());
    CHECK(counted(times(now + 5), now).empty());                                    // from a clock that was put right since
    // One written down while the clock was not set has no age, and counts.
    CHECK(counted(times(315532800, now - 3600, now - 10), now) == times(315532800, now - 10));
    // With the clock not set now, every one counts.
    CHECK(counted(times(315532800, now - 3600, 315539999), 315540000) == times(315532800, now - 3600, 315539999));
    // The last kMostKept.
    std::vector<int64_t> many;
    for (int i = 0; i < 20; ++i) many.push_back(315532800 + i);
    CHECK(counted(many, 315540000).size() == kMostKept && counted(many, 315540000).back() == 315532819);
    CHECK(allowed(0) && allowed(2) && !allowed(3) && !allowed(8) && kMostStarts == 3);

    // 3. What the starter is started with on a head unit, and where the plugin looks: as the three scripts have it.
    Paths car;
    const std::string starterText = testutil::readFile(starterScript), launcherText = testutil::readFile(launcherScript);
    const std::string installText = testutil::readFile(installScript);
    CHECK(car.home == "/var/opt/nbtapp/agent" && has(installText, "AGENT_HOME=${NBT_TEST_AGENT_HOME:-" + car.home + "}"));
    CHECK(has(installText, std::string("echo on > $AGENT_HOME/") + kAutostartSwitch));
    // What the plugin starts is the starter, which the install puts there under the name the plugin knows.
    CHECK(has(installText, std::string("mv -f \"$AGENT_HOME/launcher.new\" \"$AGENT_HOME/") + kStarterInPlace + "\""));
    CHECK(has(installText, std::string("cp \"$SRC/") + kAgentStarter + "\" \"$AGENT_HOME/launcher.new\""));
    CHECK(car.lock == "/tmp/nbtapp/log/launcher.pid" && has(starterText, "LOG_DIR=${NBT_TEST_LAUNCH_RUN:-/tmp/nbtapp/log}") &&
          has(starterText, "LOCK=$LOG_DIR/launcher.pid"));
    CHECK(car.shell == "/bin/ksh" && starterText.compare(0, 11, "#!/bin/ksh\n") == 0 && launcherText.compare(0, 11, "#!/bin/ksh\n") == 0);
    // The starter has the same shell run a version's launcher, by the name the starter looks for in a version's folder.
    CHECK(has(starterText, "SH=${NBT_TEST_LAUNCH_SH:-" + car.shell + "}") &&
          has(starterText, std::string("\"$SH\" \"$AGENT_HOME/$VERSION/") + kAgentLauncher + "\""));
    std::vector<std::string> carVars = headUnitEnvironment(kDelayS);
    CHECK(carVars.size() == 5 && carVars[0].compare(0, 10, "PATH=/bin:") == 0 && carVars[4] == "NBT_LAUNCH_DELAY=15");
    // By hand there is no wait, which the launcher takes as none.
    std::vector<std::string> handCarVars = headUnitEnvironment(0);
    CHECK(handCarVars.size() == 5 && handCarVars[0] == carVars[0] && handCarVars[4] == "NBT_LAUNCH_DELAY=0");
    CHECK(has(launcherText, "if [ \"$DELAY\" -gt 0 ]; then"));
    CHECK(has(launcherText, "DELAY=${NBT_LAUNCH_DELAY:-0}"));

    // 4. The start, on a folder. What stands in for the head unit's environment has the launcher keep its log in
    // the folder and wait one second.
    Paths paths;
    paths.home = work + "/home";
    paths.lock = work + "/ram/log/launcher.pid";
    paths.shell = "/bin/sh";
    std::vector<std::string> vars;
    vars.push_back("PATH=/usr/bin:/bin");
    vars.push_back("NBT_TEST_LAUNCH_RUN=" + work + "/ram/log");
    vars.push_back("NBT_TEST_LAUNCH_SH=/bin/sh");
    vars.push_back("NBT_LAUNCH_DELAY=1");
    const std::string wanted = paths.home + "/" + kAutostartSwitch, record = paths.home + "/" + kAutostartBoots;
    const std::string started = work + "/started";
    setenv("NBT_TEST_LEAK", "the plugin's own", 1);

    // Nobody has asked: nothing is started, written or made, whatever else is there.
    Outcome o = startWithHeadUnit(paths, now, vars);
    CHECK_IN(o.line, o.decision == kNotAsked && o.pid < 0 && has(o.line, "the agent is not started with the head unit"));
    CHECK(fs::kindOf(paths.home) == fs::kNone);
    put(paths.home + "/current", "1.0.0\n");
    put(paths.home + "/1.0.0/nbtagent", kAgent, 0755);
    put(paths.home + "/1.0.0/nbtlaunch.sh", launcherText, 0755);
    o = startWithHeadUnit(paths, now, vars);
    CHECK(o.decision == kNotAsked && fs::kindOf(record) == fs::kNone);

    // Asked, and no launcher.
    put(wanted, "on\n");
    o = startWithHeadUnit(paths, now, vars);
    CHECK_IN(o.line, o.decision == kNoLauncher && has(o.line, "/launcher is not there") && fs::kindOf(record) == fs::kNone);

    // Asked, and a launcher: it is started, and the start is on record before that.
    put(paths.home + "/launcher", starterText, 0755);
    o = startWithHeadUnit(paths, now, vars);
    CHECK_IN(o.line, o.decision == kStarted && o.pid > 0 && has(o.line, "as start 1 of the 3"));
    CHECK(testutil::readFile(record) == "1790000000\n");
    // While the launcher waits, and while its agent runs, a second start finds it and leaves it alone: the HMI
    // was restarted. Nothing more goes on record.
    CHECK(waitFor(isThere, paths.lock, 3000));
    CHECK(holderOf(paths.lock) == o.pid && isRunning(o.pid));
    Outcome second = startWithHeadUnit(paths, now + 1, vars);
    CHECK_IN(second.line, second.decision == kRunning && has(second.line, "a launcher is running (process "));
    CHECK(fs::kindOf(started) == fs::kNone);                    // the launcher is still waiting
    CHECK(waitFor(isThere, started, 3000));
    second = startWithHeadUnit(paths, now + 2, vars);
    CHECK(second.decision == kRunning && testutil::readFile(record) == "1790000000\n");
    int status = -1;
    CHECK(waitpid((pid_t)o.pid, &status, 0) == (pid_t)o.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(waitFor(isGone, paths.lock, 1000));
    // What the agent was started with: the environment it was given and none of the plugin's, no arguments,
    // nothing to read, and the launcher's log where the environment said.
    const std::string saw = testutil::readFile(started);
    CHECK_IN(saw, saw == "delay=1\npath=/usr/bin:/bin\nleak=\nargs=0\nstdin=none\n");
    const std::string log = testutil::readFile(work + "/ram/log/agent.log");
    CHECK_IN(log, has(log, "launcher: waited 1 s before this start, as NBT_LAUNCH_DELAY says\n") &&
                      has(log, "launcher: starting version 1.0.0\n") && has(log, "launcher: the agent was stopped\n"));

    // A file whose launcher is gone does not keep a start off.
    put(paths.lock, "3999999\n");
    CHECK(holderOf(paths.lock) == 3999999 && !isRunning(3999999));
    put(paths.lock, "");
    CHECK(holderOf(paths.lock) == 0);
    put(paths.lock, "12 34\n");
    CHECK(holderOf(paths.lock) == 0 && !isRunning(0) && !isRunning(-1));
    CHECK(unlink(paths.lock.c_str()) == 0);

    // Each start that no agent outlasts by five minutes stays on record, and the fourth is not made.
    for (int i = 2; i <= 3; ++i) {
        o = startWithHeadUnit(paths, now + 10 * i, vars);
        CHECK_IN(o.line, o.decision == kStarted);
        CHECK(waitpid((pid_t)o.pid, &status, 0) == (pid_t)o.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    CHECK(testutil::readFile(record) == "1790000000\n1790000020\n1790000030\n");
    o = startWithHeadUnit(paths, now + 40, vars);
    CHECK_IN(o.line, o.decision == kTooManyStarts && o.pid < 0 && has(o.line, "NOT started: 3 starts with the head unit are on record"));
    CHECK(testutil::readFile(record) == "1790000000\n1790000020\n1790000030\n");
    // With the clock set, a start is off the record a quarter of an hour later.
    o = startWithHeadUnit(paths, now + kWindowS + 5, vars);
    CHECK_IN(o.line, o.decision == kStarted && has(o.line, "as start 3 of the 3"));
    CHECK(waitpid((pid_t)o.pid, &status, 0) == (pid_t)o.pid);
    CHECK(testutil::readFile(record) == "1790000020\n1790000030\n1790000905\n");
    // With the clock not set, none is.
    o = startWithHeadUnit(paths, 315532800, vars);
    CHECK(o.decision == kTooManyStarts);
    // The agent has run for five minutes, or somebody took the file away: started again, from one.
    CHECK(AgentHome(paths.home).forgetAutostarts());
    o = startWithHeadUnit(paths, 315532800, vars);
    CHECK_IN(o.line, o.decision == kStarted && has(o.line, "as start 1 of the 3"));
    CHECK(waitpid((pid_t)o.pid, &status, 0) == (pid_t)o.pid);
    CHECK(testutil::readFile(record) == "315532800\n");

    // A start that cannot be put on record is not made.
    CHECK(unlink(record.c_str()) == 0 && mkdir(record.c_str(), 0755) == 0);
    const std::string startsBefore = testutil::readFile(started);
    o = startWithHeadUnit(paths, now, vars);
    CHECK_IN(o.line, o.decision == kNotCounted && has(o.line, "NOT started: this start cannot be put on record"));
    CHECK(rmdir(record.c_str()) == 0);
    // A launcher that cannot be started is not on record as a start.
    Paths noShell = paths;
    noShell.shell = work + "/no-such-shell";
    o = startWithHeadUnit(noShell, now, vars);
    CHECK_IN(o.line, o.decision == kNotStarted && o.pid < 0 && has(o.line, "NOT started: cannot start "));
    CHECK(fs::kindOf(record) == fs::kNone);
    put(record, "1790000000\n");
    o = startWithHeadUnit(noShell, now + 5, vars);
    CHECK(o.decision == kNotStarted && testutil::readFile(record) == "1790000000\n");
    CHECK(testutil::readFile(started) == startsBefore && fileOrNothing(paths.lock).empty());

    // 5. The start by hand: with no autostart file, and with as many starts on record as keep a start with the head
    // unit off. The record is neither read nor written.
    CHECK(unlink(wanted.c_str()) == 0);
    put(record, "1790000000\n1790000020\n1790000030\n");
    CHECK(startWithHeadUnit(paths, now + 40, vars).decision == kNotAsked);
    Paths empty = paths;
    empty.home = work + "/empty";
    o = startByHand(empty, vars);
    CHECK_IN(o.line, o.decision == kNoLauncher && o.pid < 0 && has(o.line, "/launcher is not there"));
    o = startByHand(noShell, vars);
    CHECK_IN(o.line, o.decision == kNotStarted && o.pid < 0 && has(o.line, "NOT started by hand: cannot start "));
    std::vector<std::string> handVars = vars;
    handVars.back() = "NBT_LAUNCH_DELAY=0";
    o = startByHand(paths, handVars);
    CHECK_IN(o.line, o.decision == kStarted && o.pid > 0 && has(o.line, "started the launcher by hand (process "));
    // While its agent runs, a second press finds the launcher and leaves it alone.
    CHECK(waitFor(isThere, paths.lock, 3000));
    Outcome again = startByHand(paths, handVars);
    CHECK_IN(again.line, again.decision == kRunning && again.pid < 0 && has(again.line, "a launcher is running (process "));
    CHECK(waitpid((pid_t)o.pid, &status, 0) == (pid_t)o.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(waitFor(isGone, paths.lock, 1000));
    // The agent was started with no wait, and nothing of the record changed.
    const std::string sawByHand = testutil::readFile(started).substr(startsBefore.size());
    CHECK_IN(sawByHand, sawByHand == "delay=0\npath=/usr/bin:/bin\nleak=\nargs=0\nstdin=none\n");
    CHECK(!has(testutil::readFile(work + "/ram/log/agent.log"), "waited"));
    CHECK(testutil::readFile(record) == "1790000000\n1790000020\n1790000030\n" && fs::kindOf(wanted) == fs::kNone);

    CHECK_IN(why, fs::removeTree(work, &why));
    printf("test_agent_start: ok\n");
    return 0;
}
