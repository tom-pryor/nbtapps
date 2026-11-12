// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/serial_services.cpp: the file of serial services that no package declares, which services a
// sweep removes and which it leaves alone, and the two moments of an install step at which the engine has the
// list swept and read anew. The machine's processes and the car's Bluetooth service are played by the test. All
// names are invented.
//
// usage: test_serial_services [testdata folder] [work folder]     (default build/testdata and build/agent-host/work)

#include "../../agent/serial_services.hpp"

#include <unistd.h>

#include <map>

#include "../../agent/fsutil.hpp"
#include "../../agent/host_platform.hpp"
#include "../../agent/json.hpp"
#include "test_util.hpp"

using namespace nbt;
using testutil::readFile;

static std::string gData, gWork;

static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A head unit in a folder whose processes are what the test says: a program it has not been told of is not
// running.
class FakeMachine : public HostPlatform {
public:
    explicit FakeMachine(const std::string& root) : HostPlatform(root), asked(0) {}
    virtual ProgramState programState(const std::string& name) const {
        ++asked;
        std::map<std::string, ProgramState>::const_iterator it = programs.find(name);
        return it != programs.end() ? it->second : kProgramNotRunning;
    }
    std::map<std::string, ProgramState> programs;
    mutable unsigned asked;
};

// The car's Bluetooth service, as far as a removal goes.
class FakeRemover : public SerialServiceRemover {
public:
    FakeRemover() : takes(true) {}
    virtual bool removeSerialService(const std::string& name) {
        if (takes) removed += (removed.empty() ? "" : " ") + name;
        return takes;
    }
    // The removals asked for since the last look, in order.
    std::string took() {
        std::string s = removed;
        removed.clear();
        return s;
    }
    bool takes;
    std::string removed;
};

class Lines : public SerialLog {
public:
    virtual void line(const std::string& line) { text += line + "\n"; }
    std::string took() {
        std::string s = text;
        text.clear();
        return s;
    }
    std::string text;
};

static SerialServiceOwner owner(const char* name, const char* program, const char* from) {
    SerialServiceOwner o;
    o.name = name;
    o.program = program;
    o.from = from;
    return o;
}

// The declared services as "name=program(from) ...".
static std::string text(const std::vector<SerialServiceOwner>& owners) {
    std::string s;
    for (size_t i = 0; i < owners.size(); ++i) {
        s += (i == 0 ? "" : " ") + owners[i].name + "=" + owners[i].program + "(" + owners[i].from + ")";
    }
    return s;
}

struct Bench {
    std::string root;
    FakeMachine machine;
    Store store;
    Lines lines;
    FakeRemover remover;
    SerialServices services;

    explicit Bench(const std::string& name)
        : root(gWork + "/" + name), machine(root), store(&machine), services(&machine, &store, &lines) {
        std::string why;
        CHECK_IN(why, fs::removeTree(root, &why));
        CHECK_IN(why, machine.prepare(&why) && store.init(&why));
    }
    void file(const std::string& content) {
        std::string why;
        CHECK_IN(why, fs::writeFile(machine.stateDir() + "/serial-services", content, 0644, &why));
    }
};

static void testFile() {
    Bench b("file");
    std::vector<SerialServiceOwner> out;
    std::vector<std::string> complaints;
    std::string path = b.machine.stateDir() + "/serial-services";

    // No file is no service, and no complaint.
    readSerialServicesFile(path, &out, &complaints);
    CHECK(out.empty() && complaints.empty());

    b.file("# services of programs that no package installed\n"
           "\n"
           "TestRadio radiod\n"
           "  Other-Service\t other.d  \r\n"
           "#Hidden hiddend\n"
           "   # indented comment\n"
           "Last lastd");                                   // no line end after the last line
    readSerialServicesFile(path, &out, &complaints);
    CHECK_IN(text(out), text(out) == "TestRadio=radiod(serial-services) Other-Service=other.d(serial-services) "
                                     "Last=lastd(serial-services)");
    CHECK(complaints.empty());

    // What is not a service's name and a program's file name is passed over, and said.
    out.clear();
    b.file("OnlyAName\n"
           "Three words here\n"
           "Good goodd\n"
           "NBTApp agentd\n"                                 // the agent's own service is nobody else's
           "SPP card\n"                                      // nor is the car's
           "Radio bin/radiod\n"                              // a program with its folder
           "Two=Words radiod\n");
    readSerialServicesFile(path, &out, &complaints);
    CHECK_IN(text(out), text(out) == "Good=goodd(serial-services)");
    CHECK(complaints.size() == 6);
    CHECK_IN(complaints[0], has(complaints[0], "serial-services, line 1: not a service's name and its program's file name"));
    CHECK_IN(complaints[5], has(complaints[5], "line 7"));

    // reload() takes the file's services, and says what it passed over.
    b.services.reload();
    CHECK_IN(text(b.services.declared()), text(b.services.declared()) == "Good=goodd(serial-services)");
    CHECK(has(b.lines.took(), "line 2: not a service's name"));
    // A file that has gone is an empty list again.
    std::string why;
    CHECK_IN(why, fs::removeFile(path, &why));
    b.services.reload();
    CHECK(b.services.declared().empty() && b.lines.took().empty());
}

