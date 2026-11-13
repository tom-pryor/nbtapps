// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/log_files.cpp: which files a package's `logs` entries list on a folder that stands in for a
// head unit, and which they do not (links, hidden files, the agent's own folders, folders reached through a link);
// the agent's own logs; and opening a file of a listing that has changed since.
//
// usage: test_log_files [work folder]     (default build/agent-host/work)

#include "../../agent/log_files.hpp"

#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "../../agent/fsutil.hpp"
#include "../../agent/host_platform.hpp"
#include "test_util.hpp"

using namespace nbt;

static void put(const std::string& path, const std::string& text, long modified) {
    std::string why;
    CHECK_IN(why, fs::makeDirs(fs::parentOf(path), &why));
    FILE* f = fopen(path.c_str(), "wb");
    CHECK_IN(path, f != 0);
    CHECK(fwrite(text.data(), 1, text.size(), f) == text.size());
    fclose(f);
    struct timeval times[2];
    times[0].tv_sec = times[1].tv_sec = modified;
    times[0].tv_usec = times[1].tv_usec = 0;
    CHECK_IN(path, utimes(path.c_str(), times) == 0);
}

static void symlinkAt(const std::string& target, const std::string& path) {
    std::string why;
    CHECK_IN(why, fs::makeDirs(fs::parentOf(path), &why) && fs::makeLink(target, path, &why));
}

// The paths of a listing, separated by spaces.
static std::string paths(const std::vector<LogFile>& files) {
    std::string out;
    for (size_t i = 0; i < files.size(); ++i) out += (i == 0 ? "" : " ") + files[i].path;
    return out;
}

static Manifest manifest(const char* id, const char* a, const char* b = 0, const char* c = 0, const char* d = 0) {
    Manifest m;
    m.id = id;
    m.version = "1.0.0";
    const char* all[] = {a, b, c, d};
    for (size_t i = 0; i < 4; ++i) {
        if (all[i] != 0) m.logs.push_back(all[i]);
    }
    return m;
}

static std::vector<LogFile> listed(const Manifest& m, const Platform& platform) {
    Store store(&platform);
    std::vector<LogFile> files;
    listPackageLogs(m, store, platform, &files);
    return files;
}

// A head unit whose state is under /var, where an absolute entry may lead: as on a car, where it is /var/opt/nbtapp.
class StateUnderVar : public HostPlatform {
public:
    explicit StateUnderVar(const std::string& root) : HostPlatform(root), root_(root) {}
    virtual std::string stateDir() const { return root_ + "/system/var/state"; }
    virtual std::string stagingDir() const { return root_ + "/system/var/staging"; }
    virtual std::string runDir() const { return root_ + "/system/tmp/run"; }

private:
    std::string root_;
};

