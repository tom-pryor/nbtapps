// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/hu_platform.cpp: the head unit's platform, whole, against a folder. Scripts stand in for the
// car's programs: `mount`, which notes that it was called and flips a file that says "writable", and the program
// that restarts the HMI. Nothing here can remount anything, so the one question the platform asks the system,
// whether a path is on a read-only file system, is answered from that file: everything under <folder>/system is the
// system partition. The engine then installs real test packages through it.
//
// What this does not show is that the commands in HeadUnitConfig::car() are the right ones for a car: no test here
// could tell. It holds them against what is written down, and checks that this machine is refused as a head unit.
//
// usage: test_hu_platform [testdata folder] [work folder]     (default build/testdata and build/agent-host/work)

#include "../../agent/hu_platform.hpp"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../agent/engine.hpp"
#include "../../agent/fsutil.hpp"
#include "../../agent/json.hpp"
#include "test_util.hpp"

using namespace nbt;
using testutil::readFile;

static std::string gData, gWork;
static TrustedKey gPackageKey;

static std::string good(const char* name) { return gData + "/good/" + name + ".nbtpkg"; }
static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
static bool there(const std::string& path) { return fs::kindOf(path) != fs::kNone; }

static void write(const std::string& path, const std::string& text, unsigned mode = 0644) {
    std::string why;
    CHECK_IN(why, fs::makeDirs(fs::parentOf(path), &why) && fs::writeFile(path, text, mode, &why));
}

static std::vector<std::string> command(const std::string& a, const std::string& b, const std::string& c = "") {
    std::vector<std::string> argv;
    argv.push_back(a);
    argv.push_back(b);
    if (!c.empty()) argv.push_back(c);
    return argv;
}

static std::string line(const std::vector<std::string>& argv) {
    std::string text;
    for (size_t i = 0; i < argv.size(); ++i) text += (i == 0 ? "" : " ") + argv[i];
    return text;
}

// ---- a car in a folder --------------------------------------------------------------------------------------------

//   <root>/system               the system partition: read-only unless <root>/bin/writable exists
//   <root>/elsewhere            a second read-only file system, which no command makes writable
//   <root>/var/opt/nbtapp       state, with ${app}, ${data} and staging in it      <root>/tmp/nbtapp   the run folder
//   <root>/bin/mount rw|ro      appends "mount rw" or "mount ro" to <root>/bin/calls and flips <root>/bin/writable.
//                               With <root>/bin/fail-rw or fail-ro holding a number above 0 it counts that down,
//                               prints "mount: busy" and exits with 1 instead; with noop-rw it does nothing
//   <root>/bin/restart ARGS     appends "restart ARGS"; with <root>/bin/fail-restart it prints and exits with 4
struct Car {
    std::string root, bin;
    HeadUnitConfig config;

    explicit Car(const std::string& name) : root(gWork + "/" + name), bin(root + "/bin") {
        std::string why;
        CHECK_IN(why, fs::removeTree(root, &why));
        write(bin + "/mount",
              "echo \"mount $1\" >> " + bin + "/calls\n"
              "if [ -e " + bin + "/fail-$1 ]; then\n"
              "    n=$(cat " + bin + "/fail-$1)\n"
              "    if [ \"$n\" -gt 0 ]; then echo $((n - 1)) > " + bin + "/fail-$1; echo 'mount: busy'; exit 1; fi\n"
              "fi\n"
              "if [ -e " + bin + "/noop-$1 ]; then exit 0; fi\n"
              "if [ \"$1\" = rw ]; then : > " + bin + "/writable; else rm -f " + bin + "/writable; fi\n");
        write(bin + "/restart",
              "echo \"restart $*\" >> " + bin + "/calls\n"
              "if [ -e " + bin + "/fail-restart ]; then echo 'no HMI to restart'; exit 4; fi\n");
        write(bin + "/slow", "echo \"slow\" >> " + bin + "/calls\nsleep 30\n");
        CHECK_IN(why, fs::makeDirs(root + "/system/opt/hmi/Plugins", &why) && fs::makeDirs(root + "/elsewhere", &why));

        config.stateDir = root + "/var/opt/nbtapp";
        config.stagingDir = config.stateDir + "/staging";
        config.runDir = root + "/tmp/nbtapp";
        config.appsDir = config.stateDir + "/apps";
        config.dataDir = config.stateDir + "/data";
        config.pluginsDir = root + "/system/opt/hmi/Plugins";
        config.agentDir = config.stateDir + "/agent";
        config.systemMount = root + "/system";
        config.remountWritable = command("/bin/sh", bin + "/mount", "rw");
        config.remountReadOnly = command("/bin/sh", bin + "/mount", "ro");
        config.restartHmi = command("/bin/sh", bin + "/restart", "appreset");
        config.shell = "/bin/sh";
        config.hookPath = "/usr/bin:/bin";
        config.restoreRetryMs = 1;
    }

