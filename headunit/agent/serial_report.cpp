// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The start-up report: see serial_report.hpp.
#include "serial_report.hpp"

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <vector>

#include "checks.hpp"
#include "fsutil.hpp"

namespace nbt {

namespace {

double monoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

// Printable ASCII only, `most` characters at most.
std::string shown(const std::string& s, size_t most) {
    std::string t;
    for (size_t i = 0; i < s.size() && t.size() < most; ++i) {
        char c = s[i];
        if (c == '\r') continue;
        t += (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    return t;
}

std::string number(unsigned long long n) {
    char text[32];
    snprintf(text, sizeof text, "%llu", n);
    return text;
}

std::string seconds(unsigned ms) {
    char text[32];
    snprintf(text, sizeof text, "%g s", ms / 1000.0);
    return text;
}

const char* const kLibraries[] = {"/opt/conn/lib/libbssservice.so", "/opt/conn/lib/libdsiservice.so",
                                  "/opt/conn/lib/libpal_core.so"};
const char* const kVersionFile = "/opt/sys/etc/nbt_version.txt";
const char* const kStackConfig = "/var/opt/conn/pss_config.cfg";
const size_t kMostFile = 256u << 10;   // what is read of a file at most
const size_t kMostLine = 300;          // what the log is given of a line at most

}  // namespace

SerialReport::SerialReport(Platform* platform, const Store* store, SerialLog* log, StackAsk* ask,
                           const SerialReportConfig& config)
    : platform_(platform), store_(store), log_(log), ask_(ask), config_(config), hasThread_(false), stop_(false),
      cut_(false) {
    pthread_mutex_init(&mu_, NULL);
}

SerialReport::~SerialReport() {
    finish();
    pthread_mutex_destroy(&mu_);
}

void SerialReport::say(const std::string& line) { log_->line("start-up report: " + line); }

void* SerialReport::threadMain(void* self) {
    static_cast<SerialReport*>(self)->run();
    return NULL;
}

bool SerialReport::start() {
    if (hasThread_) return true;
    hasThread_ = pthread_create(&thread_, NULL, &SerialReport::threadMain, this) == 0;
    if (!hasThread_) say("not made: its thread could not be started");
    return hasThread_;
}

void SerialReport::finish() {
    pthread_mutex_lock(&mu_);
    stop_ = true;
    pthread_mutex_unlock(&mu_);
    if (hasThread_) pthread_join(thread_, NULL);
    hasThread_ = false;
}

bool SerialReport::stopped() {
    pthread_mutex_lock(&mu_);
    bool s = stop_;
    pthread_mutex_unlock(&mu_);
    return s;
}

// Whether the part `part` is to be left out: the agent is stopping, or the time for the report is over.
bool SerialReport::late(double startedAt, const char* part) {
    if (stopped()) return true;
    if ((monoNow() - startedAt) * 1000.0 < (double)config_.limitMs) return false;
    if (!cut_) say(std::string("the rest, from ") + part + ", is left out: the report's " + seconds(config_.limitMs) +
                   " are over");
    cut_ = true;
    return true;
}

void SerialReport::run() {
    double startedAt = monoNow();
    // After the registration: the report must not hold up what a phone needs first.
    while (!stopped() && !ask_->registrationSent() && (monoNow() - startedAt) * 1000.0 < (double)config_.waitMs)
        usleep(50000);
    if (stopped()) return;
    double from = monoNow();
    if (!ask_->registrationSent()) {
        say("the registration had not gone to the car's Bluetooth service after " + seconds(config_.waitMs) +
            "; the report goes on without it");
    }
    if (!late(startedAt, "the stack's version")) {
        if (ask_->askStackVersion()) say("the stack's version is asked for (VersionGet); its reply is logged when it comes");
        else say("the stack's version cannot be asked for: the car's Bluetooth service is not attached");
    }
    if (!late(startedAt, "the libraries")) libraries();
    if (!late(startedAt, "the software version")) firmware();
    if (!late(startedAt, "the stack's options")) coding();
    if (!late(startedAt, "the serial nodes")) nodes();
    if (!late(startedAt, "the checks")) checks();
    if (stopped()) return;
    char took[96];
    snprintf(took, sizeof took, "done in %.2f s, after %.1f s of waiting for the registration", monoNow() - from,
             from - startedAt);
    say(took);
}

void SerialReport::libraries() {
    std::string line = "the Bluetooth stack's libraries:";
    for (size_t i = 0; i < sizeof kLibraries / sizeof kLibraries[0]; ++i) {
        std::string path = platform_->systemPath(kLibraries[i]);
        struct stat st;
        std::string name = std::string(kLibraries[i]).substr(std::string(kLibraries[i]).rfind('/') + 1);
        line += (i ? ", " : " ") + name + " ";
        line += stat(path.c_str(), &st) == 0 ? number((unsigned long long)st.st_size) + " B" : std::string("not there");
    }
    say(line);
}

void SerialReport::firmware() {
    std::string text, why;
    if (!fs::readFile(platform_->systemPath(kVersionFile), kMostFile, &text, &why)) {
        say(std::string(kVersionFile) + " could not be read: " + why);
        return;
    }
    size_t end = text.find('\n');
    say(std::string("the head unit's software (") + kVersionFile + "): " +
        shown(text.substr(0, end == std::string::npos ? text.size() : end), kMostLine));
}

void SerialReport::coding() {
    std::string text, why;
    if (!fs::readFile(platform_->systemPath(kStackConfig), kMostFile, &text, &why)) {
        say(std::string(kStackConfig) + " could not be read: " + why);
        return;
    }
    unsigned found = 0;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.compare(0, 10, "#Coded is:") != 0 && line.find("Bluetooth.Services") == std::string::npos) continue;
        say(std::string(kStackConfig) + ": " + shown(line, kMostLine));
        if (++found == 4) break;   // a file of that shape has one of each
    }
    if (found == 0) say(std::string(kStackConfig) + " has no line of the coding or of Bluetooth.Services");
}

void SerialReport::nodes() {
    std::string there;
    for (int i = 0; i < 8; ++i) {
        char path[32];
        snprintf(path, sizeof path, "/dev/btspp%d", i);
        // The agent's own process goes past its gate: this sees the stack's nodes only.
        if (access(platform_->systemPath(path).c_str(), F_OK) == 0) there += (there.empty() ? "" : ", ") + std::string(path);
    }
    say(there.empty() ? std::string("no serial node is there (/dev/btspp0 to 7)")
                      : "serial nodes there, each someone's link: " + there);
}

void SerialReport::checks() {
    CheckContext context(platform_, store_);
    std::string line;
    for (size_t i = 0; i < checkCount(); ++i) {
        const Check& check = checkAt(i);
        if (check.fix != kFixRemove) continue;   // the ones about software put on by hand
        CheckFinding finding;
        runCheck(check, context, &finding);
        std::string what = check.id + std::string(" ") + checkStateName(finding.state);
        if (finding.state == kCheckProblem)
            what += " (" + number(finding.entries.size()) + " to remove, " + number(finding.moves.size()) + " to move" +
                    (finding.fixable ? "" : ", no fix offered") + ")";
        else if (finding.state == kCheckUnknown)
            what += " (" + shown(finding.why, kMostLine) + ")";
        line += (line.empty() ? "" : "; ") + what;
    }
    say("software put on by hand, as the checks find it: " + (line.empty() ? std::string("no such check") : line));
}

}  // namespace nbt
