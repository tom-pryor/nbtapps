// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/engine.cpp and agent/store.cpp, on a HostPlatform folder that stands in for a head unit:
// install, upgrade and removal with their hooks; what is kept and what is put back; every refusal; and what is left
// after the agent dies at each point of a step. Also the agent's own package: placed by one agent, decided by the
// next, undone by the first if the next is not confirmed, with a death at every point of each of the three.
//
// "Dies" means the process ends there with no clearing up, which is what a crash or a kill does. A power cut can
// also lose writes that were not synced yet; that part is not simulated here.
//
// usage: test_engine [testdata folder] [work folder]     (default build/testdata and build/agent-host/work)
//        NBT_TEST_ALL_PAIRS=1 test_engine ...            the long form of the crash test, see main()

#include "../../agent/engine.hpp"

#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <map>

#include "../../agent/fsutil.hpp"
#include "../../agent/host_platform.hpp"
#include "../../agent/json.hpp"
#include "../../agent/sha256.hpp"
#include "test_util.hpp"

using namespace nbt;
using testutil::readFile;

static std::string gData, gWork;
static TrustedKey gPackageKey, gOtherKey;

static std::string good(const char* name) { return gData + "/good/" + name + ".nbtpkg"; }
static std::string bad(const char* name) { return gData + "/bad/" + name + ".nbtpkg"; }

static std::string hashOf(const std::string& data) {
    uint8_t digest[kSha256Bytes];
    sha256(data.data(), data.size(), digest);
    return toHex(digest, sizeof digest);
}

static void write(const std::string& path, const std::string& text, unsigned mode = 0644) {
    std::string why;
    CHECK_IN(why, fs::makeDirs(fs::parentOf(path), &why) && fs::writeFile(path, text, mode, &why));
}

static unsigned modeOf(const std::string& path) {
    struct stat st;
    CHECK_IN(path, stat(path.c_str(), &st) == 0);
    return (unsigned)(st.st_mode & 07777);
}

static unsigned long inodeOf(const std::string& path) {
    struct stat st;
    CHECK_IN(path, stat(path.c_str(), &st) == 0);
    return (unsigned long)st.st_ino;
}

static std::string linkOf(const std::string& path) {
    std::string target;
    CHECK_IN(path, fs::readLink(path, &target));
    return target;
}

static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// ---- a head unit in a folder --------------------------------------------------------------------------------------

struct Bench {
    std::string root;
    HostPlatform platform;
    Engine engine;

    // A new, empty head unit; or, with `reuse`, the one that is in the folder already, as after a restart.
    Bench(const std::string& name, const std::vector<TrustedKey>& keys, bool reuse = false, const char* agentVersion = "1.0.0")
        : root(gWork + "/" + name), platform(root), engine(&platform, keys, agentVersion) {
        std::string why;
        if (!reuse) CHECK_IN(why, fs::removeTree(root, &why));
        CHECK_IN(why, platform.prepare(&why) && engine.init(&why));
    }
    std::string app(const char* id) const { return platform.appDir(id); }
    std::string data(const char* id) const { return platform.dataDir(id); }
    std::string sys(const char* path) const { return platform.systemPath(path); }
    InstalledPackage record(const char* id) const {
        InstalledPackage p;
        std::string why;
        CHECK_IN(why, engine.store().load(id, &p, &why));
        return p;
    }
};

static std::vector<TrustedKey> keys(bool withOther = false, bool otherEmbedded = false) {
    std::vector<TrustedKey> k;
    k.push_back(gPackageKey);
    if (withOther) {
        k.push_back(gOtherKey);
        k.back().embedded = otherEmbedded;
    }
    return k;
}

// Everything under a head unit's folder: each path with what it is. Staging and the running log are left out: they
// are not state. A record's install.log is listed without its contents, which name the folder the test ran in.
typedef std::map<std::string, std::string> Snapshot;

static void walk(const std::string& root, const std::string& rel, Snapshot* out) {
    std::vector<std::string> names;
    std::string why;
    CHECK_IN(why, fs::listDir(rel.empty() ? root : root + "/" + rel, &names, &why));
    for (size_t i = 0; i < names.size(); ++i) {
        std::string child = rel.empty() ? names[i] : rel + "/" + names[i];
        std::string path = root + "/" + child;
        if (child == "staging" || child == "state/install.log" || child == "state/install.log.1") continue;
        fs::Kind kind = fs::kindOf(path);
        if (kind == fs::kDir) {
            (*out)[child] = "folder";
            walk(root, child, out);
        } else if (kind == fs::kLink) {
            (*out)[child] = "link to " + linkOf(path);
        } else if (names[i] == "install.log") {
            (*out)[child] = "file";
        } else {
            char mode[16];
            snprintf(mode, sizeof mode, "%04o ", modeOf(path));
            (*out)[child] = std::string("file ") + mode + hashOf(readFile(path));
        }
    }
}

static Snapshot snap(const Bench& b) {
    Snapshot s;
    walk(b.root, "", &s);
    return s;
}

static bool same(const Snapshot& a, const Snapshot& b, bool print = true) {
    if (a == b) return true;
    if (!print) return false;
    for (Snapshot::const_iterator it = a.begin(); it != a.end(); ++it) {
        Snapshot::const_iterator other = b.find(it->first);
        if (other == b.end()) fprintf(stderr, "  only in the first:  %s (%s)\n", it->first.c_str(), it->second.c_str());
        else if (other->second != it->second) fprintf(stderr, "  differs: %s (%s / %s)\n", it->first.c_str(), it->second.c_str(), other->second.c_str());
    }
    for (Snapshot::const_iterator it = b.begin(); it != b.end(); ++it) {
        if (a.find(it->first) == a.end()) fprintf(stderr, "  only in the second: %s (%s)\n", it->first.c_str(), it->second.c_str());
    }
    return false;
}

class Recorder : public StepListener {
public:
    Recorder() : bytes(0), total(0) {}
    virtual void phase(const char* name) { phases += (phases.empty() ? "" : " ") + std::string(name); }
    virtual void progress(uint64_t b, uint64_t t) {
        CHECK(b >= bytes && b <= t);
        bytes = b;
        total = t;
    }
    virtual void log(const char* source, const std::string& text) { logs += std::string(source) + ": " + text; }
    void clear() { *this = Recorder(); }

    std::string phases, logs;
    uint64_t bytes, total;
};

static void done(const StepReport& r) {
    CHECK_IN(r.error.code + ": " + r.error.message, r.outcome == kStepDone && r.error.code.empty());
}
static void refused(const StepReport& r, const char* code) {
    CHECK_IN("got " + r.error.code + ": " + r.error.message, r.outcome == kStepFailed && r.error.code == code && !r.error.message.empty());
}
static void installDone(Bench& b, const char* name) { done(b.engine.install(good(name), "", 0)); }
// Nothing of a step is left beside the state: no journal, nothing unpacked, the system partition read-only again.
static void tidy(const Bench& b) {
    std::vector<std::string> names;
    std::string why;
    CHECK(fs::listDir(b.engine.store().journalDir(), &names, &why) && names.empty());
    CHECK(fs::listDir(b.engine.store().unpackDir(), &names, &why) && names.empty());
    CHECK(!b.platform.writableNow());
}

// ---- install, upgrade, remove -------------------------------------------------------------------------------------

