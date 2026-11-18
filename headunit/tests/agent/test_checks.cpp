// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/checks.cpp: the compatibility checks, on a HostPlatform folder that stands in for a head
// unit, with files put there by hand as the checks expect to find them. The packages that are installed come from
// the test packages and go in through the engine. Every run of a check is held to the rule that it writes nothing:
// the folder is the same, file for file and time for time, before and after. The fixes are run on made-up hand
// installs: what they move and remove and what they leave, the order they do it in, a fix cut off at each of its
// points and run again, a file that came after the list, a link whose target is outside, and the programs they stop. The one
// process that is signalled is a copy of sleep that the test starts itself; the platform refuses to signal any
// other.
//
// usage: test_checks [testdata folder] [work folder]     (default build/testdata and build/agent-host/work)

#include "../../agent/checks.hpp"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../agent/agent_home.hpp"
#include "../../agent/engine.hpp"
#include "../../agent/fsutil.hpp"
#include "../../agent/host_platform.hpp"
#include "../../agent/json.hpp"
#include "test_util.hpp"

using namespace nbt;
using testutil::readFile;

static std::string gData, gWork;
static TrustedKey gPackageKey;

static std::string num(long long value) {
    char text[32];
    snprintf(text, sizeof text, "%lld", value);
    return text;
}

static void write(const std::string& path, const std::string& text, unsigned mode = 0644) {
    std::string why;
    CHECK_IN(why, fs::makeDirs(fs::parentOf(path), &why) && fs::writeFile(path, text, mode, &why));
}

static bool there(const std::string& path) { return fs::kindOf(path) != fs::kNone; }

// Every path under `dir`, with its kind, size and time: what a run that writes nothing leaves as it was.
static void snapshotOf(const std::string& dir, std::string* out) {
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0) return;
    *out += dir + " " + num((long long)st.st_mode) + " " + num((long long)st.st_size) + " " + num((long long)st.st_mtime) + "\n";
    if (!S_ISDIR(st.st_mode)) return;
    std::vector<std::string> names;
    CHECK(fs::listDir(dir, &names, 0));
    for (size_t i = 0; i < names.size(); ++i) snapshotOf(dir + "/" + names[i], out);
}

// The folder's platform, with the processes a test says are there. With `programs` below 0 it asks the machine,
// and then signals only a process this test started itself and named in `mine`: any other makes the fix fail.
class TestPlatform : public HostPlatform {
public:
    explicit TestPlatform(const std::string& root) : HostPlatform(root), programs(-1) {}

    virtual ProgramState programsUnder(const std::string& dir, std::vector<RunningProgram>* out) const {
        if (programs < 0) return HostPlatform::programsUnder(dir, out);
        out->clear();
        if (programs == 0) return kProgramNotRunning;
        if (programs == 2) return kProgramUnknown;
        RunningProgram p;
        p.pid = 999999;
        p.path = dir + "/bin/demod";
        out->push_back(p);
        return kProgramRunning;
    }
    virtual bool signalProcess(long pid, int number) {
        signals.push_back(num(pid) + (number == SIGTERM ? " TERM" : number == SIGKILL ? " KILL" : " other"));
        if (programs >= 0) return true;
        if (mine.count(pid) == 0) return false;
        return HostPlatform::signalProcess(pid, number);
    }

    int programs;   // below 0: the machine's; 0: none; 1: one that does not end; 2: they cannot be read
    std::set<long> mine;
    std::vector<std::string> signals;
};

// A head unit in a folder, with its engine.
struct Head {
    std::string root;
    TestPlatform platform;
    Engine engine;

    explicit Head(const std::string& name)
        : root(gWork + "/" + name), platform(root), engine(&platform, std::vector<TrustedKey>(1, gPackageKey), "1.4.0") {
        std::string why;
        CHECK_IN(why, fs::removeTree(root, &why));
        CHECK_IN(why, platform.prepare(&why) && engine.init(&why));
    }

    CheckContext context() {
        CheckContext c(&platform, &engine.store());
        c.stopWaitMs = 300;
        c.killWaitMs = 100;
        return c;
    }
    std::string sys(const std::string& path) const { return platform.systemPath(path); }
    std::string plugins() const { return platform.pluginsDir(); }

    void install(const char* name) {
        StepReport r = engine.install(gData + "/good/" + name + ".nbtpkg", "", 0);
        CHECK_IN(std::string(name) + ": " + r.error.message, r.outcome == kStepDone);
    }

    // Runs one check and holds it to writing nothing.
    CheckFinding run(const char* id, CheckContext* with = 0) {
        const Check* check = findCheck(id);
        CHECK_IN(id, check != 0);
        std::string before, after;
        snapshotOf(root, &before);
        CheckContext c = with != 0 ? *with : context();
        CheckFinding found;
        runCheck(*check, c, &found);
        snapshotOf(root, &after);
        CHECK_IN(std::string(id) + " wrote something", before == after);
        CHECK_IN(found.token, found.token == checkToken(id, found.entries, found.moves) && found.token.size() == 64);
        return found;
    }
};

// The entries of a finding, as "kind path" lines, with the head unit's folder left out of each path.
static std::string listed(const Head& h, const CheckFinding& f) {
    std::string text;
    for (size_t i = 0; i < f.entries.size(); ++i) {
        const char* kind = f.entries[i].kind == kEntryFile ? "f" : f.entries[i].kind == kEntryLink ? "l" : f.entries[i].kind == kEntryDir ? "d" : "o";
        std::string path = f.entries[i].path;
        if (path.compare(0, h.root.size(), h.root) == 0) path = path.substr(h.root.size());
        text += std::string(kind) + " " + path + "\n";
    }
    return text;
}

// ---- the table ----------------------------------------------------------------------------------------------------

