// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The serial services other programs register, and the removal of one that was left behind: see
// serial_services.hpp.
#include "serial_services.hpp"

#include <stdio.h>
#include <time.h>

#include <map>
#include <set>

#include "checks.hpp"
#include "fsutil.hpp"

namespace nbt {

namespace {

const char* const kFileName = "serial-services";
const size_t kMaxFileBytes = 65536;

double monoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

class Lock {
public:
    explicit Lock(pthread_mutex_t* mu) : mu_(mu) { pthread_mutex_lock(mu_); }
    ~Lock() { pthread_mutex_unlock(mu_); }

private:
    Lock(const Lock&);
    Lock& operator=(const Lock&);
    pthread_mutex_t* mu_;
};

// The words of a line, which spaces and tabs separate.
std::vector<std::string> wordsOf(const std::string& line) {
    std::vector<std::string> words;
    std::string word;
    for (size_t i = 0; i <= line.size(); ++i) {
        char c = i < line.size() ? line[i] : ' ';
        if (c == ' ' || c == '\t' || c == '\r') {
            if (!word.empty()) words.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    return words;
}

std::string number(unsigned long value) {
    char text[32];
    snprintf(text, sizeof text, "%lu", value);
    return text;
}

std::string list(const std::set<std::string>& names) {
    std::string text;
    for (std::set<std::string>::const_iterator it = names.begin(); it != names.end(); ++it) {
        text += (text.empty() ? "" : ", ") + *it;
    }
    return text;
}

}  // namespace

void readSerialServicesFile(const std::string& path, std::vector<SerialServiceOwner>* out,
                            std::vector<std::string>* complaints) {
    if (fs::kindOf(path) == fs::kNone) return;
    std::string text, why;
    if (!fs::readFile(path, kMaxFileBytes, &text, &why)) {
        complaints->push_back(path + " could not be read: " + why);
        return;
    }
    size_t lineNo = 0;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        ++lineNo;
        std::vector<std::string> words = wordsOf(line);
        if (words.empty() || words[0][0] == '#') continue;
        if (words.size() != 2 || !validSerialServiceName(words[0]) || !validProgramName(words[1])) {
            complaints->push_back(path + ", line " + number((unsigned long)lineNo) +
                                  ": not a service's name and its program's file name, so it is passed over");
            continue;
        }
        SerialServiceOwner owner;
        owner.name = words[0];
        owner.program = words[1];
        owner.from = kFileName;
        out->push_back(owner);
    }
}

SerialServices::SerialServices(const Platform* platform, const Store* store, SerialLog* log)
    : platform_(platform), store_(store), log_(log), remover_(0), lastSweep_(0), sweepEveryS_(kSerialSweepEveryS) {
    pthread_mutex_init(&mu_, NULL);
}

SerialServices::~SerialServices() { pthread_mutex_destroy(&mu_); }

void SerialServices::setRemover(SerialServiceRemover* remover) {
    std::vector<std::string> names;
    {
        Lock lock(&mu_);
        remover_ = remover;
        names = namesLocked();
    }
    if (remover != 0) remover->serialServicesDeclared(names);
}

std::vector<std::string> SerialServices::namesLocked() const {
    std::vector<std::string> names;
    for (size_t i = 0; i < declared_.size(); ++i) {
        bool twice = false;
        for (size_t k = 0; k < names.size(); ++k) twice = twice || names[k] == declared_[i].name;
        if (!twice) names.push_back(declared_[i].name);
    }
    return names;
}

void SerialServices::reload() {
    std::vector<SerialServiceOwner> owners;
    std::vector<InstalledPackage> installed;
    std::string why;
    if (!store_->loadAll(&installed, &why)) {
        // What was known stays: a list that is short of a service only leaves that service alone.
        log_->line("the installed packages could not be read, so the serial services they declare are as before: " + why);
        return;
    }
    for (size_t i = 0; i < installed.size(); ++i) {
        const Manifest& m = installed[i].manifest;
        for (size_t k = 0; k < m.serialServices.size(); ++k) {
            SerialServiceOwner owner;
            owner.name = m.serialServices[k].name;
            owner.program = m.serialServices[k].program;
            owner.from = m.id;
            owners.push_back(owner);
        }
    }
    std::vector<std::string> complaints;
    readSerialServicesFile(platform_->stateDir() + "/" + kFileName, &owners, &complaints);
    for (size_t i = 0; i < complaints.size(); ++i) log_->line(complaints[i]);
    setDeclared(owners);
}

void SerialServices::setDeclared(const std::vector<SerialServiceOwner>& declared) {
    SerialServiceRemover* remover;
    std::vector<std::string> names;
    {
        Lock lock(&mu_);
        declared_ = declared;
        remover = remover_;
        names = namesLocked();
    }
    if (remover != 0) remover->serialServicesDeclared(names);
}

std::vector<SerialServiceOwner> SerialServices::declared() {
    Lock lock(&mu_);
    return declared_;
}

std::vector<std::string> SerialServices::programs() {
    std::set<std::string> unique;
    {
        Lock lock(&mu_);
        for (size_t i = 0; i < declared_.size(); ++i) unique.insert(declared_[i].program);
    }
    return std::vector<std::string>(unique.begin(), unique.end());
}

SerialServices::Readers SerialServices::readers() {
    std::vector<std::string> names = programs();   // with the lock, so not under it here
    if (names.empty()) return kNoneDeclared;
    for (size_t i = 0; i < names.size(); ++i) {
        if (platform_->programState(names[i]) != kProgramNotRunning) return kSomeMayRun;
    }
    return kNoneRunning;
}

ProgramState SerialServices::handInstalledRunning(std::string* names) {
    std::set<std::string> found;
    ProgramState state = kProgramNotRunning;
    for (size_t i = 0; i < checkCount(); ++i) {
        const Check& check = checkAt(i);
        if (check.stopUnder == 0) continue;
        std::vector<RunningProgram> running;
        ProgramState s = platform_->programsUnder(platform_->systemPath(check.stopUnder), &running);
        if (s == kProgramRunning) {
            state = kProgramRunning;
            for (size_t k = 0; k < running.size(); ++k) found.insert(programName(running[k].path.c_str()));
        } else if (s == kProgramUnknown && state != kProgramRunning) {
            state = kProgramUnknown;
        }
    }
    *names = list(found);
    return state;
}

unsigned SerialServices::sweep(const char* why) { return sweepOf(declared(), why); }

std::vector<std::string> SerialServices::namesRunning() {
    std::vector<SerialServiceOwner> owners = declared();
    std::map<std::string, ProgramState> programs;   // each program is looked for once
    std::vector<std::string> names;
    for (size_t i = 0; i < owners.size(); ++i) {
        std::map<std::string, ProgramState>::const_iterator it = programs.find(owners[i].program);
        ProgramState state = it != programs.end() ? it->second : platform_->programState(owners[i].program);
        programs[owners[i].program] = state;
        if (state != kProgramRunning) continue;
        bool twice = false;
        for (size_t k = 0; k < names.size(); ++k) twice = twice || names[k] == owners[i].name;
        if (twice) continue;
        names.push_back(owners[i].name);
        Lock lock(&mu_);
        keptSaid_.erase(owners[i].name);
    }
    return names;
}

unsigned SerialServices::sweepIfDue(const char* why) {
    {
        Lock lock(&mu_);
        if (lastSweep_ != 0 && monoNow() - lastSweep_ < sweepEveryS_) return 0;
    }
    return sweep(why);
}

void SerialServices::setSweepEvery(double seconds) {
    Lock lock(&mu_);
    sweepEveryS_ = seconds;
}

void SerialServices::programsStopped(const Manifest& manifest) {
    std::vector<SerialServiceOwner> owners;
    for (size_t i = 0; i < manifest.serialServices.size(); ++i) {
        SerialServiceOwner owner;
        owner.name = manifest.serialServices[i].name;
        owner.program = manifest.serialServices[i].program;
        owner.from = manifest.id;
        owners.push_back(owner);
    }
    // What the installed version declared, which an upgrade may have changed.
    std::vector<SerialServiceOwner> known = declared();
    for (size_t i = 0; i < known.size(); ++i) {
        if (known[i].from == manifest.id) owners.push_back(known[i]);
    }
    if (!owners.empty()) sweepOf(owners, "a step has stopped the programs of " + manifest.id);
}

unsigned SerialServices::sweepOf(const std::vector<SerialServiceOwner>& owners, const std::string& why) {
    SerialServiceRemover* remover;
    {
        Lock lock(&mu_);
        remover = remover_;
        lastSweep_ = monoNow();
    }
    if (remover == 0 || owners.empty()) return 0;

    // A service is left alone if any program declared for it runs, or may run.
    typedef std::map<std::string, std::set<std::string> > ByService;
    std::map<std::string, ProgramState> programs;   // each program is looked for once
    ByService gone, unknown;                        // the programs declared for a service, by what was found
    std::set<std::string> answered;
    for (size_t i = 0; i < owners.size(); ++i) {
        const SerialServiceOwner& o = owners[i];
        std::map<std::string, ProgramState>::const_iterator it = programs.find(o.program);
        ProgramState state = it != programs.end() ? it->second : platform_->programState(o.program);
        programs[o.program] = state;
        if (state == kProgramRunning) answered.insert(o.name);
        else (state == kProgramUnknown ? unknown : gone)[o.name].insert(o.program);
    }

    unsigned asked = 0;
    // What a name holds while its program runs is that program's, and not the stand-in, whatever it was before.
    for (std::set<std::string>::const_iterator it = answered.begin(); it != answered.end(); ++it) {
        remover->serialServiceRuns(*it);
        Lock lock(&mu_);
        keptSaid_.erase(*it);
    }
    for (ByService::const_iterator it = unknown.begin(); it != unknown.end(); ++it) {
        if (answered.count(it->first) != 0) continue;
        log_->line("serial service '" + it->first + "': whether its program (" + list(it->second) +
                   ") is running cannot be told, so it is left alone");
    }
    for (ByService::const_iterator it = gone.begin(); it != gone.end(); ++it) {
        if (answered.count(it->first) != 0 || unknown.count(it->first) != 0) continue;
        SerialServiceRemover::Replace r = remover->replaceSerialService(it->first);
        bool say = true;
        {
            Lock lock(&mu_);
            if (r == SerialServiceRemover::kKeptOurs) say = keptSaid_.insert(it->first).second;
            else keptSaid_.erase(it->first);
        }
        if (r == SerialServiceRemover::kReplacing) ++asked;
        if (!say) continue;
        std::string head = "serial service '" + it->first + "': its program (" + list(it->second) + ") is not running, ";
        if (r == SerialServiceRemover::kReplacing)
            log_->line(head + "so what is registered under its name, which may be a registration it left behind, is "
                              "being removed, and the agent's stand-in registered in its place (" + why + ")");
        else if (r == SerialServiceRemover::kKeptOurs)
            log_->line(head + "and its name holds the agent's stand-in, which is left as it is (" + why + "); this is "
                              "said once until that changes");
        else
            log_->line(head + "so what is registered under its name would be replaced by the agent's stand-in, but the "
                              "car's Bluetooth service cannot be asked now (" + why + ")");
    }
    return asked;
}

}  // namespace nbt