static void testInstallUpgradeRemove() {
    Bench b("basic", keys());
    Snapshot empty = snap(b);
    Recorder rec;
    std::string why;

    // A dependency that is not installed: the engine does not resolve, it refuses.
    StepReport r = b.engine.install(good("org.nbtapp.hello-1.1.0"), "", &rec);
    refused(r, "internal");
    CHECK(r.action == "install" && r.id == "org.nbtapp.hello" && r.version == "1.1.0" && has(r.error.message, "org.nbtapp.runtime"));
    CHECK(same(snap(b), empty) && rec.phases == "verify");
    tidy(b);

    // A folder entry: both files, and the record of what was done.
    r = b.engine.install(good("org.nbtapp.runtime-2.1.0"), "", 0);
    done(r);
    CHECK(r.action == "install" && r.restart == "none" && r.oldVersion.empty());
    std::string runtime = b.app("org.nbtapp.runtime");
    CHECK(readFile(runtime + "/lib/libnbtrt.txt") == readFile("tests/packages/runtime-2.1.0/payload/lib/libnbtrt.txt"));
    CHECK(modeOf(runtime + "/lib/notes.txt") == 0644 && fs::isDir(b.data("org.nbtapp.runtime")));
    InstalledPackage p = b.record("org.nbtapp.runtime");
    CHECK(p.key == toHex(gPackageKey.key, 32) && p.manifest.version == "2.1.0" && p.links.empty());
    CHECK(p.files.size() == 2 && p.files[0].dest == "${app}/lib/libnbtrt.txt" && p.files[0].mode == 0644 && !p.files[0].kept);
    CHECK(p.files[0].sha256 == hashOf(readFile(runtime + "/lib/libnbtrt.txt")) && p.files[0].backup.empty());
    CHECK(p.dirs.size() == 3 && p.dirs[0] == "${app}" && p.dirs[1] == "${data}" && p.dirs[2] == "${app}/lib");
    tidy(b);

    // Files, a link and hooks.
    rec.clear();
    r = b.engine.install(good("org.nbtapp.hello-1.1.0"), "", &rec);
    done(r);
    std::string hello = b.app("org.nbtapp.hello");
    CHECK_IN(rec.phases, rec.phases == "verify pre-install backup files post-install record");
    CHECK_IN(rec.logs, has(rec.logs, "preInstall: pre-install: install org.nbtapp.hello 1.1.0\n"));
    CHECK_IN(rec.logs, has(rec.logs, "postInstall: post-install: app dir " + hello + "\n"));
    CHECK(rec.total > 0 && rec.bytes == rec.total);
    CHECK(modeOf(hello + "/bin/hello") == 0755 && modeOf(hello + "/etc/hello.conf") == 0644 && linkOf(hello + "/bin/hello-cli") == "hello");
    CHECK(readFile(hello + "/bin/hello") == readFile("tests/packages/hello-1.1.0/payload/bin/hello"));
    // The record folder: the manifest as signed, the remove hooks and the log; no install hook.
    std::string record = b.engine.store().recordDir("org.nbtapp.hello");
    FileSource source;
    Package package;
    Error error;
    CHECK(source.open(good("org.nbtapp.hello-1.1.0"), &why) && package.open(&source, keys(), &error));
    CHECK(readFile(record + "/manifest.json") == package.manifestBytes() && readFile(record + "/manifest.sig") == package.signatureBytes());
    CHECK(readFile(record + "/hooks/scripts/pre-remove.sh") == readFile("tests/packages/hello-1.1.0/scripts/pre-remove.sh"));
    CHECK(fs::kindOf(record + "/hooks/scripts/post-remove.sh") == fs::kFile && fs::kindOf(record + "/hooks/scripts/pre-install.sh") == fs::kNone);
    CHECK(has(readFile(record + "/install.log"), "post-install: app dir") && has(readFile(b.engine.store().logPath()), "is complete"));
    CHECK(b.platform.remounts.empty());   // nothing went to the system partition
    tidy(b);
    std::vector<std::string> ids;
    CHECK(b.engine.store().list(&ids, &why) && ids.size() == 2 && ids[0] == "org.nbtapp.hello" && ids[1] == "org.nbtapp.runtime");

    // The same version again, and a lower one.
    Snapshot installed = snap(b);
    refused(b.engine.install(good("org.nbtapp.hello-1.1.0"), "", 0), "already-installed");
    CHECK(same(snap(b), installed));

    // An upgrade. The config file was changed on the head unit, so it stays; the record says so.
    write(hello + "/etc/hello.conf", "option00 = mine\n");
    rec.clear();
    r = b.engine.install(good("org.nbtapp.hello-1.2.0"), "", &rec);
    done(r);
    CHECK(r.action == "upgrade" && r.oldVersion == "1.1.0" && r.version == "1.2.0");
    CHECK_IN(rec.logs, has(rec.logs, "preInstall: pre-install: upgrade org.nbtapp.hello 1.2.0\n"));
    CHECK(readFile(hello + "/etc/hello.conf") == "option00 = mine\n");
    CHECK(readFile(hello + "/bin/hello") == readFile("tests/packages/hello-1.2.0/payload/bin/hello") && modeOf(hello + "/bin/hello") == 0755);
    CHECK(readFile(hello + "/share/README.txt") == readFile("tests/packages/hello-1.2.0/payload/share/README.txt"));
    CHECK(linkOf(hello + "/bin/hello-cli") == "hello");
    p = b.record("org.nbtapp.hello");
    CHECK(p.manifest.version == "1.2.0" && p.files.size() == 3 && p.files[2].dest == "${app}/etc/hello.conf");
    CHECK(p.files[2].config && p.files[2].kept && p.files[2].sha256 == hashOf(readFile("tests/packages/hello-1.2.0/payload/etc/hello.conf")));
    CHECK(!p.files[0].kept && p.links.size() == 1 && p.links[0].target == "hello");
    tidy(b);

    installed = snap(b);
    refused(b.engine.install(good("org.nbtapp.hello-1.1.0"), "", 0), "downgrade");
    r = b.engine.remove("org.nbtapp.runtime", false, 0);
    refused(r, "required-by");
    CHECK(r.action == "remove" && r.id == "org.nbtapp.runtime" && same(snap(b), installed));

    // Removal: both hooks, ${app} gone, ${data} kept.
    rec.clear();
    r = b.engine.remove("org.nbtapp.hello", false, &rec);
    done(r);
    CHECK(r.action == "remove" && r.version == "1.2.0");
    CHECK_IN(rec.phases, rec.phases == "pre-remove remove post-remove record");
    CHECK_IN(rec.logs, has(rec.logs, "preRemove: pre-remove: remove org.nbtapp.hello\n") && has(rec.logs, "postRemove: post-remove: done\n"));
    CHECK(fs::kindOf(hello) == fs::kNone && fs::isDir(b.data("org.nbtapp.hello")) && !b.engine.store().isInstalled("org.nbtapp.hello"));
    tidy(b);
    refused(b.engine.remove("org.nbtapp.hello", false, 0), "not-installed");

    // With the data deleted as well, a removal leaves nothing.
    done(b.engine.remove("org.nbtapp.runtime", true, 0));
    Snapshot expected = empty;
    expected["data/org.nbtapp.hello"] = "folder";
    CHECK(same(snap(b), expected));
    tidy(b);
}

static void testConfigReplacedWhenUntouched() {
    Bench b("config", keys());
    installDone(b, "org.nbtapp.runtime-2.1.0");
    installDone(b, "org.nbtapp.hello-1.1.0");
    installDone(b, "org.nbtapp.hello-1.2.0");
    // Nobody changed the config file, so the new version's takes its place.
    CHECK(readFile(b.app("org.nbtapp.hello") + "/etc/hello.conf") == readFile("tests/packages/hello-1.2.0/payload/etc/hello.conf"));
    CHECK(!b.record("org.nbtapp.hello").files[2].kept);

    // A file that no record knows, in the package's own folder, is replaced; a config file there is kept.
    Bench c("stray", keys());
    installDone(c, "org.nbtapp.runtime-2.1.0");
    write(c.app("org.nbtapp.hello") + "/bin/hello", "left over\n");
    write(c.app("org.nbtapp.hello") + "/etc/hello.conf", "kept from before\n");
    installDone(c, "org.nbtapp.hello-1.2.0");
    CHECK(readFile(c.app("org.nbtapp.hello") + "/bin/hello") == readFile("tests/packages/hello-1.2.0/payload/bin/hello"));
    CHECK(readFile(c.app("org.nbtapp.hello") + "/etc/hello.conf") == "kept from before\n" && c.record("org.nbtapp.hello").files[2].kept);
    tidy(c);
}

// ---- failures that are undone -------------------------------------------------------------------------------------

static void testRollback() {
    // An upgrade whose postInstall fails: every file, the link and the record are as they were.
    Bench b("rollback", keys());
    installDone(b, "org.nbtapp.runtime-2.1.0");
    installDone(b, "org.nbtapp.hello-1.2.0");
    write(b.app("org.nbtapp.hello") + "/etc/hello.conf", "option00 = mine\n");
    Snapshot before = snap(b);
    Recorder rec;
    StepReport r = b.engine.install(good("org.nbtapp.hello-1.3.0"), "", &rec);
    refused(r, "hook-failed");
    CHECK(r.action == "upgrade" && r.error.message == "postInstall exited with 3");
    CHECK_IN(rec.phases, rec.phases == "verify pre-install backup files post-install rollback");
    CHECK_IN(rec.logs, has(rec.logs, "preInstall: pre-install: upgrade from 1.2.0\n") &&
                           has(rec.logs, "postInstall: post-install: failing on purpose\n") && has(rec.logs, "and on stderr\n"));
    CHECK(same(snap(b), before));
    CHECK(b.record("org.nbtapp.hello").manifest.version == "1.2.0");
    tidy(b);
    done(b.engine.remove("org.nbtapp.hello", false, 0));

    // The same package as a first install: nothing of it remains, not even its folders.
    before = snap(b);
    rec.clear();
    r = b.engine.install(good("org.nbtapp.hello-1.3.0"), "", &rec);
    refused(r, "hook-failed");
    CHECK_IN(rec.logs, r.action == "install" && has(rec.logs, "preInstall: pre-install: install from nothing\n"));
    CHECK(same(snap(b), before));
    tidy(b);
}

static void testHooks() {
    Bench b("hooks", keys());
    Snapshot before = snap(b);
    Recorder rec;

    // preInstall refuses: nothing was changed, so nothing is undone.
    StepReport r = b.engine.install(good("org.nbtapp.prefail-1.0.0"), "", &rec);
    refused(r, "hook-failed");
    CHECK_IN(rec.phases, r.error.message == "preInstall exited with 7" && rec.phases == "verify pre-install");
    CHECK(has(rec.logs, "preInstall: pre-install: not today\n") && same(snap(b), before));
    tidy(b);

    // postInstall outlives its one second: killed, and the file placed before it is gone again.
    rec.clear();
    time_t start = time(0);
    r = b.engine.install(good("org.nbtapp.slow-1.0.0"), "", &rec);
    refused(r, "hook-timeout");
    CHECK(time(0) - start <= 5 && has(rec.logs, "postInstall: post-install: sleeping\n") && !has(rec.logs, "never printed"));
    CHECK_IN(rec.phases, rec.phases == "verify pre-install backup files post-install rollback");
    CHECK(same(snap(b), before));
    tidy(b);
}

// ---- what a hook finds --------------------------------------------------------------------------------------------

// Notes, for every line a hook prints, whether the system partition was writable just then.
class PartitionWatch : public StepListener {
public:
    explicit PartitionWatch(const HostPlatform* platform) : platform_(platform) {}
    virtual void log(const char* source, const std::string& text) {
        if (std::string(source) != "agent") seen += std::string(source) + (platform_->writableNow() ? " rw: " : " ro: ") + text;
    }
    std::string seen;

private:
    const HostPlatform* platform_;
};

// The system partition is read-only whenever a hook runs, though the package places files on it; and what comes
// after the hook, an undoing included, still gets done.
static void testHooksFindReadOnly() {
    Bench b("hooks-ro", keys());
    std::string why;
    std::string conf = b.sys("/opt/demo/extra.conf"), link = b.sys("/opt/demo/extra-link");
    CHECK(fs::makeDirs(b.sys("/opt/demo"), &why));
    Snapshot before = snap(b);

    PartitionWatch watch(&b.platform);
    done(b.engine.install(good("org.nbtapp.syshook-1.0.0"), "", &watch));
    CHECK_IN(watch.seen, watch.seen == "preInstall ro: pre-install\npostInstall ro: post-install\n");
    CHECK(readFile(conf) == "extra = 1\n" && linkOf(link) == "extra.conf");
    // Writable for the files, read-only again for the hook, and nothing on it after that.
    CHECK(b.platform.remounts.size() == 2 && b.platform.remounts[0] == "rw" && b.platform.remounts[1] == "ro");
    tidy(b);

    watch.seen.clear();
    b.platform.remounts.clear();
    done(b.engine.remove("org.nbtapp.syshook", true, &watch));
    CHECK_IN(watch.seen, watch.seen == "preRemove ro: pre-remove\npostRemove ro: post-remove\n");
    CHECK(same(snap(b), before) && b.platform.remounts.size() == 2);
    tidy(b);

    // The hook fails with the partition read-only: the undoing makes it writable a second time.
    write(b.data("org.nbtapp.syshook") + "/fail", "");
    before = snap(b);
    watch.seen.clear();
    b.platform.remounts.clear();
    refused(b.engine.install(good("org.nbtapp.syshook-1.0.0"), "", &watch), "hook-failed");
    CHECK_IN(watch.seen, watch.seen == "preInstall ro: pre-install\npostInstall ro: post-install\n");
    CHECK(same(snap(b), before) && fs::kindOf(conf) == fs::kNone && fs::kindOf(link) == fs::kNone);
    CHECK(b.platform.remounts.size() == 4 && b.platform.remounts[2] == "rw" && b.platform.remounts[3] == "ro");
    tidy(b);
}