static void testTable() {
    CHECK(checkCount() == 2);
    for (size_t i = 0; i < checkCount(); ++i) {
        const Check& c = checkAt(i);
        CHECK_IN(c.id, findCheck(c.id) == &c && c.title[0] != 0 && c.detail[0] != 0 && c.detect != 0);
        CHECK_IN(c.id, c.fix == kFixNone || (c.fixLabel[0] != 0 && c.fixDetail[0] != 0));
        std::string restart = c.restart;
        CHECK_IN(c.id, restart == "none" || restart == "hmi-restart" || restart == "required");
        CHECK_IN(c.id, c.stopUnder == 0 || (c.fix == kFixRemove && c.stopUnder[0] == '/'));
        for (size_t k = 0; k < i; ++k) CHECK_IN(c.id, strcmp(checkAt(k).id, c.id) != 0);
    }
    CHECK(findCheck("system-writable") == &checkAt(0) && findCheck("aa-fix") == &checkAt(1));
    CHECK(findCheck("no-such-check") == 0 && findCheck("") == 0 && findCheck("plugin-by-hand") == 0);
    // The token: the id and every path with its kind count, and the order the entries come in does not.
    std::vector<CheckEntry> one, two;
    one.push_back(CheckEntry("/a", kEntryDir));
    one.push_back(CheckEntry("/a/b", kEntryFile));
    two.push_back(one[1]);
    two.push_back(one[0]);
    CHECK(checkToken("x", one) == checkToken("x", two) && checkToken("x", one) != checkToken("y", one));
    two[0].kind = kEntryLink;
    CHECK(checkToken("x", one) != checkToken("x", two));
    CHECK(checkToken("x", std::vector<CheckEntry>()) != checkToken("x", one));
    // A move counts with both its ends.
    std::vector<CheckMove> moves(1, CheckMove("/p/Demo", "/a/Demo.old")), other(1, CheckMove("/p/Demo", "/a/Demo.nbtappsold"));
    CHECK(checkToken("x", one, moves) != checkToken("x", one) && checkToken("x", one, moves) != checkToken("x", one, other));
    CHECK(checkToken("x", std::vector<CheckEntry>(), moves) != checkToken("x", std::vector<CheckEntry>()));
}

// ---- system-writable ----------------------------------------------------------------------------------------------

static void testSystemWritable() {
    Head h("system-writable");
    // A folder has no system partition to ask about.
    CHECK(h.platform.systemWritable < 0 && h.run("system-writable").state == kCheckOk);
    h.platform.systemWritable = 0;
    CHECK(h.run("system-writable").state == kCheckOk);
    // Writable by somebody's hand: a problem with nothing to list, and the token of nothing.
    h.platform.systemWritable = 1;
    CheckFinding f = h.run("system-writable");
    CHECK(f.state == kCheckProblem && f.fixable && f.entries.empty() && f.token == checkToken("system-writable", std::vector<CheckEntry>()));
    // Writable because the agent made it so for a step: that is the step's, and is put back at its end.
    h.platform.systemWritable = 0;
    std::string why;
    CHECK(h.platform.makeWritable(h.plugins() + "/x", &why) && h.platform.remountedForStep());
    CHECK(h.run("system-writable").state == kCheckOk);
    h.platform.systemWritable = 1;
    CHECK(h.run("system-writable").state == kCheckOk);
    CHECK(h.platform.restoreReadOnly(&why) && h.run("system-writable").state == kCheckProblem);
    // While a step runs, nothing can be said; nor where the partition cannot be asked.
    CheckContext during = h.context();
    during.stepRunning = true;
    f = h.run("system-writable", &during);
    CHECK_IN(f.why, f.state == kCheckUnknown && !f.fixable && !f.why.empty());
    h.platform.systemWritable = 2;
    f = h.run("system-writable");
    CHECK_IN(f.why, f.state == kCheckUnknown && f.why.find("cannot") != std::string::npos);
}

// ---- aa-fix -------------------------------------------------------------------------------------------------------

static std::string pluginManifest(int connector, const char* name) {
    return std::string("<?xml version=\"1.0\"?>\n<plugin><name>") + name + "</name><config><dependencies><pluginAppConnector>"
           "<id>" + num(connector) + "</id></pluginAppConnector></dependencies><parameters><DescFile>{pluginDir}/" + name +
           ".xml</DescFile></parameters></config></plugin>\n";
}

// A made-up app put on by hand: a program and its settings in /opt/aa, a link in it to a file outside it, its data
// in /var/opt/aa, and its plugin, Demo, with the connector id 210. What is outside it: a file the link leads to.
static void handInstall(Head& h) {
    std::string app = h.sys("/opt/aa"), data = h.sys("/var/opt/aa"), plugin = h.plugins() + "/Demo";
    write(app + "/bin/demod", "#!/bin/sh\nsleep 600\n", 0755);
    write(app + "/etc/demo.conf", "mode=1\n");
    write(h.root + "/outside/target.txt", "not the app's\n");
    std::string why;
    CHECK_IN(why, fs::makeDirs(app + "/lib", &why) && fs::makeLink(h.root + "/outside/target.txt", app + "/lib/libdemo.so", &why));
    write(data + "/state.txt", "state\n");
    write(plugin + "/Demo.manifest", pluginManifest(210, "Demo"));
    write(plugin + "/Demo.xml", "<pluginApps><pluginApp entryButtonFocusID=\"32700\"/></pluginApps>\n");
    write(plugin + "/libDemo.so", "library\n");
}

// What the check lists of it to remove: not the plugin, which is moved, and not /opt/aa, which it is moved into.
static const char kHandListing[] =
    "d /system/opt/aa/bin\n"
    "f /system/opt/aa/bin/demod\n"
    "d /system/opt/aa/etc\n"
    "f /system/opt/aa/etc/demo.conf\n"
    "d /system/opt/aa/lib\n"
    "l /system/opt/aa/lib/libdemo.so\n"
    "d /system/var/opt/aa\n"
    "f /system/var/opt/aa/state.txt\n";