    // What the scripts were called with since the last look, one call to a line.
    std::string calls() {
        std::string text = there(bin + "/calls") ? readFile(bin + "/calls") : std::string();
        fs::removeFile(bin + "/calls", 0);
        return text;
    }
    bool writable() const { return there(bin + "/writable"); }
    void set(const char* name, const char* text = "") { write(bin + "/" + name, text); }
    void unset(const char* name) { fs::removeFile(bin + "/" + name, 0); }
};

class CarPlatform : public HeadUnitPlatform {
public:
    explicit CarPlatform(const Car& car) : HeadUnitPlatform(car.config), car_(&car) {}

    // Absolute destinations go under the folder too: the real platform's answer is the path itself, which here
    // would be this machine's own /opt.
    virtual std::string systemPath(const std::string& absolute) const { return car_->root + "/system" + absolute; }

protected:
    virtual bool readOnly(const std::string& path, bool* out, std::string*) const {
        if (under(path, car_->root + "/system")) *out = !car_->writable();
        else *out = under(path, car_->root + "/elsewhere");
        return true;
    }

private:
    static bool under(const std::string& path, const std::string& dir) {
        return path == dir || path.compare(0, dir.size() + 1, dir + "/") == 0;
    }
    const Car* car_;
};

static void prepared(CarPlatform& p) {
    std::string why;
    CHECK_IN(why, p.prepare(&why));
}

// ---- the configuration --------------------------------------------------------------------------------------------