// ---- the HMI plugin folder ----------------------------------------------------------------------------------------

static std::string pluginManifest(int connector, const char* name) {
    char text[512];
    snprintf(text, sizeof text,
             "<plugin><name>%s</name><config><dependencies><pluginAppConnector><id>%d</id></pluginAppConnector>"
             "</dependencies><parameters><DescFile>{pluginDir}/%s.xml</DescFile></parameters></config></plugin>\n",
             name, connector, name);
    return text;
}

static std::string pluginDescription(int focus) {
    char text[256];
    snprintf(text, sizeof text, "<pluginApps><pluginApp entryButtonFocusID=\"%d\" text=\"Other\"/></pluginApps>\n", focus);
    return text;
}

static void testPlugin() {
    Bench b("plugin", keys());
    std::string plugins = b.platform.pluginsDir();
    // An app that was installed some other way already uses connector id 250.
    write(plugins + "/Other/Other.manifest", pluginManifest(250, "Other"));
    write(plugins + "/Other/Other.xml", pluginDescription(40000));
    Snapshot before = snap(b);
    StepReport r = b.engine.install(good("org.nbtapp.demoplugin-1.0.0"), "", 0);
    refused(r, "hmi-id-taken");
    CHECK(has(r.error.message, "250") && same(snap(b), before));
    // Now it uses our focus id instead.
    write(plugins + "/Other/Other.manifest", pluginManifest(251, "Other"));
    write(plugins + "/Other/Other.xml", pluginDescription(32760));
    r = b.engine.install(good("org.nbtapp.demoplugin-1.0.0"), "", 0);
    refused(r, "hmi-id-taken");
    CHECK(has(r.error.message, "32760"));
    tidy(b);

    // An app in another folder the HMI loads from counts as well: the stock folder, where ${plugins} is not it.
    write(plugins + "/Other/Other.manifest", pluginManifest(251, "Other"));
    write(plugins + "/Other/Other.xml", pluginDescription(40000));
    std::string stock = b.root + "/stock-plugins";
    write(stock + "/Stock/Stock.manifest", pluginManifest(250, "Stock"));
    write(stock + "/Stock/Stock.xml", pluginDescription(40001));
    installDone(b, "org.nbtapp.demoplugin-1.0.0");   // nobody says the HMI loads from there
    done(b.engine.remove("org.nbtapp.demoplugin", true, 0));
    b.platform.morePluginDirs.push_back(stock);
    b.platform.morePluginDirs.push_back(b.root + "/no-such-folder");
    r = b.engine.install(good("org.nbtapp.demoplugin-1.0.0"), "", 0);
    refused(r, "hmi-id-taken");
    CHECK(has(r.error.message, "250"));
    write(stock + "/Stock/Stock.manifest", pluginManifest(252, "Stock"));
    write(stock + "/Stock/Stock.xml", pluginDescription(32760));
    r = b.engine.install(good("org.nbtapp.demoplugin-1.0.0"), "", 0);
    refused(r, "hmi-id-taken");
    CHECK(has(r.error.message, "32760"));
    write(stock + "/Stock/Stock.xml", pluginDescription(40001));
    b.platform.remounts.clear();
    tidy(b);

    // With neither in use the plugin installs: the folder is made, the partition remounted and restored.
    write(plugins + "/Other/Other.xml", pluginDescription(32761));
    before = snap(b);
    r = b.engine.install(good("org.nbtapp.demoplugin-1.0.0"), "", 0);
    done(r);
    CHECK(r.restart == "hmi-restart");
    CHECK(readFile(plugins + "/NbtDemo/NbtDemo.manifest") == readFile("tests/packages/plugin-1.0.0/payload/plugin/NbtDemo.manifest"));
    CHECK(modeOf(plugins + "/NbtDemo/NbtDemo.xml") == 0644 && fs::kindOf(plugins + "/NbtDemo/libNbtDemo.txt") == fs::kFile);
    CHECK(b.platform.remounts.size() == 2 && b.platform.remounts[0] == "rw" && b.platform.remounts[1] == "ro");
    InstalledPackage p = b.record("org.nbtapp.demoplugin");
    CHECK(p.dirs.size() == 3 && p.dirs[2] == "${plugins}/NbtDemo" && p.files.size() == 3);
    std::vector<std::string> names;
    std::string why;
    CHECK(fs::listDir(plugins + "/NbtDemo", &names, &why) && names.size() == 3);   // nothing else: no temporary, no copy
    tidy(b);

    // Removal takes the folder with it and leaves the other app alone.
    done(b.engine.remove("org.nbtapp.demoplugin", true, 0));
    CHECK(same(snap(b), before) && b.platform.remounts.size() == 4 && b.platform.remounts[3] == "ro");
    tidy(b);
}

// ---- the iDrive plugin's own package --------------------------------------------------------------------------------
// org.nbtapp.plugin as tools/plugin_pkg.py makes it (tests/make_packages.py): the manifest and the UI description
// that are in plugin/, with stand-ins for the library and the two DBs, in two versions.

static void testOwnPlugin() {
    const char* const id = "org.nbtapp.plugin";
    const std::string first = good("org.nbtapp.plugin-1.0.0"), second = good("org.nbtapp.plugin-1.1.0");
    std::string why;
    std::vector<std::string> names;

    // An agent from before the versions that keep a launcher in their folder is too old for it.
    {
        Bench old("own-plugin-old", keys());
        refused(old.engine.install(first, "", 0), "agent-too-old");
        tidy(old);
    }

    Bench b("own-plugin", keys(), false, "1.1.0");
    const std::string folder = b.platform.pluginsDir() + "/NbtApp";
    // A plugin that was copied onto the head unit by hand is in the folder, and no record knows it: nothing of
    // it is written over. It has to be moved out of the way by hand, once.
    write(folder + "/NbtApp.manifest", readFile("plugin/NbtApp.manifest"));
    write(folder + "/libNbtApp.so", "copied here by hand\n");
    Snapshot before = snap(b);
    StepReport r = b.engine.install(first, "", 0);
    refused(r, "stock-mismatch");
    CHECK(same(snap(b), before) && b.platform.remounts.empty());
    CHECK_IN(why, fs::removeTree(folder, &why));
    tidy(b);

    // The install: the folder with its five files, and the HMI has to start again to load it.
    before = snap(b);
    r = b.engine.install(first, "", 0);
    done(r);
    CHECK(r.action == "install" && r.id == id && r.version == "1.0.0" && r.restart == "hmi-restart");
    CHECK(readFile(folder + "/NbtApp.manifest") == readFile("plugin/NbtApp.manifest") &&
          readFile(folder + "/NbtApp.xml") == readFile("plugin/NbtApp.xml"));
    CHECK(fs::listDir(folder, &names, &why) && names.size() == 5);              // nothing else: no temporary, no copy
    for (size_t i = 0; i < names.size(); ++i) CHECK_IN(names[i], modeOf(folder + "/" + names[i]) == 0644);
    CHECK(b.platform.remounts.size() == 2 && b.platform.remounts[0] == "rw" && b.platform.remounts[1] == "ro");
    InstalledPackage p = b.record(id);
    CHECK(p.manifest.version == "1.0.0" && p.files.size() == 5);
    const std::string library = readFile(folder + "/libNbtApp.so");
    CHECK(has(library, "version 1.0.0"));
    tidy(b);

    // The upgrade: the same five files, of the next version, and again a restart of the HMI.
    r = b.engine.install(second, "", 0);
    done(r);
    CHECK(r.action == "upgrade" && r.version == "1.1.0" && r.oldVersion == "1.0.0" && r.restart == "hmi-restart");
    CHECK(has(readFile(folder + "/libNbtApp.so"), "version 1.1.0") && b.record(id).manifest.version == "1.1.0");
    CHECK(readFile(folder + "/NbtApp.manifest") == readFile("plugin/NbtApp.manifest"));
    CHECK(fs::listDir(folder, &names, &why) && names.size() == 5);
    tidy(b);
    refused(b.engine.install(first, "", 0), "downgrade");

    // Removal takes the folder with it.
    done(b.engine.remove(id, true, 0));
    CHECK(same(snap(b), before) && fs::kindOf(folder) == fs::kNone);
    tidy(b);
}

// ---- a stock file -------------------------------------------------------------------------------------------------

