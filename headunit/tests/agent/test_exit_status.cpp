// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test: a version's launcher (agent/nbtlaunch.sh) and the starter (agent/nbtstart.sh) hold nbtagent's exit
// statuses as agent/exit_status.hpp has them. The launcher names all four; the starter the ones it goes by. What
// the two do with each is tests/e2e/run_launcher.py's to check.
//
// usage: test_exit_status [launcher script [starter script]]

#include "../../agent/exit_status.hpp"

#include <stdio.h>

#include <map>
#include <string>

#include "test_util.hpp"

using namespace nbt;

// The script's lines of the form EXIT_NAME=number, each name once.
static std::map<std::string, std::string> namesIn(const std::string& path) {
    std::string text = testutil::readFile(path);
    std::map<std::string, std::string> script;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        size_t equals = line.find('=');
        if (line.compare(0, 5, "EXIT_") != 0 || equals == std::string::npos) continue;
        std::string name = line.substr(0, equals);
        CHECK_IN(path + ": " + name + " is set twice", script.count(name) == 0);
        script[name] = line.substr(equals + 1);
    }
    return script;
}

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : "agent/nbtlaunch.sh";
    std::string starter = argc > 2 ? argv[2] : "agent/nbtstart.sh";
    std::map<std::string, std::string> script = namesIn(path);

    std::map<std::string, int> code;
    code["EXIT_STOPPED"] = kExitStopped;
    code["EXIT_USAGE"] = kExitUsage;
    code["EXIT_NO_RESTART"] = kExitNoRestart;
    code["EXIT_RESTART"] = kExitRestart;

    for (std::map<std::string, int>::const_iterator it = code.begin(); it != code.end(); ++it) {
        char number[16];
        snprintf(number, sizeof number, "%d", it->second);
        CHECK_IN(path + " has no " + it->first, script.count(it->first) != 0);
        CHECK_IN(it->first + ": the script says " + script[it->first] + ", agent/exit_status.hpp " + number,
                 script[it->first] == number);
    }
    for (std::map<std::string, std::string>::const_iterator it = script.begin(); it != script.end(); ++it) {
        CHECK_IN(it->first + " is in " + path + " and not in agent/exit_status.hpp", code.count(it->first) != 0);
    }

    // The starter: stopped and started again are what it tells apart, and it ends as an agent that a new start
    // would not help when another starter is running. Every name it holds is one of the four, with its number.
    std::map<std::string, std::string> second = namesIn(starter);
    CHECK_IN(starter, second.count("EXIT_STOPPED") != 0 && second.count("EXIT_RESTART") != 0 && second.count("EXIT_NO_RESTART") != 0);
    for (std::map<std::string, std::string>::const_iterator it = second.begin(); it != second.end(); ++it) {
        CHECK_IN(it->first + " is in " + starter + " and not in agent/exit_status.hpp", code.count(it->first) != 0);
        CHECK_IN(it->first + ": " + starter + " says " + it->second + ", " + path + " " + script[it->first], it->second == script[it->first]);
    }

    // A failure is every status the script does not name, so the named ones must leave kExitFailed out and differ.
    CHECK(kExitStopped == 0 && kExitFailed == 1);
    CHECK(kExitUsage != kExitFailed && kExitNoRestart != kExitFailed && kExitRestart != kExitFailed);
    CHECK(kExitUsage != kExitNoRestart && kExitUsage != kExitRestart && kExitNoRestart != kExitRestart);
    // A shell reports an end by a signal as 128 and up, and a program it could not start as 126 or 127.
    CHECK(kExitUsage < 126 && kExitNoRestart < 126 && kExitRestart < 126);

    printf("test_exit_status: ok (%u names)\n", (unsigned)code.size());
    return 0;
}
