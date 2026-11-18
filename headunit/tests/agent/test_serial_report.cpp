// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/serial_report.cpp: the start-up report, on a HostPlatform folder that stands in for a head
// unit. The files' contents are invented; only their names and shapes are the head unit's.
//
// usage: test_serial_report [work folder]

#include "../../agent/serial_report.hpp"

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "../../agent/engine.hpp"
#include "../../agent/fsutil.hpp"
#include "../../agent/host_platform.hpp"
#include "test_util.hpp"

using namespace nbt;

static std::string gWork = "build/bench/work";

class Lines : public SerialLog {
public:
    Lines() { pthread_mutex_init(&mu_, NULL); }
    ~Lines() { pthread_mutex_destroy(&mu_); }
    virtual void line(const std::string& l) {
        pthread_mutex_lock(&mu_);
        lines_.push_back(l);
        pthread_mutex_unlock(&mu_);
    }
    bool saw(const std::string& part) {
        pthread_mutex_lock(&mu_);
        bool found = false;
        for (size_t i = 0; i < lines_.size() && !found; ++i) found = lines_[i].find(part) != std::string::npos;
        pthread_mutex_unlock(&mu_);
        return found;
    }
    std::string text() {
        pthread_mutex_lock(&mu_);
        std::string all;
        for (size_t i = 0; i < lines_.size(); ++i) all += lines_[i] + "\n";
        pthread_mutex_unlock(&mu_);
        return all;
    }
    size_t count() {
        pthread_mutex_lock(&mu_);
        size_t n = lines_.size();
        pthread_mutex_unlock(&mu_);
        return n;
    }

private:
    pthread_mutex_t mu_;
    std::vector<std::string> lines_;
};

class Ask : public StackAsk {
public:
    Ask() : sent(true), attached(true), asked(0) {}
    virtual bool registrationSent() { return sent; }
    virtual bool askStackVersion() {
        ++asked;
        return attached;
    }
    volatile bool sent;
    bool attached;
    int asked;
};

static void write(const std::string& path, const std::string& text) {
    std::string why;
    CHECK_IN(why, fs::makeDirs(fs::parentOf(path), &why) && fs::writeFile(path, text, 0644, &why));
}

struct Head {
    std::string root;
    HostPlatform platform;
    Engine engine;
    explicit Head(const std::string& name)
        : root(gWork + "/" + name), platform(root), engine(&platform, std::vector<TrustedKey>(), "1.4.0") {
        std::string why;
        CHECK_IN(why, fs::removeTree(root, &why));
        CHECK_IN(why, platform.prepare(&why) && engine.init(&why));
    }
    std::string sys(const std::string& path) const { return platform.systemPath(path); }
};