static void testStockFile() {
    const std::string stockA = "stock menu, firmware A\n", stockB = "stock menu, firmware B\n";
    Bench b("stock", keys());
    std::string menu = b.sys("/opt/demo/menu.xml"), record = b.engine.store().recordDir("org.nbtapp.menupatch");
    std::string settings = b.data("org.nbtapp.menupatch") + "/settings.conf";
    std::string why;

    // No file to replace, and a file that is not one of the two the package names.
    CHECK(fs::makeDirs(b.sys("/opt/demo"), &why));
    Snapshot before = snap(b);
    refused(b.engine.install(good("org.nbtapp.menupatch-1.0.0"), "", 0), "stock-mismatch");
    CHECK(same(snap(b), before));
    write(menu, "somebody else's menu\n");
    before = snap(b);
    refused(b.engine.install(good("org.nbtapp.menupatch-1.0.0"), "", 0), "stock-mismatch");
    CHECK(same(snap(b), before));
    tidy(b);

    // The stock file of firmware B: replaced, and kept in the record with its mode.
    write(menu, stockB, 0640);
    Snapshot stock = snap(b);
    StepReport r = b.engine.install(good("org.nbtapp.menupatch-1.0.0"), "", 0);
    done(r);
    CHECK(r.restart == "required");
    CHECK(readFile(menu) == "patched menu, first version\n" && modeOf(menu) == 0644 && linkOf(b.sys("/opt/demo/menu-link")) == "menu.xml");
    InstalledPackage p = b.record("org.nbtapp.menupatch");
    CHECK(p.files.size() == 2 && p.files[0].dest == "/opt/demo/menu.xml" && p.files[0].backup == "backup/0" && p.files[1].backup.empty());
    CHECK(readFile(record + "/backup/0") == stockB && modeOf(record + "/backup/0") == 0640);
    CHECK(readFile(settings) == "volume = 5\n" && modeOf(settings) == 0600);
    CHECK(p.dirs.size() == 2 && b.platform.remounts.size() == 2);   // /opt/demo was there: not ours to remove
    tidy(b);

    // An upgrade carries the stock file over, and leaves alone a file that already is what it would place.
    unsigned long inode = inodeOf(settings);
    done(b.engine.install(good("org.nbtapp.menupatch-1.1.0"), "", 0));
    CHECK(readFile(menu) == "patched menu, second version\n" && readFile(record + "/backup/0") == stockB && modeOf(record + "/backup/0") == 0640);
    CHECK(inodeOf(settings) == inode && !b.record("org.nbtapp.menupatch").files[1].kept);
    tidy(b);

    // A version that lets go of the stock file puts it back, and its link goes.
    write(settings, "volume = 11\n", 0600);
    done(b.engine.install(good("org.nbtapp.menupatch-2.0.0"), "", 0));
    CHECK(readFile(menu) == stockB && modeOf(menu) == 0640 && fs::kindOf(b.sys("/opt/demo/menu-link")) == fs::kNone);
    p = b.record("org.nbtapp.menupatch");
    CHECK(p.files.size() == 1 && p.files[0].kept && p.files[0].backup.empty() && p.links.empty() && readFile(settings) == "volume = 11\n");
    CHECK(fs::kindOf(record + "/backup/0") == fs::kNone);
    tidy(b);
    done(b.engine.remove("org.nbtapp.menupatch", false, 0));
    CHECK(readFile(menu) == stockB && readFile(settings) == "volume = 11\n");

    // A firmware update replaces the patched file while the package is installed. Removing the package then
    // leaves the new firmware's file alone instead of putting the older stock file over it.
    installDone(b, "org.nbtapp.menupatch-1.0.0");
    write(menu, "stock menu, firmware C\n", 0640);
    Recorder rec;
    done(b.engine.remove("org.nbtapp.menupatch", true, &rec));
    CHECK_IN(rec.logs, readFile(menu) == "stock menu, firmware C\n" && has(rec.logs, "/opt/demo/menu.xml was changed since it was installed"));
    CHECK(fs::kindOf(b.sys("/opt/demo/menu-link")) == fs::kNone);
    // An upgrade over such a file checks that file, not the old backup: firmware C is not one the package names.
    write(menu, stockB, 0640);
    installDone(b, "org.nbtapp.menupatch-1.0.0");
    write(menu, "stock menu, firmware C\n", 0640);
    Snapshot changed = snap(b);
    refused(b.engine.install(good("org.nbtapp.menupatch-1.1.0"), "", 0), "stock-mismatch");
    CHECK(same(snap(b), changed));
    // Firmware A is: it becomes the stock file that is kept, and the one a removal puts back.
    write(menu, stockA, 0600);
    done(b.engine.install(good("org.nbtapp.menupatch-1.1.0"), "", 0));
    CHECK(readFile(menu) == "patched menu, second version\n" && readFile(record + "/backup/0") == stockA && modeOf(record + "/backup/0") == 0600);
    // And a version that lets go of the file leaves a changed one where it is.
    write(menu, "stock menu, firmware C\n", 0640);
    done(b.engine.install(good("org.nbtapp.menupatch-2.0.0"), "", 0));
    CHECK(readFile(menu) == "stock menu, firmware C\n");
    done(b.engine.remove("org.nbtapp.menupatch", true, 0));
    write(menu, stockB, 0640);
    CHECK(same(snap(b), stock));
    tidy(b);

    // Install and remove, data included: the head unit is as it was, to the byte and the mode.
    CHECK(fs::removeTree(b.data("org.nbtapp.menupatch"), &why));
    CHECK(same(snap(b), stock));
    installDone(b, "org.nbtapp.menupatch-1.1.0");
    CHECK(readFile(menu) == "patched menu, second version\n");
    done(b.engine.remove("org.nbtapp.menupatch", true, 0));
    CHECK(same(snap(b), stock));
    tidy(b);
}

// ---- who owns what, and what may sit beside what ------------------------------------------------------------------

static void testOwnershipAndConflicts() {
    Bench b("owners", keys());
    std::string why;
    CHECK(fs::makeDirs(b.sys("/opt/demo"), &why));
    Snapshot before = snap(b);
    installDone(b, "org.nbtapp.rival-1.0.0");
    CHECK(readFile(b.sys("/opt/demo/sub/extra.txt")) == "rival owns this\n");
    InstalledPackage p = b.record("org.nbtapp.rival");
    CHECK(p.dirs.size() == 3 && p.dirs[2] == "/opt/demo/sub");

    // A second package may not take a path the first one owns.
    Snapshot withRival = snap(b);
    refused(b.engine.install(good("org.nbtapp.rival2-1.0.0"), "", 0), "file-owned");
    CHECK(same(snap(b), withRival));

    // rival declares a conflict with hello below 2.0.0: hello is refused while rival is installed.
    installDone(b, "org.nbtapp.runtime-2.1.0");
    refused(b.engine.install(good("org.nbtapp.hello-1.1.0"), "", 0), "conflict");

    // rival's preRemove fails. That is logged, and the removal goes on, down to the folder the install made.
    Recorder rec;
    StepReport r = b.engine.remove("org.nbtapp.rival", true, &rec);
    done(r);
    CHECK_IN(rec.logs, has(rec.logs, "preRemove: pre-remove: failing, which must not stop the removal\n") &&
                           has(rec.logs, "agent: preRemove exited with 5; removing anyway\n") &&
                           has(rec.logs, "postRemove: post-remove: remove org.nbtapp.rival 1.0.0\n"));
    CHECK(fs::kindOf(b.sys("/opt/demo/sub")) == fs::kNone && fs::isDir(b.sys("/opt/demo")));
    done(b.engine.remove("org.nbtapp.runtime", true, 0));
    CHECK(same(snap(b), before));

    // The other way round: with hello installed, rival is refused.
    installDone(b, "org.nbtapp.runtime-2.1.0");
    installDone(b, "org.nbtapp.hello-1.1.0");
    refused(b.engine.install(good("org.nbtapp.rival-1.0.0"), "", 0), "conflict");

    // A file that nobody recorded, outside the package's own folders, is not overwritten.
    write(b.sys("/opt/demo/sub/extra.txt"), "put there by hand\n");
    refused(b.engine.install(good("org.nbtapp.rival2-1.0.0"), "", 0), "stock-mismatch");
    CHECK(readFile(b.sys("/opt/demo/sub/extra.txt")) == "put there by hand\n");
    // Nor is a folder in the way of a file.
    CHECK(fs::removeFile(b.sys("/opt/demo/sub/extra.txt"), &why) && fs::makeDirs(b.sys("/opt/demo/sub/extra.txt"), &why));
    refused(b.engine.install(good("org.nbtapp.rival2-1.0.0"), "", 0), "io-error");

    // Two packages may not declare the same serial service: radio2 wants a name radio has, whichever comes first.
    installDone(b, "org.nbtapp.radio-1.0.0");
    p = b.record("org.nbtapp.radio");
    CHECK(p.manifest.serialServices.size() == 2 && p.manifest.serialServices[0].name == "TestRadio" &&
          p.manifest.serialServices[0].program == "radiod" && p.manifest.serialServices[1].name == "TestRadio-2");
    StepReport twice = b.engine.install(good("org.nbtapp.radio2-1.0.0"), "", 0);
    refused(twice, "conflict");
    CHECK_IN(twice.error.message, twice.error.message.find("declares the serial service TestRadio, which the installed "
                                                            "org.nbtapp.radio 1.0.0 declares") != std::string::npos);
    done(b.engine.remove("org.nbtapp.radio", true, 0));
    installDone(b, "org.nbtapp.radio2-1.0.0");
    refused(b.engine.install(good("org.nbtapp.radio-1.0.0"), "", 0), "conflict");
    tidy(b);
}

// ---- keys, versions and archives ----------------------------------------------------------------------------------