// The moves of a finding, as "FROM -> TO" lines, with the head unit's folder left out of each path.
static std::string movesOf(const Head& h, const CheckFinding& f) {
    std::string text;
    for (size_t i = 0; i < f.moves.size(); ++i) {
        std::string from = f.moves[i].from, to = f.moves[i].to;
        if (from.compare(0, h.root.size(), h.root) == 0) from = from.substr(h.root.size());
        if (to.compare(0, h.root.size(), h.root) == 0) to = to.substr(h.root.size());
        text += from + " -> " + to + "\n";
    }
    return text;
}

// Adds paths to the record of an installed package, as though its install had placed them.
static void own(Head& h, const char* id, const std::string& file, const std::string& link) {
    InstalledPackage p;
    std::string why;
    CHECK_IN(why, h.engine.store().load(id, &p, &why));
    if (!file.empty()) {
        InstalledFile f;
        f.dest = file;
        f.sha256 = std::string(64, '0');
        p.files.push_back(f);
    }
    if (!link.empty()) {
        InstalledLink l;
        l.path = link;
        l.target = "elsewhere";
        p.links.push_back(l);
    }
    CHECK_IN(why, fs::writeFile(h.engine.store().recordDir(id) + "/record.json", Store::encodeRecord(p), 0644, &why));
}

static void testHandApp() {
    Head h("aa-fix");
    // Nothing there, and the folder of its data alone: nothing to say.
    CHECK(h.run("aa-fix").state == kCheckOk);
    write(h.sys("/var/opt/aa/state.txt"), "state\n");
    CHECK(h.run("aa-fix").state == kCheckOk);

    handInstall(h);
    // Another plugin, with another id, and one whose manifest is not named after its folder: neither is the app's.
    write(h.plugins() + "/Other/Other.manifest", pluginManifest(250, "Other"));
    write(h.plugins() + "/Misnamed/Demo.manifest", pluginManifest(210, "Demo"));
    CheckFinding f = h.run("aa-fix");
    CHECK_IN(listed(h, f), f.state == kCheckProblem && f.fixable && f.why.empty() && listed(h, f) == kHandListing);
    CHECK_IN(movesOf(h, f), movesOf(h, f) == "/plugins/Demo -> /system/opt/aa/Demo.old\n");
    // The link is listed as a link, and what it leads to is not listed.
    CHECK(f.token == h.run("aa-fix").token);
    // A folder that a fix parked there before, or that looks so, is neither listed nor in the way: the move takes
    // the other name. Where both names are taken there is no fix.
    write(h.sys("/opt/aa/Demo.old/Demo.manifest"), pluginManifest(210, "Demo"));
    write(h.sys("/opt/aa/Other.nbtappsold/x"), "x\n");
    f = h.run("aa-fix");
    CHECK_IN(listed(h, f), f.fixable && listed(h, f) == kHandListing);
    CHECK_IN(movesOf(h, f), movesOf(h, f) == "/plugins/Demo -> /system/opt/aa/Demo.nbtappsold\n");
    write(h.sys("/opt/aa/Demo.nbtappsold"), "a file\n");
    f = h.run("aa-fix");
    CHECK_IN(f.why, f.state == kCheckProblem && !f.fixable && f.why.find("both are taken") != std::string::npos);
    // A file named so is not parked: it is listed with the rest.
    CHECK_IN(listed(h, f), listed(h, f).find("f /system/opt/aa/Demo.nbtappsold\n") != std::string::npos);

    // The plugin alone is enough; /opt/aa is not there yet, and the move makes it.
    Head p("aa-fix-plugin");
    write(p.plugins() + "/Demo/Demo.manifest", pluginManifest(210, "Demo"));
    f = p.run("aa-fix");
    CHECK_IN(listed(p, f), f.state == kCheckProblem && f.fixable && f.entries.empty() &&
                               movesOf(p, f) == "/plugins/Demo -> /system/opt/aa/Demo.old\n");
    // A plugin folder in another folder the HMI loads from counts too.
    p.platform.morePluginDirs.push_back(p.root + "/stock-plugins");
    write(p.root + "/stock-plugins/Demo2/Demo2.manifest", pluginManifest(210, "Demo2"));
    f = p.run("aa-fix");
    CHECK_IN(movesOf(p, f), f.moves.size() == 2 && movesOf(p, f).find("/stock-plugins/Demo2 -> /system/opt/aa/Demo2.old\n") != std::string::npos);
    // And so does ${var_plugins}, the HMI's plugin folder on the writable partition, where apps are copied by hand.
    // A plugin there goes into the folder of the app's data, which on a car is on that partition and /opt/aa is not.
    write(p.platform.varPluginsDir() + "/Demo3/Demo3.manifest", pluginManifest(210, "Demo3"));
    f = p.run("aa-fix");
    CHECK_IN(movesOf(p, f), f.fixable && f.moves.size() == 3 &&
                                movesOf(p, f).find("/var-plugins/Demo3 -> /system/var/opt/aa/Demo3.old\n") != std::string::npos);
    // Two folders of one name go to the two names.
    write(p.root + "/stock-plugins/Demo/Demo.manifest", pluginManifest(210, "Demo"));
    f = p.run("aa-fix");
    CHECK_IN(movesOf(p, f), f.fixable && movesOf(p, f).find("/system/opt/aa/Demo.old\n") != std::string::npos &&
                                movesOf(p, f).find("/system/opt/aa/Demo.nbtappsold\n") != std::string::npos);

    // What an installed package's record owns is never listed: a file and a link in /opt/aa, and the plugin
    // folder in which the package owns a file is not the app's.
    Head o("aa-fix-owned");
    o.install("org.nbtapp.runtime-2.1.0");
    handInstall(o);
    own(o, "org.nbtapp.runtime", "/opt/aa/etc/demo.conf", "/opt/aa/lib/libdemo.so");
    f = o.run("aa-fix");
    CHECK_IN(listed(o, f), f.state == kCheckProblem && listed(o, f).find("demo.conf") == std::string::npos &&
                               listed(o, f).find("libdemo.so") == std::string::npos && listed(o, f).find("bin/demod") != std::string::npos);
    own(o, "org.nbtapp.runtime", "${plugins}/Demo/Demo.xml", "");
    f = o.run("aa-fix");
    CHECK_IN(movesOf(o, f), f.moves.empty() && listed(o, f).find("d /system/opt/aa\n") == 0);

    // /opt/aa that is a link: the link, and nothing it leads to. With a plugin to move into it there is no fix.
    Head l("aa-fix-link");
    write(l.root + "/elsewhere/bin/demod", "program\n");
    std::string why;
    CHECK_IN(why, fs::makeDirs(l.sys("/opt"), &why) && fs::makeLink(l.root + "/elsewhere", l.sys("/opt/aa"), &why));
    f = l.run("aa-fix");
    CHECK_IN(listed(l, f), f.state == kCheckProblem && f.fixable && listed(l, f) == "l /system/opt/aa\n");
    write(l.plugins() + "/Demo/Demo.manifest", pluginManifest(210, "Demo"));
    f = l.run("aa-fix");
    CHECK_IN(f.why, f.state == kCheckProblem && !f.fixable && f.why.find("not a folder") != std::string::npos);
    // A link that is named as a parked folder is not one.
    Head k("aa-fix-parked-link");
    CHECK_IN(why, fs::makeDirs(k.sys("/opt/aa"), &why) && fs::makeLink(k.root, k.sys("/opt/aa/Demo.old"), &why));
    f = k.run("aa-fix");
    CHECK_IN(listed(k, f), listed(k, f) == "d /system/opt/aa\nl /system/opt/aa/Demo.old\n");
    // /opt/aa that holds a parked folder and nothing else: nothing to say.
    Head q("aa-fix-parked");
    write(q.sys("/opt/aa/Demo.old/Demo.manifest"), pluginManifest(210, "Demo"));
    CHECK(q.run("aa-fix").state == kCheckOk);
}