static void testConfig() {
    // The car's configuration is whole, but for the reboot, which may be left out: the agent then refuses one.
    HeadUnitConfig car = HeadUnitConfig::car();
    CHECK_IN(car.missing(), car.missing().empty());
    CHECK(car.reboot.empty() && car.otherPluginDirs.empty());
    CHECK(line(car.remountWritable) == "/bin/mount -u -o rw /fs/sda0" && line(car.remountReadOnly) == "/bin/mount -u -o ro /fs/sda0");
    CHECK(line(car.restartHmi) == "/opt/core/bin/OnOffDSICommander appreset" && car.systemMount == "/fs/sda0");
    // The restart program's libraries are in a folder the loader does not look in by itself.
    CHECK(has(":" + car.hookPath + ":", ":/opt/core/bin:") && has(":" + car.libraryPath + ":", ":/opt/sys/lib:"));
    // A hook's PATH names no folder that is relative, the one it runs in least of all.
    CHECK(!has(":" + car.hookPath + ":", "::") && !has(":" + car.hookPath + ":", ":.:"));
    HeadUnitPlatform real(car);
    std::string why;
    // This machine is no head unit: nothing is made on it, and the reason names the folder it lacks.
    CHECK_IN(why, !real.prepare(&why) && has(why, "not the head unit") && has(why, "/fs/sda0"));
    CHECK(real.systemPath("/opt/demo/menu.xml") == "/opt/demo/menu.xml" && real.stateDir() == "/var/opt/nbtapp");
    CHECK(real.appDir("org.nbtapp.hello") == "/var/opt/nbtapp/apps/org.nbtapp.hello");
    CHECK(real.dataDir("org.nbtapp.hello") == "/var/opt/nbtapp/data/org.nbtapp.hello" && real.shell() == "/bin/ksh");
    CHECK(real.stagingDir() == "/var/opt/nbtapp/staging" && real.runDir() == "/tmp/nbtapp");

    // A machine that lacks a folder every head unit has is not one, or the configuration is wrong about it.
    // Either way nothing is made there.
    {
        Car other("not-a-car");
        other.config.systemMount = other.root + "/fs/sda0";
        CarPlatform noMount(other);
        CHECK_IN(why, !noMount.prepare(&why) && has(why, "not the head unit") && has(why, other.root + "/fs/sda0"));
        other.config.systemMount = other.root + "/system";
        other.config.pluginsDir = other.root + "/system/opt/hmi/ID5/Plugins";
        CarPlatform noPlugins(other);
        CHECK_IN(why, !noPlugins.prepare(&why) && has(why, "not the head unit") && has(why, "/opt/hmi/ID5/Plugins"));
        CHECK(!there(other.root + "/var") && !there(other.root + "/tmp") && other.calls().empty());
    }

    Car c("config");
    CHECK_IN(c.config.missing(), c.config.missing().empty());
    HeadUnitConfig bad = c.config;
    bad.stagingDir = "staging";                 // not absolute
    bad.runDir = "";                            // not said at all
    bad.restartHmi[0] = "OnOffDSICommander";    // a name is not enough: nothing is looked up in a PATH
    bad.reboot = command("reboot", "now");
    bad.otherPluginDirs.push_back("Plugins");
    bad.restoreAttempts = 0;
    CHECK_IN(bad.missing(), bad.missing() == "stagingDir, runDir, otherPluginDirs, restartHmi, reboot, restoreAttempts");

    c.config.otherPluginDirs.push_back(c.root + "/system/opt/hmi/StockPlugins");
    CarPlatform p(c);
    prepared(p);
    CHECK(p.startNote().empty() && !p.remounted());
    CHECK(fs::isDir(c.config.stateDir) && fs::isDir(c.config.stagingDir) && fs::isDir(c.config.appsDir) && fs::isDir(c.config.dataDir));
    // Staging is beside the state, where there is room; the marks are where a power down takes them away.
    CHECK(fs::isDir(c.config.runDir) && p.runDir() == c.root + "/tmp/nbtapp" && p.stagingDir() == c.config.stateDir + "/staging");
    CHECK(p.markerPath() == c.config.runDir + "/system-writable");
    // More is kept free on the file system that holds the state, which the head unit's own settings share, than
    // anywhere else.
    CHECK(p.reserveBytes(c.config.stagingDir) == (32u << 20) && p.reserveBytes(c.config.appsDir + "/org.nbtapp.hello/bin") == (32u << 20));
    if (!fs::sameDevice("/proc", c.config.stateDir)) CHECK(p.reserveBytes("/proc") == (64u << 10));
    CHECK(p.appDir("org.nbtapp.hello") == c.config.appsDir + "/org.nbtapp.hello" && p.hookPath() == "/usr/bin:/bin");
    std::vector<std::string> scan = p.pluginScanDirs();
    CHECK(scan.size() == 2 && scan[0] == p.pluginsDir() && scan[1] == c.root + "/system/opt/hmi/StockPlugins");
    uint64_t free = 0;
    CHECK(p.freeBytes(c.config.stagingDir + "/no/such/folder", &free) && free > 0);
    CHECK(c.calls().empty());   // nothing ran

    // The real question, asked of the system: this folder can be written to; in the bench's container the root
    // file system cannot.
    bool ro = true;
    CHECK(fs::readOnly(gWork, &ro) && !ro);
    if (access("/.dockerenv", F_OK) == 0) CHECK(fs::readOnly("/usr/no/such/file", &ro) && ro);
}

// ---- the system partition -----------------------------------------------------------------------------------------