static void testKeysAndChecks() {
    std::string why;
    {
        Bench b("keys-one", keys());
        Snapshot before = snap(b);
        StepReport r = b.engine.install(bad("signature-other-key"), "", 0);
        refused(r, "bad-signature");
        CHECK(r.id.empty());   // nothing in an archive is believed before its signature is
        refused(b.engine.install(bad("file-changed"), "", 0), "bad-archive");
        refused(b.engine.install(bad("manifest-version-2"), "", 0), "unsupported-manifest");
        refused(b.engine.install(gData + "/good/no-such-file.nbtpkg", "", 0), "io-error");
        CHECK(same(snap(b), before));
        tidy(b);

        // The manifest a plan was made with: the archive must hold exactly that one.
        FileSource source;
        Package package;
        Error error;
        CHECK(source.open(good("org.nbtapp.runtime-2.1.0"), &why) && package.open(&source, keys(), &error));
        refused(b.engine.install(good("org.nbtapp.runtime-2.1.0"), std::string(64, '0'), 0), "bad-archive");
        CHECK(same(snap(b), before));
        done(b.engine.install(good("org.nbtapp.runtime-2.1.0"), package.manifestSha256(), 0));
    }
    {
        // A key that is trusted but not built in may not sign an org.nbtapp. id.
        Bench b("keys-added", keys(true, false));
        installDone(b, "org.nbtapp.runtime-2.1.0");
        refused(b.engine.install(bad("signature-other-key"), "", 0), "reserved-id");
    }
    {
        // An update must come from the key that signed what is installed.
        Bench b("keys-two", keys(true, true));
        installDone(b, "org.nbtapp.runtime-2.1.0");
        installDone(b, "org.nbtapp.hello-1.1.0");
        Snapshot before = snap(b);
        refused(b.engine.install(bad("signature-other-key"), "", 0), "key-mismatch");
        CHECK(same(snap(b), before));
        done(b.engine.remove("org.nbtapp.hello", false, 0));
        done(b.engine.install(bad("signature-other-key"), "", 0));   // "bad" only for a reader without this key
        CHECK(b.record("org.nbtapp.hello").key == toHex(gOtherKey.key, 32));
        refused(b.engine.install(good("org.nbtapp.hello-1.3.0"), "", 0), "key-mismatch");
    }
    {
        // requires.agent, with a pre-release counted as older than its release.
        Bench old("agent-old", keys(), false, "0.9.0");
        refused(old.engine.install(good("org.nbtapp.runtime-2.1.0"), "", 0), "agent-too-old");
        Bench pre("agent-pre", keys(), false, "1.0.0-rc.1");
        refused(pre.engine.install(good("org.nbtapp.runtime-2.1.0"), "", 0), "agent-too-old");
        Bench newer("agent-new", keys(), false, "2.3.4");
        installDone(newer, "org.nbtapp.runtime-2.1.0");
    }
    {
        // Not enough room: refused before anything is written outside staging.
        Bench b("space", keys());
        Snapshot before = snap(b);
        b.platform.fakeFree = 100;
        refused(b.engine.install(good("org.nbtapp.runtime-2.1.0"), "", 0), "no-space");
        CHECK(same(snap(b), before));
        tidy(b);
        b.platform.fakeFree = -1;
        installDone(b, "org.nbtapp.runtime-2.1.0");
    }

    // The checks on their own, as a planner will call them on a set it has worked out.
    Bench b("checks", keys());
    installDone(b, "org.nbtapp.runtime-2.1.0");
    std::vector<InstalledPackage> installed;
    CHECK(b.engine.store().loadAll(&installed, &why) && installed.size() == 1);
    FileSource source;
    Package hello;
    Error error;
    CHECK(source.open(good("org.nbtapp.hello-1.2.0"), &why) && hello.open(&source, keys(), &error));
    std::vector<Problem> problems;
    std::vector<MissingDependency> missing;
    std::string keyHex = toHex(gPackageKey.key, 32);
    checkInstall(hello.manifest(), keyHex, true, installed, "1.0.0", &problems, &missing);
    CHECK(problems.empty() && missing.empty());
    checkInstall(hello.manifest(), keyHex, true, std::vector<InstalledPackage>(), "0.1.0", &problems, &missing);
    CHECK(problems.size() == 1 && problems[0].code == "agent-too-old" && problems[0].id == "org.nbtapp.hello");
    CHECK(missing.size() == 1 && missing[0].id == "org.nbtapp.runtime" && missing[0].range == ">=2.0.0 <3.0.0" && missing[0].requiredBy == "org.nbtapp.hello");
    // The dependency is installed, at a version outside the range.
    installed[0].manifest.version = "3.0.0";
    missing.clear();
    problems.clear();
    checkInstall(hello.manifest(), keyHex, true, installed, "1.0.0", &problems, &missing);
    CHECK(problems.empty() && missing.size() == 1);
    problems.clear();
    checkRemove("org.nbtapp.runtime", installed, &problems);
    CHECK(problems.empty());
    checkRemove("org.nbtapp.nothing", installed, &problems);
    CHECK(problems.size() == 1 && problems[0].code == "not-installed");
}

// ---- after the agent dies -----------------------------------------------------------------------------------------

static void testRecoverBasics() {
    Bench b("recover", keys());
    bool found = true;
    StepReport r;
    std::string why;
    CHECK(b.engine.recover(&found, &r, 0) && !found);

    // A journal with a line in the middle that cannot be read is not guessed at: the agent stops and says so.
    std::string log = b.engine.store().journalDir() + "/log";
    const std::string begin = "{\"op\":\"begin\",\"action\":\"install\",\"id\":\"org.nbtapp.x\",\"version\":\"1.0.0\"}\n";
    CHECK(fs::appendFile(log, begin + "this is not a journal line\n{\"op\":\"mkdir\",\"path\":\"/nowhere\"}\n", &why));
    CHECK(b.engine.recover(&found, &r, 0) && found && r.outcome == kStepBroken && r.error.code == "internal");
    CHECK(fs::kindOf(log) == fs::kFile);
    refused(b.engine.install(good("org.nbtapp.runtime-2.1.0"), "", 0), "internal");
    refused(b.engine.remove("org.nbtapp.runtime", false, 0), "internal");

    // A last line that cannot be read never reached the disk whole, so what it announced never started: the step
    // is undone from the lines before it.
    CHECK(fs::removeFile(log, &why) && fs::appendFile(log, begin + std::string("\0\0\0\0 half a line\n", 17), &why));
    CHECK(b.engine.recover(&found, &r, 0) && found && r.outcome == kStepFailed && r.error.code == "interrupted");
    CHECK(r.action == "install" && r.id == "org.nbtapp.x" && fs::kindOf(log) == fs::kNone);

    // A journal cut off inside its first line: nothing had started.
    CHECK(fs::appendFile(log, "{\"op\":\"begin\",\"action\":\"inst", &why));
    CHECK(b.engine.recover(&found, &r, 0) && !found && fs::kindOf(log) == fs::kNone);
    installDone(b, "org.nbtapp.runtime-2.1.0");
    tidy(b);
}

class DieAt : public CrashHook {
public:
    explicit DieAt(int n) : left_(n) {}
    virtual void point(const char*) {
        if (--left_ == 0) _exit(77);
    }

private:
    int left_;
};

// The same countdown for the points inside file operations.
static DieAt* gDie = 0;
static void filePoint(const char* name) {
    if (gDie != 0) gDie->point(name);
}

struct Scenario {
    const char* name;
    void (*setup)(Bench&);
    StepReport (*step)(Engine&);
    bool succeeds;   // false: the step fails by itself and is undone
};

static void setupRuntime(Bench& b) { installDone(b, "org.nbtapp.runtime-2.1.0"); }
static void setupHello11(Bench& b) {
    setupRuntime(b);
    installDone(b, "org.nbtapp.hello-1.1.0");
    write(b.app("org.nbtapp.hello") + "/etc/hello.conf", "option00 = mine\n");
}
static void setupHello12(Bench& b) {
    setupRuntime(b);
    installDone(b, "org.nbtapp.hello-1.2.0");
}
static void setupStock(Bench& b) { write(b.sys("/opt/demo/menu.xml"), "stock menu, firmware A\n", 0640); }
static void setupPatch10(Bench& b) {
    setupStock(b);
    installDone(b, "org.nbtapp.menupatch-1.0.0");
}
static void setupPatch11(Bench& b) {
    setupStock(b);
    installDone(b, "org.nbtapp.menupatch-1.1.0");
    write(b.data("org.nbtapp.menupatch") + "/settings.conf", "volume = 11\n", 0600);
}
static void setupNothing(Bench&) {}

static StepReport stepHello12(Engine& e) { return e.install(good("org.nbtapp.hello-1.2.0"), "", 0); }
static StepReport stepHello13(Engine& e) { return e.install(good("org.nbtapp.hello-1.3.0"), "", 0); }
static StepReport stepPatch10(Engine& e) { return e.install(good("org.nbtapp.menupatch-1.0.0"), "", 0); }
static StepReport stepPatch11(Engine& e) { return e.install(good("org.nbtapp.menupatch-1.1.0"), "", 0); }
static StepReport stepPatch20(Engine& e) { return e.install(good("org.nbtapp.menupatch-2.0.0"), "", 0); }
static StepReport stepPlugin(Engine& e) { return e.install(good("org.nbtapp.demoplugin-1.0.0"), "", 0); }
static StepReport stepRemoveHello(Engine& e) { return e.remove("org.nbtapp.hello", false, 0); }
static StepReport stepRemovePatch(Engine& e) { return e.remove("org.nbtapp.menupatch", true, 0); }

// Runs `step` in a child process that dies at its n-th crash point. Returns false if the step ended before that.
static bool dieDuringStep(Bench& b, StepReport (*step)(Engine&), int n) {
    fflush(0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        DieAt hook(n);
        gDie = &hook;
        fs::setTestHook(filePoint);
        b.engine.setCrashHook(&hook);
        step(b.engine);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 77));
    return WEXITSTATUS(status) == 77;
}

static bool dieDuringRecover(Bench& b, int n) {
    fflush(0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        DieAt hook(n);
        gDie = &hook;
        fs::setTestHook(filePoint);
        b.engine.setCrashHook(&hook);
        bool found;
        StepReport r;
        b.engine.recover(&found, &r, 0);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 77));
    return WEXITSTATUS(status) == 77;
}

// After a restart: recover, and check that the head unit is in the state before the step or the state after it,
// whole, and that the agent agrees with itself about which.
static void recoverAndCheck(const Scenario& s, const std::string& bench, const std::string& name, const Snapshot& before,
                            const Snapshot& after) {
    Bench again(bench, keys(), true);
    bool found = false;
    StepReport r;
    CHECK(again.engine.recover(&found, &r, 0));
    Snapshot now = snap(again);
    bool isAfter;
    if (!found) {
        // The step had finished, or had not started: the journal was gone or never begun.
        isAfter = same(now, after, false);
        CHECK_IN(name, isAfter || same(now, before));
    } else if (r.outcome == kStepDone) {
        isAfter = true;
        CHECK_IN(name, same(now, after));
    } else {
        CHECK_IN(name + ": " + r.error.code + ": " + r.error.message, r.outcome == kStepFailed && r.error.code == "interrupted");
        isAfter = false;
        CHECK_IN(name, same(now, before));
    }
    tidy(again);
    CHECK(again.engine.recover(&found, &r, 0) && !found);

    // The head unit is usable: where the step was undone, it can be run again and then completes.
    if (!isAfter && s.succeeds) {
        done(s.step(again.engine));
        CHECK_IN(name, same(snap(again), after));
        tidy(again);
    }
}