int main(int argc, char** argv) {
    std::string work = std::string(argc > 1 ? argv[1] : "build/agent-host/work") + "/log_files";
    char here[4096];
    CHECK(getcwd(here, sizeof here) != 0);
    if (work[0] != '/') work = std::string(here) + "/" + work;
    std::string why;
    CHECK(fs::removeTree(work, &why) && fs::makeDirs(work, &why));

    // 1. The last part of an entry: * any characters, ? one, and a dot at the start only for a pattern that has one.
    CHECK(matchName("*.log", "a.log") && matchName("*.log", ".log.log") == false && !matchName("*.log", "a.txt"));
    CHECK(matchName("crash-?.txt", "crash-1.txt") && !matchName("crash-?.txt", "crash-12.txt") && !matchName("crash-?.txt", "crash-.txt"));
    CHECK(matchName("crash-?.txt", "crash-\xc3\xa9.txt"));   // one character of two bytes
    CHECK(matchName("a*b*c", "aXbYc") && matchName("a*b*c", "abc") && !matchName("a*b*c", "acb") && matchName("*", "x"));
    CHECK(matchName(".*", ".hidden") && !matchName("*", ".hidden") && matchName(".hidden", ".hidden") && matchName("x", "x"));
    CHECK(!matchName("x", "xy") && !matchName("xy", "x") && matchName("**y", "y") && matchName("*y*", "aya"));

    // 2. ${tmp} named or not.
    CHECK(namesTmp(manifest("a.b", "${tmp}/x")) && !namesTmp(manifest("a.b", "${data}/x", "/tmp/x")));

    // 3. Each place an entry may name, newest first, in the manifest's form, regular files only.
    {
        std::string root = work + "/places";
        HostPlatform platform(root);
        CHECK_IN(why, platform.prepare(&why));
        const char* id = "org.nbtapp.logger";
        std::string data = platform.dataDir(id), tmp = platform.tmpDir(id), app = platform.appDir(id);
        put(data + "/log/a.log", "aaaa", 1000);
        put(data + "/log/b.log", "bb", 3000);
        put(data + "/log/notes.txt", "not a log", 5000);
        put(data + "/log/.hidden.log", "hidden", 6000);
        symlinkAt("a.log", data + "/log/link.log");                     // a link to a log is not listed
        CHECK_IN(why, fs::makeDirs(data + "/log/folder.log", &why));     // nor a folder that matches
        put(tmp + "/crash-1.txt", "crash", 2000);
        put(app + "/state.txt", "state", 4000);
        put(root + "/system/tmp/logger.log", "fixed place", 1500);
        Manifest m = manifest(id, "${data}/log/*.log", "${tmp}/crash-?.txt", "${app}/state.txt", "/tmp/logger.log");
        std::vector<LogFile> files = listed(m, platform);
        CHECK_IN(paths(files), paths(files) == "${app}/state.txt ${data}/log/b.log ${tmp}/crash-1.txt /tmp/logger.log ${data}/log/a.log");
        CHECK(files[1].real == data + "/log/b.log" && files[1].size == 2 && files[1].modified == 3000);
        CHECK(files[3].real == root + "/system/tmp/logger.log" && files[3].size == 11);

        // A file two entries name is listed once.
        files = listed(manifest(id, "${data}/log/*.log", "${data}/log/a.log"), platform);
        CHECK_IN(paths(files), paths(files) == "${data}/log/b.log ${data}/log/a.log");
        // A name with no wildcard is that file, a hidden one included; one that is not there is not listed.
        files = listed(manifest(id, "${data}/log/.hidden.log", "${data}/log/gone.log", "/var/log/none.log"), platform);
        CHECK_IN(paths(files), paths(files) == "${data}/log/.hidden.log");
        // A text that is not a log path, as an old record might hold, lists nothing.
        CHECK(listed(manifest(id, "${data}/../state/phones.json", "/etc/passwd"), platform).empty());

        // A folder on the way that is a link leads nowhere, inside the package's folder or as the folder itself.
        symlinkAt(platform.stateDir(), data + "/linked");
        put(platform.stateDir() + "/phones.json", "keys", 7000);
        CHECK(listed(manifest(id, "${data}/linked/*", "${data}/linked/phones.json"), platform).empty());
        const char* other = "org.nbtapp.other";
        symlinkAt(platform.stateDir(), platform.dataDir(other));
        CHECK(listed(manifest(other, "${data}/*", "${data}/phones.json"), platform).empty());
        // An absolute entry through a link, to the state.
        symlinkAt(platform.stateDir(), root + "/system/tmp/state");
        CHECK(listed(manifest(id, "/tmp/state/*", "/tmp/state/phones.json"), platform).empty());
    }

    // 4. An absolute entry whose folder is the agent's own lists nothing, whatever its text: the state, staging and
    //    the run folder, here where they are under /tmp and /var as on a car.
    {
        std::string root = work + "/state-under-var";
        StateUnderVar platform(root);
        CHECK_IN(why, platform.prepare(&why));
        put(platform.stateDir() + "/phones.json", "keys", 1000);
        put(platform.stagingDir() + "/x.log", "staged", 1000);
        put(platform.runDir() + "/agent.log", "run", 1000);
        put(root + "/system/var/log/fine.log", "fine", 1000);
        Manifest m = manifest("org.nbtapp.logger", "/var/state/*", "/var/staging/x.log", "/tmp/run/*", "/var/log/*.log");
        std::vector<LogFile> files = listed(m, platform);
        CHECK_IN(paths(files), paths(files) == "/var/log/fine.log");
    }

    // 5. More than 64: the newest 64.
    {
        std::string root = work + "/many";
        HostPlatform platform(root);
        CHECK_IN(why, platform.prepare(&why));
        for (int i = 0; i < 70; ++i) {
            char name[32];
            snprintf(name, sizeof name, "/log/%02d.log", i);
            put(platform.dataDir("org.nbtapp.logger") + name, "x", 1000 + i);
        }
        std::vector<LogFile> files = listed(manifest("org.nbtapp.logger", "${data}/log/*.log"), platform);
        CHECK(files.size() == kMaxLogFiles && files[0].path == "${data}/log/69.log" && files[63].path == "${data}/log/06.log");
    }

    // 6. The agent's own: those that are there, by their bare names; a link is not one.
    {
        std::string root = work + "/agent";
        HostPlatform platform(root);
        CHECK_IN(why, platform.prepare(&why));
        std::string own = root + "/run/log/agent.log";
        put(own, "now", 3000);
        put(own + ".2", "before that", 1000);
        put(platform.agentHome() + "/log/trial.log", "a try", 2000);
        std::vector<LogFile> files;
        listAgentLogs(own, platform, &files);
        CHECK_IN(paths(files), paths(files) == "agent.log trial.log agent.log.2");
        CHECK(files[2].real == own + ".2" && files[2].size == 11);
        // No file named in NBTAGENT_LOG: the trial log alone. And a trial log that is a link is left out.
        listAgentLogs("", platform, &files);
        CHECK_IN(paths(files), paths(files) == "trial.log");
        CHECK(fs::removeFile(platform.agentHome() + "/log/trial.log", &why));
        symlinkAt(own, platform.agentHome() + "/log/trial.log");
        listAgentLogs("", platform, &files);
        CHECK(files.empty());
    }

    // 7. Opening a file of a listing: the file as it is now, if it is still the regular file at that path.
    {
        std::string root = work + "/open";
        HostPlatform platform(root);
        CHECK_IN(why, platform.prepare(&why));
        std::string data = platform.dataDir("org.nbtapp.logger");
        put(data + "/a.log", "0123456789", 1000);
        std::vector<LogFile> files = listed(manifest("org.nbtapp.logger", "${data}/*.log"), platform);
        CHECK(files.size() == 1);
        int fd = -1;
        uint64_t size = 0;
        // Grown since it was listed: the size of now.
        put(data + "/a.log", "0123456789abc", 1000);
        CHECK_IN(why, openLogFile(files[0], &fd, &size, &why) && fd >= 0 && size == 13);
        char buffer[32];
        CHECK(read(fd, buffer, sizeof buffer) == 13);
        close(fd);
        // Gone, a link now, or a FIFO now: not opened.
        CHECK(fs::removeFile(data + "/a.log", &why));
        CHECK(!openLogFile(files[0], &fd, &size, &why) && !why.empty());
        put(data + "/b.log", "other", 1000);
        symlinkAt("b.log", data + "/a.log");
        CHECK(!openLogFile(files[0], &fd, &size, &why));
        CHECK(fs::removeFile(data + "/a.log", &why));
        CHECK(mkfifo((data + "/a.log").c_str(), 0600) == 0);
        CHECK(!openLogFile(files[0], &fd, &size, &why));
    }

    fs::removeTree(work, 0);
    printf("test_log_files: ok\n");
    return 0;
}