static void testRemount() {
    Car c("remount");
    CarPlatform p(c);
    prepared(p);
    std::string why;
    std::string onSystem = c.root + "/system/opt/demo/new/menu.xml";

    // A path that can be written to needs nothing.
    CHECK(p.makeWritable(c.config.stateDir + "/installed/x", &why) && p.makeWritable(c.config.appsDir + "/x/bin/y", &why));
    CHECK(c.calls().empty() && !p.remounted() && !there(p.markerPath()));
    CHECK(p.restoreReadOnly(&why) && c.calls().empty());

    // One on the system partition: one remount, however many paths follow, and one more to put it back.
    CHECK_IN(why, p.makeWritable(onSystem, &why));
    CHECK(c.writable() && p.remounted() && there(p.markerPath()));
    CHECK(p.makeWritable(c.config.pluginsDir + "/NbtDemo/NbtDemo.manifest", &why) && p.makeWritable(onSystem, &why));
    CHECK(c.calls() == "mount rw\n");
    CHECK_IN(why, p.restoreReadOnly(&why));
    CHECK(!c.writable() && !p.remounted() && !there(p.markerPath()) && c.calls() == "mount ro\n");
    CHECK(p.restoreReadOnly(&why) && c.calls().empty());

    // And again in the next step.
    CHECK(p.makeWritable(onSystem, &why) && p.restoreReadOnly(&why) && c.calls() == "mount rw\nmount ro\n");

    // Writable already, by somebody's hand: used as it is, and left as it is.
    c.set("writable");
    CHECK(p.makeWritable(onSystem, &why) && !p.remounted() && !there(p.markerPath()));
    CHECK(p.restoreReadOnly(&why) && c.writable() && c.calls().empty());
    c.unset("writable");

    // A read-only file system that is not the system partition: the remount is tried, does not help, and is undone.
    CHECK(!p.makeWritable(c.root + "/elsewhere/file", &why));
    CHECK_IN(why, has(why, "/elsewhere/file is on a read-only file system that is not the system partition"));
    CHECK(p.remounted() && p.restoreReadOnly(&why) && !c.writable() && c.calls() == "mount rw\nmount ro\n");
    // With the system partition writable already, nothing is run for it at all.
    c.set("writable");
    CHECK(!p.makeWritable(c.root + "/elsewhere/file", &why) && !p.remounted() && c.calls().empty());
    c.unset("writable");
}

static void testRemountFails() {
    Car c("remount-fails");
    std::string why;
    std::string onSystem = c.root + "/system/opt/demo/menu.xml";
    {
        CarPlatform p(c);
        prepared(p);
        // The command fails: said with how it ended and what it printed, and nothing is left behind.
        c.set("fail-rw", "99");
        CHECK(!p.makeWritable(onSystem, &why));
        CHECK_IN(why, has(why, "could not be made writable") && has(why, "/bin/mount rw` exited with 1 (mount: busy)"));
        CHECK(!p.remounted() && !there(p.markerPath()) && c.calls() == "mount rw\n");
        CHECK(p.restoreReadOnly(&why) && c.calls().empty());
        c.unset("fail-rw");
        // It ends well and changes nothing.
        c.set("noop-rw");
        CHECK(!p.makeWritable(onSystem, &why));
        CHECK_IN(why, has(why, "ran and changed nothing") && !p.remounted() && !there(p.markerPath()));
        c.unset("noop-rw");
        // And after all that it still works.
        c.calls();
        CHECK(p.makeWritable(onSystem, &why) && p.restoreReadOnly(&why) && c.calls() == "mount rw\nmount ro\n");
    }
    {
        // No such program.
        Car d("remount-missing");
        d.config.remountWritable = command(d.bin + "/no-such-program", "rw");
        CarPlatform p(d);
        prepared(p);
        CHECK(!p.makeWritable(d.root + "/system/x", &why));
        CHECK_IN(why, has(why, "no-such-program rw` exited with 127") && !p.remounted() && !there(p.markerPath()));
    }
    {
        // A program that never ends is killed at the timeout.
        Car d("remount-slow");
        d.config.remountWritable = command("/bin/sh", d.bin + "/slow");
        d.config.commandTimeoutSeconds = 1;
        CarPlatform p(d);
        prepared(p);
        CHECK(!p.makeWritable(d.root + "/system/x", &why));
        CHECK_IN(why, has(why, "did not end within 1 s") && !p.remounted() && d.calls() == "slow\n");
    }
}