// The bounds of a listing, and what it never enters.
static void testBounds() {
    // Four levels below the root, with an empty folder at the fourth: within bounds.
    Head d("bounds-deep");
    std::string app = d.sys("/opt/aa");
    write(app + "/1/2/3/file", "x\n");
    CHECK_IN(app, fs::makeDirs(app + "/1/2/3/4", 0));
    CheckFinding f = d.run("aa-fix");
    CHECK_IN(listed(d, f), f.state == kCheckProblem && f.fixable && f.entries.size() == 6);
    // Something at the fifth: too much to remove safely, and no fix.
    write(app + "/1/2/3/4/5", "x\n");
    f = d.run("aa-fix");
    CHECK_IN(f.why, f.state == kCheckProblem && !f.fixable && f.why.find("too much here to remove safely") != std::string::npos);
    CHECK(listed(d, f).find("/4/5") == std::string::npos);

    // 256 entries are listed; one more, and there is no fix.
    Head m("bounds-many");
    app = m.sys("/opt/aa");
    for (int i = 0; i < 254; ++i) write(app + "/many/" + num(1000 + i), "x\n");
    f = m.run("aa-fix");
    CHECK_IN(num((long long)f.entries.size()), f.fixable && f.entries.size() == 256);
    write(app + "/many/2000", "x\n");
    f = m.run("aa-fix");
    CHECK_IN(f.why, !f.fixable && f.entries.size() == kCheckMaxEntries && f.why.find("more than 256") != std::string::npos);

    // A name that is not text: listed, shown with what cannot be shown replaced, and no fix.
    Head n("bounds-name");
    write(n.sys("/opt/aa/bad\nname"), "x\n");
    f = n.run("aa-fix");
    CHECK_IN(f.why, f.state == kCheckProblem && !f.fixable && f.entries.size() == 2);
    CHECK(shownPath("/a\nb\xff") == "/a?b\xEF\xBF\xBD" && shownPath("/opt/aa/x") == "/opt/aa/x");

    // Another file system is not entered, and a folder deep enough is only looked into.
    CHECK(folderStep(7, 7, 0) == kFolderEnter && folderStep(7, 7, kCheckMaxDepth - 1) == kFolderEnter);
    CHECK(folderStep(7, 7, kCheckMaxDepth) == kFolderLast && folderStep(7, 7, kCheckMaxDepth + 3) == kFolderLast);
    CHECK(folderStep(7, 8, 1) == kFolderOtherDevice && folderStep(7, 8, kCheckMaxDepth) == kFolderOtherDevice);

    // The agent's own folders are never listed, whatever the root: here the whole head unit.
    Head a("bounds-own");
    a.install("org.nbtapp.runtime-2.1.0");
    write(a.platform.agentHome() + "/1.4.0/nbtagent", "x\n");
    write(a.platform.tmpDir("org.nbtapp.runtime") + "/x.log", "x\n");
    write(a.platform.dataDir("org.nbtapp.runtime") + "/x", "x\n");
    write(a.sys("/opt/aa/hand"), "x\n");
    CheckContext c = a.context();
    Fence fence;
    std::string why;
    CHECK_IN(why, fenceOf(c, &fence, &why));
    Listing listing;
    listBounded(std::vector<std::string>(1, a.root), fence, &listing);
    CHECK_IN(listing.error, listing.error.empty() && !listing.tooMany && !listing.entries.empty());
    for (size_t i = 0; i < listing.entries.size(); ++i) {
        const std::string& path = listing.entries[i].path;
        const char* own[] = {"/state", "/staging", "/run", "/agent", "/apps", "/data", "/tmp"};
        for (size_t k = 0; k < sizeof own / sizeof own[0]; ++k) {
            CHECK_IN(path, path != a.root + own[k] && path.compare(0, a.root.size() + strlen(own[k]) + 1, a.root + own[k] + "/") != 0);
        }
    }
    CHECK(listing.entries[0].path == a.root && listing.entries.back().path == a.sys("/opt/aa/hand"));
    CHECK(fenced(fence, a.platform.stateDir()) && fenced(fence, a.platform.runDir() + "/x") && !fenced(fence, a.sys("/opt")));
    CHECK(ownsUnder(fence, a.platform.appDir("org.nbtapp.runtime")) && !ownsUnder(fence, a.sys("/opt/aa")));
}

