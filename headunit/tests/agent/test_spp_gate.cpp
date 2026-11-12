// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/spp_gate.hpp: whom the gate in front of /dev/btspp0 refuses, the log line of an open, the
// fold of repeated lines, and the reading of a gate check. Program names and pids are invented, except the default
// one.

#include "../../agent/spp_gate.hpp"

#include <stdio.h>

#include <string>

#include "test_util.hpp"

using namespace nbt::btspp;

static GateOpen opened(long pid, const char* program, GateOpen::Kind kind, bool rd, bool wr, int answer) {
    GateOpen o;
    o.pid = pid;
    o.program = program;
    o.kind = kind;
    o.read = rd;
    o.write = wr;
    o.answer = answer;
    return o;
}

int main() {
    // 1. Modes.
    GateMode m = kGateOff;
    CHECK(parseGateMode("log", &m) && m == kGateLog);
    CHECK(parseGateMode("block", &m) && m == kGateBlock);
    m = kGateOff;
    CHECK(!parseGateMode("", &m) && !parseGateMode("Block", &m) && !parseGateMode("off", &m) && m == kGateOff);

    // 2. A program's name from the path of its executable.
    CHECK(programName("/opt/thing/bin/SomeProgram") == "SomeProgram");
    CHECK(programName("/opt/thing/bin/SomeProgram\n") == "SomeProgram");      // as a text file gives it
    CHECK(programName("/opt/thing/bin/SomeProgram \r\n") == "SomeProgram");
    CHECK(programName("SomeProgram") == "SomeProgram");
    CHECK(programName("./SomeProgram") == "SomeProgram");
    CHECK(programName("/opt/thing/bin/") == "");
    CHECK(programName("") == "" && programName(NULL) == "" && programName("\n") == "");

    // 3. Whom the gate refuses: in block mode the named program and nobody else. Never an unknown one.
    const std::string media = kGateRefuseDefault;
    CHECK(gateAnswer(kGateLog, media, media) == ENOENT);
    CHECK(gateAnswer(kGateLog, "other", media) == ENOENT);
    CHECK(gateAnswer(kGateBlock, media, media) == EACCES);
    CHECK(gateAnswer(kGateBlock, "other", media) == ENOENT);
    CHECK(gateAnswer(kGateBlock, "NBTMediaMainApp2", media) == ENOENT);       // the whole name, not its start
    CHECK(gateAnswer(kGateBlock, "nbtmediamainapp", media) == ENOENT);        // and in its own case
    CHECK(gateAnswer(kGateBlock, "", media) == ENOENT);                       // a name that could not be read
    CHECK(gateAnswer(kGateBlock, "", "") == ENOENT);
    CHECK(gateAnswer(kGateOff, media, media) == ENOENT);
    CHECK(gateAnswer(kGateBlock, programName("/x/y/NBTMediaMainApp\n"), media) == EACCES);

    // 3a. The hold: more programs, refused only while the hold is on, and only their opens. Their looks at the
    // path are passed on, and so is everybody else.
    const std::string other = "OtherProgram";
    const std::vector<std::string> one(1, other), none;
    std::vector<std::string> two(1, "Third");
    two.push_back(other);
    CHECK(gateHolds(kGateBlock, other, one, true, GateOpen::kOpen));
    CHECK(gateHolds(kGateBlock, other, one, true, GateOpen::kCombined));
    CHECK(!gateHolds(kGateBlock, other, one, true, GateOpen::kLook));
    CHECK(!gateHolds(kGateBlock, other, one, false, GateOpen::kOpen));         // the hold is off
    CHECK(!gateHolds(kGateLog, other, one, true, GateOpen::kOpen));            // log mode refuses nobody
    CHECK(!gateHolds(kGateOff, other, one, true, GateOpen::kOpen));
    CHECK(!gateHolds(kGateBlock, "another", one, true, GateOpen::kOpen));
    CHECK(!gateHolds(kGateBlock, "OtherProgram2", one, true, GateOpen::kOpen));
    CHECK(!gateHolds(kGateBlock, "", one, true, GateOpen::kOpen));             // a name that could not be read
    CHECK(!gateHolds(kGateBlock, other, none, true, GateOpen::kOpen));         // no program named for the hold
    CHECK(!gateHolds(kGateBlock, "", none, true, GateOpen::kOpen));
    CHECK(!gateHolds(kGateBlock, "", std::vector<std::string>(1, ""), true, GateOpen::kOpen));
    // Each of several programs, whatever its place in the list.
    CHECK(gateHolds(kGateBlock, other, two, true, GateOpen::kOpen) && gateHolds(kGateBlock, "Third", two, true, GateOpen::kOpen));
    CHECK(!gateHolds(kGateBlock, "another", two, true, GateOpen::kOpen) && !gateHolds(kGateBlock, "Third", two, true, GateOpen::kLook));
    CHECK(gateAnswer(kGateBlock, other, media) == ENOENT);                     // the hold is not gateAnswer's

    // 4. The log line.
    CHECK(describeOpen(opened(4711, "other", GateOpen::kOpen, true, true, ENOENT), kGateLog, media) ==
           "gate: open for read and write by other (pid 4711): passed on");
    CHECK(describeOpen(opened(4711, "other", GateOpen::kOpen, true, false, ENOENT), kGateBlock, media) ==
           "gate: open for read by other (pid 4711): passed on");
    CHECK(describeOpen(opened(4711, "other", GateOpen::kOpen, false, true, ENOENT), kGateBlock, media) ==
           "gate: open for write by other (pid 4711): passed on");
    CHECK(describeOpen(opened(12, "watcher", GateOpen::kLook, false, false, ENOENT), kGateLog, media) ==
           "gate: look (stat or access) by watcher (pid 12): passed on");
    CHECK(describeOpen(opened(12, "watcher", GateOpen::kCombined, true, false, ENOENT), kGateLog, media) ==
           "gate: open with a first request for read by watcher (pid 12): passed on");
    // In log mode the line says which opens the block mode would stop.
    CHECK(describeOpen(opened(99, media.c_str(), GateOpen::kOpen, true, true, ENOENT), kGateLog, media) ==
           "gate: open for read and write by NBTMediaMainApp (pid 99): passed on (--gate block refuses this one)");
    CHECK(describeOpen(opened(99, media.c_str(), GateOpen::kOpen, true, true, EACCES), kGateBlock, media) ==
           "gate: open for read and write by NBTMediaMainApp (pid 99): refused");
    CHECK(describeOpen(opened(7, "", GateOpen::kOpen, true, false, ENOENT), kGateBlock, media) ==
           "gate: open for read by a program whose name could not be read (pid 7): passed on");
    // A refusal by the hold says that it is for a time.
    GateOpen held = opened(31, "OtherProgram", GateOpen::kOpen, true, true, EACCES);
    CHECK(!held.held);
    held.held = true;
    CHECK(describeOpen(held, kGateBlock, media) ==
           "gate: open for read and write by OtherProgram (pid 31): refused for now (a link is being decided on, "
           "or is ours)");

    // 5. Repeated lines: the first is logged, the rest are counted until another line or a pause brings them out.
    RepeatFold f;
    unsigned rep = 9;
    CHECK(f.take() == 0);
    CHECK(f.next("a", &rep) && rep == 0);
    CHECK(!f.next("a", &rep) && rep == 0);
    CHECK(!f.next("a", &rep) && rep == 0);
    CHECK(f.next("b", &rep) && rep == 2);            // "a" came twice more before "b"
    CHECK(f.next("a", &rep) && rep == 0);            // not a run: a, b, a
    CHECK(!f.next("a", &rep));
    CHECK(f.take() == 1 && f.take() == 0);           // a pause reports the one held back
    CHECK(!f.next("a", &rep) && rep == 0);           // still the same line after the pause: counted again
    CHECK(f.next("c", &rep) && rep == 1);
    CHECK(f.next("", &rep) && rep == 0);             // an empty line is a line

    // 6. The exit code of a look, and what a check makes of it.
    CHECK(lookExitCode(0) == kLookThere && lookExitCode(ENOENT) == kLookNothing);
    CHECK(lookExitCode(EACCES) == kLookRefused && lookExitCode(EIO) == kLookOther && lookExitCode(EPERM) == kLookOther);
    CHECK(kLookThere != 2 && kLookNothing != 2 && kLookRefused != 2 && kLookOther != 2);   // 2 is a usage error

    CHECK(judgeGateCheck(true, kLookNothing) == kCheckFrontNoNode);    // attached, no link
    CHECK(judgeGateCheck(true, kLookThere) == kCheckFrontNode);        // attached, a link behind it
    CHECK(judgeGateCheck(true, kLookRefused) == kCheckRefused);        // the look itself is the refused program
    CHECK(judgeGateCheck(false, kLookThere) == kCheckBehind);          // the node took the look
    CHECK(judgeGateCheck(false, kLookNothing) == kCheckNotReached);
    CHECK(judgeGateCheck(false, kLookRefused) == kCheckUnclear);       // refused, and not by the gate
    CHECK(judgeGateCheck(true, kLookOther) == kCheckUnclear);
    CHECK(judgeGateCheck(true, 2) == kCheckUnclear && judgeGateCheck(false, -1) == kCheckUnclear);
    CHECK(!gateNotInFront(kCheckFrontNoNode) && !gateNotInFront(kCheckFrontNode) && !gateNotInFront(kCheckRefused));
    CHECK(gateNotInFront(kCheckBehind) && gateNotInFront(kCheckNotReached) && !gateNotInFront(kCheckUnclear));
    for (int c = kCheckFrontNoNode; c <= kCheckUnclear; ++c) CHECK(gateCheckText((GateCheck)c)[0] != 0);

    printf("test_spp_gate: ok\n");
    return 0;
}