static void testRestore() {
    Car c("restore");
    CarPlatform p(c);
    prepared(p);
    std::string why;
    std::string onSystem = c.root + "/system/opt/demo/menu.xml";

    // Busy twice, then it works: three tries in one call.
    CHECK(p.makeWritable(onSystem, &why));
    c.calls();
    c.set("fail-ro", "2");
    CHECK_IN(why, p.restoreReadOnly(&why));
    CHECK(!c.writable() && !p.remounted() && !there(p.markerPath()) && c.calls() == "mount ro\nmount ro\nmount ro\n");

    // Busy for good: said, and the partition stays this agent's to put back, by the next call.
    CHECK(p.makeWritable(onSystem, &why));
    c.calls();
    c.set("fail-ro", "99");
    CHECK(!p.restoreReadOnly(&why));
    CHECK_IN(why, has(why, "still writable after 5 tries") && has(why, "exited with 1 (mount: busy)"));
    CHECK(c.writable() && p.remounted() && there(p.markerPath()));
    CHECK(c.calls() == "mount ro\nmount ro\nmount ro\nmount ro\nmount ro\n");
    CHECK(p.makeWritable(onSystem, &why) && c.calls().empty());   // the next step finds it writable, and by us
    c.unset("fail-ro");
    CHECK(p.restoreReadOnly(&why) && !c.writable() && !p.remounted() && !there(p.markerPath()) && c.calls() == "mount ro\n");

    // Put back by hand in the meantime: nothing to run.
    CHECK(p.makeWritable(onSystem, &why));
    c.calls();
    c.unset("writable");
    CHECK(p.restoreReadOnly(&why) && !p.remounted() && !there(p.markerPath()) && c.calls().empty());
}

// An agent that dies with the partition writable leaves the marker, and the next one puts the partition back.
static void testAfterACrash() {
    Car c("crash");
    std::string why;
    {
        CarPlatform dies(c);
        prepared(dies);
        CHECK(dies.makeWritable(c.root + "/system/opt/demo/menu.xml", &why) && c.writable());
    }
    CHECK(c.calls() == "mount rw\n");
    {
        CarPlatform next(c);
        prepared(next);
        CHECK_IN(next.startNote(), has(next.startNote(), "had left the system partition writable; it is read-only again"));
        CHECK(!c.writable() && !next.remounted() && !there(next.markerPath()) && c.calls() == "mount ro\n");
    }
    {
        // It cannot be put back yet: the agent starts all the same, says so, and tries at the end of its next step.
        CarPlatform dies(c);
        prepared(dies);
        CHECK(dies.startNote().empty() && dies.makeWritable(c.root + "/system/opt/demo/menu.xml", &why));
    }
    c.calls();
    c.set("fail-ro", "99");
    CarPlatform next(c);
    prepared(next);
    CHECK_IN(next.startNote(), has(next.startNote(), "could not be made read-only again") && has(next.startNote(), "mount: busy"));
    CHECK(c.writable() && next.remounted() && there(next.markerPath()));
    c.unset("fail-ro");
    c.calls();
    CHECK(next.restoreReadOnly(&why) && !c.writable() && c.calls() == "mount ro\n");

    // Writable with no marker is somebody's hand, not a crash: left alone.
    c.set("writable");
    CarPlatform third(c);
    prepared(third);
    CHECK(third.startNote().empty() && c.writable() && c.calls().empty());
}

// ---- restarts -----------------------------------------------------------------------------------------------------