static void testSweep() {
    Bench b("sweep");
    std::vector<SerialServiceOwner> declared;
    declared.push_back(owner("TestRadio", "radiod", "org.nbtapp.radio"));
    declared.push_back(owner("TestRadio-2", "radiod", "org.nbtapp.radio"));
    declared.push_back(owner("Other", "otherd", "serial-services"));
    b.services.setDeclared(declared);
    CHECK(b.services.programs().size() == 2 && b.services.programs()[0] == "otherd" && b.services.programs()[1] == "radiod");

    // With nobody to remove a registration, nothing is done and nothing looked at.
    CHECK(b.services.sweep("a test") == 0 && b.machine.asked == 0 && b.lines.took().empty());

    // No program runs: every service is removed, each program looked for once.
    b.services.setRemover(&b.remover);
    CHECK(b.services.sweep("a test") == 3);
    CHECK_IN(b.remover.removed, b.remover.took() == "Other TestRadio TestRadio-2");
    CHECK(b.machine.asked == 2);
    std::string said = b.lines.took();
    CHECK_IN(said, has(said, "serial service 'TestRadio': its program (radiod) is not running, so a registration it left "
                             "behind is being removed (a test)"));

    // A program that runs keeps its services; the others go.
    b.machine.programs["radiod"] = kProgramRunning;
    CHECK(b.services.sweep("a test") == 1 && b.remover.took() == "Other");
    CHECK(!has(b.lines.took(), "TestRadio"));

    // A program the machine cannot speak for keeps its services too, and that is said.
    b.machine.programs["radiod"] = kProgramUnknown;
    b.machine.programs["otherd"] = kProgramRunning;
    CHECK(b.services.sweep("a test") == 0 && b.remover.took().empty());
    said = b.lines.took();
    CHECK_IN(said, has(said, "serial service 'TestRadio': whether its program (radiod) is running cannot be told, so it "
                             "is left alone") && has(said, "'TestRadio-2'") && !has(said, "'Other'"));

    // Two programs declared for one service: it goes only when neither runs, and stays when one cannot be told.
    b.machine.programs.clear();
    declared.clear();
    declared.push_back(owner("Shared", "firstd", "org.nbtapp.first"));
    declared.push_back(owner("Shared", "secondd", "serial-services"));
    b.services.setDeclared(declared);
    CHECK(b.services.sweep("a test") == 1 && b.remover.took() == "Shared");
    CHECK(has(b.lines.took(), "its program (firstd, secondd) is not running"));
    b.machine.programs["secondd"] = kProgramRunning;
    CHECK(b.services.sweep("a test") == 0 && b.remover.took().empty() && b.lines.took().empty());
    b.machine.programs["secondd"] = kProgramUnknown;
    CHECK(b.services.sweep("a test") == 0 && b.remover.took().empty());
    CHECK(has(b.lines.took(), "whether its program (secondd) is running cannot be told"));
    b.machine.programs["firstd"] = kProgramRunning;          // one runs and one cannot be told: answered for
    CHECK(b.services.sweep("a test") == 0 && b.lines.took().empty());

    // A removal the car's Bluetooth service cannot be asked for is not counted, and is said.
    b.machine.programs.clear();
    b.remover.takes = false;
    CHECK(b.services.sweep("a test") == 0);
    CHECK(has(b.lines.took(), "would be removed, but the car's Bluetooth service cannot be asked now"));
    b.remover.takes = true;

    // A sign that can come many times makes a sweep only so often.
    SerialServices fresh(&b.machine, &b.store, &b.lines);
    fresh.setDeclared(declared);
    fresh.setRemover(&b.remover);
    CHECK(fresh.sweepIfDue("a sign") == 1 && b.remover.took() == "Shared");
    CHECK(fresh.sweepIfDue("a sign") == 0 && b.remover.took().empty());
    CHECK(fresh.sweep("asked for outright") == 1 && b.remover.took() == "Shared");   // sweep() itself is not held back
    CHECK(fresh.sweepIfDue("a sign") == 0);

    // Nothing declared: nothing to do.
    b.services.setDeclared(std::vector<SerialServiceOwner>());
    b.lines.took();
    CHECK(b.services.sweep("a test") == 0 && b.remover.took().empty() && b.lines.took().empty());
}