// ---- the fixes ----------------------------------------------------------------------------------------------------

// A fix with this token. Returns the refusal's code, or "" if the fix ran.
static std::string fixWith(Head& h, const char* id, const std::string& token, FixOutcome* out, CrashHook* hook = 0) {
    CheckContext c = h.context();
    c.crash = hook;
    const char* code = 0;
    std::string message;
    FixOutcome ignored;
    if (fixCheck(*findCheck(id), c, token, out != 0 ? out : &ignored, &code, &message)) return "";
    CHECK_IN(message, code != 0 && !message.empty());
    return code;
}

// The fix of what a run of the check finds now; it has to run.
static FixOutcome fixNow(Head& h, const char* id, CrashHook* hook = 0) {
    FixOutcome out;
    std::string code = fixWith(h, id, h.run(id).token, &out, hook);
    CHECK_IN(code, code.empty());
    return out;
}

static std::string installLog(const Head& h) {
    return there(h.engine.store().logPath()) ? readFile(h.engine.store().logPath()) : std::string();
}

static std::string restartMarkOf(const Head& h) { return restartMark(h.platform.runDir()); }

// Every path of a list that is still there.
static std::vector<std::string> stillThere(const CheckFinding& f) {
    std::vector<std::string> out;
    for (size_t i = 0; i < f.entries.size(); ++i) {
        if (there(f.entries[i].path)) out.push_back(f.entries[i].path);
    }
    return out;
}

// The plugin is moved, whole, before anything is removed, and is left there; the check does not find it again.
static void testFixMove() {
    Head h("fix-move");
    handInstall(h);
    CheckFinding f = h.run("aa-fix");
    FixOutcome out = fixNow(h, "aa-fix");
    CHECK_IN(num((long long)out.removed), out.removed == f.entries.size() && out.left.empty() && out.moved.size() == 1);
    CHECK(out.moved[0].from == h.plugins() + "/Demo" && out.moved[0].to == h.sys("/opt/aa/Demo.old"));
    CHECK(out.state == kCheckOk && out.restart == "hmi-restart" && restartMarkOf(h) == "hmi-restart");
    CHECK(!there(h.plugins() + "/Demo") && readFile(h.sys("/opt/aa/Demo.old/libDemo.so")) == "library\n");
    std::vector<std::string> names;
    CHECK(fs::listDir(h.sys("/opt/aa"), &names, 0) && names.size() == 1 && names[0] == "Demo.old");
    std::string log = installLog(h);
    size_t moved = log.find("moved " + h.plugins() + "/Demo to " + h.sys("/opt/aa/Demo.old") + "\n"), removed = log.find("removed ");
    CHECK_IN(log, moved != std::string::npos && removed != std::string::npos && moved < removed);
    // The system partition was writable for the fix only.
    CHECK(h.platform.remounts.size() == 2 && h.platform.remounts[0] == "rw" && h.platform.remounts[1] == "ro" && !h.platform.writableNow());
    // Run again, with the token of what there is now: nothing to do.
    FixOutcome again;
    CHECK(fixWith(h, "aa-fix", h.run("aa-fix").token, &again).empty() && again.removed == 0 && again.moved.empty() && again.state == kCheckOk);
    CHECK(again.restart == "none" && h.platform.remounts.size() == 2);
    // The same plugin put on again: it goes under the other name, and the first stays as it is.
    write(h.plugins() + "/Demo/Demo.manifest", pluginManifest(210, "Demo"));
    out = fixNow(h, "aa-fix");
    CHECK(out.moved.size() == 1 && out.moved[0].to == h.sys("/opt/aa/Demo.nbtappsold") && out.state == kCheckOk);
    CHECK(there(h.sys("/opt/aa/Demo.old/Demo.xml")) && there(h.sys("/opt/aa/Demo.nbtappsold/Demo.manifest")));
    // And a third time: both names are taken, and nothing is moved or removed.
    write(h.plugins() + "/Demo/Demo.manifest", pluginManifest(210, "Demo"));
    f = h.run("aa-fix");
    CHECK(!f.fixable && fixWith(h, "aa-fix", f.token, 0) == "unsupported" && there(h.plugins() + "/Demo/Demo.manifest"));

    // The plugin alone, where /opt/aa is not there: it is made for it.
    Head p("fix-move-alone");
    write(p.plugins() + "/Demo/Demo.manifest", pluginManifest(210, "Demo"));
    out = fixNow(p, "aa-fix");
    CHECK(out.moved.size() == 1 && out.removed == 0 && out.state == kCheckOk && fs::kindOf(p.sys("/opt/aa")) == fs::kDir);
    CHECK(there(p.sys("/opt/aa/Demo.old/Demo.manifest")));
}