static void testRestarts() {
    Car c("restarts");
    std::string why;
    {
        CarPlatform p(c);
        prepared(p);
        CHECK(p.canRestartHmi(&why) && p.restartHmi(&why) && c.calls() == "restart appreset\n");
        // No command for a reboot: refused beforehand, and nothing runs.
        CHECK(!p.canReboot(&why) && why == "no way to reboot is known for this head unit");
        why.clear();
        CHECK(!p.reboot(&why) && why == "no way to reboot is known for this head unit" && c.calls().empty());
        // The program runs and fails.
        c.set("fail-restart");
        CHECK(p.canRestartHmi(&why) && !p.restartHmi(&why));
        CHECK_IN(why, has(why, "/bin/restart appreset` exited with 4 (no HMI to restart)") && c.calls() == "restart appreset\n");
        c.unset("fail-restart");
    }
    // With a command for it, and with a program that is not there.
    c.config.reboot = command("/bin/sh", c.bin + "/restart", "reboot");
    c.config.restartHmi = command(c.bin + "/OnOffDSICommander", "appreset");
    CarPlatform p(c);
    prepared(p);
    CHECK(p.canReboot(&why) && p.reboot(&why) && c.calls() == "restart reboot\n");
    CHECK(!p.canRestartHmi(&why));
    CHECK_IN(why, why == "this head unit has no " + c.bin + "/OnOffDSICommander to restart the HMI with");
    CHECK(!p.restartHmi(&why) && c.calls().empty());

    // A command's environment is the PATH and, where the configuration names one, the folders its libraries are
    // looked for in. Nothing of the agent's own environment comes along.
    write(c.bin + "/said", "echo \"path $PATH, libraries ${LD_LIBRARY_PATH-none}, own ${NBT_TEST_OWN-none}\" >> " + c.bin + "/calls\n");
    c.config.restartHmi = command("/bin/sh", c.bin + "/said");
    CHECK(setenv("LD_LIBRARY_PATH", "/the/agents/own", 1) == 0 && setenv("NBT_TEST_OWN", "set", 1) == 0);
    {
        CarPlatform none(c);
        prepared(none);
        CHECK_IN(why, none.restartHmi(&why));
        CHECK(c.calls() == "path /usr/bin:/bin, libraries none, own none\n");
    }
    c.config.libraryPath = "/lib:/opt/demo/lib";
    CarPlatform some(c);
    prepared(some);
    CHECK_IN(why, some.restartHmi(&why));
    CHECK(c.calls() == "path /usr/bin:/bin, libraries /lib:/opt/demo/lib, own none\n");
    CHECK(unsetenv("LD_LIBRARY_PATH") == 0 && unsetenv("NBT_TEST_OWN") == 0);
}

// ---- the engine on it ---------------------------------------------------------------------------------------------

// For every line a hook prints: whether the system partition was writable just then.
class PartitionWatch : public StepListener {
public:
    explicit PartitionWatch(const Car* car) : car_(car) {}
    virtual void log(const char* source, const std::string& text) {
        if (std::string(source) != "agent") seen += std::string(source) + (car_->writable() ? " rw: " : " ro: ") + text;
    }
    std::string seen;

private:
    const Car* car_;
};

static void done(const StepReport& r) {
    CHECK_IN(r.error.code + ": " + r.error.message, r.outcome == kStepDone && r.error.code.empty());
}