// `copyMoves`: every move is a copy and a removal, as between the head unit's two file systems.
// `pairs`: a second death during the recovery is tried as well: 0 for no scenario, 1 for the four that undo and
// redo the most, 2 for all of them.
static int testCrashRecovery(bool copyMoves, int pairs) {
    fs::setCopyOnMove(copyMoves);
    const Scenario scenarios[] = {
        {"install", setupRuntime, stepHello12, true},
        {"upgrade", setupHello11, stepHello12, true},
        {"upgrade-fails", setupHello12, stepHello13, false},
        {"stock", setupStock, stepPatch10, true},
        {"stock-upgrade", setupPatch10, stepPatch11, true},
        {"stock-let-go", setupPatch11, stepPatch20, true},
        {"plugin", setupNothing, stepPlugin, true},
        {"remove", setupHello12, stepRemoveHello, true},
        {"remove-stock", setupPatch10, stepRemovePatch, true},
    };
    int deaths = 0;
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; ++i) {
        const Scenario& s = scenarios[i];
        std::string name = std::string(copyMoves ? "crash-copy-" : "crash-") + s.name;

        // What the step does when nothing goes wrong.
        Bench reference(name, keys());
        s.setup(reference);
        Snapshot before = snap(reference);
        StepReport r = s.step(reference.engine);
        CHECK_IN(name, (r.outcome == kStepDone) == s.succeeds && r.outcome != kStepBroken);
        Snapshot after = snap(reference);
        CHECK_IN(name, s.succeeds ? !same(before, after, false) : same(before, after));

        // The agent dies at the first point, the second, ... until the step gets through.
        int points = 0;
        for (int n = 1;; ++n) {
            Bench b(name, keys());
            s.setup(b);
            if (!dieDuringStep(b, s.step, n)) break;
            char where[64];
            snprintf(where, sizeof where, ", died at point %d", n);
            recoverAndCheck(s, name, name + where, before, after);
            ++points;
            ++deaths;
        }
        CHECK_IN(name, points >= 5);

        // It dies during the step, and then again during the recovery: every pair of points.
        std::string which = s.name;
        if (pairs == 0) continue;
        if (pairs == 1 && which != "upgrade" && which != "upgrade-fails" && which != "stock-let-go" && which != "remove-stock") continue;
        for (int n = 1; n <= points; ++n) {
            for (int k = 1;; ++k) {
                Bench b(name, keys());
                s.setup(b);
                CHECK(dieDuringStep(b, s.step, n));
                Bench restarted(name, keys(), true);
                if (!dieDuringRecover(restarted, k)) break;
                char where[96];
                snprintf(where, sizeof where, ", died at point %d and then at point %d of the recovery", n, k);
                recoverAndCheck(s, name, name + where, before, after);
                ++deaths;
            }
        }
    }
    fs::setCopyOnMove(false);
    return deaths;
}

// ---- the agent's own package --------------------------------------------------------------------------------------

static const char kAgentId[] = "org.nbtapp.agent";
static const char kStandIn[] = "#!/bin/sh\necho \"nbtagent 1.0.0 (build stand-in)\"\n";
static const char kStandInLauncher[] = "#!/bin/sh\nexec \"${0%/*}/nbtagent\" \"$@\"\n";

// The agent's own folder as hu_install.sh leaves one, with `version` as the one the starter starts: the program,
// and the version's launcher beside it.
static void makeHome(const Bench& b, const char* version) {
    write(b.platform.agentHome() + "/" + version + "/nbtagent", kStandIn, 0755);
    write(b.platform.agentHome() + "/" + version + "/nbtlaunch.sh", kStandInLauncher, 0755);
    write(b.platform.agentHome() + "/current", std::string(version) + "\n");
}

static std::string homeFile(const Bench& b, const char* name) {
    std::string path = b.platform.agentHome() + "/" + name;
    return fs::kindOf(path) == fs::kFile ? readFile(path) : std::string("(none)");
}

// What the launcher does before it starts a version that waits for its try.
static void launcherRenames(const Bench& b) {
    std::string home = b.platform.agentHome();
    CHECK(rename((home + "/trial").c_str(), (home + "/trial.started").c_str()) == 0);
}

// The version the launcher starts next, by its own rule: a version that waits gets its one try, and the file its
// new name; otherwise it is the version in current.
static std::string launcherStarts(const Bench& b) {
    std::string waiting = homeFile(b, "trial");
    if (waiting != "(none)") {
        launcherRenames(b);
        return waiting.substr(0, waiting.find('\n'));
    }
    std::string current = homeFile(b, "current");
    return current.substr(0, current.find('\n'));
}

static StepReport stepAgent11(Engine& e) { return e.install(good("org.nbtapp.agent-1.1.0"), "", 0); }

// The agent's own package is placed, and not decided.
static void placed(const StepReport& r) {
    CHECK_IN(r.error.code + ": " + r.error.message, r.outcome == kStepRestart && r.error.code.empty());
}

// A whole update: placed by the agent `from`, started by the launcher, and confirmed as the agent `to`.
static void updateAgent(const std::string& bench, const std::vector<TrustedKey>& k, const char* from, const char* to,
                        const std::string& archive) {
    std::string why;
    {
        Bench b(bench, k, true, from);
        placed(b.engine.install(archive, "", 0));
        CHECK_IN(why, b.engine.beginTrial(to, &why));
        CHECK(launcherStarts(b) == to);
    }
    Bench b(bench, k, true, to);
    bool found = false;
    StepReport r;
    CHECK(b.engine.recover(&found, &r, 0) && found && r.outcome == kStepRestart);
    CHECK_IN(why, b.engine.confirmTrial(&why));
    CHECK(b.engine.recover(&found, &r, 0) && found);
    done(r);
    tidy(b);
}