// A step of the engine: the package's services are swept when its programs have been stopped, and the list is read
// anew when the step has ended.
static void testSteps(const TrustedKey& key) {
    Bench b("steps");
    std::vector<TrustedKey> keys(1, key);
    Engine engine(&b.machine, keys, "1.0.0");
    std::string why;
    CHECK_IN(why, engine.init(&why));
    b.file("Other otherd\n");
    b.services.setRemover(&b.remover);
    b.services.reload();
    engine.setStepObserver(&b.services);

    // An install: the new package's services, which nothing answers for yet. The file's service is not this
    // package's, and is not touched.
    StepReport r = engine.install(gData + "/good/org.nbtapp.radio-1.0.0.nbtpkg", "", 0);
    CHECK_IN(r.error.message, r.outcome == kStepDone);
    CHECK_IN(b.remover.removed, b.remover.took() == "TestRadio TestRadio-2");
    CHECK(has(b.lines.took(), "(a step has stopped the programs of org.nbtapp.radio)"));
    CHECK_IN(text(b.services.declared()),
             text(b.services.declared()) == "TestRadio=radiod(org.nbtapp.radio) TestRadio-2=radiod(org.nbtapp.radio) "
                                            "Other=otherd(serial-services)");

    // A step that is refused changes nothing, and still ends.
    r = engine.install(gData + "/good/org.nbtapp.radio2-1.0.0.nbtpkg", "", 0);
    CHECK(r.outcome == kStepFailed && r.error.code == "conflict" && b.remover.took().empty());
    CHECK(b.services.declared().size() == 3);

    // The whole list, as when the agent's own service has been registered: the program runs now.
    b.machine.programs["radiod"] = kProgramRunning;
    CHECK(b.services.sweep("a test") == 1 && b.remover.took() == "Other");

    // A removal whose program is still there: its services stay registered. The list no longer has them after.
    r = engine.remove("org.nbtapp.radio", true, 0);
    CHECK_IN(r.error.message, r.outcome == kStepDone && b.remover.took().empty());
    CHECK_IN(text(b.services.declared()), text(b.services.declared()) == "Other=otherd(serial-services)");

    // And one whose program has stopped.
    r = engine.install(gData + "/good/org.nbtapp.radio-1.0.0.nbtpkg", "", 0);
    CHECK(r.outcome == kStepDone && b.remover.took().empty());                      // radiod still counts as running
    b.machine.programs.clear();
    r = engine.remove("org.nbtapp.radio", true, 0);
    CHECK_IN(r.error.message, r.outcome == kStepDone);
    CHECK_IN(b.remover.removed, b.remover.took() == "TestRadio TestRadio-2");

    // An upgrade to a version that declares another service: what the installed version declared is swept as well.
    std::vector<SerialServiceOwner> declared;
    declared.push_back(owner("OldName", "oldd", "org.nbtapp.changed"));
    declared.push_back(owner("Other", "otherd", "serial-services"));
    b.services.setDeclared(declared);
    Manifest next;
    next.id = "org.nbtapp.changed";
    ManifestSerialService service;
    service.name = "NewName";
    service.program = "newd";
    next.serialServices.push_back(service);
    b.services.programsStopped(next);
    CHECK_IN(b.remover.removed, b.remover.took() == "NewName OldName");
    // A package with no service of its own, now or before: no sweep at all.
    b.lines.took();
    Manifest plain;
    plain.id = "org.nbtapp.plain";
    b.services.programsStopped(plain);
    CHECK(b.remover.took().empty() && b.lines.took().empty());

    // Without an observer the engine runs as before.
    engine.setStepObserver(0);
    r = engine.install(gData + "/good/org.nbtapp.radio-1.0.0.nbtpkg", "", 0);
    CHECK(r.outcome == kStepDone && b.remover.took().empty());
}

int main(int argc, char** argv) {
    char here[4096];
    CHECK(getcwd(here, sizeof here) != 0);
    gData = argc > 1 ? argv[1] : "build/testdata";
    gWork = std::string(argc > 2 ? argv[2] : "build/agent-host/work") + "/serial-services";
    if (gData[0] != '/') gData = std::string(here) + "/" + gData;
    if (gWork[0] != '/') gWork = std::string(here) + "/" + gWork;
    fs::setSyncing(false);

    json::Document vectors;
    std::string why;
    CHECK_IN(why, vectors.parse(readFile(gData + "/vectors.json"), &why));
    TrustedKey key = testutil::key(vectors.root().find("keys").find("package").string(), "test package key", true);

    testFile();
    testSweep();
    testSteps(key);

    printf("test_serial_services: ok\n");
    return 0;
}
