// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SERIAL_REPORT_HPP
#define NBT_AGENT_SERIAL_REPORT_HPP

// The start-up report: what the head unit says of its Bluetooth when the agent starts with Bluetooth serial, for
// whoever reads the log later. Lines of "start-up report: ...", in this order:
//   * the stack's version strings: VersionGet is sent, and its reply is logged when it comes (spp_service.hpp)
//   * the sizes of the stack's three libraries in /opt/conn/lib, which tell its build apart
//   * the first line of /opt/sys/etc/nbt_version.txt: the head unit's software
//   * the `#Coded is:` and `Bluetooth.Services` lines of /var/opt/conn/pss_config.cfg: the stack's options as
//     the car's coding chose them
//   * which serial nodes /dev/btspp0 to 7 are there
//   * what the compatibility checks for software put on by hand find (checks.hpp): their ids, and how much each
//     found, by their read-only detect
//   * how long the report took
// Which program holds a node open is not in it: on QNX 6.5 the only way is to ask every process for its open
// files (`pidin fd`), which sends a message to every server behind each one of them.
//
// It runs on a thread of its own, once the agent is up and its registration has been sent (or has not, after a
// while), within a time limit, and nothing in it stops or delays the agent: a part that fails is one line. Every
// path goes through Platform::systemPath(), so that a test points it at a folder.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <pthread.h>

#include <string>

#include "platform.hpp"
#include "serial_log.hpp"
#include "store.hpp"

namespace nbt {

// The part of the report that is asked of the car's Bluetooth service: the serial listener.
class StackAsk {
public:
    virtual ~StackAsk() {}
    // Whether the agent's registration has gone to the car's Bluetooth service: the report waits for that.
    virtual bool registrationSent() = 0;
    // Sends VersionGet; false if it cannot be sent now.
    virtual bool askStackVersion() = 0;
};

struct SerialReportConfig {
    unsigned waitMs;    // how long the registration is waited for, at most
    unsigned limitMs;   // the whole report, the wait included: a part that would start later is left out
    SerialReportConfig() : waitMs(15000), limitMs(30000) {}
};

class SerialReport {
public:
    // All of them outlive the report's thread: finish() waits for it.
    SerialReport(Platform* platform, const Store* store, SerialLog* log, StackAsk* ask,
                 const SerialReportConfig& config = SerialReportConfig());
    ~SerialReport();

    // Starts the report's thread. False, with a line in the log, if it cannot.
    bool start();
    // Asks the report to end soon, and waits for its thread.
    void finish();
    // The report on the caller's thread: what the thread runs.
    void run();

private:
    SerialReport(const SerialReport&);
    SerialReport& operator=(const SerialReport&);
    static void* threadMain(void* self);
    bool stopped();
    bool late(double startedAt, const char* part);
    void say(const std::string& line);
    void libraries();
    void firmware();
    void coding();
    void nodes();
    void checks();

    Platform* platform_;
    const Store* store_;
    SerialLog* log_;
    StackAsk* ask_;
    SerialReportConfig config_;
    pthread_t thread_;
    bool hasThread_;
    pthread_mutex_t mu_;
    bool stop_;
    bool cut_;   // a part was left out for the time limit: said once
};

}  // namespace nbt

#endif