// A plugin in ${var_plugins} is moved into /var/opt/aa, the folder of the app's data, and every other into /opt/aa:
// each on its own file system. A folder parked in /var/opt/aa is fenced as one in /opt/aa is.
static void testFixMoveVarPlugins() {
    Head h("fix-move-var");
    handInstall(h);
    std::string varPlugins = h.platform.varPluginsDir(), data = h.sys("/var/opt/aa");
    write(varPlugins + "/Demo/Demo.manifest", pluginManifest(210, "Demo"));
    write(varPlugins + "/Demo/libDemo.so", "library under /var\n");
    // The two folders of one name each take .old, in a folder of their own. /var/opt/aa receives one, and is not
    // listed itself; what is in it is.
    CheckFinding f = h.run("aa-fix");
    CHECK_IN(movesOf(h, f), f.fixable && movesOf(h, f) == "/plugins/Demo -> /system/opt/aa/Demo.old\n"
                                                         "/var-plugins/Demo -> /system/var/opt/aa/Demo.old\n");
    CHECK_IN(listed(h, f), listed(h, f).find("d /system/var/opt/aa\n") == std::string::npos &&
                               listed(h, f).find("f /system/var/opt/aa/state.txt\n") != std::string::npos);
    FixOutcome out = fixNow(h, "aa-fix");
    CHECK_IN(num((long long)out.removed), out.removed == f.entries.size() && out.left.empty() && out.moved.size() == 2);
    CHECK(out.state == kCheckOk && !there(varPlugins + "/Demo") && !there(h.plugins() + "/Demo"));
    CHECK(readFile(data + "/Demo.old/libDemo.so") == "library under /var\n" && there(h.sys("/opt/aa/Demo.old/Demo.manifest")));
    std::vector<std::string> names;
    CHECK(fs::listDir(data, &names, 0) && names.size() == 1 && names[0] == "Demo.old");
    // Run again: nothing, with /opt/aa there or not.
    CHECK(h.run("aa-fix").state == kCheckOk);
    std::string why;
    CHECK_IN(why, fs::removeTree(h.sys("/opt/aa"), &why));
    CHECK(h.run("aa-fix").state == kCheckOk);
    // With /opt/aa back, what is in it is listed, and neither /var/opt/aa nor what is parked in it is.
    write(h.sys("/opt/aa/bin/demod"), "program\n");
    f = h.run("aa-fix");
    CHECK_IN(listed(h, f), f.fixable && f.moves.empty() && listed(h, f) == "d /system/opt/aa\nd /system/opt/aa/bin\nf /system/opt/aa/bin/demod\n");
    out = fixNow(h, "aa-fix");
    CHECK(out.state == kCheckOk && !there(h.sys("/opt/aa")) && there(data + "/Demo.old/Demo.manifest"));

    // The plugin alone: /var/opt/aa is made for it, and /opt/aa is not.
    Head a("fix-move-var-alone");
    write(a.platform.varPluginsDir() + "/Demo/Demo.manifest", pluginManifest(210, "Demo"));
    f = a.run("aa-fix");
    CHECK_IN(movesOf(a, f), f.fixable && f.entries.empty() && movesOf(a, f) == "/var-plugins/Demo -> /system/var/opt/aa/Demo.old\n");
    out = fixNow(a, "aa-fix");
    CHECK(out.moved.size() == 1 && out.state == kCheckOk && !there(a.sys("/opt/aa")) && there(a.sys("/var/opt/aa/Demo.old/Demo.manifest")));
    CHECK(a.run("aa-fix").state == kCheckOk);

    // Both names taken in /var/opt/aa: no fix, though they are free in /opt/aa.
    Head t("fix-move-var-taken");
    write(t.platform.varPluginsDir() + "/Demo/Demo.manifest", pluginManifest(210, "Demo"));
    write(t.sys("/var/opt/aa/Demo.old/Demo.manifest"), pluginManifest(210, "Demo"));
    write(t.sys("/var/opt/aa/Demo.nbtappsold"), "a file\n");
    f = t.run("aa-fix");
    CHECK_IN(f.why, f.state == kCheckProblem && !f.fixable && f.why.find("both are taken") != std::string::npos);
    CHECK(fixWith(t, "aa-fix", f.token, 0) == "unsupported" && there(t.platform.varPluginsDir() + "/Demo/Demo.manifest"));

    // /var/opt/aa that is a link, or a file: nothing is moved through it or onto it, and there is no fix.
    Head l("fix-move-var-link");
    write(l.platform.varPluginsDir() + "/Demo/Demo.manifest", pluginManifest(210, "Demo"));
    write(l.root + "/elsewhere/x", "x\n");
    CHECK_IN(why, fs::makeDirs(l.sys("/var/opt"), &why) && fs::makeLink(l.root + "/elsewhere", l.sys("/var/opt/aa"), &why));
    f = l.run("aa-fix");
    CHECK_IN(f.why, f.state == kCheckProblem && !f.fixable && f.why.find(l.sys("/var/opt/aa") + " is not a folder") != std::string::npos);
    CHECK(listed(l, f) == "l /system/var/opt/aa\n");
    CHECK_IN(why, fs::removeFile(l.sys("/var/opt/aa"), &why));
    write(l.sys("/var/opt/aa"), "a file\n");
    f = l.run("aa-fix");
    CHECK_IN(f.why, f.state == kCheckProblem && !f.fixable && f.why.find("is not a folder") != std::string::npos);
    CHECK(fixWith(l, "aa-fix", f.token, 0) == "unsupported" && there(l.platform.varPluginsDir() + "/Demo/Demo.manifest"));
    // A plugin that goes into /opt/aa is not held back by it: the file is listed to be removed.
    CHECK_IN(why, fs::removeTree(l.platform.varPluginsDir(), &why));
    write(l.plugins() + "/Demo/Demo.manifest", pluginManifest(210, "Demo"));
    f = l.run("aa-fix");
    CHECK_IN(f.why, f.fixable && listed(l, f) == "f /system/var/opt/aa\n" && movesOf(l, f) == "/plugins/Demo -> /system/opt/aa/Demo.old\n");
}

// A move that cannot be done: something came to the destination since the list. The fix goes on with the rest, and
// says that the folder was left.
class TakeDestination : public CrashHook {
public:
    explicit TakeDestination(const std::string& path) : path_(path) {}
    virtual void point(const char* name) {
        if (strcmp(name, "check-marked") == 0) write(path_, "came after the list\n");
    }

private:
    std::string path_;
};

static void testFixMoveRefused() {
    Head h("fix-move-taken");
    handInstall(h);
    TakeDestination hook(h.sys("/opt/aa/Demo.old"));
    FixOutcome out = fixNow(h, "aa-fix", &hook);
    CHECK(out.moved.empty() && out.left.size() == 1 && out.left[0] == h.plugins() + "/Demo" && out.removed == 8);
    CHECK(there(h.plugins() + "/Demo/Demo.manifest") && readFile(h.sys("/opt/aa/Demo.old")) == "came after the list\n");
    CHECK_IN(installLog(h), installLog(h).find("is there already, and nothing is written over") != std::string::npos);
    CHECK(out.state == kCheckProblem);
}