static void testAgentPackage() {
    std::string why;
    bool found = false;
    StepReport r;
    const std::string payload = readFile("tests/packages/agent-1.1.0/payload/nbtagent");

    // An agent that nothing would start again cannot take a newer version of itself.
    {
        Bench b("agent-nowhere", keys());
        Snapshot before = snap(b);
        refused(stepAgent11(b.engine), "no-launcher");
        CHECK(same(snap(b), before));
        // The folder names another version than the one that runs,
        makeHome(b, "0.9.0");
        refused(stepAgent11(b.engine), "no-launcher");
        // or this one, whose program is not in it.
        write(b.platform.agentHome() + "/current", "1.0.0\n");
        before = snap(b);
        refused(stepAgent11(b.engine), "no-launcher");
        CHECK(same(snap(b), before));
        // or whose program is there with no launcher beside it for the starter to run.
        write(b.platform.agentHome() + "/1.0.0/nbtagent", kStandIn, 0755);
        before = snap(b);
        refused(stepAgent11(b.engine), "no-launcher");
        CHECK(same(snap(b), before));
        tidy(b);
    }

    // The step: the new version in a folder of its own, the one that runs untouched, and nothing decided.
    Bench b("agent", keys());
    makeHome(b, "1.0.0");
    const std::string home = b.platform.agentHome();
    Snapshot before = snap(b);
    Recorder rec;
    r = b.engine.install(good("org.nbtapp.agent-1.1.0"), "", &rec);
    placed(r);
    CHECK(r.action == "upgrade" && r.id == kAgentId && r.version == "1.1.0" && r.oldVersion == "1.0.0" && r.restart == "none");
    CHECK_IN(rec.phases, rec.phases == "verify pre-install backup files post-install record");
    CHECK_IN(rec.logs, has(rec.logs, "asking the new version which one it is") && has(rec.logs, "agent: nbtagent 1.1.0 (build ") &&
                           has(rec.logs, "having the shell read the new version's launcher"));
    CHECK(modeOf(home + "/1.1.0/nbtagent") == 0755 && modeOf(home + "/1.1.0/nbtctl") == 0755 && readFile(home + "/1.1.0/nbtagent") == payload);
    CHECK(modeOf(home + "/1.1.0/nbtlaunch.sh") == 0755 &&
          readFile(home + "/1.1.0/nbtlaunch.sh") == readFile("tests/packages/agent-1.1.0/payload/nbtlaunch.sh"));
    CHECK(readFile(home + "/1.0.0/nbtagent") == kStandIn && homeFile(b, "current") == "1.0.0\n" && homeFile(b, "trial") == "(none)");
    CHECK(!b.engine.store().isInstalled(kAgentId) && fs::isDir(b.data(kAgentId)) && b.platform.remounts.empty());
    std::string journal = readFile(b.engine.store().journalDir() + "/log");
    CHECK_IN(journal, has(journal, "\"op\":\"begin\"") && has(journal, "\"oldVersion\":\"1.0.0\"") && !has(journal, "commit"));
    CHECK(fs::kindOf(b.engine.store().journalDir() + "/record/record.json") == fs::kFile);
    // Until it is decided, no other step runs.
    refused(b.engine.install(good("org.nbtapp.runtime-2.1.0"), "", 0), "internal");

    // Handed to the launcher: one file.
    CHECK(b.engine.trialState() == kNoTrial && !b.engine.beginTrial("1.3.0", &why) && homeFile(b, "trial") == "(none)");
    CHECK_IN(why, b.engine.beginTrial("1.1.0", &why) && homeFile(b, "trial") == "1.1.0\n");

    // The version before it is started again and the try never began: the step is undone, to the last folder.
    {
        Bench old("agent", keys(), true);
        CHECK(old.engine.trialState() == kWayBack);
        CHECK(old.engine.recover(&found, &r, 0) && found && r.outcome == kStepFailed && r.error.code == "not-confirmed");
        CHECK_IN(r.error.message, r.id == kAgentId && r.version == "1.1.0" && has(r.error.message, "never tried"));
        CHECK(same(snap(old), before) && old.engine.trialState() == kNoTrial);
        tidy(old);
        CHECK(old.engine.recover(&found, &r, 0) && !found);
    }

    // Placed again, and started by the launcher: on trial. The new version touches nothing, the log included.
    placed(stepAgent11(b.engine));
    CHECK(b.engine.beginTrial("1.1.0", &why) && launcherStarts(b) == "1.1.0" && homeFile(b, "trial.started") == "1.1.0\n");
    Snapshot handed = snap(b);
    std::string log = readFile(b.engine.store().logPath());
    {
        Bench next("agent", keys(), true, "1.1.0");
        CHECK(next.engine.trialState() == kOnTrial);
        CHECK(next.engine.recover(&found, &r, 0) && found && r.outcome == kStepRestart && r.id == kAgentId && r.version == "1.1.0");
        CHECK(next.engine.recover(&found, &r, 0) && found && r.outcome == kStepRestart);
        CHECK(same(snap(next), handed) && readFile(next.engine.store().logPath()) == log);
        refused(next.engine.install(good("org.nbtapp.runtime-2.1.0"), "", 0), "internal");

        // Confirmed: one rename decides it, and recover() finishes the step that placed it.
        CHECK_IN(why, next.engine.confirmTrial(&why) && homeFile(next, "current") == "1.1.0\n");
        CHECK(next.engine.trialState() == kConfirmed && !next.engine.confirmTrial(&why));
        CHECK(next.engine.recover(&found, &r, 0) && found);
        done(r);
        CHECK(r.action == "upgrade" && r.version == "1.1.0" && r.oldVersion == "1.0.0");
        InstalledPackage p = next.record(kAgentId);
        CHECK(p.manifest.version == "1.1.0" && p.key == toHex(gPackageKey.key, 32) && p.files.size() == 3 && p.links.empty());
        CHECK(p.files[0].dest == "${app}/nbtagent" && p.files[0].mode == 0755 && p.files[0].sha256 == hashOf(payload));
        CHECK(p.dirs.size() == 2 && p.dirs[0] == "${app}" && p.dirs[1] == "${data}");
        CHECK(homeFile(next, "trial.started") == "(none)" && next.engine.trialState() == kNoTrial);
        CHECK(readFile(home + "/1.0.0/nbtagent") == kStandIn);                // the version before stays, one back
        CHECK(has(readFile(next.engine.store().logPath()), "version 1.1.0 of the agent is confirmed"));
        tidy(next);
        CHECK(next.engine.recover(&found, &r, 0) && !found);
        // The agent does not remove itself, and takes neither its own version again nor a lower one.
        Snapshot confirmed = snap(next);
        refused(next.engine.remove(kAgentId, false, 0), "required-by");
        refused(stepAgent11(next.engine), "already-installed");
        CHECK(same(snap(next), confirmed));
    }

    // The update after it, by the new version: its own folder stays whole, and the version before the last goes.
    {
        Bench next("agent", keys(), true, "1.1.0");
        r = next.engine.install(good("org.nbtapp.agent-1.2.0"), "", 0);
        placed(r);
        CHECK(r.oldVersion == "1.1.0" && readFile(home + "/1.1.0/nbtagent") == payload && modeOf(home + "/1.1.0/nbtctl") == 0755);
        CHECK(fs::kindOf(home + "/1.2.0/share/notes.txt") == fs::kFile && next.record(kAgentId).manifest.version == "1.1.0");
        CHECK(next.engine.beginTrial("1.2.0", &why) && launcherStarts(next) == "1.2.0");
    }
    {
        Bench last("agent", keys(), true, "1.2.0");
        CHECK(last.engine.recover(&found, &r, 0) && found && r.outcome == kStepRestart);
        CHECK(last.engine.confirmTrial(&why) && last.engine.recover(&found, &r, 0) && found);
        done(r);
        InstalledPackage p = last.record(kAgentId);
        CHECK(p.manifest.version == "1.2.0" && p.files.size() == 4);
        CHECK(p.dirs.size() == 3 && p.dirs[0] == "${data}" && p.dirs[1] == "${app}" && p.dirs[2] == "${app}/share");
        CHECK(fs::kindOf(home + "/1.0.0") == fs::kNone && readFile(home + "/1.1.0/nbtagent") == payload && fs::isDir(home + "/1.2.0"));
        CHECK(homeFile(last, "current") == "1.2.0\n" && homeFile(last, "trial.started") == "(none)");
        tidy(last);
        refused(stepAgent11(last.engine), "downgrade");
    }

    // The way back: the try began, nobody confirmed the new version, and the launcher starts the old one again.
    {
        Bench c("agent-back", keys());
        makeHome(c, "1.0.0");
        before = snap(c);
        placed(stepAgent11(c.engine));
        CHECK(c.engine.beginTrial("1.1.0", &why) && launcherStarts(c) == "1.1.0");
        // Something the new version left in its folder, which no journal knows.
        write(c.platform.agentHome() + "/1.1.0/core", "left by the version that fell over\n");
        CHECK(launcherStarts(c) == "1.0.0");
        Bench old("agent-back", keys(), true);
        CHECK(old.engine.trialState() == kWayBack);
        CHECK(old.engine.recover(&found, &r, 0) && found && r.outcome == kStepFailed && r.error.code == "not-confirmed");
        CHECK_IN(r.error.message, has(r.error.message, "no controller reached"));
        CHECK(same(snap(old), before));
        tidy(old);
        CHECK(old.engine.recover(&found, &r, 0) && !found);
        placed(stepAgent11(old.engine));                                       // and it can be tried again
    }

    // The agent that placed it stopped before it had handed it over: undone, as any step that was interrupted.
    {
        Bench d("agent-early", keys());
        makeHome(d, "1.0.0");
        before = snap(d);
        placed(stepAgent11(d.engine));
        Bench again("agent-early", keys(), true);
        CHECK(again.engine.recover(&found, &r, 0) && found && r.outcome == kStepFailed && r.error.code == "interrupted");
        CHECK(same(snap(again), before));
        tidy(again);
    }

    // What is left of an update with no journal: a trial file, and with a try that failed its version's folder.
    {
        Bench e("agent-leftover", keys());
        makeHome(e, "1.0.0");
        before = snap(e);
        write(e.platform.agentHome() + "/1.1.0/nbtagent", kStandIn, 0755);
        write(e.platform.agentHome() + "/trial.started", "1.1.0\n");
        CHECK(e.engine.recover(&found, &r, 0) && !found && same(snap(e), before));
        // A trial file that names nothing is cleared away too, and one that names the version in current.
        write(e.platform.agentHome() + "/trial.started", "../1.0.0\n");
        CHECK(e.engine.recover(&found, &r, 0) && !found && same(snap(e), before));
        write(e.platform.agentHome() + "/trial.started", "1.0.0\n");
        CHECK(e.engine.trialState() == kConfirmed && e.engine.recover(&found, &r, 0) && !found && same(snap(e), before));
    }

    // An update between two other versions is not this agent's to decide: it is left as it is.
    {
        Bench f("agent-others", keys());
        makeHome(f, "1.0.0");
        placed(stepAgent11(f.engine));
        CHECK(f.engine.beginTrial("1.1.0", &why) && launcherStarts(f) == "1.1.0");
        handed = snap(f);
        Bench other("agent-others", keys(), true, "0.9.0");
        CHECK(other.engine.trialState() == kNotOurs);
        CHECK(other.engine.recover(&found, &r, 0) && found && r.outcome == kStepBroken && r.error.code == "internal");
        CHECK(same(snap(other), handed));
        refused(other.engine.install(good("org.nbtapp.runtime-2.1.0"), "", 0), "internal");
    }

    // What is refused of the agent's own package.
    {
        Bench g("agent-refused", keys());
        makeHome(g, "1.0.0");
        const std::string at = g.platform.agentHome();
        before = snap(g);
        r = g.engine.install(good("org.nbtapp.agent-2.0.0"), "", 0);          // says it is another version
        refused(r, "not-confirmed");
        CHECK_IN(r.error.message, has(r.error.message, "nbtagent 2.0.1 (build stand-in)") && has(r.error.message, "2.0.0"));
        r = g.engine.install(good("org.nbtapp.agent-2.1.0"), "", 0);          // does not start
        refused(r, "not-confirmed");
        CHECK_IN(r.error.message, has(r.error.message, "does not start here"));
        refused(g.engine.install(good("org.nbtapp.agent-2.2.0"), "", 0), "bad-manifest");   // no program
        refused(g.engine.install(good("org.nbtapp.agent-2.3.0"), "", 0), "bad-manifest");   // a file outside its folders
        refused(g.engine.install(good("org.nbtapp.agent-2.4.0"), "", 0), "bad-manifest");   // no launcher
        r = g.engine.install(good("org.nbtapp.agent-2.5.0"), "", 0);          // a launcher the shell cannot read
        refused(r, "not-confirmed");
        CHECK_IN(r.error.message, has(r.error.message, "nbtlaunch.sh") && has(r.error.message, "the shell says"));
        CHECK(same(snap(g), before));
        tidy(g);
        // A folder of that version is there already: it is never written over.
        CHECK(fs::makeDirs(at + "/1.1.0", &why));
        refused(stepAgent11(g.engine), "already-installed");
        CHECK(fs::removeTree(at + "/1.1.0", &why));
        // An update is under way.
        write(at + "/trial", "1.1.0\n");
        refused(stepAgent11(g.engine), "conflict");
        CHECK(rename((at + "/trial").c_str(), (at + "/trial.started").c_str()) == 0);
        refused(stepAgent11(g.engine), "conflict");
        CHECK(fs::removeFile(at + "/trial.started", &why) && same(snap(g), before));
        // The version that counts is the one that runs, with no record of it.
        Bench newer("agent-newer", keys(), false, "1.1.0");
        makeHome(newer, "1.1.0");
        refused(stepAgent11(newer.engine), "already-installed");
        Bench newest("agent-newest", keys(), false, "1.2.0-rc.1");
        makeHome(newest, "1.2.0-rc.1");
        refused(stepAgent11(newest.engine), "downgrade");
        placed(newest.engine.install(good("org.nbtapp.agent-1.2.0"), "", 0));
    }

    // Either built-in key may sign it, whichever signed the version before; a key that is only trusted may not.
    {
        Bench h("agent-keys", keys(true, true));
        makeHome(h, "1.0.0");
        updateAgent("agent-keys", keys(true, true), "1.0.0", "1.1.0", good("org.nbtapp.agent-1.1.0"));
        CHECK(h.record(kAgentId).key == toHex(gPackageKey.key, 32));
        updateAgent("agent-keys", keys(true, true), "1.1.0", "1.2.0", bad("agent-other-key"));
        CHECK(h.record(kAgentId).key == toHex(gOtherKey.key, 32) && h.record(kAgentId).manifest.version == "1.2.0");
        Bench added("agent-keys-added", keys(true, false));
        makeHome(added, "1.0.0");
        refused(added.engine.install(bad("agent-other-key"), "", 0), "reserved-id");
    }

    // The starter. The agent that current names puts the one it came with in place of another one: only that
    // agent, only with no update under way, and only a starter that the shell can read.
    {
        Bench s("agent-starter", keys());
        makeHome(s, "1.0.0");
        const std::string at = s.platform.agentHome();
        const std::string older = "#!/bin/sh\n# the starter of a version before\n", mine = "#!/bin/sh\n# the starter 1.0.0 came with\n";
        std::string said;
        write(at + "/launcher", older, 0755);
        before = snap(s);
        s.engine.settleStarter(&said);                                         // this version came with none
        CHECK_IN(said, said.empty() && same(snap(s), before));
        write(at + "/1.0.0/nbtstart.sh", "#!/bin/sh\nif true; then\n", 0755);  // one that no shell reads to its end
        before = snap(s);
        s.engine.settleStarter(&said);
        CHECK_IN(said, has(said, "is not put in place") && has(said, "the shell says") && same(snap(s), before));
        write(at + "/1.0.0/nbtstart.sh", mine, 0755);
        write(at + "/trial", "1.1.0\n");                                       // an update is under way
        before = snap(s);
        s.engine.settleStarter(&said);
        CHECK_IN(said, said.empty() && same(snap(s), before));
        CHECK(fs::removeFile(at + "/trial", &why));
        {
            Bench other("agent-starter", keys(), true, "0.9.0");             // not the version current names
            other.engine.settleStarter(&said);
            CHECK_IN(said, said.empty() && readFile(at + "/launcher") == older);
        }
        s.engine.settleStarter(&said);
        CHECK_IN(said, has(said, "is now the starter that version 1.0.0 came with"));
        CHECK(readFile(at + "/launcher") == mine && modeOf(at + "/launcher") == 0755 && fs::kindOf(fs::tempIn(at)) == fs::kNone);
        before = snap(s);
        s.engine.settleStarter(&said);                                         // the same one: nothing is written
        CHECK_IN(said, said.empty() && same(snap(s), before));
        CHECK(fs::removeFile(at + "/launcher", &why));                         // and with none in place
        s.engine.settleStarter(&said);
        CHECK_IN(said, !said.empty() && readFile(at + "/launcher") == mine);
    }
}