static void testEngine() {
    Car c("engine");
    c.config.otherPluginDirs.push_back(c.root + "/system/opt/hmi/StockPlugins");
    CarPlatform p(c);
    prepared(p);
    std::vector<TrustedKey> keys(1, gPackageKey);
    Engine engine(&p, keys, "1.0.0");
    std::string why;
    bool found = true;
    StepReport recovered;
    CHECK_IN(why, engine.init(&why) && engine.recover(&found, &recovered, 0) && !found);

    // A package that stays in its own folders: no remount.
    done(engine.install(good("org.nbtapp.runtime-2.1.0"), "", 0));
    CHECK(there(p.appDir("org.nbtapp.runtime") + "/lib/libnbtrt.txt") && c.calls().empty());

    // A plugin: one remount for its three files, and back.
    std::string demo = c.config.pluginsDir + "/NbtDemo";
    StepReport r = engine.install(good("org.nbtapp.demoplugin-1.0.0"), "", 0);
    done(r);
    CHECK(r.restart == "hmi-restart" && there(demo + "/NbtDemo.manifest") && there(demo + "/NbtDemo.xml"));
    CHECK(!c.writable() && !p.remounted() && !there(p.markerPath()) && c.calls() == "mount rw\nmount ro\n");
    done(engine.remove("org.nbtapp.demoplugin", true, 0));
    CHECK(!there(demo) && !c.writable() && c.calls() == "mount rw\nmount ro\n");

    // Its connector id is taken by an app in the stock folder, which is not where ${plugins} points.
    write(c.root + "/system/opt/hmi/StockPlugins/Stock/Stock.manifest",
          "<plugin><config><dependencies><pluginAppConnector><id>250</id></pluginAppConnector></dependencies></config></plugin>\n");
    r = engine.install(good("org.nbtapp.demoplugin-1.0.0"), "", 0);
    CHECK_IN(r.error.code + ": " + r.error.message, r.outcome == kStepFailed && r.error.code == "hmi-id-taken");
    CHECK(!there(demo) && c.calls().empty());

    // Files on the system partition and hooks: read-only whenever a hook runs.
    CHECK(fs::makeDirs(p.systemPath("/opt/demo"), &why));
    PartitionWatch watch(&c);
    done(engine.install(good("org.nbtapp.syshook-1.0.0"), "", &watch));
    CHECK_IN(watch.seen, watch.seen == "preInstall ro: pre-install\npostInstall ro: post-install\n");
    CHECK(readFile(p.systemPath("/opt/demo/extra.conf")) == "extra = 1\n" && c.calls() == "mount rw\nmount ro\n");
    watch.seen.clear();
    done(engine.remove("org.nbtapp.syshook", true, &watch));
    CHECK_IN(watch.seen, watch.seen == "preRemove ro: pre-remove\npostRemove ro: post-remove\n");
    CHECK(!there(p.systemPath("/opt/demo/extra.conf")) && !c.writable() && c.calls() == "mount rw\nmount ro\n");

    // The hook fails: undone, with a second remount for the undoing.
    write(p.dataDir("org.nbtapp.syshook") + "/fail", "");
    r = engine.install(good("org.nbtapp.syshook-1.0.0"), "", 0);
    CHECK_IN(r.error.code + ": " + r.error.message, r.outcome == kStepFailed && r.error.code == "hook-failed");
    CHECK(!there(p.systemPath("/opt/demo/extra.conf")) && !there(p.systemPath("/opt/demo/extra-link")));
    CHECK(!c.writable() && c.calls() == "mount rw\nmount ro\nmount rw\nmount ro\n");
    fs::removeFile(p.dataDir("org.nbtapp.syshook") + "/fail", 0);

    // The partition cannot be made writable: the step fails with nothing changed, and says why.
    CHECK(fs::removeTree(c.root + "/system/opt/hmi/StockPlugins", &why));
    c.set("fail-rw", "99");
    r = engine.install(good("org.nbtapp.demoplugin-1.0.0"), "", 0);
    CHECK_IN(r.error.code + ": " + r.error.message, r.outcome == kStepFailed && r.error.code == "io-error" && has(r.error.message, "mount: busy"));
    CHECK(!there(demo) && !c.writable() && !p.remounted() && !there(p.markerPath()));
    c.unset("fail-rw");

    // It cannot be put back: the step is done all the same, its log says so, and the next step's end tries again.
    c.calls();
    c.set("fail-ro", "99");
    r = engine.install(good("org.nbtapp.demoplugin-1.0.0"), "", 0);
    done(r);
    CHECK(c.writable() && p.remounted() && there(p.markerPath()));
    CHECK(has(readFile(engine.store().logPath()), "the system partition could not be made read-only again: "));
    c.unset("fail-ro");
    c.calls();
    done(engine.remove("org.nbtapp.runtime", true, 0));   // a step that touches nothing on the partition
    CHECK(!c.writable() && !p.remounted() && !there(p.markerPath()) && c.calls() == "mount ro\n");
}

int main(int argc, char** argv) {
    char here[4096];
    CHECK(getcwd(here, sizeof here) != 0);
    gData = argc > 1 ? argv[1] : "build/testdata";
    gWork = std::string(argc > 2 ? argv[2] : "build/agent-host/work") + "/hu-platform";
    if (gData[0] != '/') gData = std::string(here) + "/" + gData;
    if (gWork[0] != '/') gWork = std::string(here) + "/" + gWork;

    json::Document vectors;
    std::string why;
    CHECK_IN(why, vectors.parse(readFile(gData + "/vectors.json"), &why));
    gPackageKey = testutil::key(vectors.root().find("keys").find("package").string(), "test package key", true);

    testConfig();
    testRemount();
    testRemountFails();
    testRestore();
    testAfterACrash();
    testRestarts();
    testEngine();

    CHECK(fs::removeTree(gWork, &why));
    printf("test_hu_platform: ok\n");
    return 0;
}