static void testFixRefused() {
    Head h("fix-refused");
    handInstall(h);
    CheckFinding f = h.run("aa-fix");
    // Something that came since the list: the token is stale, and nothing is removed.
    write(h.sys("/opt/aa/etc/new.conf"), "new\n");
    CHECK(fixWith(h, "aa-fix", f.token, 0) == "stale" && stillThere(f).size() == f.entries.size());
    CHECK(fixWith(h, "aa-fix", std::string(64, '0'), 0) == "stale" && fixWith(h, "aa-fix", "", 0) == "stale");
    // A token of another check is stale too.
    CHECK(fixWith(h, "system-writable", f.token, 0) == "stale");
    // Past a bound there is no fix.
    for (int i = 0; i < 300; ++i) write(h.sys("/opt/aa/many/" + num(i)), "x\n");
    f = h.run("aa-fix");
    CHECK(!f.fixable && fixWith(h, "aa-fix", f.token, 0) == "unsupported" && there(h.sys("/opt/aa/many/0")));
    CHECK(there(h.plugins() + "/Demo/Demo.manifest"));
    CHECK(installLog(h).empty() && restartMarkOf(h) == "none" && h.platform.remounts.empty());
    // The records cannot be read: nothing can be said, and nothing is done.
    Head u("fix-unknown");
    handInstall(u);
    f = u.run("aa-fix");
    write(u.engine.store().recordDir("org.nbtapp.broken") + "/record.json", "{}");
    CHECK(fixWith(u, "aa-fix", f.token, 0) == "io-error" && stillThere(f).size() == f.entries.size());
}

static void testFixHandApp() {
    Head h("fix-aa");
    handInstall(h);
    write(h.plugins() + "/Other/Other.manifest", pluginManifest(250, "Other"));
    CheckFinding f = h.run("aa-fix");
    FixOutcome out = fixNow(h, "aa-fix");
    CHECK_IN(num((long long)out.removed), out.removed == f.entries.size() && out.left.empty() && out.stopped.empty());
    CHECK(out.state == kCheckOk && out.restart == "hmi-restart" && restartMarkOf(h) == "hmi-restart");
    CHECK(stillThere(f).empty() && there(h.sys("/opt/aa/Demo.old")) && there(h.sys("/var/opt")) && !there(h.sys("/var/opt/aa")));
    // The link went as a link: what it led to is there.
    CHECK(readFile(h.root + "/outside/target.txt") == "not the app's\n");
    CHECK(there(h.plugins() + "/Other/Other.manifest"));
    // The files and links went before the folders; one line for each path, and none for /opt/aa, which stays.
    std::string log = installLog(h);
    size_t program = log.find("removed " + h.sys("/opt/aa/bin/demod") + "\n"), folder = log.find("removed " + h.sys("/opt/aa/bin") + "/\n");
    CHECK_IN(log, program != std::string::npos && folder != std::string::npos && program < folder);
    CHECK_IN(log, log.find("removed " + h.sys("/opt/aa") + "/\n") == std::string::npos);
    size_t lines = 0;
    for (size_t at = log.find("\nremoved "); at != std::string::npos; at = log.find("\nremoved ", at + 1)) ++lines;
    CHECK_IN(log, lines == f.entries.size());
}

// A hook that adds a file to a folder of the list once the fix has listed again: the file stays, and its folder.
class AddFile : public CrashHook {
public:
    explicit AddFile(const std::string& path) : path_(path) {}
    virtual void point(const char* name) {
        if (strcmp(name, "check-stopped") == 0) write(path_, "came after the list\n");
    }

private:
    std::string path_;
};

static void testFixLeavesWhatCameAfter() {
    Head h("fix-after");
    handInstall(h);
    std::string late = h.sys("/opt/aa/etc/late.conf");
    AddFile hook(late);
    FixOutcome out = fixNow(h, "aa-fix", &hook);
    CHECK(readFile(late) == "came after the list\n");
    CHECK_IN(out.left.size() == 1 ? out.left[0] : num((long long)out.left.size()), out.left.size() == 1 && out.left[0] == h.sys("/opt/aa/etc"));
    CHECK(!there(h.sys("/opt/aa/etc/demo.conf")) && !there(h.sys("/opt/aa/bin")) && !there(h.plugins() + "/Demo"));
    // What came is still there, so the check still finds the app; the plugin it parked is not listed.
    CheckFinding now = h.run("aa-fix");
    CHECK_IN(listed(h, now), out.state == kCheckProblem && now.moves.empty() &&
                                 listed(h, now) == "d /system/opt/aa/etc\nf /system/opt/aa/etc/late.conf\n");
}

// Ends the process at the n-th point of a fix, as a power cut or a kill would.
class DieAt : public CrashHook {
public:
    explicit DieAt(int n) : left_(n) {}
    virtual void point(const char*) {
        if (--left_ == 0) _exit(77);
    }

private:
    int left_;
};