// The agent's own update, with a death at every point of each of its three parts. Whatever the point, the head
// unit ends as one of two things, whole: the old version with nothing left of the update, or the new version
// confirmed. The launcher's part is played by its rule alone (launcherStarts).
typedef void (*Work)(Engine&);

static void workPlace(Engine& e) {
    StepReport r = stepAgent11(e);
    std::string why;
    if (r.outcome == kStepRestart) e.beginTrial("1.1.0", &why);
}
static void workConfirm(Engine& e) {
    bool found = false;
    StepReport r;
    std::string why;
    e.recover(&found, &r, 0);
    if (e.confirmTrial(&why)) e.recover(&found, &r, 0);
}
static void workRecover(Engine& e) {
    bool found = false;
    StepReport r;
    e.recover(&found, &r, 0);
}

// Runs `work` as the agent `version` on the bench that is there, in a child that dies at its n-th point. False if
// the work ended before that.
static bool dieDuring(const std::string& bench, const char* version, Work work, int n) {
    fflush(0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        Bench b(bench, keys(), true, version);
        DieAt hook(n);
        gDie = &hook;
        fs::setTestHook(filePoint);
        b.engine.setCrashHook(&hook);
        work(b.engine);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 77));
    return WEXITSTATUS(status) == 77;
}

static void setupAgentHome(Bench& b) { makeHome(b, "1.0.0"); }
static void setupOnTrial(Bench& b) {
    std::string why;
    setupAgentHome(b);
    placed(stepAgent11(b.engine));
    CHECK(b.engine.beginTrial("1.1.0", &why) && launcherStarts(b) == "1.1.0");
}

// Starts agents as the launcher would until nothing is left to decide, with no controller ever reaching a version
// on trial unless `confirm` says so. `first` is the version of a try that the launcher has begun already. Returns
// the version that is left running.
static std::string runToRest(const std::string& bench, bool confirm, const std::string& where, const char* first = 0) {
    for (int round = 0; round < 4; ++round) {
        Bench probe(bench, keys(), true);
        std::string version = round == 0 && first != 0 ? std::string(first) : launcherStarts(probe);
        Bench b(bench, keys(), true, version.c_str());
        bool found = false;
        StepReport r;
        std::string why;
        CHECK_IN(where, b.engine.recover(&found, &r, 0));
        if (found && r.outcome == kStepRestart) {
            if (!confirm) continue;                 // the try ends unconfirmed: the launcher starts again
            CHECK_IN(where, b.engine.confirmTrial(&why) && b.engine.recover(&found, &r, 0) && found && r.outcome == kStepDone);
        } else if (found) {
            CHECK_IN(where + ": " + r.error.code + ": " + r.error.message, r.outcome == kStepDone || r.outcome == kStepFailed);
        }
        tidy(b);
        CHECK_IN(where, b.engine.recover(&found, &r, 0) && !found && b.engine.trialState() == kNoTrial);
        return version;
    }
    CHECK_IN(where, false);
    return std::string();
}

static int testAgentCrash() {
    const std::string name = "agent-crash";
    // The two states a head unit may be left in.
    Snapshot before, after;
    {
        Bench b(name, keys());
        setupAgentHome(b);
        before = snap(b);
        setupOnTrial(b);
        CHECK(runToRest(name, true, "reference", "1.1.0") == "1.1.0");
        after = snap(b);
        CHECK(!same(before, after, false));
    }
    int deaths = 0;
    char where[96];

    // 1. The old version dies while it places the new one, or hands it over.
    int points = 0, tried = 0;
    for (int n = 1;; ++n) {
        {
            Bench b(name, keys());
            setupAgentHome(b);
        }
        if (!dieDuring(name, "1.0.0", workPlace, n)) break;
        snprintf(where, sizeof where, "placing, died at point %d", n);
        Bench b(name, keys(), true);
        if (homeFile(b, "trial") != "(none)") ++tried;
        // Nobody reaches the new version, if it gets a try at all: the old one is back, with nothing left over.
        CHECK_IN(where, runToRest(name, false, where) == "1.0.0");
        CHECK_IN(where, same(snap(b), before));
        // And the update can be run again.
        placed(stepAgent11(b.engine));
        ++points;
    }
    CHECK(points >= 10 && tried >= 1);
    deaths += points;

    // 2. The new version dies while it confirms itself and finishes the step.
    int kept = 0, back = 0;
    points = 0;
    for (int n = 1;; ++n) {
        {
            Bench b(name, keys());
            setupOnTrial(b);
        }
        if (!dieDuring(name, "1.1.0", workConfirm, n)) break;
        snprintf(where, sizeof where, "confirming, died at point %d", n);
        Bench b(name, keys(), true);
        // The launcher starts the version in current: the new one if the rename had happened, else the old one.
        std::string left = runToRest(name, false, where);
        if (left == "1.1.0") {
            CHECK_IN(where, same(snap(b), after));
            ++kept;
        } else {
            CHECK_IN(where, left == "1.0.0" && same(snap(b), before));
            ++back;
        }
        ++points;
    }
    CHECK(points >= 5 && kept >= 3 && back >= 1);
    deaths += points;

    // 3. The old version dies while it undoes an update that was not confirmed.
    points = 0;
    for (int n = 1;; ++n) {
        {
            Bench b(name, keys());
            setupOnTrial(b);
            write(b.platform.agentHome() + "/1.1.0/core", "left by the version that fell over\n");
        }
        if (!dieDuring(name, "1.0.0", workRecover, n)) break;
        snprintf(where, sizeof where, "undoing, died at point %d", n);
        Bench b(name, keys(), true);
        CHECK_IN(where, runToRest(name, false, where) == "1.0.0");
        CHECK_IN(where, same(snap(b), before));
        ++points;
    }
    CHECK(points >= 5);
    deaths += points;
    return deaths;
}

int main(int argc, char** argv) {
    char here[4096];
    CHECK(getcwd(here, sizeof here) != 0);
    gData = argc > 1 ? argv[1] : "build/testdata";
    gWork = std::string(argc > 2 ? argv[2] : "build/agent-host/work") + "/engine";
    if (gData[0] != '/') gData = std::string(here) + "/" + gData;
    if (gWork[0] != '/') gWork = std::string(here) + "/" + gWork;

    // Deaths here are processes ending, which lose nothing that was written; see the note at the top.
    fs::setSyncing(false);

    json::Document vectors;
    std::string why;
    CHECK_IN(why, vectors.parse(readFile(gData + "/vectors.json"), &why));
    gPackageKey = testutil::key(vectors.root().find("keys").find("package").string(), "test package key", true);
    gOtherKey = testutil::key(vectors.root().find("keys").find("other").string(), "test other key", false);

    testInstallUpgradeRemove();
    testConfigReplacedWhenUntouched();
    testRollback();
    testHooks();
    testHooksFindReadOnly();
    testPlugin();
    testOwnPlugin();
    testStockFile();
    testOwnershipAndConflicts();
    testKeysAndChecks();
    testRecoverBasics();
    testAgentPackage();
    int agentDeaths = testAgentCrash();
    // A death at every point of every scenario, with moves as renames and as copies. A second death during the
    // recovery as well: with copies for four scenarios, or with NBT_TEST_ALL_PAIRS set for all nine both ways,
    // which takes two minutes.
    bool allPairs = getenv("NBT_TEST_ALL_PAIRS") != 0;
    int deaths = testCrashRecovery(false, allPairs ? 2 : 0);
    deaths += testCrashRecovery(true, allPairs ? 2 : 1);

    CHECK(fs::removeTree(gWork, &why));
    printf("test_engine: ok (recovered from %d deaths, and from %d during an update of the agent)\n", deaths, agentDeaths);
    return 0;
}