static double monoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
    if (argc > 1) gWork = std::string(argv[1]) + "/serial_report";

    {   // everything there
        Head head("full");
        write(head.sys("/opt/conn/lib/libbssservice.so"), std::string(1234, 'b'));
        write(head.sys("/opt/conn/lib/libdsiservice.so"), std::string(56, 'd'));
        write(head.sys("/opt/sys/etc/nbt_version.txt"), "TEST_HU_0001\r\nsecond line\n");
        write(head.sys("/var/opt/conn/pss_config.cfg"),
              "#CFG V1.0 UTF-8;\n#Version: 9.99\n#Coded is: def, test\nOther.Key = 1\n"
              "Bluetooth.Services = RFCOMM, SPP\nMore = 2\n");
        write(head.sys("/dev/btspp1"), "");
        write(head.sys("/opt/aa/bin/demod"), "x");
        Lines log;
        Ask ask;
        SerialReport report(&head.platform, &head.engine.store(), &log, &ask);
        report.run();
        std::string t = log.text();
        CHECK_IN(t, log.saw("start-up report: the stack's version is asked for (VersionGet); its reply is logged when it comes"));
        CHECK(ask.asked == 1);
        CHECK_IN(t, log.saw("start-up report: the Bluetooth stack's libraries: libbssservice.so 1234 B, libdsiservice.so "
                            "56 B, libpal_core.so not there"));
        CHECK_IN(t, log.saw("start-up report: the head unit's software (/opt/sys/etc/nbt_version.txt): TEST_HU_0001\n") ||
                        log.saw("start-up report: the head unit's software (/opt/sys/etc/nbt_version.txt): TEST_HU_0001"));
        CHECK(!log.saw("second line") && !log.saw("\r"));
        CHECK_IN(t, log.saw("start-up report: /var/opt/conn/pss_config.cfg: #Coded is: def, test"));
        CHECK_IN(t, log.saw("start-up report: /var/opt/conn/pss_config.cfg: Bluetooth.Services = RFCOMM, SPP"));
        CHECK(!log.saw("Other.Key") && !log.saw("#Version"));
        CHECK_IN(t, log.saw("start-up report: serial nodes there, each someone's link: /dev/btspp1"));
        CHECK_IN(t, log.saw("start-up report: software put on by hand, as the checks find it: aa-fix problem (3 to "
                            "remove, 0 to move)"));
        CHECK_IN(t, log.saw("start-up report: done in ") && log.saw(" s of waiting for the registration"));
        // Nothing of it was changed.
        CHECK(fs::kindOf(head.sys("/opt/aa/bin/demod")) == fs::kFile);
    }
    {   // nothing there, and the car's Bluetooth service not attached: a line for each part
        Head head("empty");
        Lines log;
        Ask ask;
        ask.attached = false;
        SerialReport report(&head.platform, &head.engine.store(), &log, &ask);
        report.run();
        std::string t = log.text();
        CHECK_IN(t, log.saw("the stack's version cannot be asked for: the car's Bluetooth service is not attached"));
        CHECK_IN(t, log.saw("libbssservice.so not there, libdsiservice.so not there, libpal_core.so not there"));
        CHECK_IN(t, log.saw("start-up report: /opt/sys/etc/nbt_version.txt could not be read: "));
        CHECK_IN(t, log.saw("start-up report: /var/opt/conn/pss_config.cfg could not be read: "));
        CHECK_IN(t, log.saw("start-up report: no serial node is there (/dev/btspp0 to 7)"));
        CHECK_IN(t, log.saw("as the checks find it: aa-fix ok"));
        CHECK(log.saw("done in "));
    }
    {   // a config with neither line
        Head head("plain");
        write(head.sys("/var/opt/conn/pss_config.cfg"), "Some.Key = 1\n");
        Lines log;
        Ask ask;
        SerialReport report(&head.platform, &head.engine.store(), &log, &ask);
        report.run();
        CHECK_IN(log.text(), log.saw("/var/opt/conn/pss_config.cfg has no line of the coding or of Bluetooth.Services"));
    }
    {   // on its thread: it waits for the registration, and the time limit leaves the rest out
        Head head("thread");
        Lines log;
        Ask ask;
        ask.sent = false;
        SerialReportConfig config;
        config.waitMs = 200;
        config.limitMs = 100;
        SerialReport report(&head.platform, &head.engine.store(), &log, &ask, config);
        CHECK(report.start());
        double end = monoNow() + 3.0;
        while (monoNow() < end && !log.saw("done in")) usleep(10000);
        report.finish();
        std::string t = log.text();
        CHECK_IN(t, log.saw("start-up report: the registration had not gone to the car's Bluetooth service after 0.2 s; "
                            "the report goes on without it"));
        CHECK_IN(t, log.saw("start-up report: the rest, from the stack's version, is left out: the report's 0.1 s are over"));
        CHECK(ask.asked == 0 && !log.saw("libraries"));
    }
    {   // the registration goes out while it waits: the report follows at once
        Head head("waits");
        Lines log;
        Ask ask;
        ask.sent = false;
        SerialReport report(&head.platform, &head.engine.store(), &log, &ask);
        CHECK(report.start());
        usleep(150000);
        CHECK(log.count() == 0);
        ask.sent = true;
        double end = monoNow() + 3.0;
        while (monoNow() < end && !log.saw("done in")) usleep(10000);
        CHECK_IN(log.text(), log.saw("done in") && ask.asked == 1 && !log.saw("had not gone"));
        report.finish();
    }
    {   // a stop while it waits: it ends at once, having said nothing
        Head head("stop");
        Lines log;
        Ask ask;
        ask.sent = false;
        SerialReport report(&head.platform, &head.engine.store(), &log, &ask);
        CHECK(report.start());
        usleep(100000);
        double before = monoNow();
        report.finish();
        CHECK(monoNow() - before < 1.0 && log.count() == 0);
    }

    printf("test_serial_report: ok\n");
    return 0;
}