// A fix that is cut off at each of its points in turn, and then run again from a new run of the check, ends where
// a fix that was not cut off ends: the plugin parked whole, and everything else of the app gone.
static void testFixCutOff() {
    int points = 0;
    for (int n = 1;; ++n) {
        Head h("fix-cut");
        handInstall(h);
        CheckFinding before = h.run("aa-fix");
        fflush(0);
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            DieAt hook(n);
            CheckContext c = h.context();
            c.crash = &hook;
            const char* code = 0;
            std::string message;
            FixOutcome out;
            _exit(fixCheck(*findCheck("aa-fix"), c, before.token, &out, &code, &message) ? 0 : 1);
        }
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
        if (WEXITSTATUS(status) == 0) break;   // the fix had fewer points than n: it ran to its end
        CHECK_IN(num(n), WEXITSTATUS(status) == 77);
        ++points;
        // The mark is there from before the first change on.
        if (n >= 2) CHECK_IN(num(n), restartMarkOf(h) == "hmi-restart");
        CheckFinding again = h.run("aa-fix");
        if (again.state == kCheckProblem) {
            CHECK_IN(num(n), again.token != before.token || n <= 2);
            fixNow(h, "aa-fix");
        }
        CHECK_IN(num(n), h.run("aa-fix").state == kCheckOk && restartMarkOf(h) == "hmi-restart");
        CHECK_IN(num(n), readFile(h.root + "/outside/target.txt") == "not the app's\n");
        CHECK_IN(num(n), stillThere(before).empty() && !there(h.plugins() + "/Demo"));
        CHECK_IN(num(n), readFile(h.sys("/opt/aa/Demo.old/libDemo.so")) == "library\n" && !there(h.sys("/opt/aa/Demo.nbtappsold")));
    }
    // stopped, marked, moved, one for each of the four files and links, one for each of the four folders.
    CHECK_IN(num(points), points == 3 + 4 + 4);
}

// The app's program runs: it is stopped before its files go. The program is a copy of sleep that this test starts.
static void testFixStopsPrograms() {
    Head h("fix-stop");
    handInstall(h);
    std::string program = h.sys("/opt/aa/bin/demod"), why;
    CHECK_IN(why, fs::removeFile(program, &why) && fs::copyFile("/bin/sleep", program, 0755, &why));
    fflush(0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        execl(program.c_str(), "demod", "600", (char*)0);
        _exit(127);
    }
    h.platform.mine.insert((long)pid);
    // Until the process is the program.
    std::vector<RunningProgram> running;
    for (int i = 0; i < 200 && h.platform.programsUnder(h.sys("/opt/aa"), &running) != kProgramRunning; ++i) usleep(10000);
    CHECK(running.size() == 1 && running[0].pid == (long)pid);
    FixOutcome out = fixNow(h, "aa-fix");
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    CHECK(out.stopped.size() == 1 && out.stopped[0] == "demod" && h.platform.signals.size() == 1);
    CHECK(h.platform.signals[0] == num(pid) + " TERM" && out.state == kCheckOk && !there(program));
    CHECK_IN(installLog(h), installLog(h).find("sent SIGTERM to demod (process " + num(pid) + ")\n") != std::string::npos);
}

// A program that does not end, and processes that cannot be read: the fix ends before anything is removed.
static void testFixStopFails() {
    Head h("fix-stop-fails");
    handInstall(h);
    CheckFinding f = h.run("aa-fix");
    h.platform.programs = 1;
    CHECK(fixWith(h, "aa-fix", f.token, 0) == "io-error" && stillThere(f).size() == f.entries.size());
    CHECK(there(h.plugins() + "/Demo/Demo.manifest"));
    CHECK(h.platform.signals.size() == 2 && h.platform.signals[0] == "999999 TERM" && h.platform.signals[1] == "999999 KILL");
    CHECK_IN(installLog(h), installLog(h).find("nothing was removed: demod (process 999999) is still running after SIGKILL") != std::string::npos);
    CHECK(restartMarkOf(h) == "none" && h.platform.remounts.empty());
    h.platform.programs = 2;
    h.platform.signals.clear();
    CHECK(fixWith(h, "aa-fix", f.token, 0) == "io-error" && stillThere(f).size() == f.entries.size() && h.platform.signals.empty());
    // A program that ends at SIGTERM, as the test says.
    h.platform.programs = 0;
    CHECK(fixNow(h, "aa-fix").state == kCheckOk);
}

static void testFixRemount() {
    Head h("fix-remount");
    // Nothing to do where the question does not apply: the token of nothing is answered as fixed.
    FixOutcome out;
    CHECK(fixWith(h, "system-writable", checkToken("system-writable", std::vector<CheckEntry>()), &out).empty());
    CHECK(out.state == kCheckOk && out.restart == "none" && h.platform.remounts.empty());
    h.platform.systemWritable = 1;
    h.platform.noRemount = "mount: busy";
    CheckFinding f = h.run("system-writable");
    CHECK(fixWith(h, "system-writable", f.token, 0) == "io-error" && h.platform.systemWritable == 1);
    h.platform.noRemount.clear();
    out = fixNow(h, "system-writable");
    CHECK(out.state == kCheckOk && out.removed == 0 && out.left.empty() && out.restart == "none");
    CHECK(h.platform.systemWritable == 0 && h.platform.remounts.size() == 1 && h.platform.remounts[0] == "ro");
    CHECK_IN(installLog(h), installLog(h).find("the system partition is read-only again") != std::string::npos);
}

int main(int argc, char** argv) {
    char here[4096];
    CHECK(getcwd(here, sizeof here) != 0);
    gData = argc > 1 ? argv[1] : "build/testdata";
    gWork = std::string(argc > 2 ? argv[2] : "build/agent-host/work") + "/checks";
    if (gData[0] != '/') gData = std::string(here) + "/" + gData;
    if (gWork[0] != '/') gWork = std::string(here) + "/" + gWork;
    fs::setSyncing(false);

    json::Document vectors;
    std::string why;
    CHECK_IN(why, vectors.parse(readFile(gData + "/vectors.json"), &why));
    gPackageKey = testutil::key(vectors.root().find("keys").find("package").string(), "test package key", true);
    CHECK_IN(why, fs::removeTree(gWork, &why) && fs::makeDirs(gWork, &why));

    testTable();
    testSystemWritable();
    testHandApp();
    testBounds();
    testFixMove();
    testFixMoveVarPlugins();
    testFixMoveRefused();
    testFixRefused();
    testFixHandApp();
    testFixLeavesWhatCameAfter();
    testFixCutOff();
    testFixStopsPrograms();
    testFixStopFails();
    testFixRemount();

    CHECK_IN(why, fs::removeTree(gWork, &why));
    printf("test_checks: ok\n");
    return 0;
}
