// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/serial_link.cpp: a link on a serial node, the listener that opens a link's node as soon as
// it is there, and an Agent that phones reach through it.
//
// The car's parts are played here. A serial node is a pseudo-terminal: its far end is the phone, and a symbolic
// link to its near end, made when "the link comes up", is the node the listener finds. The Bluetooth service is a
// GapClient of the test's own, which takes the registration and says what a phone asked for, and the gate is a
// NodeGate that only notes what was done to it. Where a test needs the node's queue as the car's stack keeps it (the
// phone's bytes held back until a read, a put-back), the listener is given the test's calls on the node in place
// of the platform's (heldCalls()). All names are invented.
//
// Unlike the agent's other tests this one runs on real time, with the listener's and the links' threads: it waits
// for what should happen, up to a few seconds each, and for a short while where nothing should.
//
// usage: test_serial [work folder]

#include "../../agent/serial_link.hpp"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include "../../agent/agent.hpp"
#include "../../agent/frame.hpp"
#include "../../agent/fsutil.hpp"
#include "../../agent/host_platform.hpp"
#include "../../agent/process.hpp"
#include "test_util.hpp"

using namespace nbt;
using namespace nbt::btspp;

static std::string gWork = "build/bench/work/serial";

static double monoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void pause(int ms) { usleep((useconds_t)ms * 1000); }

static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// ---- what was done, in order, by whichever thread did it ----------------------------------------------------------

class Trace : public SerialLog {
public:
    Trace() { pthread_mutex_init(&mu_, NULL); }
    ~Trace() { pthread_mutex_destroy(&mu_); }

    virtual void line(const std::string& line) { add("log: " + line); }
    void add(const std::string& what) {
        pthread_mutex_lock(&mu_);
        items_.push_back(what);
        pthread_mutex_unlock(&mu_);
    }
    // The place of the first item that holds `part`; below 0 if none does.
    int at(const std::string& part) {
        pthread_mutex_lock(&mu_);
        int found = -1;
        for (size_t i = 0; i < items_.size() && found < 0; ++i)
            if (has(items_[i], part)) found = (int)i;
        pthread_mutex_unlock(&mu_);
        return found;
    }
    bool saw(const std::string& part) { return at(part) >= 0; }
    // Whether a line of the log holds `part`: the trace's own items are not the log.
    bool logged(const std::string& part) { return loggedCount(part) > 0; }
    unsigned loggedCount(const std::string& part) {
        pthread_mutex_lock(&mu_);
        unsigned n = 0;
        for (size_t i = 0; i < items_.size(); ++i)
            if (items_[i].compare(0, 5, "log: ") == 0 && has(items_[i], part)) ++n;
        pthread_mutex_unlock(&mu_);
        return n;
    }
    // How many items hold `part`.
    unsigned count(const std::string& part) {
        pthread_mutex_lock(&mu_);
        unsigned n = 0;
        for (size_t i = 0; i < items_.size(); ++i)
            if (has(items_[i], part)) ++n;
        pthread_mutex_unlock(&mu_);
        return n;
    }
    // How many items are `item` and nothing more.
    unsigned exactly(const std::string& item) {
        pthread_mutex_lock(&mu_);
        unsigned n = 0;
        for (size_t i = 0; i < items_.size(); ++i)
            if (items_[i] == item) ++n;
        pthread_mutex_unlock(&mu_);
        return n;
    }
    std::string text() {
        pthread_mutex_lock(&mu_);
        std::string all;
        for (size_t i = 0; i < items_.size(); ++i) all += items_[i] + "\n";
        pthread_mutex_unlock(&mu_);
        return all;
    }

private:
    pthread_mutex_t mu_;
    std::vector<std::string> items_;
};

// Declared after a Trace and before what makes links: on its way out it waits until every link's threads have
// ended, which they must before the trace they write to goes.
struct LinksGone {
    LinksGone() {}
    ~LinksGone() { CHECK(waitForSerialLinks(5000)); }
};

// Waits until the trace holds `part`.
static bool sawWithin(Trace& trace, const std::string& part, int ms) {
    double end = monoNow() + ms / 1000.0;
    while (monoNow() < end) {
        if (trace.saw(part)) return true;
        pause(5);
    }
    return trace.saw(part);
}

// ---- the car's Bluetooth service ----------------------------------------------------------------------------------

class Node;

class FakeGap : public GapClient {
public:
    explicit FakeGap(Trace* trace)
        : canOpen(true), openFinal_(false), silent(false), registerError(0), deregisterError(0), refusesWhileDialled(false),
          answersList(true), listed(false),
          connection(0), security(0), answersQuery(true), queryLists(true), queryError(0), queryInforms(false),
          queryRejected(false), queryUnread(false),
          answersConnect(true), connectError(0), dialNode(0), rejectsConnect(false), answersRepoint(true),
          repointError(5), takesRepoint(true), rejectsRepoint(false), answersDisconnect(false), disconnectError(0), events_(0), up_(false),
          trace_(trace) {
        pthread_mutex_init(&connectsMu_, NULL);
    }
    ~FakeGap() { pthread_mutex_destroy(&connectsMu_); }

    // What each ServiceConnect named, in the order they came, the re-points among them.
    struct Connect {
        std::string device, instance, address, service;
    };
    std::vector<Connect> connects() {
        pthread_mutex_lock(&connectsMu_);
        std::vector<Connect> c = connects_;
        pthread_mutex_unlock(&connectsMu_);
        return c;
    }

    virtual bool open(Events* events) {
        trace_->add("gap open");
        events_ = events;
        return canOpen;
    }
    virtual bool openFinal() const { return openFinal_; }
    virtual bool attached() const { return up_; }
    virtual bool request(uint32_t id, const std::vector<uint8_t>& payload) {
        if (!up_) return false;
        std::vector<uint8_t> reply;
        if (id == kReqSppUuidRegister && silent) {
            trace_->add("register, not answered");
        } else if (id == kReqSppUuidRegister) {
            trace_->add("register");
            putU32(reply, dialledUp() ? 5 : registerError);
            events_->gapMessage(kTypeResponse, kRspSppUuidRegister, &reply[0], (uint32_t)reply.size());
        } else if (id == kReqSppUuidDeregister) {
            Reader name(&payload[0], (uint32_t)payload.size());
            trace_->add("deregister " + name.str());
            putU32(reply, dialledUp() ? 5 : deregisterError);
            events_->gapMessage(kTypeResponse, kRspSppUuidDeregister, &reply[0], (uint32_t)reply.size());
        } else if (id == kReqServiceDisconnect) {
            disconnect = payload;   // before the trace says so, which is what the test waits for
            Reader r(&payload[0], (uint32_t)payload.size());
            r.str();
            r.str();
            std::string address = r.str();
            trace_->add("gap disconnect");
            if (answersDisconnect && address != unansweredFor) answerDisconnect(payload);
        } else if (id == kReqServiceConnect) {
            Reader r(&payload[0], (uint32_t)payload.size());
            std::string device = r.str(), instance = r.str(), address = r.str(), service = r.str();
            Connect c;
            c.device = device;
            c.instance = instance;
            c.address = address;
            c.service = service;
            pthread_mutex_lock(&connectsMu_);
            connects_.push_back(c);
            pthread_mutex_unlock(&connectsMu_);
            // The re-point is answered on its own terms, and has a line of its own in the trace: "gap connect" is a dial.
            if (address == kRepointAddress && service == kRepointService) {
                if (!takesRepoint) {
                    trace_->add("gap repoint not taken");
                    return false;
                }
                trace_->add("gap repoint " + device + " " + instance + " " + address + " " + service);
                if (rejectsRepoint) events_->gapMessage(kTypeRejectedA, kReqServiceConnect, 0, 0);
                else if (answersRepoint) connectReply(address, service, repointError);
                return true;
            }
            trace_->add("gap connect " + device + " " + instance + " " + address + " " + service);
            if (rejectsConnect) {
                events_->gapMessage(kTypeRejectedA, kReqServiceConnect, 0, 0);
            } else if (answersConnect) {
                uint32_t error = connectError;
                if (!nextConnectErrors.empty()) {
                    error = nextConnectErrors.front();
                    nextConnectErrors.erase(nextConnectErrors.begin());
                }
                if (error == 0) dialUp();
                putString(reply, address);
                putString(reply, service);
                putU32(reply, error);
                events_->gapMessage(kTypeResponse, kRspServiceConnect, &reply[0], (uint32_t)reply.size());
            }
        } else if (id == kReqServiceListQuery) {
            Reader r(&payload[0], (uint32_t)payload.size());
            std::string address = r.str();
            trace_->add("gap query " + address);
            // As the car does: a ServiceListChanged inform as the query starts, and two with its answer.
            if (queryInforms) serviceListChanged(address);
            if (queryRejected) {
                events_->gapMessage(kTypeRejectedA, kReqServiceListQuery, 0, 0);
            } else if (queryUnread) {
                std::vector<uint8_t> cut;
                putU32(cut, 0);
                putU32(cut, 40);   // an address longer than what follows
                events_->gapMessage(kTypeResponse, kRspServiceListQuery, &cut[0], (uint32_t)cut.size());
            } else if (answersQuery) {
                queryReply(address, queryLists, queryError);
                if (queryInforms) {
                    serviceListChanged(address);
                    serviceListChanged(address);
                }
            }
        } else if (id == kReqDeviceListGet) {
            trace_->add("device list");
            if (answersList) {
                putU32(reply, 0);
                putDevices(reply);
                events_->gapMessage(kTypeResponse, kRspDeviceListGet, &reply[0], (uint32_t)reply.size());
            }
        }
        return true;
    }
    // The device lists: the phone of the test's requests, in the colon form, with the test's connection and
    // security, if `listed`; then the phones in `phones`.
    void putDevices(std::vector<uint8_t>& p) {
        std::vector<BtDevice> all;
        if (listed) {
            BtDevice d;
            d.address = "AA:BB:CC:DD:EE:FF";
            d.connection = connection;
            d.security = security;
            all.push_back(d);
        }
        all.insert(all.end(), phones.begin(), phones.end());
        putU32(p, (uint32_t)all.size());
        for (size_t i = 0; i < all.size(); ++i) putString(p, "Test Phone");
        putU32(p, (uint32_t)all.size());
        for (size_t i = 0; i < all.size(); ++i) putString(p, all[i].address);
        putU32(p, (uint32_t)all.size());
        for (size_t i = 0; i < all.size(); ++i) putU32(p, 0x5a020c);
        putU32(p, (uint32_t)all.size());
        for (size_t i = 0; i < all.size(); ++i) putU32(p, all[i].security);
        putU32(p, (uint32_t)all.size());
        for (size_t i = 0; i < all.size(); ++i) putU32(p, all[i].connection);
    }
    // The dialled node comes up (defined below, with Node).
    void dialUp();
    // With refusesWhileDialled: whether the dialled node is there, so that a registration and a removal are answered 5.
    bool dialledUp() const;
    void dialDown();
    // The answer to a ServiceDisconnect; one that ends the dialled link takes its node away and says so in an inform.
    void answerDisconnect(const std::vector<uint8_t>& payload) {
        Reader r(&payload[0], (uint32_t)payload.size());
        std::string device = r.str(), instance = r.str(), address = r.str(), service = r.str();
        std::vector<uint8_t> reply;
        uint32_t error = disconnectErrorFor.count(address) ? disconnectErrorFor[address] : disconnectError;
        putString(reply, address);
        putString(reply, service);
        putU32(reply, error);
        putU32(reply, 0);
        events_->gapMessage(kTypeResponse, kRspServiceDisconnect, &reply[0], (uint32_t)reply.size());
        if (error == 0 && instance == "nbtapp") {
            dialDown();
            ended(instance, address, service, 0);
        }
    }
    // ServiceDisconnected: a link of the car's ended, `reason` 1 by the phone, 0 by the car.
    void ended(const std::string& instance, const std::string& address, const std::string& service, uint32_t reason) {
        std::vector<uint8_t> p;
        putString(p, "BtCtrl");
        putString(p, instance);
        putString(p, address);
        putString(p, service);
        putU32(p, reason);
        events_->gapMessage(0x400, kInfServiceDisconnected, &p[0], (uint32_t)p.size());
    }
    // A ServiceListQuery reply: the phone's services, the agent's among them if `lists`.
    void queryReply(const std::string& address, bool lists, uint32_t error = 0) {
        std::vector<uint8_t> p;
        putU32(p, error);
        putString(p, address);
        putU32(p, lists ? 2 : 1);
        putString(p, "GATT");
        if (lists) putString(p, "NBTApp");
        putU32(p, lists ? 2 : 1);
        putU32(p, 0);
        if (lists) putU32(p, 0);
        putU32(p, 0);
        putU32(p, 0);
        events_->gapMessage(kTypeResponse, kRspServiceListQuery, &p[0], (uint32_t)p.size());
    }
    void serviceListChanged(const std::string& address) {
        std::vector<uint8_t> p;
        putString(p, address);
        putU32(p, 1); putString(p, "GATT");
        putU32(p, 1); putU32(p, 0);
        putU32(p, 0); putU32(p, 0);
        putU32(p, 2);
        events_->gapMessage(0x400, kInfServiceListChanged, &p[0], (uint32_t)p.size());
    }
    // A ServiceConnect reply that the test sends itself, with answersConnect false.
    void connectReply(const std::string& address, const std::string& service, uint32_t error) {
        std::vector<uint8_t> p;
        putString(p, address);
        putString(p, service);
        putU32(p, error);
        events_->gapMessage(kTypeResponse, kRspServiceConnect, &p[0], (uint32_t)p.size());
    }
    // A pairing inform, with what the test gives as its payload.
    void pairing(uint32_t informId, const std::vector<uint8_t>& payload) {
        events_->gapMessage(0x400, informId, payload.empty() ? 0 : &payload[0], (uint32_t)payload.size());
    }
    // DeviceListChanged, with the lists as they are now.
    void devicesChanged() {
        std::vector<uint8_t> p;
        putDevices(p);
        putU32(p, 0);
        events_->gapMessage(0x400, kInfDeviceListChanged, &p[0], (uint32_t)p.size());
    }
    virtual bool subscribe(uint32_t) { return up_; }
    virtual std::string describe() const { return "Test.Server 1.0"; }

    void attach() {
        up_ = true;
        events_->gapAttached();
    }
    void detach() {
        up_ = false;
        events_->gapDetached();
    }
    // A phone asks for a link to `service`, is given it, or is refused.
    void phone(uint32_t informId, const char* service, const char* address = "AABBCCDDEEFF") {
        std::vector<uint8_t> p;
        if (informId == kInfServiceRequest) putString(p, "Test Phone");
        putString(p, address);
        putString(p, service);
        events_->gapMessage(0x400, informId, &p[0], (uint32_t)p.size());
    }

    bool canOpen;
    bool openFinal_;   // with canOpen false: the failure is one no later try mends
    bool silent;                     // a registration is not answered
    uint32_t registerError;          // what a registration is answered with
    uint32_t deregisterError;        // and a removal
    bool refusesWhileDialled;        // both are answered 5 while the dialled node is there, as for a link on the service
    bool answersList;                // DeviceListGet is answered, with the phone in it if `listed`
    bool listed;
    uint32_t connection, security;
    std::vector<BtDevice> phones;      // more phones in the car's device list
    bool answersQuery;                 // ServiceListQuery is answered, at once, with queryError, and with the
    bool queryLists;                   // agent's service among the phone's if queryLists
    uint32_t queryError;
    bool queryInforms;                 // each query brings three ServiceListChanged informs, as the car's do
    bool queryRejected;                // a query is rejected by the server, at once, in place of an answer
    bool queryUnread;                  // a query is answered, at once, with a reply that cannot be read
    std::vector<uint8_t> disconnect;   // the last ServiceDisconnect's payload
    bool answersConnect;               // ServiceConnect is answered, at once, with connectError; with 0 the
    uint32_t connectError;             // dialled node comes up first
    Node* dialNode;
    std::vector<uint32_t> nextConnectErrors;   // the answers to the next ServiceConnects, before connectError
    bool rejectsConnect;               // a dial's ServiceConnect is rejected by the server, at once
    bool answersRepoint;               // the re-point's ServiceConnect is answered, at once, with repointError; it
    uint32_t repointError;             // is taken by the client unless takesRepoint is false, and rejected by the
    bool takesRepoint;                 // server, at once, with rejectsRepoint
    bool rejectsRepoint;
    bool answersDisconnect;            // ServiceDisconnect is answered, at once, with disconnectError, or what
    uint32_t disconnectError;          // disconnectErrorFor has for the address
    std::map<std::string, uint32_t> disconnectErrorFor;
    std::string unansweredFor;         // a ServiceDisconnect for this address is never answered

private:
    Events* events_;
    bool up_;
    Trace* trace_;
    pthread_mutex_t connectsMu_;
    std::vector<Connect> connects_;
};

// What the system's log says, as the test has it.
class TestLook : public ServerLook {
public:
    TestLook() : line("Notification set on BSS_GAP.SGBluetoothGap 36.0, version available: 35.1"), asked(0) {}
    virtual std::string look(const std::string& name, std::string* why) {
        ++asked;
        iface = name;
        if (line.empty()) *why = "the test's log has nothing";
        return line;
    }
    std::string line, iface;
    int asked;
};

class FakeGate : public NodeGate {
public:
    explicit FakeGate(Trace* trace) : inFront(true), otherAt(0), trace_(trace) { pthread_mutex_init(&mu_, NULL); }
    ~FakeGate() { pthread_mutex_destroy(&mu_); }
    virtual bool attach(std::string* why) {
        trace_->add("gate attach");
        if (!inFront) *why = "the test's gate is not in front";
        return inFront;
    }
    virtual void watch() {}
    virtual void detach() { trace_->add("gate detach"); }
    virtual void hold(const std::vector<std::string>& programs) {
        std::string all;
        for (size_t i = 0; i < programs.size(); ++i) all += (i ? ", " : "") + programs[i];
        trace_->add("gate hold " + all);
        pthread_mutex_lock(&mu_);
        held_ = programs;
        pthread_mutex_unlock(&mu_);
    }
    virtual void release() {
        trace_->add("gate release");
        pthread_mutex_lock(&mu_);
        held_.clear();
        pthread_mutex_unlock(&mu_);
    }
    // Whether an open by `program` would be refused now, as the hold stands (the car's media process aside).
    bool refuses(const std::string& program) {
        pthread_mutex_lock(&mu_);
        bool refused = false;
        for (size_t i = 0; i < held_.size(); ++i) refused = refused || held_[i] == program;
        pthread_mutex_unlock(&mu_);
        return refused;
    }
    virtual bool sawOtherOpen(double since, std::string* who) {
        pthread_mutex_lock(&mu_);
        bool saw = otherAt != 0 && otherAt >= since;
        if (saw) *who = otherWho;
        pthread_mutex_unlock(&mu_);
        return saw;
    }
    // Another program opens the node now.
    void otherOpens(const std::string& who) {
        pthread_mutex_lock(&mu_);
        otherAt = monoNow();
        otherWho = who;
        pthread_mutex_unlock(&mu_);
    }

    bool inFront;
    double otherAt;
    std::string otherWho;
    pthread_mutex_t mu_;

private:
    Trace* trace_;
    std::vector<std::string> held_;   // the programs the hold refuses; empty: no hold
};

// ---- a serial node, and the phone at its far end ------------------------------------------------------------------

class Node;
static void addNode(Node* node);
static void dropNode(Node* node);

// A node can also play the car's stack as it treats a link no read has been made on (node_reader.hpp): with `held`,
// what the phone sends (send()) waits ahead of the node, where the terminal does not have it, until a read is made
// through the test's calls (NodeCalls, below); from then on it comes into the node as it comes. A node also says how
// many descriptors this process holds on it, and what the test's calls on its queue are to do.
class Node {
public:
    // The node `prefix` + `index`, not there yet.
    Node(const std::string& prefix, int index) : phone(-1) {
        char digit[8];
        snprintf(digit, sizeof(digit), "%d", index);
        make(prefix + digit);
    }
    // The node at `at`, not there yet.
    explicit Node(const std::string& at) : phone(-1) { make(at); }
    ~Node() {
        dropNode(this);
        vanish();
        hangUp();
        pthread_mutex_destroy(&mu_);
    }

    void make(const std::string& at) {
        path = at;
        held = false;
        primed_ = false;
        notRaw = false;
        queueSize = 6144;
        injectError = 0;
        injects = 0;
        pthread_mutex_init(&mu_, NULL);
        phone = posix_openpt(O_RDWR | O_NOCTTY);
        CHECK(phone >= 0 && grantpt(phone) == 0 && unlockpt(phone) == 0);
        near_ = ptsname(phone);
        // Bytes as they are, both ways, with no echo.
        int fd = open(near_.c_str(), O_RDWR | O_NOCTTY);
        CHECK(fd >= 0);
        struct termios raw;
        CHECK(tcgetattr(fd, &raw) == 0);
        cfmakeraw(&raw);
        CHECK(tcsetattr(fd, TCSANOW, &raw) == 0);
        struct stat st;
        CHECK(fstat(fd, &st) == 0);
        rdev_ = st.st_rdev;
        close(fd);
        fcntl(phone, F_SETFL, fcntl(phone, F_GETFL, 0) | O_NONBLOCK);
        addNode(this);
    }

    // What the phone sends: ahead of the node while it is held and no read has been made, otherwise into it.
    void send(const std::string& bytes) {
        pthread_mutex_lock(&mu_);
        bool ahead = held && !primed_;
        if (ahead) ahead_ += bytes;
        pthread_mutex_unlock(&mu_);
        if (!ahead) say(bytes);
    }
    // A read has been made through `fd`: what waited ahead of the node comes into it. True if this was the first.
    bool prime(int fd) {
        pthread_mutex_lock(&mu_);
        bool first = held && !primed_;
        primed_ = true;
        std::string ahead;
        ahead.swap(ahead_);
        pthread_mutex_unlock(&mu_);
        if (!ahead.empty()) into(fd, ahead);
        return first;
    }
    // Bytes into the node's queue, as the phone's: the call returns once `fd` has them to read.
    void into(int fd, const std::string& bytes) {
        int before = 0;
        ioctl(fd, FIONREAD, &before);
        say(bytes);
        double end = monoNow() + 2.0;
        int now = 0;
        while (monoNow() < end && (ioctl(fd, FIONREAD, &now) != 0 || now < before + (int)bytes.size())) pause(1);
    }
    // What the test's calls do once, as they come to them: `gap` comes into the node just before the read that does
    // not wait, `late` just before the queue is looked at for a put-back.
    void setGap(const std::string& bytes) { set(&gap_, bytes); }
    void setLate(const std::string& bytes) { set(&late_, bytes); }
    std::string takeGap() { return take(&gap_); }
    std::string takeLate() { return take(&late_); }
    // A terminal's number is given again once both its ends are closed: one that was hung up is no longer this node.
    bool is(dev_t rdev) const { return phone >= 0 && rdev == rdev_; }
    // How many descriptors of this process are open on the node now.
    int holders() const {
        int n = 0;
        for (int fd = 0; fd < 1024; ++fd) {
            struct stat st;
            if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == rdev_) ++n;
        }
        return n;
    }

    // The link comes up: the node is there.
    void appear() { CHECK_IN(path, symlink(near_.c_str(), path.c_str()) == 0); }
    void vanish() { unlink(path.c_str()); }
    void hangUp() {
        if (phone >= 0) close(phone);
        phone = -1;
    }
    // Opens the near end as the listener would.
    int openNear() {
        int fd = open(near_.c_str(), O_RDWR | O_NOCTTY);
        CHECK(fd >= 0);
        return fd;
    }

    void say(const std::string& bytes) {
        size_t off = 0;
        double end = monoNow() + 5.0;
        while (off < bytes.size() && monoNow() < end) {
            ssize_t n = write(phone, bytes.data() + off, bytes.size() - off);
            if (n > 0) off += (size_t)n;
            else pause(2);
        }
        CHECK(off == bytes.size());
    }
    // What has arrived at the phone within `ms`, read until `want` bytes are there or the near end is closed.
    // *closed: every holder of the near end has closed it.
    std::string hear(size_t want, int ms, bool* closed) {
        std::string got;
        *closed = false;
        double end = monoNow() + ms / 1000.0;
        while (got.size() < want && monoNow() < end) {
            char buffer[4096];
            ssize_t n = read(phone, buffer, sizeof buffer);
            if (n > 0) {
                got.append(buffer, (size_t)n);
            } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                *closed = true;
                break;
            } else {
                pause(2);
            }
        }
        return got;
    }

    int phone;          // the far end; -1 once hung up or given away
    std::string path;   // where the listener finds the node
    bool held;          // the phone's bytes wait ahead of the node until a read is made (send())
    bool notRaw;        // the test's calls say that the node is not raw
    long queueSize;     // and that its queue holds this much
    int injectError;    // and that a put-back fails with this errno; 0: it works
    int injects;        // put-backs made

private:
    void set(std::string* what, const std::string& bytes) {
        pthread_mutex_lock(&mu_);
        *what = bytes;
        pthread_mutex_unlock(&mu_);
    }
    std::string take(std::string* what) {
        pthread_mutex_lock(&mu_);
        std::string bytes;
        bytes.swap(*what);
        pthread_mutex_unlock(&mu_);
        return bytes;
    }

    std::string near_;
    dev_t rdev_;
    pthread_mutex_t mu_;   // ahead_, primed_, gap_, late_
    std::string ahead_, gap_, late_;
    bool primed_;
};

// Every Node there is, for the test's calls to find a node by a descriptor the listener opened on it.
static pthread_mutex_t gNodesMu = PTHREAD_MUTEX_INITIALIZER;
static std::vector<Node*> gNodes;

static void addNode(Node* node) {
    pthread_mutex_lock(&gNodesMu);
    gNodes.push_back(node);
    pthread_mutex_unlock(&gNodesMu);
}

static void dropNode(Node* node) {
    pthread_mutex_lock(&gNodesMu);
    for (size_t i = 0; i < gNodes.size(); ++i)
        if (gNodes[i] == node) gNodes.erase(gNodes.begin() + (long)i--);
    pthread_mutex_unlock(&gNodesMu);
}

static Node* nodeOf(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return 0;
    pthread_mutex_lock(&gNodesMu);
    Node* found = 0;
    for (size_t i = 0; i < gNodes.size() && found == 0; ++i)
        if (gNodes[i]->is(st.st_rdev)) found = gNodes[i];
    pthread_mutex_unlock(&gNodesMu);
    return found;
}

// ---- the test's calls on a node: the car's stack as the node meets it ----------------------------------------------
// What the platform's would do on a terminal, but for a held node, on which a read is what brings the phone's bytes
// into the node, and for the size of the node's queue and a put-back, which a terminal does not have: a put-back is
// a write from the phone's end, which puts the bytes behind what is in the node's queue, as the car's does.

static long heldQueued(int fd) { return platformNodeCalls().queued(fd); }

static int heldRaw(int fd) {
    Node* n = nodeOf(fd);
    if (n != 0 && n->notRaw) return 0;
    return platformNodeCalls().raw(fd);
}

static long heldQueueSize(int fd) {
    Node* n = nodeOf(fd);
    if (n == 0) return 6144;
    std::string late = n->takeLate();
    if (!late.empty()) n->into(fd, late);
    return n->queueSize;
}

// A read made while nothing is in the node's queue takes nothing, and brings what waits ahead of the node into it.
static long heldReadNow(int fd, void* buf, size_t len) {
    Node* n = nodeOf(fd);
    if (n != 0 && n->prime(fd)) {
        errno = EAGAIN;
        return -1;
    }
    if (n != 0) {
        std::string gap = n->takeGap();
        if (!gap.empty()) n->into(fd, gap);
    }
    return platformNodeCalls().readNow(fd, buf, len);
}

static int heldInject(int fd, const void* buf, size_t len) {
    Node* n = nodeOf(fd);
    if (n == 0) {
        errno = ENODEV;
        return -1;
    }
    if (n->injectError != 0) {
        errno = n->injectError;
        return -1;
    }
    ++n->injects;
    n->into(fd, std::string((const char*)buf, len));
    return 0;
}

// The reader's timed read: a read brings what waits ahead of the node into it.
static long heldRead(int fd, void* buf, size_t len, int ms) {
    Node* n = nodeOf(fd);
    if (n != 0) n->prime(fd);
    return timedNodeRead(fd, buf, len, ms);
}

static NodeCalls heldCalls() {
    NodeCalls c;
    c.queued = &heldQueued;
    c.raw = &heldRaw;
    c.queueSize = &heldQueueSize;
    c.readNow = &heldReadNow;
    c.inject = &heldInject;
    return c;
}

// Another program on the node, which waits on select() before each read, as the car's own readers do.
class OtherProgram {
public:
    explicit OtherProgram(Node& node) : fd_(node.openNear()) {}
    ~OtherProgram() { close(fd_); }
    // What it has read within `ms`, until `want` bytes are there.
    std::string reads(size_t want, int ms) {
        std::string got;
        double end = monoNow() + ms / 1000.0;
        while (got.size() < want && monoNow() < end) {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(fd_, &rf);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 20000;
            if (select(fd_ + 1, &rf, NULL, NULL, &tv) <= 0) continue;
            char buffer[256];
            size_t room = want - got.size() < sizeof buffer ? want - got.size() : sizeof buffer;
            ssize_t n = read(fd_, buffer, room);
            if (n <= 0) break;
            got.append(buffer, (size_t)n);
        }
        return got;
    }

private:
    int fd_;
};

void FakeGap::dialUp() {
    if (dialNode != 0) dialNode->appear();
}

bool FakeGap::dialledUp() const {
    return refusesWhileDialled && dialNode != 0 && access(dialNode->path.c_str(), F_OK) == 0;
}

void FakeGap::dialDown() {
    if (dialNode == 0) return;
    dialNode->vanish();
    dialNode->hangUp();
}

// A pattern that a shifted or repeated piece of itself does not match.
static std::string pattern(size_t n, unsigned seed) {
    std::string s(n, '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        s[i] = (char)(x >> 24);
    }
    return s;
}

// Sends `up` from the phone and `down` from the link at the same time, and checks that each arrives whole.
static void exchange(Node& node, Link* link, const std::string& up, const std::string& down) {
    std::string gotUp, gotDown;
    size_t sentUp = 0, sentDown = 0;
    double end = monoNow() + 20.0;
    while ((gotUp.size() < up.size() || gotDown.size() < down.size()) && monoNow() < end) {
        bool moved = false;
        char buffer[8192];
        if (sentUp < up.size()) {
            ssize_t n = write(node.phone, up.data() + sentUp, up.size() - sentUp);
            if (n > 0) { sentUp += (size_t)n; moved = true; }
        }
        if (sentDown < down.size()) {
            long n = link->write(down.data() + sentDown, down.size() - sentDown);
            CHECK(n >= 0);
            if (n > 0) { sentDown += (size_t)n; moved = true; }
        }
        long n = link->read(buffer, sizeof buffer);
        CHECK(n >= 0);
        if (n > 0) { gotUp.append(buffer, (size_t)n); moved = true; }
        ssize_t m = read(node.phone, buffer, sizeof buffer);
        if (m > 0) { gotDown.append(buffer, (size_t)m); moved = true; }
        if (!moved) pause(1);
    }
    CHECK(gotUp == up);
    CHECK(gotDown == down);
}

// What the link gives within `ms`, until `want` bytes are there.
static std::string readFrom(Link* link, size_t want, int ms) {
    std::string got;
    double end = monoNow() + ms / 1000.0;
    while (got.size() < want && monoNow() < end) {
        char buffer[256];
        size_t room = want - got.size() < sizeof buffer ? want - got.size() : sizeof buffer;
        long n = link->read(buffer, room);
        if (n < 0) break;
        if (n > 0) got.append(buffer, (size_t)n);
        else pause(2);
    }
    return got;
}

// Whether the link has ended, within `ms`.
static bool endsWithin(Link* link, int ms) {
    double end = monoNow() + ms / 1000.0;
    char buffer[256];
    while (monoNow() < end) {
        if (link->read(buffer, sizeof buffer) < 0) return true;
        pause(5);
    }
    return false;
}

static std::string freshFolder(const std::string& name) {
    std::string dir = gWork + "/" + name, why;
    CHECK_IN(why, fs::removeTree(dir, &why));
    CHECK_IN(why, fs::makeDirs(dir, &why));
    return dir;
}

// ---- a link on a node ---------------------------------------------------------------------------------------------

static void testLink() {
    std::string dir = freshFolder("link");
    {   // bytes both ways at once, more than any buffer on the way holds
        Trace trace;
        LinksGone gone;
        Node node(dir + "/btspp", 0);
        Link* link = newSerialLink(node.openNear(), node.path, &trace, std::vector<uint8_t>(), 0);
        CHECK(link != 0 && std::string(link->kind()) == "serial");
        exchange(node, link, pattern(300000, 1), pattern(200000, 2));
        exchange(node, link, "a", "b");
        // The agent's side closes: what was still on its way is sent first, and the node stays open until the
        // phone ends the link, even where no time is given for that.
        std::string last = pattern(5000, 3);
        size_t sent = 0;
        while (sent < last.size()) {
            long n = link->write(last.data() + sent, last.size() - sent);
            CHECK(n >= 0);
            sent += (size_t)n;
        }
        delete link;
        bool closed = false;
        CHECK(node.hear(last.size(), 5000, &closed) == last);
        CHECK(node.hear(1, 300, &closed).empty() && !closed && serialLinksOpen() == 1);
        node.hangUp();
        CHECK_IN(trace.text(), sawWithin(trace, "closed, closed by the agent, and the phone ended the link ", 2000));
        CHECK_IN(trace.text(), trace.saw(" ms later: 300001 B from the phone, 205001 B to it"));
        CHECK(trace.saw("the link is still up 0 ms after the agent's end closed (closed by the agent)"));
    }
    {   // the agent's side closes and the phone still holds the link: the node stays open and is read, what the
        // phone sends is dropped, and the phone's own end of the link is what closes the node
        Trace trace;
        LinksGone gone;
        Node node(dir + "/btspp", 3);
        Link* link = newSerialLink(node.openNear(), node.path, &trace);   // the protocol's time: 5 s
        CHECK(link != 0);
        exchange(node, link, "hello", "and the last answer");
        double at = monoNow();
        delete link;
        bool closed = false;
        for (int i = 0; i < 4; ++i) {   // a phone that has not heard yet, and goes on talking
            node.say(pattern(3000, 10 + (unsigned)i));
            CHECK(node.hear(1, 150, &closed).empty() && !closed);
        }
        CHECK(serialLinksOpen() == 1 && !trace.saw("closed,"));
        node.hangUp();
        CHECK_IN(trace.text(), sawWithin(trace, "closed, closed by the agent, and the phone ended the link ", 2000));
        CHECK_IN(trace.text(), trace.saw(" ms later: 5 B from the phone, 19 B to it, and 12000 B that the phone sent "
                                         "after the agent's close were dropped"));
        CHECK(waitForSerialLinks(1000) && monoNow() - at < 4.0);   // well inside the time it would have waited
    }
    {   // a phone that does not end the link and goes on sending, for longer than `lingerMs`: nothing is closed under
        // it, all it sends is read and dropped, the log says once that the link is still up, and the node closes
        // only when the phone ends the link
        Trace trace;
        LinksGone gone;
        Node node(dir + "/btspp", 4);
        Link* link = newSerialLink(node.openNear(), node.path, &trace, std::vector<uint8_t>(), 300);
        CHECK(link != 0);
        exchange(node, link, "hello", "bye");
        delete link;
        bool closed = false;
        size_t sent = 0;
        double until = monoNow() + 1.2;
        while (monoNow() < until) {
            node.say("still here");
            sent += 10;
            CHECK(node.hear(1, 50, &closed).empty() && !closed);
        }
        CHECK_IN(trace.text(), sawWithin(trace, "btspp4: the link is still up 300 ms after the agent's end closed (closed "
                                                "by the agent): its node is kept, and read, until the phone ends the "
                                                "link", 1000));
        pause(300);
        CHECK(trace.count("is still up") == 1 && !trace.saw("closed,") && serialLinksOpen() == 1);
        node.hangUp();
        char counts[160];
        snprintf(counts, sizeof counts, " ms later: 5 B from the phone, 3 B to it, and %u B that the phone sent after "
                 "the agent's close were dropped", (unsigned)sent);
        CHECK_IN(trace.text(), sawWithin(trace, "btspp4 closed, closed by the agent, and the phone ended the link ", 2000));
        CHECK_IN(trace.text(), trace.saw(counts));
    }
    {   // the phone's side ends: the link ends, after what the phone had sent
        Trace trace;
        LinksGone gone;
        Node node(dir + "/btspp", 1);
        Link* link = newSerialLink(node.openNear(), node.path, &trace);
        CHECK(link != 0);
        node.say("last words");
        char buffer[64];
        long n = 0;
        double end = monoNow() + 3.0;
        while (n == 0 && monoNow() < end) {
            n = link->read(buffer, sizeof buffer);
            if (n == 0) pause(2);
        }
        CHECK(n == 10 && memcmp(buffer, "last words", 10) == 0);
        node.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK_IN(trace.text(), sawWithin(trace, ": 10 B from the phone, 0 B to it", 2000));
    }
    {   // what was read from the node before the link was made comes first, and counts as the phone's
        Trace trace;
        LinksGone gone;
        Node node(dir + "/btspp", 2);
        const std::string opening = "NBTL+";
        Link* link = newSerialLink(node.openNear(), node.path, &trace,
                                   std::vector<uint8_t>(opening.begin(), opening.end()));
        CHECK(link != 0);
        node.say(" and more");
        CHECK(readFrom(link, 14, 3000) == "NBTL+ and more");
        node.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK_IN(trace.text(), sawWithin(trace, ": 14 B from the phone, 0 B to it", 2000));
    }
}

// ---- the listener -------------------------------------------------------------------------------------------------

// What a phone app of ours opens its link with.
static const std::string kMagic("NBTL");

// The tests' listener decides on a link in less than the real one's time, which they would otherwise wait out for
// every link that is let go, and gives a phone less time to end a link that the agent has closed, which they
// would wait out for every phone that does not.
static SerialConfig configFor(const std::string& dir) {
    SerialConfig c;
    c.nodePrefix = dir + "/btspp";
    c.decideMs = 300;
    c.lingerMs = 300;
    c.checkMs = 0;   // testRegistrationCheck() turns it on
    c.dial.on = false;   // the car dialling the phone has tests of its own, which turn it on
    // and dials as before the knock (--dial-knock off) in them: the knock's tests turn it on (knockConfig())
    c.dial.knock = false;
    c.dial.settleMs = 0;   // a phone in the first device list is asked at once; the settle time has a test of its own
    return c;
}

// The link the listener has ready, waiting up to `ms` for one.
static Link* acceptWithin(SerialListener& listener, int ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(listener.fd(), &rf);
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (select(listener.fd() + 1, &rf, NULL, NULL, &tv) <= 0) return 0;
    return listener.accept();
}

// The phone at `node` opens its link as a phone app of ours does, and the listener hands the link over. It starts
// with the magic bytes the listener read, which are taken from it here.
static Link* dialled(SerialListener& listener, Node& node, Trace& trace) {
    node.say(kMagic);
    Link* link = acceptWithin(listener, 3000);
    CHECK_IN(trace.text(), link != 0);
    CHECK(readFrom(link, kMagic.size(), 3000) == kMagic);
    return link;
}

static bool inactiveWithin(SerialListener& listener, int ms) {
    double end = monoNow() + ms / 1000.0;
    while (monoNow() < end && listener.active()) pause(5);
    return !listener.active();
}

static void testListener() {
    std::string dir = freshFolder("listener");
    {
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        Node before(dir + "/btspp", 7);
        before.appear();                                  // a link that was up before the agent came
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        std::string why;
        CHECK(!listener.active());
        CHECK_IN(why, listener.start(&why));
        CHECK(listener.active() && listener.fd() >= 0 && listener.accept() == 0);
        // The gate is there before the stack is asked for anything, and nothing is registered before it answers.
        CHECK(trace.at("gate attach") >= 0 && trace.at("gate attach") < trace.at("gap open") && !trace.saw("register"));
        gap.attach();
        CHECK(trace.saw("register"));
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));

        // A phone asks, the node appears, the phone says the magic bytes, and the link is there: opened by the
        // listener, handed over once.
        Node first(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        first.appear();
        Link* link = dialled(listener, first, trace);
        CHECK(std::string(link->kind()) == "serial" && listener.accept() == 0 && acceptWithin(listener, 50) == 0);
        CHECK(trace.saw("btspp0: it opened with the magic bytes, so the link is the agent's"));
        gap.phone(kInfServiceRequestComplete, kCarSerialService);
        exchange(first, link, pattern(20000, 4), pattern(20000, 5));
        CHECK(trace.saw("btspp0 opened") && trace.saw("a phone asked for a serial link (service 'SPP')"));
        CHECK(!trace.saw("Test Phone") && !trace.saw("AABBCCDDEEFF"));
        // The link knows the address of the phone whose request its node came with.
        CHECK(link->phoneAddress() == "AABBCCDDEEFF");

        // A second phone, while the first link is up, and by the service's own name.
        Node second(dir + "/btspp", 1);
        gap.phone(kInfServiceRequest, kSerialServiceName, "001122334455");
        second.appear();
        Link* other = dialled(listener, second, trace);
        CHECK(other->phoneAddress() == "001122334455" && !trace.saw("001122334455"));
        exchange(second, other, "two", "zwei");
        exchange(first, link, "one", "eins");

        // The first link ends and its node goes; the same node comes back with a new link.
        first.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
        first.vanish();
        pause(450);                                       // the listener sees that it is gone
        Node again(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        again.appear();
        link = dialled(listener, again, trace);
        exchange(again, link, "again", "wieder");

        // Not ours: a node that comes with a request for another service only, which brings no serial node and is
        // not kept for one, so the node came with no request; a link no phone asked for, one the car refused, and the
        // node that was there at the start.
        Node foreign(dir + "/btspp", 2), unasked(dir + "/btspp", 3);
        gap.phone(kInfServiceRequest, "Another");
        CHECK(trace.saw("a phone asked for a serial link (service 'Another'): another service's, left alone"));
        foreign.appear();
        CHECK(acceptWithin(listener, 500) == 0);
        CHECK_IN(trace.text(), sawWithin(trace, "btspp2: no request from a phone came with it, so it is not ours", 4000));
        CHECK(!trace.saw("btspp2 opened"));
        gap.phone(kInfServiceRequest, kCarSerialService);
        gap.phone(kInfServiceRequestError, kCarSerialService);
        CHECK_IN(trace.text(), trace.saw("the car refused the phone's link (1 so far). The phone is not in the car's "
                                         "device list"));
        unasked.appear();
        CHECK(acceptWithin(listener, 500) == 0);
        CHECK(trace.saw("btspp7 exists already") && !trace.saw("btspp7 opened") && !trace.saw("btspp3 opened"));

        // stop(): the service goes, then the gate. The links that are up stay up.
        CHECK(!trace.saw("deregister") && !trace.saw("gate detach"));
        listener.stop();
        CHECK(!listener.active());
        CHECK(trace.at("deregister") >= 0 && trace.at("deregister") < trace.at("gate detach"));
        exchange(again, link, "still", "noch");
        exchange(second, other, "here", "da");
        listener.stop();                                  // twice does nothing more
        delete link;
        delete other;
    }
    {   // what the removal at stop() came to: the stack had dropped the registration (10), or a link on the service
        // is up (5)
        uint32_t errors[] = {10, 5};
        for (int i = 0; i < 2; ++i) {
            Trace trace;
            LinksGone gone;
            FakeGap gap(&trace);
            FakeGate gate(&trace);
            gap.deregisterError = errors[i];
            SerialListener listener(&gap, &gate, &trace, configFor(dir));
            std::string why;
            CHECK_IN(why, listener.start(&why));
            gap.attach();
            CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));
            listener.stop();
            if (errors[i] == 10) {
                CHECK_IN(trace.text(), trace.saw("'NBTApp' was not registered any more: the car's Bluetooth had dropped "
                                                 "the registration, as switching Bluetooth off and on in iDrive does, so "
                                                 "there was nothing to remove"));
                CHECK(!trace.saw("did not remove"));
            } else {
                CHECK_IN(trace.text(), trace.saw("the car's Bluetooth service did not remove the registration of 'NBTApp' "
                                                 "(5 DEFAULT (a link on that service is up)): it stays until the car's "
                                                 "Bluetooth is switched off and on or restarts, or an agent removes it"));
            }
        }
    }
    {   // the gate is not in front, and the listener is not to try again: the stack is never asked, and nothing is
        // left behind
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gate.inFront = false;
        SerialConfig config = configFor(dir);
        config.retryMs = 0;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK(!listener.start(&why) && has(why, "not in front") && !listener.active());
        CHECK(!trace.saw("gap open") && !trace.saw("register"));
    }
    {   // the stack cannot be reached: the gate is taken away again
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gap.canOpen = false;
        SerialConfig config = configFor(dir);
        config.retryMs = 0;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK(!listener.start(&why) && !why.empty() && !listener.active());
        CHECK(trace.at("gate attach") >= 0 && trace.at("gate attach") < trace.at("gate detach"));
    }
    {   // otherwise a start that does not work is tried again, each reason said once: the gate not in front, then
        // the client of the car's Bluetooth service not opening, then all of it working
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gate.inFront = false;
        SerialConfig config = configFor(dir);
        config.retryMs = 1000;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        CHECK(!listener.active() && !trace.saw("gap open"));
        CHECK_IN(trace.text(), trace.saw("Bluetooth serial is not set up: the test's gate is not in front. Again in 1 s"));
        double end = monoNow() + 4.0;
        while (monoNow() < end && trace.count("gate attach") < 3) pause(5);
        CHECK_IN(trace.text(), trace.count("gate attach") == 3 && trace.count("not in front") == 1);
        gate.inFront = true;
        gap.canOpen = false;
        CHECK_IN(trace.text(), sawWithin(trace, "Bluetooth serial is not set up again: no connection to the car's "
                                                "Bluetooth service. Again in 1 s", 2500));
        end = monoNow() + 3.0;
        while (monoNow() < end && trace.count("gap open") < 2) pause(5);
        CHECK_IN(trace.text(), trace.count("gap open") == 2 && trace.count("no connection to the car's") == 1);
        CHECK(!listener.active() && trace.count("gate detach") == 2);
        gap.canOpen = true;
        CHECK_IN(trace.text(), sawWithin(trace, "setting Bluetooth serial up again", 2500));
        CHECK(listener.active() && trace.count("gap open") == 3);
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));
        listener.stop();
        CHECK(trace.saw("deregister NBTApp"));
    }
    {   // a client that failed to open for good, at the start: no thread, no retry, said once by the agent
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gap.canOpen = false;
        gap.openFinal_ = true;
        SerialConfig config = configFor(dir);
        config.retryMs = 1000;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK(!listener.start(&why) && !listener.active());
        CHECK_IN(why, why == "the client of the car's Bluetooth service failed to open and cannot be opened again in "
                             "this run. Bluetooth serial is off until the agent is started again");
        CHECK(trace.at("gate attach") >= 0 && trace.at("gate attach") < trace.at("gate detach"));
        pause(1500);
        CHECK_IN(trace.text(), trace.count("gap open") == 1 && trace.count("gate attach") == 1);
        listener.stop();
    }
    {   // the same on a later try: the listener says so once and stops trying; one that can mend is tried again
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gap.canOpen = false;
        SerialConfig config = configFor(dir);
        config.retryMs = 500;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        CHECK_IN(trace.text(), trace.saw("Bluetooth serial is not set up: no connection to the car's Bluetooth service"));
        double end = monoNow() + 3.0;
        while (monoNow() < end && trace.count("gap open") < 3) pause(5);
        CHECK_IN(trace.text(), trace.count("gap open") >= 3);
        gap.openFinal_ = true;
        CHECK_IN(trace.text(), sawWithin(trace, "Bluetooth serial is not set up again: the client of the car's Bluetooth "
                                                "service failed to open and cannot be opened again in this run. It is "
                                                "off until the agent is started again, and not tried again", 2500));
        unsigned opens = trace.count("gap open");
        pause(1500);
        CHECK_IN(trace.text(), trace.count("gap open") == opens && trace.count("not tried again") == 1);
        CHECK(!listener.active());
        listener.stop();
    }
    {   // a stop while it waits to try again does not wait for that
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gate.inFront = false;
        SerialConfig config = configFor(dir);
        config.retryMs = 60000;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        pause(50);
        double before = monoNow();
        listener.stop();
        CHECK(monoNow() - before < 1.0 && trace.count("gate attach") == 1 && !trace.saw("gate detach"));
    }
    {   // the stack does not take the registration: the listener gives up and takes the gate away, for good when
        // it is told not to try again
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gap.registerError = 5;
        SerialConfig config = configFor(dir);
        config.retryMs = 0;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK(inactiveWithin(listener, 3000));
        CHECK_IN(trace.text(), trace.saw("did not take the registration: Bluetooth serial is off until the agent is "
                                         "started again") && trace.saw("gate detach"));
        CHECK(listener.accept() == 0);
        pause(300);
        CHECK_IN(trace.text(), trace.count("gate attach") == 1);
        listener.stop();
    }
    {   // otherwise it sets itself up again, as often as it takes, and is then a listener like any other
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gap.registerError = 5;
        SerialConfig config = configFor(dir);
        config.retryMs = 1000;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK(inactiveWithin(listener, 3000));
        CHECK_IN(trace.text(), trace.saw("did not take the registration: Bluetooth serial is off, and is set up again in 1 s"));
        CHECK(trace.count("gate attach") == 1 && trace.count("gate detach") == 1 && trace.count("gap open") == 1);
        // The second time the gate is not in front, the third the stack refuses again, the fourth it works.
        gate.inFront = false;
        CHECK_IN(trace.text(), sawWithin(trace, "Bluetooth serial is not set up again: the test's gate is not in front. Again in 1 s", 3000));
        CHECK(!listener.active());
        gate.inFront = true;
        CHECK_IN(trace.text(), sawWithin(trace, "setting Bluetooth serial up again", 3000));
        double end = monoNow() + 3.0;
        while (monoNow() < end && trace.count("gate detach") < 2) pause(5);
        CHECK_IN(trace.text(), trace.count("is set up again in 1 s") == 2 && trace.count("gate detach") == 2);
        gap.registerError = 0;
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 3000));
        CHECK(listener.active() && trace.count("gap open") == 1);   // on the client it had: that one is opened once
        CHECK(trace.count("gate attach") == 4);

        Node node(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        node.appear();
        Link* link = dialled(listener, node, trace);
        exchange(node, link, "back", "again");
        delete link;
        listener.stop();
        CHECK_IN(trace.text(), trace.count("gate detach") == 3);
    }
    {   // stop() does not wait for a listener that is waiting to set itself up again
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gap.registerError = 5;
        SerialConfig config = configFor(dir);
        config.retryMs = 60000;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK(inactiveWithin(listener, 3000));
        double before = monoNow();
        listener.stop();
        CHECK(monoNow() - before < 1.0 && trace.count("gate attach") == 1 && trace.count("gate detach") == 1);
    }
    {   // the stack never answers: the same, once it has been too long
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        SerialConfig config = configFor(dir);
        config.registerMs = 300;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        CHECK(listener.active());
        CHECK(inactiveWithin(listener, 3000));
        CHECK_IN(trace.text(), trace.saw("the car's Bluetooth service never attached (the agent asked the service "
                                         "broker for Test.Server 1.0): Bluetooth serial is off, and is set up again in "
                                         "30 s") && trace.saw("gate detach"));
        CHECK(!trace.saw("deregister"));   // the stack was never there to be asked
    }
    {   // it is there and does not answer the registration: said apart from that
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gap.silent = true;
        SerialConfig config = configFor(dir);
        config.registerMs = 300;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK(inactiveWithin(listener, 3000));
        CHECK_IN(trace.text(), trace.saw("the car's Bluetooth service is attached and has not answered the registration: "
                                         "Bluetooth serial is off, and is set up again in 30 s"));
        CHECK(!trace.saw("never attached"));
    }
    {   // not attached for attachWaitMs: said once, with the version asked for, and the system's log asked once
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        SerialConfig config = configFor(dir);
        config.attachWaitMs = 100;
        TestLook look;
        SerialListener listener(&gap, &gate, &trace, config);
        listener.setServerLook(&look);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        CHECK_IN(trace.text(), sawWithin(trace, "the car's Bluetooth service has not attached 0.1 s after the agent asked "
                                                "the service broker for Test.Server 1.0. The broker attaches a client "
                                                "only to a server with the same major version and a minor version as "
                                                "high or higher, and otherwise tells the client nothing", 2000));
        CHECK_IN(trace.text(), sawWithin(trace, "s to read, says of the service: Notification set on "
                                                "BSS_GAP.SGBluetoothGap 36.0, version available: 35.1", 2000));
        CHECK(look.asked == 1 && look.iface == "SGBluetoothGap");
        pause(300);
        CHECK(trace.count("has not attached") == 1 && look.asked == 1);
        // It attaches, registers, goes away for longer than the wait: said again, the look not asked again.
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));
        gap.detach();
        CHECK_IN(trace.text(), sawWithin(trace, "the car's Bluetooth service has been gone for 0.1 s: the agent waits for "
                                                "it to come back (it asks the service broker for Test.Server 1.0)", 2000));
        CHECK(look.asked == 1);
        listener.stop();
    }
    {   // a look that finds nothing says why
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        SerialConfig config = configFor(dir);
        config.attachWaitMs = 100;
        TestLook look;
        look.line = "";
        SerialListener listener(&gap, &gate, &trace, config);
        listener.setServerLook(&look);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        CHECK_IN(trace.text(), sawWithin(trace, "the system log gave no line about the service's version: the test's "
                                                "log has nothing", 2000));
        listener.stop();
    }
}

// ---- requests for services that bring no serial node ----------------------------------------------------------------

// A phone asks for audio or hands-free as well as for serial links. Only a request that may bring a serial node is
// kept for the nodes: one for another service, within the tracker's window, does not have the next serial node left
// alone, nor does its refusal take a serial request out.
static void testOtherServices() {
    std::string dir = freshFolder("other-services");
    Trace trace;
    LinksGone gone;
    FakeGap gap(&trace);
    FakeGate gate(&trace);
    SerialListener listener(&gap, &gate, &trace, configFor(dir));
    std::string why;
    CHECK_IN(why, listener.start(&why));
    gap.attach();
    CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));

    // A request for another service, then the agent's app's link on a new node, well within the window: taken.
    Node first(dir + "/btspp", 0);
    gap.phone(kInfServiceRequest, "AudioSink");
    pause(100);
    gap.phone(kInfServiceRequest, kCarSerialService);
    first.appear();
    Link* link = dialled(listener, first, trace);
    CHECK(trace.saw("btspp0: it opened with the magic bytes, so the link is the agent's"));
    CHECK(!trace.saw("btspp0: no request") && !trace.saw("another service with it"));
    exchange(first, link, "ours", "unser");

    // Another service's request, and a serial request's node after it: decided on as before, and let go for what
    // the phone said, not left alone for the other request.
    Node second(dir + "/btspp", 1);
    gap.phone(kInfServiceRequest, "Handsfree");
    gap.phone(kInfServiceRequest, kCarSerialService);
    second.appear();
    second.say("\x10\x20\x30\x40 another app's opening");
    CHECK_IN(trace.text(), sawWithin(trace, "btspp1 let go", 3000));
    CHECK(trace.saw("btspp1 opened") && trace.saw("(it did not open with the magic bytes)"));
    second.hangUp();
    second.vanish();
    pause(450);

    // The car refuses the other service's request: the serial request is still there for its node.
    Node third(dir + "/btspp", 2);
    gap.phone(kInfServiceRequest, kCarSerialService);
    gap.phone(kInfServiceRequestError, "AudioSink");
    third.appear();
    Link* other = dialled(listener, third, trace);
    CHECK(trace.saw("btspp2: it opened with the magic bytes, so the link is the agent's"));
    CHECK(listener.serialStatus().refusals == 0);   // not a refusal of a serial link
    exchange(third, other, "still", "noch");

    first.hangUp();
    third.hangUp();
    CHECK(endsWithin(link, 3000) && endsWithin(other, 3000));
    delete link;
    delete other;
    listener.stop();
}

// ---- a refusal ------------------------------------------------------------------------------------------------

static void testRefusal() {
    std::string dir = freshFolder("refusal");
    Trace trace;
    LinksGone gone;
    FakeGap gap(&trace);
    FakeGate gate(&trace);
    SerialListener listener(&gap, &gate, &trace, configFor(dir));
    std::string why;
    CHECK_IN(why, listener.start(&why));
    gap.attach();
    CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));
    // The phone is in the car's device list; another program's link is up on a node.
    gap.listed = true;
    gap.connection = 2;
    gap.security = 7;
    Node other(dir + "/btspp", 0);
    other.appear();
    pause(450);
    gap.phone(kInfServiceRequest, kCarSerialService);
    gap.phone(kInfServiceRequestError, kCarSerialService);
    // Connected with no service connected: the line says that the phone has no media audio at the car, and the status
    // counts it.
    CHECK_IN(trace.text(), trace.saw("the car refused the phone's link (1 so far). In the car's device list the phone is "
                                     "connected (connection 2), security 7. So the car has no media audio connection to "
                                     "the phone, and the car refuses the phone's own serial links while it has none, "
                                     "so the phone's app cannot knock meanwhile: the driver can connect it again by "
                                     "choosing the phone in the car's media menu. A serial node is there already and "
                                     "is not the agent's: the car takes one phone-opened serial link at a time. Other "
                                     "causes, in the order to look at them: the iDrive Bluetooth pairing menu is "
                                     "open"));
    CHECK(trace.exactly("device list") == 1 && listener.serialStatus().refusalsNoAudio == 1);
    // The 2nd to 19th are not looked into or said; the 20th is.
    for (int i = 2; i <= 20; ++i) {
        gap.phone(kInfServiceRequest, kCarSerialService);
        gap.phone(kInfServiceRequestError, kCarSerialService);
    }
    CHECK_IN(trace.text(), trace.exactly("device list") == 2 && trace.count("the car refused the phone's link (") == 2 &&
                               trace.saw("the car refused the phone's link (20 so far)"));
    // Said, and counted, with the line only: not for every refusal.
    CHECK(trace.count("no media audio connection") == 2 && listener.serialStatus().refusalsNoAudio == 2 &&
          listener.serialStatus().refusals == 20);
    // A device list that does not come: the line is said without it, after the time.
    gap.answersList = false;
    other.vanish();
    for (int i = 21; i <= 40; ++i) {
        gap.phone(kInfServiceRequest, kCarSerialService);
        gap.phone(kInfServiceRequestError, kCarSerialService);
    }
    pause(450);
    CHECK(!trace.saw("(40 so far)"));
    CHECK_IN(trace.text(), sawWithin(trace, "the car refused the phone's link (40 so far). The car's device list did not "
                                            "come in 3 s.", 4000));
    CHECK(listener.serialStatus().refusalsNoAudio == 2);
    CHECK(!trace.saw("Test Phone") && !trace.saw("AA:BB") && !trace.saw("AABB"));
    listener.stop();
    {   // a refusal is a problem for the problem file: the first one only
        Trace trace2;
        FakeGap gap2(&trace2);
        FakeGate gate2(&trace2);
        std::string file = dir + "/serial-problems.log";
        ProblemLog problems(file, "1.0.0 (build test)");
        SerialListener listener2(&gap2, &gate2, &trace2, configFor(dir));
        listener2.setProblemLog(&problems);
        CHECK_IN(why, listener2.start(&why));
        gap2.attach();
        CHECK_IN(trace2.text(), sawWithin(trace2, "the serial service 'NBTApp' is registered", 2000));
        for (int i = 0; i < 3; ++i) {
            gap2.phone(kInfServiceRequest, kCarSerialService);
            gap2.phone(kInfServiceRequestError, kCarSerialService);
        }
        double end = monoNow() + 3.0;
        while (fs::kindOf(file) == fs::kNone && monoNow() < end) pause(10);
        listener2.stop();
        std::string text = testutil::readFile(file);
        CHECK_IN(text, text.find(": the car refused a phone's serial link (1 so far)\n") != std::string::npos);
        CHECK(problems.recorded() == 1 && problems.counted() == 2 && text.find("AABB") == std::string::npos);
    }
}

// ---- which of the car's phones are there --------------------------------------------------------------------------

static BtDevice carPhone(const char* address, uint32_t connection, uint32_t security) {
    BtDevice d;
    d.address = address;
    d.connection = connection;
    d.security = security;
    return d;
}

static void testPresence() {
    std::string dir = freshFolder("presence");
    {   // the device list is asked for once the service is registered, and again after the service comes back; what
        // changes of whether a phone is there, and of its security, is said by the phone's number
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gap.phones.push_back(carPhone("00:11:22:33:44:55", kConnConnected, 7));
        gap.phones.push_back(carPhone("66:77:88:99:AA:BB", kConnInRange, 8));
        SerialConfig config = configFor(dir);
        config.dial.on = true;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        CHECK(!trace.saw("device list"));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the car's device list: phone 2 is in range (connection 1), security 8", 2000));
        CHECK(trace.exactly("device list") == 1 && trace.at("is registered") < trace.at("device list"));
        CHECK(trace.saw("the car's device list: phone 1 is connected (connection 2), security 7"));
        // An inform: phone 2 comes, phone 1 loses its key; a change from connected to service connected is not said.
        gap.phones[0] = carPhone("00:11:22:33:44:55", kConnServiceConnected, 9);
        gap.phones[1] = carPhone("66:77:88:99:AA:BB", kConnConnected, 8);
        gap.devicesChanged();
        CHECK_IN(trace.text(), trace.saw("phone 1 is service connected (connection 3), security 9: the car has its key and "
                                         "the phone has not"));
        CHECK(trace.saw("phone 2 is connected (connection 2), security 8"));
        unsigned lines = trace.count("the car's device list:");
        gap.phones[0] = carPhone("00:11:22:33:44:55", kConnConnected, 9);
        gap.devicesChanged();
        CHECK(trace.count("the car's device list:") == lines);
        gap.phones.pop_back();
        gap.devicesChanged();
        CHECK(trace.saw("phone 2 is no longer in it"));
        // The service goes and comes back: asked again, and the phones keep their numbers.
        gap.detach();
        pause(450);
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "registered", 2000));
        double end = monoNow() + 2.0;
        while (trace.exactly("device list") < 2 && monoNow() < end) pause(5);
        CHECK(trace.exactly("device list") == 2);
        CHECK(!trace.saw("00:11:22") && !trace.saw("66:77:88") && !trace.saw("Test Phone"));
        listener.stop();
    }
    {   // the dial off: nothing of it
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        gap.phones.push_back(carPhone("00:11:22:33:44:55", kConnConnected, 7));
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));
        gap.devicesChanged();
        pause(300);
        CHECK(!trace.saw("device list") && !trace.saw("phone 1"));
        listener.stop();
    }
}

// ---- the car dials the phone ----------------------------------------------------------------------------------------

static const char* const kDialled = "00:11:22:33:44:55";

// A node that takes at most 7 bytes of a write.
static long shortWrite(int fd, const void* buf, size_t len) { return (long)write(fd, buf, len < 7 ? len : 7); }

// A listener that dials, with the tests' short times, and phone kDialled connected to the car and paired with it.
struct Dialler {
    Trace trace;
    LinksGone gone;
    FakeGap gap;
    FakeGate gate;
    std::string dir;
    PhoneAddressList ours;
    SerialConfig config;
    SerialListener* listener;
    ProblemLog* problems;   // given to the listener before it starts; 0: none

    explicit Dialler(const std::string& name)
        : gap(&trace), gate(&trace), dir(freshFolder(name)), listener(0), problems(0) {
        config = configFor(dir);
        config.dial.on = true;
        config.dial.node = dir + "/nbtapp";
        config.dial.magicWaitMs = 400;
        config.dial.disconnectWaitMs = 500;
        config.dial.replyLimitMs = 1000;
        config.writeNode = &shortWrite;
        // The phone is not the agent's unless a case says so: no poll dials it, and only a forced dial does.
        gap.phones.push_back(carPhone(kDialled, kConnConnected, 7));
    }
    ~Dialler() {
        if (listener != 0) {
            listener->stop();
            delete listener;
        }
        // NBT_TEST_TRACE=1 shows what each case did.
        if (getenv("NBT_TEST_TRACE") != 0) fprintf(stderr, "---- %s\n%s", dir.c_str(), trace.text().c_str());
    }
    void start() {
        listener = new SerialListener(&gap, &gate, &trace, config);
        listener->setPhoneAddresses(&ours);
        listener->setProblemLog(problems);
        std::string why;
        CHECK_IN(why, listener->start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the car's device list: phone 1 is connected", 2000));
    }
    // A forced dial of the phones there.
    void dial(unsigned phones = 1) {
        std::string why;
        CHECK_IN(why, listener->dialNow(&why) == phones);
    }
};

static void testDial() {
    {   // the car dials, the node comes up, the app says the magic bytes: a serial link of the agent's, with the
        // phone's address, that carries bytes both ways through a node that takes writes short
        Dialler d("dial");
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.start();
        std::string why;
        node.say(kMagic);   // the app writes once it has accepted; here it is waiting already
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap connect BtCtrl nbtapp 00:11:22:33:44:55 NBTApp", 2000));
        Link* link = acceptWithin(*d.listener, 3000);
        CHECK_IN(d.trace.text(), link != 0);
        CHECK(!d.trace.logged("the car dials it"));   // one line for the dial, with its answer
        CHECK(std::string(link->kind()) == "serial" && link->phoneAddress() == kDialled && link->dialledByCar());
        CHECK(d.listener->serialStatus().link == "dialled" && d.listener->serialStatus().dialState == "up");
        CHECK(readFrom(link, kMagic.size(), 3000) == kMagic);
        CHECK(d.trace.logged("phone 1 (asked for): the car's dial answered 0 OK in "));
        CHECK(d.trace.logged("opened with the magic bytes") && d.trace.logged("the dialled link is the agent's"));
        CHECK(serialLinksOpen() == 1 && dialledLinksOpen() == 1);
        exchange(node, link, pattern(30000, 7), pattern(30000, 8));
        // No dial while it is up.
        CHECK(d.listener->dialNow(&why) == 0 && why == "a link of the agent's is up, or a dial is out");
        pause(300);
        CHECK(d.trace.count("gap connect") == 1);
        // The phone ends it: the node goes, its read ends, and the car says so.
        node.vanish();
        node.hangUp();
        d.gap.ended("nbtapp", kDialled, "NBTApp", 1);
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: the dialled link has ended", 3000));
        CHECK(waitForSerialLinks(2000) && dialledLinksOpen() == 0);
        SerialStatus st = d.listener->serialStatus();
        CHECK(st.dials == 1 && st.answeredOk == 1 && st.lastResult == "OK" && st.link == "none" && st.phonesEligible == 0);
        // Nothing of it holds the address or the phone's name.
        CHECK(!d.trace.logged("00:11:22") && !d.trace.logged("001122") && !d.trace.logged("Test Phone"));
    }
    {   // the car ends the dialled link (ServiceDisconnected for the instance), with the node still readable: its
        // node is closed at once, not read to the end
        Dialler d("dial-car-ends");
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.start();
        node.say(kMagic);
        d.dial();
        Link* link = acceptWithin(*d.listener, 3000);
        CHECK_IN(d.trace.text(), link != 0);
        CHECK(readFrom(link, kMagic.size(), 3000) == kMagic);
        d.gap.ended("nbtapp", kDialled, "NBTApp", 0);
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "nbtapp closed, the car ended the link", 3000));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: the dialled link has ended", 3000));
        // Another instance's end is none of the dial's.
        d.gap.ended("ExtBox1", kDialled, "SPP", 1);
    }
    {   // the app does not speak: the car is asked to end the link, and the phone is backed off
        Dialler d("dial-silent");
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.gap.answersDisconnect = true;
        d.start();
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "something answered on the agent's UUID that is not the app: "
                                                    "nothing came in the time", 3000));
        CHECK(d.trace.logged("the phone is not dialled by a poll for 30 s"));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap disconnect", 2000));
        Reader r(&d.gap.disconnect[0], (uint32_t)d.gap.disconnect.size());
        CHECK(r.str() == "BtCtrl" && r.str() == "nbtapp" && r.str() == kDialled && r.str() == "NBTApp");
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: ServiceDisconnect on the dialled instance answered 0 OK in ", 3000));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, node.path + " is not there", 3000));
        CHECK(acceptWithin(*d.listener, 100) == 0);
    }
    {   // something else speaks first: the same
        Dialler d("dial-other");
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.gap.answersDisconnect = true;
        d.start();
        node.say("SSH-2.0-another program\r\n");
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "something answered on the agent's UUID that is not the app: the "
                                                    "link did not open with the magic bytes", 3000));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "ServiceDisconnect on the dialled instance answered 0 OK", 3000));
        bool closed = false;
        CHECK(node.hear(1, 200, &closed).empty());   // not a byte was written into it
    }
    {   // the answers that open no node: a failure backs the phone off, which the line says
        struct Row {
            uint32_t error;
            const char* says;
            const char* then;
        } rows[] = {
            {5, "5 DEFAULT in ", "its app may have stopped listening since, or something else went wrong; it is not "
                                 "dialled by a poll for 30 s"},
            {14, "14 PAGE_TIMEOUT in ", ": the phone is not in reach, and it is not dialled by a poll for 30 s"},
            {1, "1 TIMEOUT in ", " ms: it is not dialled by a poll for 30 s"},
            {10, "10 ", " ms: again in 15 s"},
            {2, "2 BUSY in ", "its node is not there: again later"},
        };
        for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            Dialler d("dial-answers");
            Node node(d.dir + "/nbtapp");
            d.gap.connectError = rows[i].error;
            d.start();
            d.dial();
            CHECK_IN(d.trace.text(), sawWithin(d.trace, std::string("phone 1 (asked for): the car's dial answered ") + rows[i].says, 2000));
            CHECK_IN(d.trace.text(), d.trace.logged(rows[i].then));
            pause(200);
            CHECK(!d.trace.saw("nbtapp opened") && acceptWithin(*d.listener, 50) == 0 && !d.trace.saw("gap disconnect"));
        }
    }
    {   // no answer at all: given up, one ServiceDisconnect, and the phone backed off
        Dialler d("dial-no-answer");
        d.gap.answersConnect = false;
        d.gap.answersDisconnect = true;
        d.gap.disconnectError = 10;
        d.start();
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: no answer to the car's dial in 1 s", 3000));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap disconnect", 2000));
        CHECK(d.trace.count("gap disconnect") == 1);
    }
    {   // pairing trouble: a phone the car is pairing anew is not dialled, forced or not
        Dialler d("dial-pairing");
        d.start();
        std::vector<uint8_t> p;
        putString(p, kDialled);
        putString(p, "Test Phone");
        d.gap.pairing(kInfSimplePairingConfirmationRequired, p);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: the car is pairing it anew, or a pairing failed", 2000));
        std::string why;
        CHECK(d.listener->dialNow(&why) == 0 && why == "no phone is connected to the car and paired with it");
        CHECK(!d.trace.logged("Test Phone"));
    }
    {   // a pairing that worked stops nothing: PairingComplete naming the phone, and one naming nobody while the
        // phone is being dialled, leave it dialled
        Dialler d("dial-pairing-complete");
        d.gap.answersConnect = false;
        d.start();
        std::vector<uint8_t> p;
        putString(p, kDialled);
        putString(p, "Test Phone");
        d.gap.pairing(kInfPairingComplete, p);
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap connect BtCtrl nbtapp 00:11:22:33:44:55 NBTApp", 2000));
        d.gap.pairing(kInfPairingComplete, std::vector<uint8_t>());
        pause(100);
        d.gap.connectReply(kDialled, "NBTApp", 5);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 (asked for): the car's dial answered 5 DEFAULT", 2000));
        pause(100);
        d.dial();   // still dialled
        double end = monoNow() + 2.0;
        while (d.trace.count("gap connect BtCtrl nbtapp") < 2 && monoNow() < end) pause(5);
        CHECK_IN(d.trace.text(), d.trace.count("gap connect BtCtrl nbtapp") == 2);
        CHECK(!d.trace.logged("pairing anew") && !d.trace.logged("is not dialled until"));
    }
    {   // PairingError and SimplePairingConfirmationRequired still stop the dials, the first naming nobody while the
        // phone is being dialled; the log says so once, and never the phone's address or name
        uint32_t informs[] = { kInfPairingError, kInfSimplePairingConfirmationRequired };
        for (size_t i = 0; i < 2; ++i) {
            Dialler d("dial-pairing-trouble");
            d.gap.answersConnect = false;
            d.start();
            d.dial();
            CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap connect BtCtrl nbtapp", 2000));
            d.gap.pairing(informs[i], std::vector<uint8_t>());
            std::vector<uint8_t> p;
            putString(p, kDialled);
            putString(p, "Test Phone");
            d.gap.pairing(informs[i], p);
            pause(100);
            d.gap.connectReply(kDialled, "NBTApp", 5);
            CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 (asked for): the car's dial answered 5 DEFAULT", 2000));
            CHECK(d.trace.loggedCount("phone 1: the car is pairing it anew, or a pairing failed") == 1);
            std::string why;
            CHECK(d.listener->dialNow(&why) == 0 && why == "no phone is connected to the car and paired with it");
            CHECK(!d.trace.logged("Test Phone") && !d.trace.logged(kDialled));
        }
    }
    {   // a ServiceConnect reply for another client while the dial is out, for another service and then another
        // phone, is left: the dial's own answer that follows is taken, and the dial is not given up
        Dialler d("dial-others-reply");
        d.gap.answersConnect = false;
        d.gap.phones.push_back(carPhone("66:77:88:99:AA:BB", kConnConnected, 7));
        d.start();
        d.dial(2);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap connect BtCtrl nbtapp 00:11:22:33:44:55 NBTApp", 2000));
        d.gap.connectReply(kDialled, "Other_Service", 0);
        d.gap.connectReply("66:77:88:99:AA:BB", "NBTApp", 0);
        pause(600);
        d.gap.connectReply("001122334455", "NBTApp", 5);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 (asked for): the car's dial answered 5 DEFAULT", 2000));
        CHECK(d.trace.loggedCount("a ServiceConnect reply for another client came while the agent's dial waited") == 2);
        // The round goes on to the second phone, whose dial is answered in turn.
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap connect BtCtrl nbtapp 66:77:88:99:AA:BB NBTApp", 2000));
        d.gap.connectReply("66:77:88:99:AA:BB", "NBTApp", 14);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 2 (asked for): the car's dial answered 14 PAGE_TIMEOUT", 2000));
        pause(1200);
        CHECK(!d.trace.logged("no answer to the car's dial") && !d.trace.saw("gap disconnect"));
        CHECK(!d.trace.logged(kDialled) && !d.trace.logged("66:77:88") && !d.trace.logged("001122"));
    }
    {   // the agent ends the dialled link and the phone does not: after the phone's time the car is asked to, and the
        // node is closed, not kept and read
        Dialler d("dial-agent-ends");
        d.config.dial.closeWaitMs = 500;
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.gap.answersDisconnect = true;
        d.start();
        node.say(kMagic);
        d.dial();
        Link* link = acceptWithin(*d.listener, 3000);
        CHECK_IN(d.trace.text(), link != 0);
        CHECK(readFrom(link, kMagic.size(), 3000) == kMagic);
        double at = monoNow();
        delete link;
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: the agent ended the dialled link and the phone has not in "
                                                    "500 ms: the car is asked to end it", 3000));
        CHECK(monoNow() - at >= 0.45);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "ServiceDisconnect on the dialled instance answered 0 OK", 3000));
        CHECK(waitForSerialLinks(2000) && d.trace.count("gap disconnect") == 1);
        Reader r(&d.gap.disconnect[0], (uint32_t)d.gap.disconnect.size());
        CHECK(r.str() == "BtCtrl" && r.str() == "nbtapp" && r.str() == kDialled && r.str() == "NBTApp");
        CHECK(!d.trace.saw("is still up"));
    }
    {   // the phone ends it within its time: the car is not asked
        Dialler d("dial-phone-ends-in-time");
        d.config.dial.closeWaitMs = 1000;
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.start();
        node.say(kMagic);
        d.dial();
        Link* link = acceptWithin(*d.listener, 3000);
        CHECK_IN(d.trace.text(), link != 0);
        delete link;
        pause(200);
        node.vanish();
        node.hangUp();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: the dialled link has ended", 3000));
        pause(1000);
        CHECK(!d.trace.saw("gap disconnect"));
    }
    {   // serial.reset with a dialled link up: the agent is to end it
        Dialler d("dial-reset");
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.start();
        node.say(kMagic);
        d.dial();
        Link* link = acceptWithin(*d.listener, 3000);
        CHECK_IN(d.trace.text(), link != 0);
        int64_t forMs = 0;
        std::string why;
        CHECK(d.listener->resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetDialled);
        CHECK(d.trace.logged("serial.reset: a link the car dialled is up: the agent ends it"));
        node.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
    }
    {   // the listener stops with a dialled link up: the car ends it first, and then the registration goes
        Dialler d("dial-stop");
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.gap.answersDisconnect = true;
        d.start();
        node.say(kMagic);
        d.dial();
        Link* link = acceptWithin(*d.listener, 3000);
        CHECK_IN(d.trace.text(), link != 0);
        d.listener->stop();
        CHECK_IN(d.trace.text(), d.trace.at("gap disconnect") >= 0 && d.trace.at("gap disconnect") < d.trace.at("deregister"));
        CHECK(endsWithin(link, 3000));
        delete link;
    }
    {   // a link left behind on the dialled instance when the agent starts: ended for each phone there in turn
        // until the node has gone (an error answer for a phone without such a link does no harm), never taken, and
        // then the phones are dialled afresh
        Dialler d("dial-stale-start");
        Node node(d.dir + "/nbtapp");
        node.appear();
        d.gap.dialNode = &node;
        d.gap.phones.insert(d.gap.phones.begin(), carPhone("66:77:88:99:AA:BB", kConnConnected, 8));
        d.gap.answersDisconnect = true;
        d.gap.disconnectErrorFor["66:77:88:99:AA:BB"] = 10;
        d.gap.connectError = 5;
        d.ours.set(std::vector<std::string>(1, kDialled));
        d.config.dial.pollFirstMs = d.config.dial.pollLaterMs = 3600000;
        d.start();
        CHECK(d.trace.logged("nbtapp exists already: a link an earlier agent left behind"));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "the car dials the phones afresh", 4000));
        CHECK(d.trace.count("gap disconnect") == 2 && d.trace.logged("the link left behind on " + node.path + " is ended"));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap connect BtCtrl nbtapp 00:11:22:33:44:55 NBTApp", 2000));
        CHECK(!d.trace.saw("gap connect BtCtrl nbtapp 66:77") && !d.trace.saw("nbtapp opened"));
        CHECK(!d.trace.logged("66:77:88") && !d.trace.logged("00:11:22"));
    }
    {   // the same, and the car never answers the first phone's ServiceDisconnect: it is given up after the clean-up's
        // wait, the next phone's goes, the link ends, and the phones are dialled afresh
        Dialler d("dial-stale-unanswered");
        Node node(d.dir + "/nbtapp");
        node.appear();
        d.gap.dialNode = &node;
        d.gap.phones.insert(d.gap.phones.begin(), carPhone("66:77:88:99:AA:BB", kConnConnected, 8));
        d.gap.answersDisconnect = true;
        d.gap.unansweredFor = "66:77:88:99:AA:BB";
        d.gap.connectError = 5;
        d.ours.set(std::vector<std::string>(1, kDialled));
        d.config.dial.pollFirstMs = d.config.dial.pollLaterMs = 3600000;
        d.start();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "no answer to ServiceDisconnect on the dialled instance in ", 3000));
        CHECK(d.trace.logged(" ms; " + node.path + " is still there") && !d.trace.logged("before its answer"));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "the car dials the phones afresh", 3000));
        CHECK(d.trace.count("gap disconnect") == 2 && d.trace.logged("the link left behind on " + node.path + " is ended"));
        CHECK(d.trace.logged("ServiceDisconnect on the dialled instance answered 0 OK"));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap connect BtCtrl nbtapp 00:11:22:33:44:55 NBTApp", 2000));
        CHECK(!d.trace.logged("66:77:88") && !d.trace.logged("00:11:22"));
    }
    {   // serial.reset while the dial's clean-up waits for the answer to its ServiceDisconnect: one at a time, so the
        // reset's goes once that answer has come, and the log says both
        Dialler d("dial-reset-waits");
        d.config.dial.disconnectWaitMs = 3000;
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.start();
        d.dial();
        // The app says nothing: the car is asked to end the link.
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap disconnect", 3000));
        std::vector<uint8_t> cleanup = d.gap.disconnect;
        // A phone's own link, which serial.reset is to end.
        Node first(d.dir + "/btspp", 0);
        d.gap.phone(kInfServiceRequest, kCarSerialService);
        first.appear();
        Link* link = dialled(*d.listener, first, d.trace);
        int64_t forMs = 0;
        std::string why;
        CHECK(d.listener->resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetSent);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "serial.reset: another ServiceDisconnect waits for the car's answer: "
                                                    "this one goes once that is answered, or given up after 3000 ms", 2000));
        pause(300);
        CHECK(d.trace.count("gap disconnect") == 1);
        d.gap.answerDisconnect(cleanup);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "serial.reset: ServiceDisconnect sent, ", 2000));
        CHECK(d.trace.logged(" ms after it was asked for, once the one before it was answered or given up"));
        CHECK(d.trace.count("gap disconnect") == 2);
        CHECK(d.gap.disconnect == buildServiceDisconnect("BtCtrl", "ExtBox1", "AABBCCDDEEFF", kCarSerialService));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: ServiceDisconnect on the dialled instance answered 0 OK", 2000));
        first.hangUp();
        first.vanish();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "serial.reset: " + first.path + " went ", 2000));
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK(!d.trace.logged("AABBCCDDEEFF") && !d.trace.logged("00:11:22"));
    }
    {   // the same with no answer to the clean-up's, and serial.reset armed for the next link: at that link's open
        // the reset's waits, and goes once the clean-up's is given up
        Dialler d("dial-reset-waits-unanswered");
        d.config.dial.disconnectWaitMs = 1500;
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.start();
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap disconnect", 3000));
        double cleanupAt = monoNow();
        int64_t forMs = 0;
        std::string why;
        CHECK(d.listener->resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetArmed);
        Node first(d.dir + "/btspp", 0);
        d.gap.phone(kInfServiceRequest, kCarSerialService);
        first.appear();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "btspp0: serial.reset asked ", 2000));
        CHECK(d.trace.logged("With no decide step, ServiceDisconnect goes once another that waits for the car's answer "
                             "is answered or given up"));
        CHECK(d.trace.count("gap disconnect") == 1);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "once the one before it was answered or given up", 3000));
        CHECK(monoNow() - cleanupAt >= 1.4);
        CHECK(d.trace.count("gap disconnect") == 2);
        CHECK(d.gap.disconnect == buildServiceDisconnect("BtCtrl", "ExtBox1", "AABBCCDDEEFF", kCarSerialService));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: no answer to ServiceDisconnect on the dialled instance in ", 2000));
        CHECK(!d.trace.logged("before its answer"));
        first.hangUp();
        first.vanish();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "serial.reset: " + first.path + " went ", 2000));
        CHECK(!d.trace.logged("AABBCCDDEEFF") && !d.trace.logged("00:11:22"));
    }
    {   // the node goes a moment before the car's answer to the clean-up's ServiceDisconnect comes: the log says the
        // node went before the answer, not that there was none, and the answer that comes after is the car's own line
        Dialler d("dial-gone-before-answer");
        d.config.dial.disconnectWaitMs = 3000;
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.start();
        d.dial();
        // The app says nothing: the car is asked to end the link.
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap disconnect", 3000));
        std::vector<uint8_t> cleanup = d.gap.disconnect;
        double sentAt = monoNow();
        d.gap.dialDown();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: " + node.path + " went ", 2000));
        CHECK(monoNow() - sentAt < 2.5);
        CHECK(d.trace.logged(" ms after ServiceDisconnect on the dialled instance, before its answer"));
        CHECK(!d.trace.logged("no answer to ServiceDisconnect"));
        d.gap.answerDisconnect(cleanup);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "ServiceDisconnect reply: 0 OK", 2000));
        pause(300);
        CHECK(!d.trace.logged("no answer to ServiceDisconnect") && d.trace.count("gap disconnect") == 1);
        CHECK(!d.trace.logged("00:11:22") && !d.trace.logged("001122"));
    }
    {   // a dial answered 2 with the node there: a link left behind, ended for the dialled phone, which is then
        // dialled again, once
        Dialler d("dial-stale-busy");
        Node node(d.dir + "/nbtapp");
        node.appear();
        d.gap.dialNode = &node;
        d.gap.answersDisconnect = true;
        d.gap.nextConnectErrors.push_back(2);
        d.gap.connectError = 5;
        d.config.dial.node = d.dir + "/nbtapp";
        // Not there at the start: it comes with the link the dial finds.
        node.vanish();
        d.start();
        node.appear();
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "a link left behind on the dialled instance, which the agent does not "
                                                    "take", 3000));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 (afresh, after a link left behind): the car's dial answered", 3000));
        pause(300);
        CHECK(d.trace.count("gap connect") == 2 && d.trace.count("gap disconnect") == 1);
    }
    {   // the same, and the car does not end that link: the phone is backed off and not dialled again at once. The
        // first round is said in full, the same again only counted, and one that ends it says how many came before;
        // the problem file says what is true when it is written, once each, and does not grow with the rounds
        std::string file = gWork + "/dial-stale-stays/serial-problems.log";
        ProblemLog problems(file, "1.0.0 (build test)");
        Dialler d("dial-stale-stays");
        d.problems = &problems;
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.gap.connectError = 2;
        d.config.dial.pollFirstMs = d.config.dial.pollLaterMs = 200;
        d.start();
        node.appear();
        d.ours.set(std::vector<std::string>(1, kDialled));   // polled from here on
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "after the car was asked to end its link", 4000));
        CHECK(d.trace.logged("no answer to ServiceDisconnect on the dialled instance in "));
        CHECK(d.trace.logged(" ms; " + node.path + " is still there") && !d.trace.logged("before its answer"));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 is not dialled again at once, and not by a poll for 30 s", 2000));
        pause(1200);
        CHECK_IN(d.trace.text(), d.trace.count("gap connect") == 1 && d.trace.count("gap disconnect") == 1);
        double end = monoNow() + 3.0;
        while (fs::kindOf(file) == fs::kNone && monoNow() < end) pause(10);
        pause(300);
        std::string text = testutil::readFile(file);
        CHECK_IN(text, text.find(": the car is asked to end a link left behind on the dialled instance\n") != std::string::npos);
        CHECK_IN(text, text.find(": a link left behind on the dialled instance was not ended\n") != std::string::npos);
        // Two more rounds that come to the same, forced: only counted.
        for (unsigned i = 2; i <= 3; ++i) {
            std::string why;
            CHECK_IN(why, d.listener->dialNow(&why) == 1);
            end = monoNow() + 3.0;
            while (d.trace.count("gap disconnect") < i && monoNow() < end) pause(5);
            end = monoNow() + 3.0;
            while (d.listener->serialStatus().dialState != "idle" && monoNow() < end) pause(5);
            pause(100);
        }
        CHECK_IN(d.trace.text(), d.trace.count("gap connect") == 3 && d.trace.count("gap disconnect") == 3);
        CHECK(d.trace.loggedCount("is there: a link left behind on the dialled instance") == 1);
        CHECK(d.trace.loggedCount("no answer to ServiceDisconnect on the dialled instance") == 1);
        CHECK(d.trace.loggedCount("ServiceDisconnect device 'BtCtrl'") == 1);
        CHECK(d.trace.loggedCount("after the car was asked") == 1 && d.trace.loggedCount("is not dialled again at once") == 1);
        CHECK(testutil::readFile(file) == text && problems.recorded() == 3);
        // The car ends it this time: said, with the rounds before it, and the phone is dialled again at once.
        d.gap.answersDisconnect = true;
        std::string why;
        CHECK_IN(why, d.listener->dialNow(&why) == 1);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "has ended this time (the same outcome as before came 2 more times "
                                                    "before this)", 3000));
        CHECK(d.trace.logged("phone 1: ServiceDisconnect on the dialled instance answered 0 OK"));
        end = monoNow() + 3.0;
        while (d.trace.count("gap connect") < 5 && monoNow() < end) pause(5);
        CHECK_IN(d.trace.text(), d.trace.count("gap connect") == 5);
        CHECK(!d.trace.logged("00:11:22") && !d.trace.logged("001122"));
    }
    {   // the registration refused while a link an earlier agent left behind holds the service: that link is ended
        // first, with no registration, and the service registered again at once
        Dialler d("dial-left-register");
        Node node(d.dir + "/nbtapp");
        node.appear();
        d.gap.dialNode = &node;
        d.gap.refusesWhileDialled = true;
        d.gap.answersDisconnect = true;
        d.gap.connectError = 5;
        d.start();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "the serial service 'NBTApp' is registered", 3000));
        CHECK(d.trace.logged("did not take the registration while " + node.path + " is there"));
        CHECK(d.trace.logged("phone 1: ServiceDisconnect on the dialled instance answered 0 OK"));
        CHECK(d.trace.logged("has ended: the serial service is registered again now"));
        CHECK(d.trace.count("gap disconnect") == 1 && d.trace.exactly("register") == 3);
        CHECK(d.trace.at("gap disconnect") < d.trace.at("'NBTApp' is registered") && !d.trace.logged("is set up again in"));
        pause(300);
        CHECK(d.trace.count("gap disconnect") == 1);   // not ended a second time once registered
        CHECK(!d.trace.logged("00:11:22") && !d.trace.logged("001122"));
    }
    {   // the same, and the car does not end it: the refusal goes as any other; each set-up tries once more, with what
        // the clean-up came to only counted while it is the same; once the link has gone, the service is registered
        Dialler d("dial-left-register-stays");
        Node node(d.dir + "/nbtapp");
        node.appear();
        d.gap.dialNode = &node;
        d.gap.refusesWhileDialled = true;
        d.config.retryMs = 1000;
        d.start();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: no answer to ServiceDisconnect on the dialled instance in ", 3000));
        CHECK(d.trace.logged(" ms; " + node.path + " is still there") && !d.trace.logged("before its answer"));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "did not take the registration: Bluetooth serial is off, and is set "
                                                    "up again in 1 s", 3000));
        CHECK(d.trace.logged(node.path + " is still there after the car was asked to end its link"));
        CHECK(d.trace.count("gap disconnect") == 1);
        double end = monoNow() + 5.0;
        while (d.trace.loggedCount("is set up again in 1 s") < 2 && monoNow() < end) pause(5);
        CHECK_IN(d.trace.text(), d.trace.loggedCount("is set up again in 1 s") == 2 && d.trace.count("gap disconnect") == 2);
        CHECK(d.trace.loggedCount("did not take the registration while") == 2);
        CHECK(d.trace.loggedCount("no answer to ServiceDisconnect") == 1 && d.trace.loggedCount("after the car was asked") == 1);
        // The phone gives the link up: the next set-up registers.
        node.vanish();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "the serial service 'NBTApp' is registered", 4000));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "which was there when the agent started, has gone without the agent "
                                                    "ending it", 2000));
        pause(300);
        CHECK(d.trace.count("gap disconnect") == 2);
        CHECK(!d.trace.logged("00:11:22") && !d.trace.logged("001122"));
    }
    {   // the car ends a phone's request as its own dial to that phone comes up: while the dial is out, or just after
        // its answer 0, that is no refusal (not counted, no problem line, no advice); a while after, it is one
        std::string file = gWork + "/dial-overtaken/serial-problems.log";
        ProblemLog problems(file, "1.0.0 (build test)");
        Dialler d("dial-overtaken");
        d.problems = &problems;
        d.gap.answersConnect = false;
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.start();
        node.say(kMagic);
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap connect BtCtrl nbtapp", 2000));
        d.gap.phone(kInfServiceRequest, kCarSerialService, "001122334455");
        d.gap.phone(kInfServiceRequestError, kCarSerialService);
        CHECK(d.trace.logged("a phone's request for a serial link (service 'SPP') ended as the car's own dial came up: "
                             "not a refusal"));
        node.appear();
        d.gap.connectReply(kDialled, "NBTApp", 0);
        Link* link = acceptWithin(*d.listener, 3000);
        CHECK_IN(d.trace.text(), link != 0);
        d.gap.phone(kInfServiceRequestError, kCarSerialService);
        CHECK(d.trace.loggedCount("ended as the car's own dial came up") == 2);
        pause(300);
        CHECK(!d.trace.logged("was refused a serial link") && !d.trace.logged("the car refused the phone's link"));
        CHECK(d.listener->serialStatus().refusals == 0 && problems.recorded() == 0);
        // A while after the answer: a refusal as any other.
        pause(800);
        d.gap.phone(kInfServiceRequestError, kCarSerialService);
        CHECK(d.trace.logged("a phone was refused a serial link by the car") && d.listener->serialStatus().refusals == 1);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "the car refused the phone's link (1 so far)", 4000));
        CHECK(problems.recorded() == 1);
        node.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
    }
    {   // the triggers: a poll as soon as the device list has come; a phone's own request that gives no link of the
        // agent's; the pairing window, for a phone that is not the agent's
        Dialler d("dial-triggers");
        d.ours.set(std::vector<std::string>(1, kDialled));
        d.config.dial.signalWaitMs = 300;
        d.config.dial.pollLaterMs = 3600000;
        d.config.dial.pollFirstMs = 3600000;
        d.config.dial.backOffMs = 1500;
        d.gap.connectError = 5;
        d.gap.phones.push_back(carPhone("66:77:88:99:AA:BB", kConnConnected, 8));
        d.start();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 (poll): the car's dial answered 5 DEFAULT", 2000));
        double failed = monoNow();
        pause(300);
        CHECK(d.trace.count("gap connect") == 1);   // the other phone is not the agent's
        // The phone asks the car for a link and none of the agent's comes: dialled signalWaitMs after the request,
        // while the request is still pending (linkWaitMs, 3 s), but not before the back-off that the failed dial
        // brought is over; serial.dial is busy meanwhile.
        double asked = monoNow();
        d.gap.phone(kInfServiceRequest, kCarSerialService, "001122334455");
        std::string why;
        CHECK(d.listener->dialNow(&why) == 0 && why == "a link of the agent's is up, or a dial is out");
        pause(500);
        CHECK(d.trace.count("gap connect") == 1);   // its time has run, and it is still backed off
        // The same answer as the last: counted, not said again.
        double end = monoNow() + 3.0;
        while (d.trace.count("gap connect") < 2 && monoNow() < end) pause(5);
        double after = monoNow() - asked;
        CHECK(d.trace.count("gap connect BtCtrl nbtapp 00:11:22:33:44:55") == 2);
        CHECK_IN(d.trace.text(), monoNow() - failed >= 1.45 && after < 2.5);
        pause(100);
        CHECK(d.trace.loggedCount("phone 1 (") == 1);
        // Not for a phone that is not the agent's.
        d.gap.phone(kInfServiceRequest, kCarSerialService, "66:77:88:99:AA:BB");
        pause(600);
        CHECK(d.trace.count("gap connect") == 2);
        // The pairing window: every phone there, the other one too.
        d.listener->pairingWindow(true);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap connect BtCtrl nbtapp 66:77:88:99:AA:BB NBTApp", 5000));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 2 (", 2000));
        d.listener->pairingWindow(false);
        CHECK(d.trace.logged("the pairing window has closed"));
        // What serial.status says of it: counts and names, no address.
        SerialStatus st = d.listener->serialStatus();
        CHECK(st.registered && st.checkedMsAgo >= 0 && st.dialOn && st.dialState == "idle" && st.link == "none");
        CHECK(st.phonesPresent == 2 && st.phonesEligible == 1 && st.dials >= 3 && st.notListening == st.dials);
        CHECK(st.lastResult == "DEFAULT" && st.answeredOk == 0 && st.noAnswer == 0);
    }
    {   // the log: the same answer to the same phone again is counted, and the next that differs says how many; the
        // problem file: the first problem of each kind. A dial answered 5 after the phone listed the service is no
        // longer how a dial goes: the car's record of phone-opened links is doubted, said once, and that goes to
        // the file once
        std::string file = gWork + "/dial-log/serial-problems.log";
        ProblemLog problems(file, "1.0.0 (build test)");   // before the listener, which writes to it until it stops
        Dialler d("dial-log");
        d.problems = &problems;
        d.gap.connectError = 5;
        d.start();
        std::string why;
        for (int i = 0; i < 3; ++i) {
            CHECK_IN(why, d.listener->dialNow(&why) == 1);
            double end = monoNow() + 2.0;
            while (d.trace.count("gap connect") < (unsigned)i + 1 && monoNow() < end) pause(5);
            pause(250);
        }
        CHECK_IN(d.trace.text(), d.trace.loggedCount("the car's dial answered") == 1);
        CHECK(d.trace.logged("5 DEFAULT in ") && d.trace.logged("The same answer again is only counted"));
        CHECK(d.trace.loggedCount("may leave the car's record of phone-opened serial links wrong") == 1);
        CHECK(d.trace.logged("phone 1: the car's dial came to 5 DEFAULT although the phone's service list named the "
                             "agent's service"));
        CHECK(d.listener->serialStatus().recordDoubtful);
        d.gap.connectError = 1;
        CHECK(d.listener->dialNow(&why) == 1);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "answered 1 TIMEOUT in ", 2000));
        CHECK(d.trace.logged("(5 DEFAULT came 2 more times before this)"));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap connect", 2000));
        double end = monoNow() + 3.0;
        while (fs::kindOf(file) == fs::kNone && monoNow() < end) pause(10);
        std::string text = testutil::readFile(file);
        end = monoNow() + 3.0;
        while (text.find("TIMEOUT") == std::string::npos && monoNow() < end) {
            pause(10);
            text = testutil::readFile(file);
        }
        CHECK_IN(text, text.find("agent 1.0.0 (build test): phone 1: the car's dial answered 1 TIMEOUT\n") != std::string::npos);
        CHECK_IN(text, text.find(": phone 1: the car's dial came to 5 DEFAULT after the phone listed the agent's "
                                 "service: the car's record of phone-opened links may be wrong\n") != std::string::npos);
        CHECK(text.find("00:11") == std::string::npos);
        CHECK(d.trace.loggedCount("may leave the car's record") == 1);   // doubted already: not said again
        // The same again: counted, not written. The retry holds polls only; a forced dial goes at once.
        CHECK(d.listener->dialNow(&why) == 1);
        end = monoNow() + 3.0;
        while (problems.counted() == 0 && monoNow() < end) pause(10);
        CHECK(problems.counted() == 1 && problems.recorded() == 2 && testutil::readFile(file) == text);
        SerialStatus st = d.listener->serialStatus();
        CHECK(st.problemsRecorded == 2 && st.problemsCounted == 1 && st.otherAnswers == 2);
        CHECK(st.asks == 5 && st.asksListed == 5 && st.dials == 5 && st.recordDoubtful);
    }
    {   // a healthy run, with a link, leaves no problem file
        std::string file = gWork + "/dial-healthy/serial-problems.log";
        ProblemLog problems(file, "1.0.0 (build test)");
        Dialler d("dial-healthy");
        d.problems = &problems;
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.start();
        node.say(kMagic);
        d.dial();
        Link* link = acceptWithin(*d.listener, 3000);
        CHECK_IN(d.trace.text(), link != 0);
        node.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: the dialled link has ended", 3000));
        d.listener->stop();
        CHECK(fs::kindOf(file) == fs::kNone && problems.recorded() == 0 && problems.counted() == 0);
    }
    {   // with the dial on, a phone's own link is taken as before
        Dialler d("dial-phone-opened");
        d.start();
        Node first(d.dir + "/btspp", 0);
        d.gap.phone(kInfServiceRequest, kCarSerialService);
        first.appear();
        Link* link = dialled(*d.listener, first, d.trace);
        exchange(first, link, "both", "ways");
        CHECK(dialledLinksOpen() == 0 && !d.trace.saw("gap connect"));
        first.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
    }
}

// Waits until the trace has `n` items that hold `part`.
static bool countWithin(Trace& trace, const std::string& part, unsigned n, int ms) {
    double end = monoNow() + ms / 1000.0;
    while (trace.count(part) < n && monoNow() < end) pause(5);
    return trace.count(part) >= n;
}

// The car is asked for the phone's service list before every dial, and dials only if it names the agent's service.
static void testDialAsk() {
    {   // the app not listening: polls ask, never dial; the answer is said once and counted after; the three
        // ServiceListChanged informs of each ask change nothing. Once it lists the service the dial goes at once, and
        // the line says how many asks came before
        Dialler d("ask-not-listening");
        d.ours.set(std::vector<std::string>(1, kDialled));
        d.config.dial.pollFirstMs = d.config.dial.pollLaterMs = 200;
        d.gap.queryLists = false;
        d.gap.queryInforms = true;
        d.gap.connectError = 5;
        d.start();
        CHECK_IN(d.trace.text(), countWithin(d.trace, "gap query 00:11:22:33:44:55", 4, 3000));
        CHECK(!d.trace.saw("gap connect"));
        CHECK_IN(d.trace.text(), d.trace.loggedCount("does not name the agent's service 'NBTApp': its app is not "
                                                     "listening, and it is not dialled") == 1);
        CHECK(d.trace.logged("phone 1 (poll): its service list came in "));
        SerialStatus st = d.listener->serialStatus();
        CHECK(st.asks >= 4 && st.asksNotListed >= 3 && st.asksListed == 0 && st.dials == 0 && !st.recordDoubtful);
        unsigned lines = d.trace.loggedCount("");
        CHECK_IN(d.trace.text(), countWithin(d.trace, "gap query", st.asks + 3, 3000));
        CHECK_IN(d.trace.text(), d.trace.loggedCount("") == lines);   // nothing said of the asks or their informs
        // The app listens: the next ask lists the service, and the dial goes.
        d.gap.queryLists = true;
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap connect BtCtrl nbtapp 00:11:22:33:44:55 NBTApp", 3000));
        CHECK(d.trace.logged("names the agent's service 'NBTApp' again: the car dials it (not listed came "));
        CHECK(d.trace.at("gap query") < d.trace.at("gap connect"));
        CHECK(!d.trace.logged("00:11:22") && !d.trace.logged("001122"));
    }
    {   // no answer to an ask in its time: as not listed, and nothing held against the phone: the next poll asks
        // again, however many came. The third in a row is said, once
        Dialler d("ask-no-answer");
        d.ours.set(std::vector<std::string>(1, kDialled));
        d.config.dial.pollFirstMs = d.config.dial.pollLaterMs = 100;
        d.config.dial.askLimitMs = 500;
        d.gap.answersQuery = false;
        d.start();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: 3 asks of its service list in a row had an error or no "
                                                    "answer; polls go on asking it", 3000));
        CHECK_IN(d.trace.text(), countWithin(d.trace, "gap query", 5, 3000));
        CHECK(d.trace.loggedCount("no answer to the ask for its service list in 500 ms") == 1);
        CHECK(d.trace.loggedCount("asks of its service list in a row") == 1 && !d.trace.saw("gap connect"));
        CHECK(!d.trace.logged("polls leave it alone"));
        SerialStatus st = d.listener->serialStatus();
        CHECK(st.asks >= 5 && st.asksNoAnswer >= 4 && st.dials == 0);
        // Not the agent's any more: polls stop asking it, and once the last ask's time is over a forced dial asks; its
        // answer comes late, as another client's would, and is taken.
        d.ours.set(std::vector<std::string>());
        double end = monoNow() + 2.0;
        while (d.listener->serialStatus().dialState != "idle" && monoNow() < end) pause(5);
        unsigned asked = d.trace.count("gap query");
        d.dial();
        CHECK_IN(d.trace.text(), countWithin(d.trace, "gap query", asked + 1, 2000));
        d.gap.queryReply("66:77:88:99:AA:BB", true);   // another phone's: passed over
        pause(100);
        CHECK(!d.trace.saw("gap connect"));
        d.gap.queryReply(kDialled, true);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap connect BtCtrl nbtapp 00:11:22:33:44:55 NBTApp", 2000));
        CHECK(d.trace.logged("phone 1 (asked for): its service list came in "));
    }
    {   // an error answer, and one that names another phone's services, and a phone's request that comes while the
        // ask is out: the ask is ended, its answer passed over, and the phone's own link is taken as before
        Dialler d("ask-ended");
        d.gap.answersQuery = false;
        d.start();
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap query", 2000));
        CHECK(d.listener->serialStatus().dialState == "asking");
        std::string why;
        CHECK(d.listener->dialNow(&why) == 0 && why == "a link of the agent's is up, or a dial is out");
        Node first(d.dir + "/btspp", 0);
        d.gap.phone(kInfServiceRequest, kCarSerialService);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 (asked for): the ask for its service list is ended, and its "
                                                    "answer will be passed over: a phone has asked the car for a link", 2000));
        first.appear();
        Link* link = dialled(*d.listener, first, d.trace);
        d.gap.queryReply(kDialled, true);
        pause(300);
        CHECK(!d.trace.saw("gap connect") && d.listener->serialStatus().dialState == "idle");
        first.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
        // An error answer: as not listed, said once.
        CHECK(waitForSerialLinks(2000));
        d.gap.answersQuery = true;
        d.gap.queryError = 1;
        pause(200);
        CHECK_IN(why, d.listener->dialNow(&why) == 1);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "the car answered the ask for its service list with 1 TIMEOUT: "
                                                    "taken as not listening", 2000));
        CHECK(!d.trace.saw("gap connect") && d.listener->serialStatus().askErrors == 1);
    }
    {   // a rejection of the ask by the car's Bluetooth service, and a reply that cannot be read, are each said as what
        // they are: as not listed, both, and counted as errors
        Dialler d("ask-rejected");
        d.gap.queryRejected = true;
        d.start();
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 (asked for): the car's Bluetooth service rejected the ask for "
                                                    "its service list: taken as not listening, and it is not dialled", 2000));
        CHECK(d.trace.logged("the Bluetooth server rejected ServiceListQuery (type 0x202)"));
        CHECK(!d.trace.logged("a reply not read") && !d.trace.saw("gap connect"));
        double end = monoNow() + 2.0;
        while (d.listener->serialStatus().dialState != "idle" && monoNow() < end) pause(5);
        d.gap.queryRejected = false;
        d.gap.queryUnread = true;
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 (asked for): the car answered the ask for its service list "
                                                    "with a reply not read: taken as not listening, and it is not dialled",
                                           2000));
        CHECK(d.trace.loggedCount("rejected the ask") == 1 && !d.trace.saw("gap connect"));
        CHECK(d.listener->serialStatus().askErrors == 2 && d.listener->serialStatus().asksNoAnswer == 0);
        CHECK(!d.trace.logged("00:11:22") && !d.trace.logged("001122"));
    }
    {   // an ask that is ended is said when its reason differs from that phone's last ended ask, and otherwise only
        // counted, with a record apart from the answers': ended asks between lists without the service bring no line
        // for either
        Dialler d("ask-ended-again");
        d.config.linkWaitMs = 300;
        d.gap.answersQuery = false;
        d.gap.queryLists = false;
        d.start();
        for (int i = 0; i < 4; ++i) {
            // An ask that a phone's request ends; its answer comes after, is passed over, and frees the car.
            d.dial();
            CHECK_IN(d.trace.text(), countWithin(d.trace, "gap query", 2 * i + 1, 2000));
            d.gap.phone(kInfServiceRequest, kCarSerialService);
            double end = monoNow() + 2.0;
            while (d.listener->serialStatus().dialState != "idle" && monoNow() < end) pause(5);
            CHECK(d.listener->serialStatus().dialState == "idle");
            d.gap.queryReply(kDialled, false);
            pause(450);   // the request's time is over
            // An ask answered without the service.
            d.gap.answersQuery = true;
            d.dial();
            CHECK_IN(d.trace.text(), countWithin(d.trace, "gap query", 2 * i + 2, 2000));
            end = monoNow() + 2.0;
            while (d.listener->serialStatus().asksNotListed < (unsigned)i + 1 && monoNow() < end) pause(5);
            d.gap.answersQuery = false;
        }
        CHECK_IN(d.trace.text(), d.trace.loggedCount("the ask for its service list is ended") == 1);
        CHECK(d.trace.logged("phone 1 (asked for): the ask for its service list is ended, and its answer will be "
                             "passed over: a phone has asked the car for a link. The same again is only counted"));
        CHECK_IN(d.trace.text(), d.trace.loggedCount("does not name the agent's service") == 1);
        SerialStatus st = d.listener->serialStatus();
        CHECK(st.asks == 8 && st.asksNotListed == 4 && st.dials == 0);
    }
    {   // the settle time: a phone in the first device list is not asked by a poll until it is over
        Dialler d("ask-settle");
        d.ours.set(std::vector<std::string>(1, kDialled));
        d.config.dial.settleMs = 800;
        d.gap.queryLists = false;
        double at = monoNow();
        d.start();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap query", 3000));
        CHECK_IN(d.trace.text(), monoNow() - at >= 0.75);
        CHECK(d.trace.count("gap query") == 1);
    }
    {   // a dial that fails after the service was listed: doubted, said once; the car's word that a phone-opened link
        // ended takes the doubt away, and so does the service's going
        Dialler d("ask-doubt");
        d.gap.connectError = 14;
        d.start();
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: the car's dial came to 14 PAGE_TIMEOUT although the "
                                                    "phone's service list named the agent's service", 2000));
        CHECK(d.listener->serialStatus().recordDoubtful);
        d.gap.ended("ExtBox2", kDialled, "SPP", 1);   // another instance's says nothing of it
        pause(100);
        CHECK(d.listener->serialStatus().recordDoubtful);
        d.gap.ended("ExtBox1", kDialled, "SPP", 1);
        CHECK(d.trace.logged("as it does when its record of such links is right: that record is taken as right again"));
        CHECK(!d.listener->serialStatus().recordDoubtful);
        d.gap.ended("ExtBox1", kDialled, "SPP", 1);
        CHECK(d.trace.loggedCount("taken as right again") == 1);
        pause(100);
        std::string why;
        CHECK_IN(why, d.listener->dialNow(&why) == 1);
        CHECK_IN(d.trace.text(), countWithin(d.trace, "gap connect", 2, 2000));
        pause(200);
        CHECK(d.listener->serialStatus().recordDoubtful);
        d.gap.detach();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "its record of phone-opened serial links starts afresh", 2000));
        CHECK(!d.listener->serialStatus().recordDoubtful);
    }
}

// ---- the registration check -------------------------------------------------------------------------------------

// The re-point after a dial the car answered with a failure (dial_rules.hpp).
static const char* const kRepointItem = "gap repoint BtCtrl nbtapp 02:00:00:DD:DD:DD HFPGW";

static bool isRepoint(const FakeGap::Connect& c) {
    return c.device == "BtCtrl" && c.instance == "nbtapp" && c.address == kRepointAddress && c.service == kRepointService;
}

// Until the listener's status holds `pred`, `ms` at most.
struct StatusIs {
    virtual ~StatusIs() {}
    virtual bool operator()(const SerialStatus& s) const = 0;
};
static bool statusWithin(SerialListener& listener, const StatusIs& pred, int ms) {
    double end = monoNow() + ms / 1000.0;
    while (monoNow() < end) {
        if (pred(listener.serialStatus())) return true;
        pause(5);
    }
    return pred(listener.serialStatus());
}
struct Stopped : StatusIs {
    virtual bool operator()(const SerialStatus& s) const { return s.repointStopped; }
};

// The problem file's text once it holds `part`, 3 s at most.
static std::string problemsWith(const std::string& file, const std::string& part) {
    double end = monoNow() + 3.0;
    std::string text;
    while (monoNow() < end) {
        if (fs::kindOf(file) != fs::kNone) text = testutil::readFile(file);
        if (has(text, part)) break;
        pause(10);
    }
    return text;
}

static void testRepoint() {
    {   // a dial answered 5, 14 or another failure: exactly one re-point, with the four values, sent before anything
        // else of the agent's (in the trace, the item right after the answer is given); answered 5 at once, which is
        // as expected, said, and counted. The line that doubts the car's record says it went
        uint32_t errors[] = {5, 14, 6};
        for (size_t i = 0; i < 3; ++i) {
            Dialler d("repoint-sent");
            d.gap.answersConnect = false;
            d.start();
            d.dial();
            CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap connect BtCtrl nbtapp 00:11:22:33:44:55 NBTApp", 2000));
            d.trace.add("test: the dial is answered");
            d.gap.connectReply(kDialled, "NBTApp", errors[i]);
            int at = d.trace.at("test: the dial is answered");
            CHECK_IN(d.trace.text(), at >= 0 && d.trace.at(kRepointItem) == at + 1);
            std::string answer = gapErrorText(errors[i], "");
            CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: the re-point request after the dial's answer " + answer +
                                                            " was answered 5 DEFAULT in ", 2000));
            CHECK(d.trace.logged("as expected: a link end that the car books against the agent's instance does not count "
                                 "against the phone's connected services"));
            CHECK(d.trace.logged("the agent has sent the car one re-point request"));
            CHECK(!d.trace.logged("Nothing is sent to mend it"));
            pause(300);
            std::vector<FakeGap::Connect> c = d.gap.connects();
            CHECK_IN(d.trace.text(), c.size() == 2 && !isRepoint(c[0]) && isRepoint(c[1]));
            CHECK(d.trace.count("gap repoint") == 1);
            SerialStatus st = d.listener->serialStatus();
            CHECK(st.repoints == 1 && st.repointsExpected == 1 && st.repointsUnexpected == 0 && st.repointsNotSent == 0);
            CHECK(!st.repointStopped && st.recordDoubtful);
            // Never the phone's address, nor the re-point's.
            CHECK(!d.trace.logged("00:11:22") && !d.trace.logged("001122") && !d.trace.logged("02:00:00") &&
                  !d.trace.logged("020000") && !d.trace.logged("DD:DD"));
        }
    }
    {   // none after 0, 2, 10 or a rejection
        struct Row {
            uint32_t error;
            bool rejected;
        } rows[] = {{0, false}, {2, false}, {10, false}, {0, true}};
        for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            Dialler d("repoint-none");
            Node node(d.dir + "/nbtapp");
            d.gap.dialNode = &node;
            d.gap.connectError = rows[i].error;
            d.gap.rejectsConnect = rows[i].rejected;
            d.gap.answersDisconnect = true;
            d.start();
            d.dial();
            CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 (asked for): the car's dial answered ", 2000));
            pause(300);
            CHECK_IN(d.trace.text(), d.trace.count("gap repoint") == 0 && d.gap.connects().size() == 1);
            CHECK(d.listener->serialStatus().repoints == 0 && !d.trace.logged("re-point"));
            node.hangUp();
        }
    }
    {   // a dial with no answer is given up; the car's answer that comes after all brings the re-point: the car wrote
        // its entry all the same
        Dialler d("repoint-late");
        d.gap.answersConnect = false;
        d.gap.answersDisconnect = true;
        d.gap.disconnectError = 10;
        d.start();
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: no answer to the car's dial in 1 s", 3000));
        CHECK(d.trace.logged("should the car answer the dial after all, one re-point request is sent then"));
        CHECK(d.trace.count("gap repoint") == 0);
        d.trace.add("test: the dial is answered late");
        d.gap.connectReply(kDialled, "NBTApp", 5);
        int at = d.trace.at("test: the dial is answered late");
        CHECK_IN(d.trace.text(), d.trace.at(kRepointItem) == at + 1);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: the re-point request after the dial's answer 5 DEFAULT was "
                                                    "answered 5 DEFAULT in ", 2000));
        CHECK(d.listener->serialStatus().repointsExpected == 1);
    }
    {   // while it waits for its answer, nothing that could bring a ServiceConnect starts: a forced dial asks only once
        // the answer has come
        Dialler d("repoint-waits");
        d.config.dial.repointSlowMs = 2000;
        d.config.dial.repointLimitMs = 4000;
        d.gap.answersRepoint = false;
        d.gap.connectError = 5;
        d.start();
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, kRepointItem, 2000));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 (asked for): the car's dial answered 5 DEFAULT", 2000));
        d.dial();
        pause(600);
        CHECK_IN(d.trace.text(), d.trace.count("gap query") == 1 && d.trace.count("gap connect") == 1);
        d.gap.answersRepoint = true;   // the next dial's, at once
        d.gap.connectReply(kRepointAddress, kRepointService, 5);
        CHECK_IN(d.trace.text(), countWithin(d.trace, "gap query", 2, 2000));
        CHECK_IN(d.trace.text(), countWithin(d.trace, "gap connect BtCtrl nbtapp 00:11", 2, 2000));
        CHECK(d.listener->serialStatus().repointsExpected >= 1 && !d.listener->serialStatus().repointStopped);
    }
    {   // another client's ServiceConnect reply while the re-point waits is left alone, and is not its answer
        Dialler d("repoint-others");
        d.gap.answersRepoint = false;
        d.gap.connectError = 5;
        d.start();
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, kRepointItem, 2000));
        d.gap.connectReply("66:77:88:99:AA:BB", "Other_Service", 0);
        d.gap.connectReply(kRepointAddress, "Other_Service", 0);
        d.gap.connectReply("66:77:88:99:AA:BB", kRepointService, 7);
        pause(50);
        SerialStatus st = d.listener->serialStatus();
        CHECK(st.repointsExpected == 0 && st.repointsUnexpected == 0 && !st.repointStopped);
        CHECK(d.trace.loggedCount("a ServiceConnect reply for another client came") == 3);
        d.gap.connectReply(kRepointAddress, kRepointService, 5);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "was answered 5 DEFAULT in ", 2000));
        st = d.listener->serialStatus();
        CHECK(st.repoints == 1 && st.repointsExpected == 1 && st.repointsUnexpected == 0 && !st.repointStopped);
    }
    {   // the guard: an answer other than 5, a 5 that is slow, none in the limit, and 0. Each stops the re-points for
        // the rest of the run, said once in the log and once in the problem file; dials go on afterwards, with no
        // re-point
        enum { kOther, kSlow, kNone, kZero, kRejected };
        for (int kind = kOther; kind <= kRejected; ++kind) {
            std::string file = gWork + "/repoint-guard/serial-problems.log";
            ProblemLog problems(file, "1.0.0 (build test)");
            Dialler d("repoint-guard");
            d.problems = &problems;
            d.config.dial.repointSlowMs = 150;
            d.config.dial.repointLimitMs = 600;
            d.gap.connectError = 5;
            d.gap.answersDisconnect = true;
            d.gap.disconnectError = 10;
            d.gap.answersRepoint = kind == kOther || kind == kZero;
            d.gap.repointError = kind == kZero ? 0 : 7;
            d.gap.rejectsRepoint = kind == kRejected;
            d.start();
            d.dial();
            CHECK_IN(d.trace.text(), sawWithin(d.trace, kRepointItem, 2000));
            if (kind == kSlow) {
                pause(300);
                d.gap.connectReply(kRepointAddress, kRepointService, 5);
            }
            CHECK_IN(d.trace.text(), statusWithin(*d.listener, Stopped(), 3000));
            CHECK_IN(d.trace.text(), sawWithin(d.trace, "where 5 within 150 ms is expected of the car: no more re-point "
                                                        "requests are sent in this run of the agent", 2000));
            const char* came[] = {"was answered 7 RFCOMM in ", "was answered 5 DEFAULT in ", "had no answer in 600 ms",
                                  "was answered 0 OK in ", "was rejected, or its reply could not be read, in "};
            CHECK_IN(d.trace.text(), d.trace.logged(std::string("phone 1: the re-point request after the dial's answer 5 "
                                                                "DEFAULT ") + came[kind]));
            if (kind == kNone) CHECK(d.trace.logged("had no answer in 600 ms, and is given up, where 5 within"));
            std::string part = std::string("phone 1: the re-point after a failed dial ") + came[kind];
            std::string text = problemsWith(file, part);
            CHECK_IN(text, has(text, part) && has(text, "not 5 within 150 ms: no more are sent in this run\n"));
            // Answered 0: the one ServiceDisconnect for what the car says it connected; never otherwise.
            if (kind == kZero) {
                CHECK_IN(d.trace.text(), sawWithin(d.trace, "gap disconnect", 2000));
                CHECK(d.trace.logged("The car says it has a link for the request, so it is asked to end that link"));
                CHECK(d.gap.disconnect == buildServiceDisconnect("BtCtrl", "nbtapp", kRepointAddress, kRepointService));
                CHECK_IN(d.trace.text(), sawWithin(d.trace, "ServiceDisconnect reply: 10 ", 2000));
            }
            // Dials go on, with no re-point: a second answered 5, and a third.
            for (unsigned n = 2; n <= 3; ++n) {
                std::string why;
                CHECK_IN(why, d.listener->dialNow(&why) == 1);
                CHECK_IN(d.trace.text(), countWithin(d.trace, "gap connect BtCtrl nbtapp 00:11", n, 2000));
                pause(100);
            }
            pause(200);
            CHECK_IN(d.trace.text(), d.trace.count("gap repoint") == 1);
            CHECK(d.trace.count("gap disconnect") == (kind == kZero ? 1u : 0u));
            CHECK(d.trace.loggedCount("no more re-point requests are sent") == 1);
            SerialStatus st = d.listener->serialStatus();
            CHECK(st.repointStopped && st.repoints == 1 && st.repointsUnexpected == 1 && st.repointsExpected == 0);
            CHECK(st.dials == 3);
            text = testutil::readFile(file);
            size_t first = text.find("the re-point after a failed dial");
            CHECK_IN(text, first != std::string::npos &&
                               text.find("the re-point after a failed dial", first + 1) == std::string::npos);
            CHECK(!d.trace.logged("02:00:00") && !d.trace.logged("DD:DD") && !d.trace.logged("00:11:22"));
        }
    }
    {   // the guard holds when the listener is set up again, after the car's Bluetooth service went and came back
        Dialler d("repoint-guard-holds");
        d.config.retryMs = 500;
        d.config.registerMs = 400;
        d.gap.connectError = 5;
        d.gap.repointError = 7;
        d.start();
        d.dial();
        CHECK_IN(d.trace.text(), statusWithin(*d.listener, Stopped(), 3000));
        d.gap.detach();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "Bluetooth serial is off, and is set up again in", 3000));
        d.gap.attach();
        CHECK_IN(d.trace.text(), countWithin(d.trace, "log: the serial service 'NBTApp' is registered", 2, 4000));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "the car's device list: phone 1 is connected", 2000));
        std::string why;
        double end = monoNow() + 3.0;
        while (d.listener->dialNow(&why) == 0 && monoNow() < end) pause(20);
        CHECK_IN(d.trace.text(), countWithin(d.trace, "gap connect BtCtrl nbtapp 00:11", 2, 3000));
        pause(300);
        CHECK(d.trace.count("gap repoint") == 1 && d.listener->serialStatus().repointStopped);
    }
    {   // a re-point that cannot be sent is counted and said, and stops nothing
        Dialler d("repoint-not-sent");
        d.gap.connectError = 5;
        d.gap.takesRepoint = false;
        d.start();
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: the re-point request after the dial's answer 5 DEFAULT could "
                                                    "not be sent (the car's Bluetooth service did not take the request)",
                                           2000));
        CHECK(d.trace.logged("the re-point request, which keeps the end of such a link that the car books against the "
                             "agent's instance from counting against the phone's connected services, could not be sent"));
        SerialStatus st = d.listener->serialStatus();
        CHECK(st.repoints == 0 && st.repointsNotSent == 1 && st.repointsUnexpected == 0 && !st.repointStopped);
        // The next goes, and the same outcome again is only counted.
        d.gap.takesRepoint = true;
        std::string why;
        CHECK_IN(why, d.listener->dialNow(&why) == 1);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "was answered 5 DEFAULT in ", 2000));
        st = d.listener->serialStatus();
        CHECK(st.repoints == 1 && st.repointsExpected == 1 && st.repointsNotSent == 1);
    }
    {   // the same answer as expected again, for the same phone: counted, and the next line that differs says how many
        Dialler d("repoint-repeats");
        d.gap.connectError = 5;
        d.start();
        std::string why;
        for (unsigned n = 1; n <= 3; ++n) {
            CHECK_IN(why, d.listener->dialNow(&why) == 1);
            CHECK_IN(d.trace.text(), countWithin(d.trace, "gap repoint", n, 2000));
            pause(250);
        }
        CHECK_IN(d.trace.text(), d.trace.loggedCount("the re-point request after") == 1);
        d.gap.takesRepoint = false;
        CHECK_IN(why, d.listener->dialNow(&why) == 1);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "could not be sent (the car's Bluetooth service did not take the "
                                                    "request) (answered as expected came 2 more times before this)", 2000));
        CHECK(d.listener->serialStatus().repointsExpected == 3);
    }
    {   // the listener stops while a re-point waits: stop() does not wait for its time, and it is given up unjudged
        Dialler d("repoint-stop");
        d.config.dial.repointLimitMs = 10000;
        d.gap.answersRepoint = false;
        d.gap.connectError = 5;
        d.start();
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, kRepointItem, 2000));
        double at = monoNow();
        d.listener->stop();
        CHECK_IN(d.trace.text(), monoNow() - at < 2.0);
        CHECK_IN(d.trace.text(), d.trace.logged("phone 1: the re-point request after the dial's answer 5 DEFAULT had no "
                                                "answer when Bluetooth serial stops: it is given up, and not judged"));
        SerialStatus st = d.listener->serialStatus();
        CHECK(!st.repointStopped && st.repoints == 1 && st.repointsUnexpected == 0);
    }
}

// The dials of kDialled that went to the car, re-points left out.
static unsigned dialsOfPhone(FakeGap& gap) {
    std::vector<FakeGap::Connect> c = gap.connects();
    unsigned n = 0;
    for (size_t i = 0; i < c.size(); ++i)
        if (!isRepoint(c[i]) && c[i].address == kDialled && c[i].service == "NBTApp" && c[i].instance == "nbtapp") ++n;
    return n;
}
static bool dialsWithin(FakeGap& gap, unsigned n, int ms) {
    double end = monoNow() + ms / 1000.0;
    while (dialsOfPhone(gap) < n && monoNow() < end) pause(5);
    return dialsOfPhone(gap) == n;
}
struct DialStateIs : StatusIs {
    explicit DialStateIs(const char* s) : state(s) {}
    virtual bool operator()(const SerialStatus& s) const { return s.dialState == state; }
    std::string state;
};
struct DialsAre : StatusIs {
    explicit DialsAre(unsigned n) : dials(n) {}
    virtual bool operator()(const SerialStatus& s) const { return s.dials == dials; }
    unsigned dials;
};

// A dial that is given up, for no answer in its time or as the listener stops: the next dial goes, and the car's
// answer that comes after all is told late, never as the answer to an ask or a dial that is out.
static void testDialGivenUp() {
    const char* late = "phone 1: the car answered a dial of it late, with ";
    {   // given up for no answer: the next poll's dial is sent, not refused for the one given up
        Dialler d("given-up-next");
        d.ours.set(std::vector<std::string>(1, kDialled));
        d.config.dial.pollFirstMs = d.config.dial.pollLaterMs = 200;
        d.config.dial.backOffMs = 300;
        d.gap.answersConnect = false;
        d.gap.answersDisconnect = true;
        d.gap.disconnectError = 10;
        d.start();
        CHECK_IN(d.trace.text(), dialsWithin(d.gap, 1, 2000));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: no answer to the car's dial in 1 s", 3000));
        SerialStatus st = d.listener->serialStatus();
        CHECK(st.noAnswer == 1 && st.lastResult == "no answer" && st.recordDoubtful);
        CHECK_IN(d.trace.text(), dialsWithin(d.gap, 2, 3000));
        CHECK(!d.trace.logged("the car could not be asked to dial it"));
        CHECK_IN(d.trace.text(), statusWithin(*d.listener, DialsAre(2), 1000));
        st = d.listener->serialStatus();
        CHECK(st.dialState == "dialling" && st.noAnswer == 1);
        // The second is given up in its turn, and a third goes.
        CHECK_IN(d.trace.text(), countWithin(d.trace, "no answer to the car's dial in 1 s", 2, 3000));
        CHECK_IN(d.trace.text(), dialsWithin(d.gap, 3, 4000));
        CHECK(d.listener->serialStatus().noAnswer == 2 && d.trace.count("gap repoint") == 0);
    }
    {   // a late 5 while nothing of the agent's is out, with an ask of the phone out: exactly one re-point, at once;
        // said once, and the answer to neither the ask nor the dial that follows it
        Dialler d("given-up-late-5");
        d.ours.set(std::vector<std::string>(1, kDialled));
        d.config.dial.pollFirstMs = d.config.dial.pollLaterMs = 200;
        d.config.dial.backOffMs = 300;
        d.gap.answersConnect = false;
        d.gap.answersDisconnect = true;
        d.gap.disconnectError = 10;
        d.start();
        CHECK_IN(d.trace.text(), dialsWithin(d.gap, 1, 2000));
        d.gap.answersQuery = false;   // the next ask stays out
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: no answer to the car's dial in 1 s", 3000));
        CHECK_IN(d.trace.text(), countWithin(d.trace, "gap query", 2, 3000));
        d.trace.add("test: the dial is answered late");
        d.gap.connectReply(kDialled, "NBTApp", 5);
        int at = d.trace.at("test: the dial is answered late");
        CHECK_IN(d.trace.text(), d.trace.at(kRepointItem) == at + 1);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, std::string(late) + "5 DEFAULT, after the dial had been given up: "
                                                    "it answers nothing that is out, and one re-point request has been "
                                                    "sent for it. The same late answer again is only counted", 2000));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: the re-point request after the dial's answer 5 DEFAULT was "
                                                    "answered 5 DEFAULT in ", 2000));
        CHECK(!d.trace.logged("the car's dial answered 5"));
        SerialStatus st = d.listener->serialStatus();
        CHECK(st.repoints == 1 && st.repointsExpected == 1 && st.notListening == 0 && st.noAnswer == 1);
        CHECK(st.lastResult == "no answer" && st.dials == 1 && st.dialState != "dialling");
        // The ask's answer (or, if the re-point ended that ask, the next one's) brings a dial, which waits for its own
        // answer: the late one was not it.
        d.gap.answersQuery = true;
        d.gap.queryReply(kDialled, true);
        CHECK_IN(d.trace.text(), dialsWithin(d.gap, 2, 3000));
        pause(300);
        CHECK_IN(d.trace.text(), d.listener->serialStatus().dialState == "dialling");
        // Its own answer.
        d.gap.connectReply(kDialled, "NBTApp", 14);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 (poll): the car's dial answered 14 PAGE_TIMEOUT", 2000));
        CHECK_IN(d.trace.text(), countWithin(d.trace, "gap repoint", 2, 2000));
        CHECK(d.trace.loggedCount(late) == 1);
        st = d.listener->serialStatus();
        CHECK(st.notInReach == 1 && st.notListening == 0 && st.dials == 2 && st.lastResult == "PAGE_TIMEOUT");
        CHECK(!d.trace.logged("00:11:22") && !d.trace.logged("001122"));
    }
    {   // a late answer that is told is told once: the same again is dropped; a late 10 brings no re-point
        Dialler d("given-up-late-once");
        d.gap.answersConnect = false;
        d.gap.answersDisconnect = true;
        d.gap.disconnectError = 10;
        d.start();
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: no answer to the car's dial in 1 s", 3000));
        d.gap.connectReply(kDialled, "NBTApp", 10);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, std::string(late) + "10 INVALID_PARAM, after the dial had been "
                                                    "given up: it answers nothing that is out, and nothing more comes "
                                                    "of it", 2000));
        d.gap.connectReply(kDialled, "NBTApp", 5);
        pause(300);
        CHECK(d.trace.loggedCount(late) == 1 && d.trace.count("gap repoint") == 0);
        CHECK(d.listener->serialStatus().repoints == 0);
    }
    {   // a late 0: the car has a link up on the dialled instance after all, which is not taken: the car is asked to
        // end it, for the phone it named, and nothing of it counts as an answered dial
        Dialler d("given-up-late-0");
        Node node(d.dir + "/nbtapp");
        d.gap.dialNode = &node;
        d.gap.answersConnect = false;
        d.gap.answersDisconnect = true;
        d.gap.disconnectError = 10;
        d.start();
        node.say(kMagic);   // what the app would say: never read
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: no answer to the car's dial in 1 s", 3000));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "ServiceDisconnect on the dialled instance answered 10 ", 2000));
        CHECK_IN(d.trace.text(), statusWithin(*d.listener, DialStateIs("idle"), 2000));
        pause(200);
        CHECK(d.trace.count("gap disconnect") == 1);
        d.gap.disconnectError = 0;   // this time it ends the link
        node.appear();
        d.gap.connectReply(kDialled, "NBTApp", 0);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, std::string(late) + "0 OK, after the dial had been given up: it "
                                                    "answers nothing that is out, and the car has a link up on the "
                                                    "dialled instance that nothing of the agent's reads, and is asked "
                                                    "to end it", 2000));
        CHECK_IN(d.trace.text(), countWithin(d.trace, "gap disconnect", 2, 2000));
        CHECK(d.gap.disconnect == buildServiceDisconnect("BtCtrl", "nbtapp", kDialled, "NBTApp"));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: ServiceDisconnect on the dialled instance answered 0 OK", 2000));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, node.path + " is not there", 2000));
        pause(200);
        CHECK(acceptWithin(*d.listener, 50) == 0 && !d.trace.saw("nbtapp opened") && d.trace.count("gap repoint") == 0);
        SerialStatus st = d.listener->serialStatus();
        CHECK(st.answeredOk == 0 && st.noAnswer == 1 && st.leftBehind == 0 && st.dialState == "idle" && st.link == "none");
        // Dials go on.
        d.dial();
        CHECK_IN(d.trace.text(), dialsWithin(d.gap, 2, 2000));
    }
    {   // a late answer while a new dial of the same phone is out cannot be told from that dial's own: it is taken as
        // that dial's answer, and the one after it is dropped
        Dialler d("given-up-same-phone");
        d.gap.answersConnect = false;
        d.gap.answersDisconnect = true;
        d.gap.disconnectError = 10;
        d.start();
        d.dial();
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: no answer to the car's dial in 1 s", 3000));
        CHECK_IN(d.trace.text(), statusWithin(*d.listener, DialStateIs("idle"), 2000));
        d.dial();   // a forced dial asks at once, back-off or not
        CHECK_IN(d.trace.text(), dialsWithin(d.gap, 2, 2000));
        d.gap.connectReply(kDialled, "NBTApp", 5);   // the first dial's, late
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 (asked for): the car's dial answered 5 DEFAULT", 2000));
        CHECK_IN(d.trace.text(), countWithin(d.trace, "gap repoint", 1, 2000));
        d.gap.connectReply(kDialled, "NBTApp", 14);   // the second's own
        pause(300);
        CHECK(!d.trace.logged(late) && !d.trace.logged("PAGE_TIMEOUT") && d.trace.count("gap repoint") == 1);
        SerialStatus st = d.listener->serialStatus();
        CHECK(st.dials == 2 && st.noAnswer == 1 && st.notListening == 1 && st.notInReach == 0 && st.dialState == "idle");
    }
    {   // the listener stops with a dial out, and is set up again in the same process: the dial was given up (the
        // rules idle, counted as no answer, no ServiceDisconnect), the late failed answer after the set-up brings the
        // re-point, serial.dial is not refused as busy, and polls ask and dial again
        Dialler d("given-up-at-stop");
        d.ours.set(std::vector<std::string>(1, kDialled));
        d.config.dial.pollFirstMs = d.config.dial.pollLaterMs = 200;
        d.config.dial.backOffMs = 1000;
        d.config.dial.replyLimitMs = 60000;
        d.gap.answersConnect = false;
        d.start();
        CHECK_IN(d.trace.text(), dialsWithin(d.gap, 1, 2000));
        CHECK_IN(d.trace.text(), statusWithin(*d.listener, DialStateIs("dialling"), 1000));
        d.listener->stop();
        CHECK_IN(d.trace.text(), d.trace.logged("phone 1: the car's dial had no answer when Bluetooth serial stops: it is "
                                                "given up, and the phone is not dialled by a poll for 1 s"));
        CHECK(d.trace.logged("should the car answer the dial after all, one re-point request is sent then"));
        SerialStatus st = d.listener->serialStatus();
        CHECK(st.dialState == "idle" && st.noAnswer == 1 && st.lastResult == "no answer" && st.recordDoubtful);
        CHECK(d.trace.count("gap disconnect") == 0 && d.trace.count("gap repoint") == 0);
        std::string why;
        CHECK_IN(why, d.listener->start(&why));
        CHECK_IN(d.trace.text(), countWithin(d.trace, "log: the serial service 'NBTApp' is registered", 2, 2000));
        // The late answer, while the phone is still backed off: nothing of the agent's is out.
        d.trace.add("test: the dial is answered late");
        d.gap.connectReply(kDialled, "NBTApp", 5);
        int at = d.trace.at("test: the dial is answered late");
        CHECK_IN(d.trace.text(), d.trace.at(kRepointItem) == at + 1);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, std::string(late) + "5 DEFAULT", 2000));
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1: the re-point request after the dial's answer 5 DEFAULT was "
                                                    "answered 5 DEFAULT in ", 2000));
        CHECK(d.listener->serialStatus().repointsExpected == 1);
        // serial.dial is taken, and its dial goes.
        CHECK_IN(why, d.listener->dialNow(&why) == 1);
        CHECK_IN(d.trace.text(), dialsWithin(d.gap, 2, 2000));
        d.gap.connectReply(kDialled, "NBTApp", 14);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 (asked for): the car's dial answered 14 PAGE_TIMEOUT", 2000));
        // And once the back-off is over, a poll.
        CHECK_IN(d.trace.text(), dialsWithin(d.gap, 3, 4000));
        d.gap.connectReply(kDialled, "NBTApp", 1);
        CHECK_IN(d.trace.text(), sawWithin(d.trace, "phone 1 (poll): the car's dial answered 1 TIMEOUT", 2000));
        st = d.listener->serialStatus();
        CHECK(st.dials == 3 && st.noAnswer == 1 && st.notInReach == 1 && st.otherAnswers == 1 && st.repoints == 3);
        CHECK(!d.trace.logged("the car could not be asked to dial it") && !d.trace.logged("00:11:22"));
    }
}

static unsigned registersIn(Trace& trace) { return trace.exactly("register"); }

static void testRegistrationCheck() {
    std::string dir = freshFolder("check");
    Trace trace;
    LinksGone gone;
    FakeGap gap(&trace);
    FakeGate gate(&trace);
    SerialConfig config = configFor(dir);
    config.checkMs = 100;
    config.checkQuietMs = 1000;
    SerialListener listener(&gap, &gate, &trace, config);
    std::string why;
    CHECK_IN(why, listener.start(&why));
    gap.attach();
    CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));
    // Still there: the stack answers 5, which changes nothing and is not logged.
    gap.registerError = 5;
    double end = monoNow() + 3.0;
    while (monoNow() < end && registersIn(trace) < 4) pause(5);
    CHECK_IN(trace.text(), registersIn(trace) >= 4);
    CHECK(!trace.saw("deregister") && !trace.saw("was answered") && !trace.saw("had dropped") && listener.active());
    // The stack was busy: the next check asks again.
    gap.registerError = 2;
    unsigned before = registersIn(trace);
    end = monoNow() + 3.0;
    while (monoNow() < end && registersIn(trace) < before + 2) pause(5);
    CHECK_IN(trace.text(), registersIn(trace) >= before + 2 && !trace.saw("was answered") && !trace.saw("deregister"));
    // Lost, and registered again: said the first time (and once more that it keeps being dropped, if it does).
    gap.registerError = 0;
    CHECK_IN(trace.text(), sawWithin(trace, "the car's Bluetooth had dropped the registration of 'NBTApp', as switching "
                                            "Bluetooth off and on in iDrive does, with no word of it: the registration "
                                            "check found it gone and has registered it again", 2000));
    gap.registerError = 5;
    // Another answer is logged.
    gap.registerError = 12;
    CHECK_IN(trace.text(), sawWithin(trace, "the registration check of 'NBTApp' was answered 12 HW_NOT_READY", 2000));
    gap.registerError = 5;
    // A phone that asks for a link, for whatever service, holds the checks off for checkQuietMs.
    gap.phone(kInfServiceRequest, "Another");
    pause(50);
    before = registersIn(trace);
    pause(600);
    CHECK_IN(trace.text(), registersIn(trace) == before);
    end = monoNow() + 3.0;
    while (monoNow() < end && registersIn(trace) == before) pause(5);
    CHECK_IN(trace.text(), registersIn(trace) > before);
    // A check never removes anything, and never sets the service up anew.
    CHECK(!trace.saw("deregister") && trace.count("is registered:") == 1 && listener.active());
    listener.stop();
    CHECK_IN(trace.text(), trace.saw("registration checks so far: ") && trace.saw("found it dropped and registered it again"));
    CHECK_IN(trace.text(), trace.saw("had another answer (the last: 12 HW_NOT_READY, "));
    CHECK_IN(trace.text(), trace.count("had dropped the registration") == 1);
}

// ---- an agent behind the listener ---------------------------------------------------------------------------------

// The phone's side of a link. It counts the agent's pings and does not answer them.
class Answers : public PeerHandler {
public:
    Answers() : pings(0) {}
    virtual void onRequest(Peer*, int64_t, const std::string& method, const json::Value&) {
        if (method == "ping") ++pings;
    }
    virtual void onResponse(Peer*, int64_t id, bool ok, const json::Value& result, const std::string& code,
                            const std::string&) {
        if (!ok) {
            got[id] = code;
            return;
        }
        json::Value role = result.find("role"), maxFrame = result.find("maxFrame");
        char frame[32];
        snprintf(frame, sizeof frame, " maxFrame=%lld", maxFrame.isInteger() ? (long long)maxFrame.integer() : -1LL);
        got[id] = std::string("ok") + (role.isString() ? " role=" + role.string() + frame : std::string());
        json::Value listed = result.find("capabilities");
        if (role.isString()) caps.clear();
        for (size_t i = 0; listed.isArray() && i < listed.size(); ++i) caps += " " + listed.at(i).string();
    }
    virtual void onEvent(Peer*, const std::string& name, int64_t, const json::Value& params) {
        json::Value reason = params.find("reason");
        events.push_back(name + (reason.isString() ? " " + reason.string() : std::string()));
    }
    virtual void onData(Peer*, uint16_t, const std::string&, bool) {}

    std::map<int64_t, std::string> got;
    std::vector<std::string> events;   // each event's name, and its reason if it has one
    std::string caps;                  // the capabilities of the last hello answered, each after a space
    unsigned pings;
};

// The time, and as much more as the test says has passed.
class SkewClock : public Clock {
public:
    SkewClock() : ahead(0) {}
    virtual int64_t nowMs() { return real_.nowMs() + ahead; }
    int64_t ahead;

private:
    SystemClock real_;
};

class ZeroRandom : public RandomSource {
public:
    virtual void bytes(uint8_t* out, size_t len) { memset(out, 0, len); }
    virtual bool secret(uint8_t*, size_t) { return false; }   // nothing here pairs a phone or does auth
};

struct Car {
    Trace trace;
    LinksGone gone;
    FakeGap gap;
    FakeGate gate;
    std::string dir;
    HostPlatform platform;
    SkewClock clock;
    ZeroRandom random;
    Agent agent;
    SerialListener listener;

    // `phones`: the record of paired phones the agent finds when it starts, if not empty.
    explicit Car(const std::string& phones = std::string())
        : gap(&trace), gate(&trace), dir(freshFolder("agent")), platform(dir + "/root"),
          agent(&platform, std::vector<TrustedKey>(), "1.0.0", &clock, &random),
          listener(&gap, &gate, &trace, configFor(dir)) {
        std::string why;
        CHECK_IN(why, platform.prepare(&why));
        if (!phones.empty()) CHECK_IN(why, fs::writeFile(platform.stateDir() + "/phones.json", phones, 0600, &why));
        agent.setLog(0);
        CHECK_IN(why, agent.init(&why));
        CHECK_IN(why, listener.start(&why));
        agent.addListener(&listener);
        gap.attach();
    }

    // A phone connects: its link is `node`.
    void connect(Node& node) {
        gap.phone(kInfServiceRequest, kCarSerialService);
        node.appear();
    }
    // The agent takes turns until it has `sessions` links, for `ms` at most.
    bool sessionsWithin(size_t sessions, int ms) {
        double end = monoNow() + ms / 1000.0;
        while (monoNow() < end && agent.sessions() != sessions) agent.turn(10);
        return agent.sessions() == sessions;
    }
};

static void testAgent() {
    Car car;
    {   // a phone that opens with the magic bytes and says hello gets its answer
        Node node(car.dir + "/btspp", 0);
        car.connect(node);
        Answers answers;
        Peer phone(new SocketLink(node.phone, "phone"), true, &answers);
        node.phone = -1;   // the peer's now
        phone.flush();     // the magic bytes, at once
        int64_t id = phone.request("hello", "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"phone\",\"phoneId\":"
                                            "\"11111111111111111111111111111111\",\"name\":\"Test phone\","
                                            "\"maxFrame\":65536,\"capabilities\":[]}");
        CHECK(id > 0);
        double end = monoNow() + 5.0;
        while (answers.got.empty() && phone.open() && monoNow() < end) {
            car.agent.turn(10);
            phone.flush();
            phone.readable();
        }
        // The link the listener made is of the serial kind: the agent gives the serial frame size.
        CHECK_IN(car.trace.text() + phone.closeReason(), answers.got[id] == "ok role=agent maxFrame=4096");
        CHECK(car.agent.sessions() == 1);
        // ping, both ways: the phone's is answered; the agent's comes after 10 s with nothing heard.
        id = phone.request("ping", "{}");
        end = monoNow() + 5.0;
        while (answers.got.count(id) == 0 && phone.open() && monoNow() < end) {
            car.agent.turn(10);
            phone.flush();
            phone.readable();
        }
        CHECK_IN(phone.closeReason(), answers.got[id] == "ok" && answers.pings == 0);
        car.clock.ahead += kSerialPingAfterMs;
        end = monoNow() + 5.0;
        while (answers.pings == 0 && phone.open() && monoNow() < end) {
            car.agent.turn(10);
            phone.readable();
        }
        CHECK_IN(phone.closeReason(), answers.pings == 1 && car.agent.sessions() == 1);
        phone.close("done");
        CHECK(car.sessionsWithin(0, 3000));
    }
    {   // a link that opens with anything else never reaches the agent: it is let go, with not a byte written. No
        // program is declared for a serial service here, so nobody would read it: its node is read until the link
        // ends
        Node node(car.dir + "/btspp", 1);
        car.connect(node);
        node.say("\x10\x20\x30\x40 another app's opening");
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "btspp1 let go", 3000));
        CHECK_IN(car.trace.text(), car.trace.saw("No program is declared for a serial service, the gate has seen no "
                                                 "other program open the node since the link's node appeared, and no "
                                                 "program of software put on by hand is running, so nobody would read "
                                                 "it"));
        CHECK(!car.sessionsWithin(1, 200));
        bool closed = false;
        node.say("more of it");
        CHECK(node.hear(1, 300, &closed).empty() && !closed);
        node.hangUp();
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "btspp1: the link ended ", 3000));
        CHECK(car.trace.saw("s after it was let go; ") && car.trace.saw(" B were dropped, and its node is closed now"));
    }
    {   // nor does one on which the phone says nothing
        Node node(car.dir + "/btspp", 2);
        car.connect(node);
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "btspp2 let go", 3000));
        CHECK(!car.sessionsWithin(1, 200));
        bool closed = false;
        CHECK(node.hear(1, 300, &closed).empty() && !closed);
        node.hangUp();
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "btspp2: the link ended ", 3000));
    }
    {   // one that opens with the magic bytes and then says nothing is the agent's, which gives it 30 s for its
        // hello: then the agent ends it, nothing was written, and its node is closed when the phone ends the link
        Node node(car.dir + "/btspp", 3);
        car.connect(node);
        node.say(kMagic);
        CHECK(car.sessionsWithin(1, 3000));
        car.clock.ahead += kSerialSilenceMs - 8000;   // well short of it, however slowly this machine runs
        CHECK(!car.sessionsWithin(0, 400));
        car.clock.ahead += 8000;
        CHECK(car.sessionsWithin(0, 3000));
        bool closed = false;
        CHECK(node.hear(1, 300, &closed).empty() && !closed);
        node.hangUp();
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "btspp3 closed, closed by the agent, and the phone ended the link ", 3000));
    }
    {   // the agent stops: a phone that has said hello is told so, with link.closing as the last thing on its
        // link, and when the phone ends the link on that, the node closes with the phone and not under it
        Node node(car.dir + "/btspp", 4);
        car.connect(node);
        Answers answers;
        Peer phone(new SocketLink(node.phone, "phone"), true, &answers);
        node.phone = -1;
        phone.flush();
        int64_t id = phone.request("hello", "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"phone\",\"phoneId\":"
                                            "\"22222222222222222222222222222222\",\"name\":\"Test phone\","
                                            "\"maxFrame\":4096,\"capabilities\":[]}");
        double end = monoNow() + 5.0;
        while (answers.got.empty() && phone.open() && monoNow() < end) {
            car.agent.turn(10);
            phone.flush();
            phone.readable();
        }
        CHECK_IN(car.trace.text() + phone.closeReason(), answers.got[id] == "ok role=agent maxFrame=4096");
        CHECK(car.agent.sessions() == 1 && answers.events.empty() && !car.trace.saw("btspp4 closed"));
        car.agent.endPhoneLinks(kClosingStopping, 2000);
        CHECK(car.agent.sessions() == 0);
        end = monoNow() + 5.0;
        while (answers.events.empty() && phone.open() && monoNow() < end) {
            phone.readable();
            pause(2);
        }
        CHECK_IN(phone.closeReason(), answers.events.size() == 1 && answers.events[0] == "link.closing stopping" && phone.open());
        pause(100);   // inside the time the node waits for the phone, with the node still open
        CHECK(!phone.readable() && phone.open() && serialLinksOpen() == 1 && !car.trace.saw("btspp4 closed"));
        phone.close("told that the agent is stopping");
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "btspp4 closed, closed by the agent, and the phone ended the link ", 2000));
        CHECK(waitForSerialLinks(1000));
    }
    {   // a link that a phone opens while the agent is going, and that the agent never takes: stop() ends the
        // agent's end of it, and its node is read until the phone ends the link
        Node node(car.dir + "/btspp", 5);
        car.connect(node);
        node.say(kMagic);
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "btspp5: it opened with the magic bytes, so the link is the agent's", 3000));
        CHECK(serialLinksOpen() == 1);
        car.listener.stop();
        CHECK(car.trace.saw("1 link had been opened and not yet taken by the agent"));
        CHECK(car.listener.accept() == 0 && car.agent.sessions() == 0);
        bool closed = false;
        CHECK(node.hear(1, 600, &closed).empty() && !closed);   // not a byte was written into it
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "btspp5: the link is still up 300 ms after the agent's end closed", 2000));
        CHECK(serialLinksOpen() == 1);
        node.hangUp();
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "btspp5 closed, closed by the agent, and the phone ended the link ", 2000));
    }
    car.listener.stop();
    CHECK(car.trace.saw("deregister") && car.trace.saw("gate detach"));
}

// ---- an agent behind a listener that dials ------------------------------------------------------------------------

// The phone's side of a link on `node`: says hello, and keeps what it hears.
struct PhoneApp {
    Answers answers;
    Peer* peer;
    int64_t helloId;
    explicit PhoneApp(Node& node) : peer(new Peer(new SocketLink(node.phone, "phone"), true, &answers)), helloId(0) {
        node.phone = -1;   // the peer's now
        peer->flush();     // the magic bytes, at once
    }
    ~PhoneApp() { delete peer; }
    void hello(const char* idDigit) {
        helloId = peer->request("hello", std::string("{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"phone\",\"phoneId\":\"") +
                                             std::string(32, idDigit[0]) + "\",\"name\":\"Test phone\",\"maxFrame\":4096,"
                                             "\"capabilities\":[]}");
    }
    void pump() {
        if (!peer->open()) return;
        peer->flush();
        peer->readable();
    }
};

// Turns of the agent, with the phones' sides pumped, until `done` or `ms` are over.
struct Until {
    virtual ~Until() {}
    virtual bool done() = 0;
};

static bool turnsUntil(Agent& agent, std::vector<PhoneApp*> apps, Until& until, int ms) {
    double end = monoNow() + ms / 1000.0;
    while (monoNow() < end && !until.done()) {
        agent.turn(10);
        for (size_t i = 0; i < apps.size(); ++i) apps[i]->pump();
    }
    return until.done();
}

struct Answered : Until {
    PhoneApp* app;
    explicit Answered(PhoneApp* a) : app(a) {}
    virtual bool done() { return app->answers.got.count(app->helloId) != 0; }
};

struct Told : Until {
    PhoneApp* app;
    explicit Told(PhoneApp* a) : app(a) {}
    virtual bool done() { return !app->answers.events.empty(); }
};

static void testAgentDials() {
    {   // the car dials a phone of the agent's: the agent takes the link and answers hello, with the capability;
        // a link the phone opens beside it says hello later and is ended, told why; the agent ends the dialled
        // link, and the car ends it after the phone's time
        Car car;
        car.listener.stop();
        SerialConfig config = configFor(car.dir);
        config.dial.on = true;
        config.dial.node = car.dir + "/nbtapp";
        config.dial.closeWaitMs = 500;
        Node node(config.dial.node);
        car.gap.dialNode = &node;
        car.gap.answersDisconnect = true;
        car.gap.phones.push_back(carPhone(kDialled, kConnConnected, 7));
        SerialListener listener(&car.gap, &car.gate, &car.trace, config);
        PhoneAddressList* ours = car.agent.phoneAddresses();
        ours->set(std::vector<std::string>(1, kDialled));
        listener.setPhoneAddresses(ours);
        car.agent.addListener(&listener);
        car.agent.setSerialDial(&listener);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        car.gap.attach();   // to this listener's client, which the stand-in now serves
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "gap connect BtCtrl nbtapp 00:11:22:33:44:55 NBTApp", 3000));
        PhoneApp dialled(node);
        CHECK(car.sessionsWithin(1, 3000));
        dialled.hello("1");
        std::vector<PhoneApp*> apps(1, &dialled);
        Answered answered(&dialled);
        CHECK_IN(car.trace.text(), turnsUntil(car.agent, apps, answered, 3000));
        CHECK(dialled.answers.got[dialled.helloId] == "ok role=agent maxFrame=4096");
        // The dial's capability; with --dial-knock off, not the knock's.
        CHECK_IN(dialled.answers.caps, has(dialled.answers.caps + " ", " serial-dial ") &&
                                           !has(dialled.answers.caps, "serial-knock"));
        // The phone-opened link, from the same phone: hello later, and ended.
        Node opened(car.dir + "/btspp", 0);
        car.connect(opened);
        PhoneApp other(opened);
        CHECK(car.sessionsWithin(2, 3000));
        other.hello("1");
        apps.push_back(&other);
        Told told(&other);
        CHECK(turnsUntil(car.agent, apps, told, 3000));
        CHECK(other.answers.got.count(other.helloId) && other.answers.events[0] == "link.closing other-link");
        CHECK(dialled.answers.events.empty());
        other.peer->close("told");
        CHECK(car.sessionsWithin(1, 3000));
        // The agent ends the dialled link; the phone does not: the car is asked to, and the node goes.
        car.agent.endPhoneLinks(kClosingStopping, 1000);
        Told stopping(&dialled);
        CHECK(turnsUntil(car.agent, apps, stopping, 3000) && dialled.answers.events[0] == "link.closing stopping");
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "the agent ended the dialled link and the phone has not in 500 ms", 3000));
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "phone 1: the dialled link has ended", 3000));
        CHECK(waitForSerialLinks(3000));
        CHECK(!car.trace.logged("00:11:22") && !car.trace.logged("001122") && !car.trace.logged("Test Phone"));
        car.agent.setSerialDial(0);
        listener.stop();
    }
    {   // --dial off: the listener and the agent are as before. No device list, no dial, no capability
        Car car;
        PhoneAddressList ours;
        ours.set(std::vector<std::string>(1, kDialled));
        car.gap.phones.push_back(carPhone(kDialled, kConnConnected, 7));
        car.listener.setPhoneAddresses(&ours);
        car.agent.setSerialDial(&car.listener);
        car.gap.devicesChanged();
        car.listener.pairingWindow(true);
        std::string why;
        CHECK(car.listener.dialNow(&why) == 0 && why == "the car does not dial phones in this run (--dial off)");
        Node node(car.dir + "/btspp", 0);
        car.connect(node);
        PhoneApp app(node);
        CHECK(car.sessionsWithin(1, 3000));
        app.hello("1");
        std::vector<PhoneApp*> apps(1, &app);
        Answered answered(&app);
        CHECK(turnsUntil(car.agent, apps, answered, 3000));
        CHECK(app.answers.got[app.helloId] == "ok role=agent maxFrame=4096");
        CHECK_IN(app.answers.caps, !has(app.answers.caps, "serial-dial") && !has(app.answers.caps, "serial-knock"));
        pause(300);
        CHECK(!car.trace.saw("gap connect") && !car.trace.saw("device list") && !car.trace.logged("phone 1"));
        CHECK(!car.trace.logged("pairing window") && car.listener.serialStatus().dialOn == false);
        car.agent.setSerialDial(0);
        app.peer->close("done");
        CHECK(car.sessionsWithin(0, 3000));
        car.listener.stop();
    }
    {   // a link the car dialled that the agent takes marks its phone in the record of paired phones: on disk, shared
        // with the listener, there for the next agent, and gone with the phone
        const std::string id(32, '1');
        Car car("{\"version\":1,\"phones\":[{\"phoneId\":\"" + id + "\",\"name\":\"Test phone\",\"key\":\"" +
                std::string(64, '3') + "\",\"pairedAt\":3,\"address\":\"00:11:22:33:44:55\"}]}\n");
        PhoneAddressList* ours = car.agent.phoneAddresses();
        CHECK(ours->has(kDialled) && !ours->dialledBefore(kDialled) && !car.agent.phones().find(id)->dialled);
        car.listener.stop();
        SerialConfig config = configFor(car.dir);
        config.dial.on = true;
        config.dial.node = car.dir + "/nbtapp";
        Node node(config.dial.node);
        car.gap.dialNode = &node;
        car.gap.phones.push_back(carPhone(kDialled, kConnConnected, 7));
        SerialListener listener(&car.gap, &car.gate, &car.trace, config);
        listener.setPhoneAddresses(ours);
        car.agent.addListener(&listener);
        car.agent.setSerialDial(&listener);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        car.gap.attach();
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "gap connect BtCtrl nbtapp 00:11:22:33:44:55 NBTApp", 3000));
        PhoneApp dialled(node);
        CHECK(car.sessionsWithin(1, 3000));
        CHECK(ours->dialledBefore(kDialled) && car.agent.phones().find(id)->dialled);
        std::string file = car.platform.stateDir() + "/phones.json";
        CHECK_IN(testutil::readFile(file), testutil::readFile(file).find("\"dialled\":true") != std::string::npos);
        dialled.peer->close("done");
        node.vanish();
        car.gap.ended("nbtapp", kDialled, "NBTApp", 1);
        CHECK(car.sessionsWithin(0, 3000));
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "phone 1: the dialled link has ended", 3000));
        car.agent.setSerialDial(0);
        listener.stop();
        CHECK(waitForSerialLinks(3000));
        // The agent started again has it.
        {
            Agent again(&car.platform, std::vector<TrustedKey>(), "1.0.0", &car.clock, &car.random);
            again.setLog(0);
            CHECK_IN(why, again.init(&why));
            CHECK(again.phoneAddresses()->dialledBefore(kDialled));
        }
        // Forgotten: the mark goes with the phone's entry.
        size_t count = 0;
        CHECK_IN(why, car.agent.phones().forget(id, &count, &why) && count == 1);
        Agent after(&car.platform, std::vector<TrustedKey>(), "1.0.0", &car.clock, &car.random);
        after.setLog(0);
        CHECK_IN(why, after.init(&why));
        CHECK(!after.phoneAddresses()->has(kDialled) && !after.phoneAddresses()->dialledBefore(kDialled));
        CHECK(testutil::readFile(file).find("dialled") == std::string::npos);
    }
}

// ---- what other programs left registered ----------------------------------------------------------------------------

// A machine on which the programs the test names run, and no other.
class Machine : public HostPlatform {
public:
    explicit Machine(const std::string& root) : HostPlatform(root) { pthread_mutex_init(&mu_, NULL); }
    ~Machine() { pthread_mutex_destroy(&mu_); }
    // How many times the programs were looked for since `since`, counting the looks that came within 0.5 s of the
    // one before as one: a look walks the process list once for each folder.
    unsigned looksSince(double since) const {
        pthread_mutex_lock(&mu_);
        unsigned n = 0;
        double last = 0;
        for (size_t i = 0; i < lookAt_.size(); ++i) {
            if (lookAt_[i] >= since && (n == 0 || lookAt_[i] - last > 0.5)) ++n;
            if (lookAt_[i] >= since) last = lookAt_[i];
        }
        pthread_mutex_unlock(&mu_);
        return n;
    }
    virtual ProgramState programState(const std::string& name) const {
        return name == running ? kProgramRunning : kProgramNotRunning;
    }
    // Software put on by hand: the programs under a folder, as the test says.
    virtual ProgramState programsUnder(const std::string& dir, std::vector<RunningProgram>* out) const {
        out->clear();
        pthread_mutex_lock(&mu_);
        lookAt_.push_back(monoNow());
        std::string hand = handRunning;
        pthread_mutex_unlock(&mu_);
        if (hand.empty()) return kProgramNotRunning;
        RunningProgram p;
        p.pid = 999999;
        p.path = dir + "/bin/" + hand;
        out->push_back(p);
        return kProgramRunning;
    }
    void setHandRunning(const std::string& name) {
        pthread_mutex_lock(&mu_);
        handRunning = name;
        pthread_mutex_unlock(&mu_);
    }
    std::string running, handRunning;

private:
    mutable pthread_mutex_t mu_;
    mutable std::vector<double> lookAt_;
};

static SerialServiceOwner declares(const char* name, const char* program) {
    SerialServiceOwner o;
    o.name = name;
    o.program = program;
    o.from = "the test";
    return o;
}

// The listener has the list of other programs' services swept each time its own has been registered, and is what
// the removals go through.
static void testLeftBehind() {
    std::string dir = freshFolder("left-behind");
    Trace trace;
    FakeGap gap(&trace);
    FakeGate gate(&trace);
    Machine machine(dir + "/root");
    Store store(&machine);
    SerialServices services(&machine, &store, &trace);
    std::vector<SerialServiceOwner> declared;
    declared.push_back(declares("TestRadio", "radiod"));
    declared.push_back(declares("Other", "otherd"));
    services.setDeclared(declared);
    machine.running = "otherd";

    SerialListener listener(&gap, &gate, &trace, configFor(dir));
    services.setRemover(&listener);
    listener.setServices(&services);
    std::string why;
    // Before the car's Bluetooth service is there, nothing can be removed.
    CHECK(!listener.removeSerialService("TestRadio") && services.sweep("too early") == 0 && !trace.saw("deregister"));
    CHECK_IN(why, listener.start(&why));
    gap.attach();
    CHECK_IN(trace.text(), sawWithin(trace, "deregister TestRadio", 2000));
    CHECK_IN(trace.text(), sawWithin(trace, "SppUuidDeregister reply for 'TestRadio': 0 OK", 2000));
    // After its own registration, and for the service whose program is gone only.
    CHECK(trace.at("the serial service 'NBTApp' is registered") < trace.at("deregister TestRadio"));
    CHECK(trace.saw("serial service 'TestRadio': its program (radiod) is not running") && !trace.saw("deregister Other"));
    CHECK(!trace.saw("deregister NBTApp") && listener.active());
    // The listener's own name is not one it removes this way, and its service stays.
    CHECK(!listener.removeSerialService(kSerialServiceName) && listener.removeSerialService("Other"));
    CHECK_IN(trace.text(), sawWithin(trace, "deregister Other", 2000) && listener.active());
    listener.stop();
    services.setRemover(0);
    CHECK(trace.saw("deregister NBTApp") && trace.saw("gate detach"));
}

// ---- the decide step ------------------------------------------------------------------------------------------------

// Whether the phone's end shows that the node was closed, with nothing written to it first.
static bool closedUnwritten(Node& node, int ms) {
    bool closed = false;
    return node.hear(1, ms, &closed).empty() && closed;
}

// What the phone says first decides whose link it is. One that is not the agent's is let go: closed, unread from
// then on, with nothing written into it, and never handed to the agent.
static void testDecide() {
    std::string dir = freshFolder("decide");
    {   // a program declared for a serial service runs, so a link that is let go is that program's: kept open for it
        // when nothing was read from it, and, with the host's own calls on the node, which cannot put bytes back
        // (testPutBack() has the put-back), closed for it otherwise
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        Machine machine(dir + "/root");
        machine.running = "radiod";
        Store store(&machine);
        SerialServices services(&machine, &store, &trace);
        services.setDeclared(std::vector<SerialServiceOwner>(1, declares("TestRadio", "radiod")));
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        services.setRemover(&listener);
        listener.setServices(&services);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));

        // Nothing from the phone in the time it has.
        Node silent(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        silent.appear();
        double asked = monoNow();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp0 let go", 3000));
        CHECK(monoNow() - asked >= 0.25);   // not before the time was over
        CHECK_IN(trace.text(), trace.saw("(nothing came from the phone in that time). Nothing had been read from it, "
                                         "and its node is kept open, unread, until the link ends, for another reader: "
                                         "a program declared for a serial service is running, or whether one is "
                                         "cannot be told"));
        bool closed = false;
        CHECK(silent.hear(1, 200, &closed).empty() && !closed && acceptWithin(listener, 50) == 0);
        // The log says when the link of a node that was kept went, and the node is closed then.
        silent.vanish();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp0: the link ended ", 2000) &&
                               trace.saw("s after it was let go, and its node is closed now"));
        CHECK(closedUnwritten(silent, 2000));

        // Another app's opening: let go for what it said, without the wait. What was read cannot be put back here,
        // so its node is closed for the other program as it was before put-backs.
        Node foreign(dir + "/btspp", 1);
        gap.phone(kInfServiceRequest, kCarSerialService);
        foreign.appear();
        foreign.say("\x10\x20\x30\x40 another app's opening");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp1 let go", 3000));
        CHECK_IN(trace.text(), trace.saw("btspp1: its first message came") &&
                               trace.saw("(it did not open with the magic bytes) and closed: its node is still there. "
                                         "It is closed for another reader: a program declared for a serial service is "
                                         "running, or whether one is cannot be told. The 26 B read from it could not "
                                         "be put back: the size of its queue could not be told: "));
        CHECK(closedUnwritten(foreign, 2000) && acceptWithin(listener, 50) == 0);
        foreign.vanish();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp1 is gone, ", 2000) && trace.saw("s after it was let go and closed"));
        CHECK(!trace.saw("another app's opening"));   // what another app said is not for the log

        // One that starts like the magic bytes and goes on otherwise.
        Node almost(dir + "/btspp", 2);
        gap.phone(kInfServiceRequest, kCarSerialService);
        almost.appear();
        almost.say("NBTX");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp2 let go", 3000) && closedUnwritten(almost, 2000));

        // The magic bytes in two messages: the agent's, and the link gives all that was read, in order.
        Node split(dir + "/btspp", 3);
        gap.phone(kInfServiceRequest, kCarSerialService);
        split.appear();
        split.say("NB");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp3: its first message came", 2000));
        split.say("TL and the rest");
        Link* link = acceptWithin(listener, 3000);
        CHECK_IN(trace.text(), link != 0);
        const std::string whole = "NBTL and the rest";
        CHECK(readFrom(link, whole.size(), 3000) == whole);
        exchange(split, link, "up", "down");
        split.hangUp();
        split.vanish();   // or its name would lead to the next node's terminal
        CHECK(endsWithin(link, 3000));
        delete link;

        // A part of them and no more.
        Node part(dir + "/btspp", 4);
        gap.phone(kInfServiceRequest, kCarSerialService);
        part.appear();
        part.say("NB");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp4 let go", 3000));
        CHECK(trace.saw("(only a part of the magic bytes came in that time)") && closedUnwritten(part, 2000));

        // The phone goes before it has said anything: closed, and not counted as let go.
        Node leaver(dir + "/btspp", 5);
        gap.phone(kInfServiceRequest, kCarSerialService);
        leaver.appear();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp5 opened", 2000));
        leaver.hangUp();
        CHECK_IN(trace.text(), sawWithin(trace, "before it was known whose link it is", 3000));
        CHECK(!trace.saw("btspp5 let go") && acceptWithin(listener, 50) == 0);
        listener.stop();
        services.setRemover(0);
    }
    {   // no program declared for a serial service runs, and yet another program may read the link: the gate saw it
        // open the node after the link's node appeared, or a program of software put on by hand runs. Closed for it
        // then; otherwise read to its end
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        Machine machine(dir + "/root");
        Store store(&machine);
        SerialServices services(&machine, &store, &trace);
        services.setDeclared(std::vector<SerialServiceOwner>(1, declares("TestRadio", "radiod")));
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        services.setRemover(&listener);
        listener.setServices(&services);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));

        // An open seen before the node appeared is another link's: no sign of a reader for this one.
        gate.otherOpens("handreader");
        pause(450);
        Node first(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        first.appear();
        first.say("\x10\x20\x30\x40 another app's opening");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp0 let go", 3000));
        CHECK_IN(trace.text(), trace.saw("No program declared for a serial service is running, the gate has seen no "
                                         "other program open the node since the link's node appeared"));
        bool closed = false;
        CHECK(first.hear(1, 200, &closed).empty() && !closed);   // read by the agent, not closed
        first.hangUp();
        first.vanish();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp0: the link ended", 3000));
        pause(450);

        // The gate sees another program open the node while the link is decided on: kept for that program.
        Node second(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        second.appear();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp0 opened", 2000));
        gate.otherOpens("handreader");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp0 let go", 3000));
        CHECK_IN(trace.text(), sawWithin(trace, "its node is kept open, unread, until the link ends, for another reader: "
                                                "the gate saw handreader open the node, or try to, since this link's "
                                                "node appeared", 1000));
        CHECK(second.hear(1, 100, &closed).empty() && !closed);
        second.vanish();
        CHECK_IN(trace.text(), countWithin(trace, "btspp0: the link ended ", 2, 2000) && closedUnwritten(second, 2000));
        pause(450);

        // The gate is in front of the first node only: an open it saw says nothing of a link on another node. A
        // program of software put on by hand that runs does.
        machine.setHandRunning("aad");
        Node third(dir + "/btspp", 1);
        gap.phone(kInfServiceRequest, kCarSerialService);
        third.appear();
        gate.otherOpens("handreader");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp1 let go", 3000));
        CHECK_IN(trace.text(), sawWithin(trace, "for another reader: a program of software put on by hand is running "
                                                "(aad), which may read serial links", 1000));
        CHECK(third.hear(1, 100, &closed).empty() && !closed);
        third.vanish();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp1: the link ended ", 2000) && closedUnwritten(third, 2000));
        listener.stop();
        services.setRemover(0);
    }
    {   // a link that is let go and read, with no sign of another reader then: closed for one when a sign comes
        // later. Programs of software put on by hand are looked for every 2 s at most meanwhile
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        Machine machine(dir + "/root2");
        Store store(&machine);
        SerialServices services(&machine, &store, &trace);
        services.setDeclared(std::vector<SerialServiceOwner>(1, declares("TestRadio", "radiod")));
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        services.setRemover(&listener);
        listener.setServices(&services);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));

        // The gate sees another program open the first node a few passes after the link was let go.
        Node first(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        first.appear();
        first.say("\x10\x20\x30\x40 another app's opening");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp0 let go", 3000));
        double letGoAt = monoNow();
        CHECK_IN(trace.text(), trace.saw("so nobody would read it: its node is read, with what comes dropped"));
        first.say("more of it");
        pause(2600);
        CHECK_IN(trace.text(), !trace.saw("reading it stopped"));
        unsigned looks = machine.looksSince(letGoAt);
        CHECK_IN(trace.text(), looks >= 1 && looks <= 2);
        gate.otherOpens("latereader");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp0: reading it stopped, and its node closed for another reader, ", 2000));
        CHECK_IN(trace.text(), trace.saw("s after it was let go: the gate saw latereader open the node, or try to. ") &&
                               trace.saw(" B had been read and dropped by then") &&
                               !trace.saw(" 0 B had been read and dropped by then"));
        CHECK(closedUnwritten(first, 2000));
        first.vanish();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp0 is gone, ", 2000) && trace.saw("s after it was let go and closed"));
        CHECK_IN(trace.text(), !trace.saw("btspp0: the link ended"));
        pause(450);

        // A program of software put on by hand starts to run while a link on another node is read.
        Node second(dir + "/btspp", 1);
        gap.phone(kInfServiceRequest, kCarSerialService);
        second.appear();
        second.say("\x10\x20\x30\x40 another app's opening");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp1 let go", 3000));
        CHECK_IN(trace.text(), trace.saw("btspp1 let go") && !trace.saw("btspp1: reading it stopped"));
        gate.otherOpens("latereader");   // the gate is in front of the first node only: no sign for this one
        pause(300);
        CHECK_IN(trace.text(), !trace.saw("btspp1: reading it stopped"));
        machine.setHandRunning("aad");
        double handAt = monoNow();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp1: reading it stopped, and its node closed for another reader, ", 3000));
        CHECK(monoNow() - handAt <= 2.6);
        CHECK_IN(trace.text(), trace.saw("s after it was let go: a program of software put on by hand is running (aad). "));
        CHECK(closedUnwritten(second, 2000));
        second.vanish();
        listener.stop();
        services.setRemover(0);
    }
    {   // a link that was let go is a sign of a service that nothing answers: the other programs' services are
        // swept, and no more often than they may be
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        Machine machine(dir + "/root");
        Store store(&machine);
        SerialServices services(&machine, &store, &trace);
        services.setDeclared(std::vector<SerialServiceOwner>(1, declares("TestRadio", "radiod")));
        services.setSweepEvery(1.0);
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        services.setRemover(&listener);
        listener.setServices(&services);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "deregister TestRadio", 2000));
        pause(1050);

        // A link of the agent's is no such sign.
        Node ours(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        ours.appear();
        Link* link = dialled(listener, ours, trace);
        ours.hangUp();
        ours.vanish();
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK(trace.count("deregister TestRadio") == 1);

        Node first(dir + "/btspp", 1), second(dir + "/btspp", 2);
        gap.phone(kInfServiceRequest, kCarSerialService);
        first.appear();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp1 let go", 3000));
        // Its program is declared and not running: nobody would read the link, so its node is read until it ends.
        CHECK_IN(trace.text(), trace.saw("No program declared for a serial service is running, the gate has seen no "
                                         "other program open the node since the link's node appeared, and no program "
                                         "of software put on by hand is running, so nobody would read it"));
        CHECK_IN(trace.text(), sawWithin(trace, "(a phone's link was not the agent's)", 2000));
        for (int i = 0; i < 400 && trace.count("deregister TestRadio") < 2; ++i) pause(5);
        CHECK_IN(trace.text(), trace.count("deregister TestRadio") == 2);
        gap.phone(kInfServiceRequest, kCarSerialService);
        second.appear();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp2 let go", 3000));
        pause(100);
        CHECK_IN(trace.text(), trace.count("deregister TestRadio") == 2);
        listener.stop();
        services.setRemover(0);
    }
}

// ---- what was read from a link that is let go, put back for another program ------------------------------------------

// A listener whose links that are let go have another reader (radiod, declared for a serial service, runs), and the
// test's calls on the node, which can put bytes back.
struct Letting {
    Trace trace;
    LinksGone gone;
    FakeGap gap;
    FakeGate gate;
    std::string dir;
    Machine machine;
    Store store;
    SerialServices services;
    SerialConfig config;
    SerialListener* listener;

    explicit Letting(const std::string& name)
        : gap(&trace), gate(&trace), dir(freshFolder(name)), machine(dir + "/root"), store(&machine),
          services(&machine, &store, &trace), listener(0) {
        config = configFor(dir);
        config.nodeCalls = heldCalls();
        config.readNode = &heldRead;
        config.resetArmedMs = 1000;
        services.setDeclared(std::vector<SerialServiceOwner>(1, declares("TestRadio", "radiod")));
        machine.running = "radiod";
    }
    ~Letting() {
        if (listener != 0) {
            listener->stop();
            services.setRemover(0);
            delete listener;
        }
        if (getenv("NBT_TEST_TRACE") != 0) fprintf(stderr, "---- %s\n%s", dir.c_str(), trace.text().c_str());
    }
    void start() {
        listener = new SerialListener(&gap, &gate, &trace, config);
        services.setRemover(listener);
        listener->setServices(&services);
        std::string why;
        CHECK_IN(why, listener->start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));
    }
};

static std::string frameOf(const std::string& text);

static void testPutBack() {
    const std::string opening = "\x10\x20\x30\x40 another app's opening";
    const std::string reader = "for another reader: a program declared for a serial service is running, or whether "
                               "one is cannot be told";
    {   // everything the decide step read goes back, in order, and what came after its last read behind it; the node
        // stays open, unread, until the link ends, and the other program reads it all, and what follows
        Letting l("put-back");
        l.start();
        Node node(l.dir + "/btspp", 0);
        node.held = true;
        node.setLate("and more");   // comes into the node after the reader's last read, before the put-back
        l.gap.phone(kInfServiceRequest, kCarSerialService);
        node.send(opening);
        node.appear();
        CHECK_IN(l.trace.text(), sawWithin(l.trace, "btspp0 let go ", 3000));
        CHECK_IN(l.trace.text(), l.trace.saw("(it did not open with the magic bytes). The ") &&
                                 l.trace.saw(" B read from it were put back, in the order they came, with ") &&
                                 l.trace.saw(" B that came after the last read behind them, and its node is kept open, "
                                             "unread, until the link ends, " + reader));
        CHECK(!l.trace.saw("and closed") && node.injects == 1 && l.listener->serialStatus().putBack == 1);
        CHECK(node.holders() == 1 && acceptWithin(*l.listener, 50) == 0);
        {
            OtherProgram other(node);
            CHECK(other.reads(opening.size() + 8, 2000) == opening + "and more");
            node.send("and the next");
            CHECK(other.reads(12, 2000) == "and the next");
        }
        bool closed = false;
        CHECK(node.hear(1, 100, &closed).empty() && !closed);   // nothing written, and still open
        node.vanish();
        CHECK_IN(l.trace.text(), sawWithin(l.trace, "btspp0: the link ended ", 2000) &&
                                 l.trace.saw("s after it was let go, and its node is closed now"));
        CHECK(node.holders() == 0 && closedUnwritten(node, 1000));
        pause(450);

        // Leftovers of an earlier link at its start are put back with the rest: they are the phone's bytes.
        const std::string old = frameOf("{\"id\":7,\"result\":{}}");
        Node left(l.dir + "/btspp", 1);
        l.gap.phone(kInfServiceRequest, kCarSerialService);
        const std::string rest = "\x10\x20\x30\x40 not a frame";
        left.send(old + rest);
        left.appear();
        CHECK_IN(l.trace.text(), sawWithin(l.trace, "btspp1 let go ", 3000));
        CHECK(l.trace.saw("left over from an earlier link came first, as whole frames, and were passed over") &&
              l.listener->serialStatus().putBack == 2);
        CHECK(OtherProgram(left).reads(old.size() + rest.size(), 2000) == old + rest);
        left.vanish();
        pause(450);

        // serial.reset with only such a node open: armed for the next link, and nothing sent.
        Node kept(l.dir + "/btspp", 2);
        l.gap.phone(kInfServiceRequest, kCarSerialService);
        kept.say(opening);
        kept.appear();
        CHECK_IN(l.trace.text(), sawWithin(l.trace, "btspp2 let go ", 3000));
        // Its decide step is over once the hold is off.
        CHECK_IN(l.trace.text(), countWithin(l.trace, "are passed on again (the link was let go)", 3, 2000));
        std::string why;
        int64_t forMs = 0;
        CHECK(l.listener->resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetArmed);
        CHECK_IN(l.trace.text(), sawWithin(l.trace, "serial.reset: no serial link came within 1 s", 3000));
        CHECK(!l.trace.saw("gap disconnect") && kept.holders() == 1);
        CHECK(OtherProgram(kept).reads(opening.size(), 2000) == opening);
    }
    {   // a put-back that cannot be made: the node is closed for the other program, as before put-backs, and the
        // log says why
        Letting l("put-back-not");
        l.start();
        for (int c = 0; c < 3; ++c) {
            Node node(l.dir + "/btspp", c);
            const char* why = "";
            if (c == 0) {
                node.notRaw = true;
                why = "the node does not take bytes as they are (it is not raw)";
            } else if (c == 1) {
                node.queueSize = 10;
                why = " would not fit in its queue of 10 B";
            } else {
                node.injectError = EIO;
                why = "putting the bytes back failed: ";
            }
            l.gap.phone(kInfServiceRequest, kCarSerialService);
            node.say(opening);
            node.appear();
            CHECK_IN(l.trace.text(), sawWithin(l.trace, node.path + " let go ", 3000));
            CHECK_IN(l.trace.text(), l.trace.count("(it did not open with the magic bytes) and closed: its node is still "
                                                   "there. It is closed " + reader + ". The ") == (unsigned)c + 1 &&
                                     l.trace.count(" B read from it could not be put back: ") == (unsigned)c + 1 &&
                                     l.trace.saw(why));
            CHECK(closedUnwritten(node, 2000) && node.holders() == 0 && node.injects == 0);
            node.vanish();
            CHECK_IN(l.trace.text(), sawWithin(l.trace, node.path + " is gone, ", 2000));
            pause(450);
        }
        CHECK(l.listener->serialStatus().putBack == 0);
    }
    {   // no other reader known: read, with what comes dropped, until the link ends, as ever; nothing put back
        Letting l("put-back-none");
        l.machine.running = "";
        l.start();
        Node node(l.dir + "/btspp", 0);
        l.gap.phone(kInfServiceRequest, kCarSerialService);
        node.say(opening);
        node.appear();
        CHECK_IN(l.trace.text(), sawWithin(l.trace, "btspp0 let go ", 3000));
        CHECK(l.trace.saw("so nobody would read it: its node is read, with what comes dropped, until the link ends"));
        node.say("dropped too");
        bool closed = false;
        CHECK(node.hear(1, 200, &closed).empty() && !closed && node.injects == 0);
        node.hangUp();
        CHECK_IN(l.trace.text(), sawWithin(l.trace, "btspp0: the link ended ", 3000));
        CHECK(l.trace.saw("s after it was let go; 11 B were dropped") && l.listener->serialStatus().putBack == 0);
    }
}

// ---- the gate's hold ------------------------------------------------------------------------------------------------

// The programs that are declared for a serial service are kept off the node from a phone's request until its link
// has been let go, and for as long as a link of the agent's is up.
static void testHold() {
    std::string dir = freshFolder("hold");
    const std::string on = "gate: opens by otherd, radiod are refused as well from now on ";
    const std::string off = "gate: opens by otherd, radiod are passed on again ";
    {
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        Machine machine(dir + "/root");
        machine.running = "radiod";
        Store store(&machine);
        SerialServices services(&machine, &store, &trace);
        std::vector<SerialServiceOwner> declared;
        declared.push_back(declares("TestRadio", "radiod"));
        declared.push_back(declares("TestRadio-2", "radiod"));
        declared.push_back(declares("Other", "otherd"));
        services.setDeclared(declared);
        SerialConfig config = configFor(dir);
        config.linkWaitMs = 600;
        SerialListener listener(&gap, &gate, &trace, config);
        services.setRemover(&listener);
        listener.setServices(&services);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));
        pause(300);
        CHECK(!trace.saw("gate hold") && !trace.saw("gate release"));

        // A phone asks: the hold is on before its node is there, with each program once. The link is not the
        // agent's, and the hold goes off when it has been let go.
        Node silent(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        CHECK_IN(trace.text(), trace.count("gate hold otherd, radiod") == 1);
        CHECK_IN(trace.text(), sawWithin(trace, on + "(a phone has asked for a link)", 2000));
        silent.appear();
        CHECK_IN(trace.text(), sawWithin(trace, off + "(the link was let go)", 3000) && trace.saw("gate release"));
        CHECK(trace.at("gate hold") < trace.at("btspp0 opened") && trace.at("btspp0 opened") < trace.at("btspp0 let go") &&
              trace.at("btspp0 let go") < trace.at("gate release"));

        // The car refuses the phone: off at once.
        gap.phone(kInfServiceRequest, kCarSerialService);
        CHECK(trace.count("gate hold") == 2 && trace.count("gate release") == 1);
        gap.phone(kInfServiceRequestError, kCarSerialService);
        CHECK_IN(trace.text(), trace.count("gate release") == 2);
        CHECK_IN(trace.text(), sawWithin(trace, off + "(the car refused the phone's link)", 2000));

        // No link comes of the request: off when the wait for it is over, and not before.
        gap.phone(kInfServiceRequest, kCarSerialService);
        CHECK(trace.count("gate hold") == 3);
        pause(200);
        CHECK(trace.count("gate release") == 2);
        CHECK_IN(trace.text(), sawWithin(trace, off + "(no link came of the phone's request)", 3000));
        CHECK(trace.count("gate release") == 3);

        // A link of the agent's: the hold stays on for as long as the link is up, whatever else happens.
        Node ours(dir + "/btspp", 1);
        gap.phone(kInfServiceRequest, kCarSerialService);
        ours.appear();
        Link* link = dialled(listener, ours, trace);
        CHECK(trace.count("gate hold") == 4);
        pause(700);                                       // well past the wait for a request's link
        gap.phone(kInfServiceRequest, kCarSerialService);
        gap.phone(kInfServiceRequestError, kCarSerialService);
        pause(300);
        exchange(ours, link, "held", "gehalten");
        CHECK_IN(trace.text(), trace.count("gate hold") == 4 && trace.count("gate release") == 3);
        ours.hangUp();
        ours.vanish();
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK_IN(trace.text(), sawWithin(trace, off + "(the agent's link has ended)", 3000));
        CHECK(trace.count("gate release") == 4);

        // The listener stops with the hold on: off, and then the gate goes.
        gap.phone(kInfServiceRequest, kCarSerialService);
        CHECK(trace.count("gate hold") == 5);
        listener.stop();
        CHECK_IN(trace.text(), trace.count("gate release") == 5 && trace.saw("gate detach"));
        // And a request that comes after that turns nothing on.
        gap.phone(kInfServiceRequest, kCarSerialService);
        CHECK(trace.count("gate hold") == 5);
        services.setRemover(0);
    }
    {   // nothing declared: no hold, and the links work as they do
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        Machine machine(dir + "/root");
        Store store(&machine);
        SerialServices services(&machine, &store, &trace);
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        services.setRemover(&listener);
        listener.setServices(&services);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        Node ours(dir + "/btspp", 2);
        gap.phone(kInfServiceRequest, kCarSerialService);
        ours.appear();
        Link* link = dialled(listener, ours, trace);
        exchange(ours, link, "free", "frei");
        // A package's program is declared while the link is up: it is held off from then on.
        services.setDeclared(std::vector<SerialServiceOwner>(1, declares("TestRadio", "radiod")));
        CHECK_IN(trace.text(), sawWithin(trace, "gate hold radiod", 2000));
        CHECK(trace.saw("gate: opens by radiod are refused as well from now on (a link of the agent's is up)"));
        ours.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK_IN(trace.text(), sawWithin(trace, "gate release", 3000));
        listener.stop();
        services.setRemover(0);
    }
}

// ---- a phone's own link left to another program ---------------------------------------------------------------------

// A listener for the rule that leaves a phone's own link to another program unread, with its three conditions true
// unless a case changes one: the car dials; a program declared for a serial service (radiod) runs; and the phone
// kDialled is one of the agent's, whose app a link the car dialled has reached before. The phone's service list never
// names the agent's service, so nothing is dialled, and polls come only once, at the start.
struct Rule {
    Trace trace;
    LinksGone gone;
    FakeGap gap;
    FakeGate gate;
    std::string dir;
    Machine machine;
    Store store;
    SerialServices services;
    PhoneAddressList ours;
    SerialConfig config;
    SerialListener* listener;

    explicit Rule(const std::string& name)
        : gap(&trace), gate(&trace), dir(freshFolder(name)), machine(dir + "/root"), store(&machine),
          services(&machine, &store, &trace), listener(0) {
        config = configFor(dir);
        config.dial.on = true;
        config.dial.node = dir + "/nbtapp";
        config.dial.signalWaitMs = 300;
        config.dial.pollFirstMs = config.dial.pollLaterMs = 3600000;
        config.nodeCalls = heldCalls();
        config.readNode = &heldRead;
        gap.queryLists = false;
        gap.phones.push_back(carPhone(kDialled, kConnConnected, 7));
        services.setDeclared(std::vector<SerialServiceOwner>(1, declares("TestRadio", "radiod")));
        machine.running = "radiod";
        std::vector<std::string> one(1, kDialled);
        ours.set(one, one);
    }
    ~Rule() {
        if (listener != 0) {
            listener->stop();
            services.setRemover(0);
            delete listener;
        }
        if (getenv("NBT_TEST_TRACE") != 0) fprintf(stderr, "---- %s\n%s", dir.c_str(), trace.text().c_str());
    }
    void start() {
        listener = new SerialListener(&gap, &gate, &trace, config);
        services.setRemover(listener);
        listener->setServices(&services);
        listener->setPhoneAddresses(&ours);
        std::string why;
        CHECK_IN(why, listener->start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));
        // The poll at the start, with the dial on: its ask, and that it found the app not listening.
        if (config.dial.on) CHECK_IN(trace.text(), sawWithin(trace, "its app is not listening", 3000));
    }
    // What the phone that has the near end of `node` would read now, as another program that opens the node does.
    std::string otherReads(Node& node, size_t want) {
        int fd = node.openNear();
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        std::string got;
        double end = monoNow() + 2.0;
        while (got.size() < want && monoNow() < end) {
            char buffer[256];
            ssize_t n = read(fd, buffer, sizeof buffer);
            if (n > 0) got.append(buffer, (size_t)n);
            else pause(2);
        }
        close(fd);
        return got;
    }
};

// What the rule changes, and that it changes nothing in any other case.
static void testLeftUnread() {
    const std::string opening = "\x10\x20\x30\x40 another app's opening";
    const std::string rule = "left to another program, and never read: the car dials this phone of the agent's, whose "
                             "app a dialled link has reached, and a program declared for a serial service is running, "
                             "or whether one is cannot be told. ";
    {   // all three hold: the node is opened and primed, never read, no hold goes on, the other program, which waits
        // on select() before it reads, reads the link whole from its first byte, and the phone's request still
        // brings the ask that reaches the app by the dial
        Rule r("left-unread");
        r.start();
        unsigned asks = r.trace.count("gap query 00:11:22:33:44:55");
        Node node(r.dir + "/btspp", 0);
        node.held = true;
        // No agent on the node: what the phone sent before the other program's open never reaches it.
        {
            Node alone(r.dir + "/alone");
            alone.held = true;
            alone.send(opening);
            OtherProgram other(alone);
            CHECK(other.reads(1, 300).empty());
        }
        r.gap.phone(kInfServiceRequest, kCarSerialService, "001122334455");
        node.send(opening);   // before the node is there, or opened
        node.appear();
        CHECK_IN(r.trace.text(), sawWithin(r.trace, "btspp0: " + rule + "Its node is opened and primed for that program "
                                                    "with one read that does not wait, which took nothing. It is kept "
                                                    "open, unread, until the link ends", 2000));
        CHECK(node.holders() == 1);
        {
            OtherProgram other(node);
            CHECK(other.reads(opening.size(), 2000) == opening);
            node.send("and the next");
            CHECK(other.reads(12, 2000) == "and the next");
        }
        CHECK_IN(r.trace.text(), countWithin(r.trace, "gap query 00:11:22:33:44:55", asks + 1, 3000));
        CHECK(acceptWithin(*r.listener, 100) == 0);
        CHECK_IN(r.trace.text(), !r.trace.saw("btspp0 opened") && !r.trace.saw("gate hold") && !r.trace.saw("btspp0 let go"));
        CHECK(r.trace.loggedCount("left to another program") == 1 && r.listener->serialStatus().leftUnread == 1);
        CHECK(r.listener->serialStatus().putBack == 0 && r.listener->serialStatus().link == "none" && node.injects == 0);
        bool closed = false;
        CHECK(node.hear(1, 100, &closed).empty() && !closed);   // nothing written into it, and still open
        CHECK(node.holders() == 1);
        // Kept open until the link ends, and closed then.
        node.vanish();
        CHECK_IN(r.trace.text(), sawWithin(r.trace, "btspp0: the link ended ", 2000));
        CHECK(r.trace.saw("s after it was primed for another program, and its node is closed now"));
        CHECK(node.holders() == 0 && closedUnwritten(node, 1000));
        node.hangUp();
        pause(450);

        // The rule's cost: a link the agent's own app opens on such a phone, with the magic bytes, is left alone too.
        // The app is reached by the dial.
        Node app(r.dir + "/btspp", 1);
        app.held = true;
        r.gap.phone(kInfServiceRequest, kCarSerialService, "001122334455");
        app.send(kMagic);
        app.appear();
        CHECK_IN(r.trace.text(), sawWithin(r.trace, "btspp1: left to another program, and never read", 2000));
        CHECK(acceptWithin(*r.listener, 300) == 0 && !r.trace.saw("btspp1 opened") && !r.trace.saw("gate hold"));
        CHECK_IN(r.trace.text(), r.otherReads(app, kMagic.size()) == kMagic && r.listener->serialStatus().leftUnread == 2);
        app.hangUp();
        app.vanish();
        CHECK(!r.trace.logged("00:11:22") && !r.trace.logged("001122"));
    }
    {   // bytes that come into the node in the moment between the look at its queue and the primer's read are taken
        // by that read, and put back: the other program gets them first, then what follows, in order
        Rule r("left-unread-gap");
        r.start();
        Node node(r.dir + "/btspp", 0);
        node.setGap("just come");
        r.gap.phone(kInfServiceRequest, kCarSerialService, "001122334455");
        node.appear();
        CHECK_IN(r.trace.text(), sawWithin(r.trace, "btspp0: " + rule + "Its node is opened and primed for that program "
                                                    "with one read that does not wait, which took 9 B that had just "
                                                    "come: they were put back. It is kept open, unread, until the link "
                                                    "ends", 2000));
        CHECK(node.injects == 1 && r.listener->serialStatus().putBack == 1 && r.listener->serialStatus().leftUnread == 1);
        OtherProgram other(node);
        node.send("then this");
        CHECK(other.reads(18, 2000) == "just comethen this");
        CHECK(node.holders() == 2);   // the listener's and the other program's
    }
    {   // bytes in the node's queue already: nothing is read. A put-back that fails: the bytes the read took are lost,
        // the node is closed again and left as it is, and the link still counts as left
        Rule r("left-unread-fail");
        r.start();
        Node queued(r.dir + "/btspp", 0);
        r.gap.phone(kInfServiceRequest, kCarSerialService, "001122334455");
        queued.say("waiting");   // a terminal that is not held: into its queue at once
        pause(20);
        queued.appear();
        CHECK_IN(r.trace.text(), sawWithin(r.trace, "btspp0: " + rule + "Its node is opened, and 7 B were in its queue "
                                                    "already, so nothing is read. It is kept open", 2000));
        CHECK(OtherProgram(queued).reads(7, 2000) == "waiting");
        queued.vanish();
        pause(450);

        Node failing(r.dir + "/btspp", 1);
        failing.setGap("just come");
        failing.injectError = EIO;
        r.gap.phone(kInfServiceRequest, kCarSerialService, "001122334455");
        failing.appear();
        CHECK_IN(r.trace.text(), sawWithin(r.trace, "btspp1: " + rule + "Its node could not be primed for that program "
                                                    "(putting the bytes back failed: " + strerror(EIO) + "), and the 9 B "
                                                    "its read took are lost: it is closed again, and left as it is", 2000));
        CHECK(failing.holders() == 0 && closedUnwritten(failing, 1000));
        CHECK(r.listener->serialStatus().leftUnread == 2 && r.listener->serialStatus().putBack == 0);
        CHECK(!r.trace.saw("btspp1 opened") && !r.trace.saw("gate hold") && acceptWithin(*r.listener, 100) == 0);
    }
    {   // serial.reset with only such a node open: it is not a node of the listener's, so the reset is armed for the
        // next link and nothing is sent; with a link of the agent's up as well, it is sent as ever, and only that
        // link's node is watched
        Rule r("left-unread-reset");
        r.config.resetArmedMs = 1000;
        r.start();
        Node node(r.dir + "/btspp", 0);
        node.held = true;
        r.gap.phone(kInfServiceRequest, kCarSerialService, "001122334455");
        node.appear();
        CHECK_IN(r.trace.text(), sawWithin(r.trace, "btspp0: left to another program", 2000));
        std::string why;
        int64_t forMs = 0;
        CHECK(r.listener->resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetArmed && forMs == 1000);
        CHECK_IN(r.trace.text(), r.trace.saw("serial.reset: no serial node is open, so the next serial link whose node "
                                             "opens within 1 s is ended at its open"));
        CHECK_IN(r.trace.text(), sawWithin(r.trace, "serial.reset: no serial link came within 1 s", 3000));
        CHECK(!r.trace.saw("gap disconnect") && node.holders() == 1);

        // A link of the agent's, from a phone the rule does not hold for.
        Node ours(r.dir + "/btspp", 1);
        r.gap.phone(kInfServiceRequest, kCarSerialService, "66:77:88:99:AA:BB");
        ours.appear();
        Link* link = dialled(*r.listener, ours, r.trace);
        CHECK(r.listener->resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetSent);
        CHECK_IN(r.trace.text(), sawWithin(r.trace, "gap disconnect", 2000));
        CHECK_IN(r.trace.text(), sawWithin(r.trace, "serial.reset: ServiceDisconnect sent, ", 2000));
        ours.hangUp();
        ours.vanish();
        CHECK_IN(r.trace.text(), sawWithin(r.trace, "serial.reset: " + ours.path + " went ", 2000));
        CHECK(endsWithin(link, 3000));
        delete link;
        pause(300);
        CHECK_IN(r.trace.text(), !r.trace.saw("serial.reset: " + node.path + " went") &&
                                 !r.trace.saw("the link was still up 3 s after ServiceDisconnect"));
        CHECK(node.holders() == 1 && r.trace.count("gap disconnect") == 1);
    }
    // Each of the three not so, and a request with no address: the link is decided on as ever, with the hold on
    // from the request, and the agent's app's link is the agent's.
    for (int c = 0; c < 5; ++c) {
        Rule r("left-unread-not");
        const char* address = "001122334455";
        if (c == 0) r.config.dial.on = false;
        if (c == 1) r.machine.running = "";
        if (c == 2) address = "66:77:88:99:AA:BB";                       // not one of the agent's phones
        if (c == 3) r.ours.set(std::vector<std::string>(1, kDialled));   // the agent's, never reached by a dial
        if (c == 4) address = "";
        r.start();
        Node node(r.dir + "/btspp", 0);
        r.gap.phone(kInfServiceRequest, kCarSerialService, address);
        CHECK_IN(r.trace.text(), r.trace.saw("gate hold radiod"));
        node.appear();
        Link* link = dialled(*r.listener, node, r.trace);
        CHECK_IN(r.trace.text(), r.trace.saw("btspp0 opened") && r.trace.saw("btspp0: it opened with the magic bytes"));
        CHECK(!r.trace.saw("left to another program") && r.listener->serialStatus().leftUnread == 0);
        node.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
    }
    {   // the rule held at the request, and the program declared for a serial service is gone when the node comes:
        // the link is decided on as any other
        Rule r("left-unread-changed");
        r.start();
        Node node(r.dir + "/btspp", 0);
        r.gap.phone(kInfServiceRequest, kCarSerialService, "001122334455");
        CHECK(!r.trace.saw("gate hold"));
        r.services.setDeclared(std::vector<SerialServiceOwner>());
        node.appear();
        Link* link = dialled(*r.listener, node, r.trace);
        CHECK_IN(r.trace.text(), r.trace.saw("btspp0: the phone's request was to be left to another program, but no "
                                             "program declared for a serial service is running now: the link is "
                                             "decided on as any other"));
        CHECK(r.listener->serialStatus().leftUnread == 0);
        node.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
    }
}

// What the app is told of the rule for its phone (phoneOpenedLeft(), info.get): true only while all three hold.
static void testPhoneOpenedLeft() {
    {
        Rule r("opened-left");
        r.start();
        CHECK(r.listener->phoneOpenedLeft(kDialled));
        CHECK(!r.listener->phoneOpenedLeft("66:77:88:99:AA:BB"));   // not one of the agent's phones
        CHECK(!r.listener->phoneOpenedLeft(""));                    // a link that knows no address
        r.services.setDeclared(std::vector<SerialServiceOwner>());   // none is declared
        CHECK(!r.listener->phoneOpenedLeft(kDialled));
        r.services.setDeclared(std::vector<SerialServiceOwner>(1, declares("TestRadio", "radiod")));
        CHECK(r.listener->phoneOpenedLeft(kDialled));
        r.ours.set(std::vector<std::string>(1, kDialled));           // the agent's, never reached by a dial
        CHECK(!r.listener->phoneOpenedLeft(kDialled));
        r.ours.set(std::vector<std::string>());                      // forgotten
        CHECK(!r.listener->phoneOpenedLeft(kDialled));
        CHECK(!r.trace.logged("00:11:22") && !r.trace.logged("66:77:88"));
    }
    for (int c = 0; c < 2; ++c) {
        Rule r("opened-left-not");
        if (c == 0) r.config.dial.on = false;   // the car does not dial in this run
        if (c == 1) r.machine.running = "";     // the program declared for a serial service is not running
        r.start();
        CHECK_IN(c == 0 ? "dial off" : "not running", !r.listener->phoneOpenedLeft(kDialled));
    }
}

// A dial that does not give the agent a link, kDialFailsToUnmark times in a row for a phone that a dialled link has
// reached before: the mark goes from the record of paired phones, on the agent's thread, and the phone's own links
// are decided on again; the next dialled link the app takes sets it again.
static void testUndialled() {
    const std::string id(32, '1');
    Car car("{\"version\":1,\"phones\":[{\"phoneId\":\"" + id + "\",\"name\":\"Test phone\",\"key\":\"" +
            std::string(64, '3') + "\",\"pairedAt\":3,\"address\":\"00:11:22:33:44:55\",\"dialled\":true}]}\n");
    std::string file = car.platform.stateDir() + "/phones.json";
    PhoneAddressList* ours = car.agent.phoneAddresses();
    CHECK(ours->dialledBefore(kDialled));
    car.listener.stop();
    // A program declared for a serial service runs, so the rule leaves the phone's own links while the mark holds.
    Machine machine(car.dir + "/machine");
    Store store(&machine);
    SerialServices services(&machine, &store, &car.trace);
    services.setDeclared(std::vector<SerialServiceOwner>(1, declares("TestRadio", "radiod")));
    machine.running = "radiod";
    SerialConfig config = configFor(car.dir);
    config.dial.on = true;
    config.dial.node = car.dir + "/nbtapp";
    config.dial.pollFirstMs = config.dial.pollLaterMs = 3600000;   // the first poll only, at the start
    Node node(config.dial.node);
    car.gap.dialNode = &node;
    car.gap.connectError = 5;
    car.gap.phones.push_back(carPhone(kDialled, kConnConnected, 7));
    SerialListener listener(&car.gap, &car.gate, &car.trace, config);
    services.setRemover(&listener);
    listener.setServices(&services);
    listener.setPhoneAddresses(ours);
    car.agent.addListener(&listener);
    car.agent.setSerialDial(&listener);
    std::string why;
    CHECK_IN(why, listener.start(&why));
    car.gap.attach();
    CHECK(listener.phoneOpenedLeft(kDialled));
    // The poll at the start, then two forced dials: each answered 5.
    for (unsigned i = 1; i <= kDialFailsToUnmark; ++i) {
        if (i > 1) CHECK_IN(why, listener.dialNow(&why) == 1);
        CHECK_IN(car.trace.text(), countWithin(car.trace, "gap connect BtCtrl nbtapp", i, 3000));
        double end = monoNow() + 2.0;
        while (listener.serialStatus().dialState != "idle" && monoNow() < end) pause(5);
        if (i < kDialFailsToUnmark) CHECK(!car.trace.logged("dials in a row gave the agent no link"));
    }
    CHECK_IN(car.trace.text(), sawWithin(car.trace, "phone 1: 3 dials in a row gave the agent no link: the mark that a "
                                                    "dialled link has reached its app is given up, and a link the phone "
                                                    "opens itself is decided on again as any other", 2000));
    // The agent's thread clears it, at its next turn.
    double end = monoNow() + 3.0;
    while (car.agent.phones().find(id)->dialled && monoNow() < end) car.agent.turn(10);
    CHECK(!car.agent.phones().find(id)->dialled && !ours->dialledBefore(kDialled) && ours->has(kDialled));
    CHECK_IN(testutil::readFile(file), testutil::readFile(file).find("dialled") == std::string::npos);
    CHECK(!listener.phoneOpenedLeft(kDialled));
    // A fourth failure says nothing more, and hands nothing more over.
    CHECK_IN(why, listener.dialNow(&why) == 1);
    CHECK_IN(car.trace.text(), countWithin(car.trace, "gap connect BtCtrl nbtapp", 4, 3000));
    end = monoNow() + 2.0;
    while (listener.serialStatus().dialState != "idle" && monoNow() < end) pause(5);
    CHECK(car.trace.loggedCount("dials in a row gave the agent no link") == 1 && listener.takeUndialled().empty());

    // The phone's own link is decided on, and is the agent's.
    Node opened(car.dir + "/btspp", 0);
    car.gap.phone(kInfServiceRequest, kCarSerialService, "001122334455");
    opened.appear();
    PhoneApp own(opened);
    CHECK_IN(car.trace.text(), car.sessionsWithin(1, 3000));
    CHECK(car.trace.logged("btspp0: it opened with the magic bytes, so the link is the agent's"));
    CHECK(!car.trace.logged("left to another program") && listener.serialStatus().leftUnread == 0);
    own.peer->close("done");
    CHECK(car.sessionsWithin(0, 3000));
    CHECK(waitForSerialLinks(3000));

    // A dial the app takes marks the phone again.
    car.gap.connectError = 0;
    unsigned dials = car.trace.count("gap connect BtCtrl nbtapp");
    end = monoNow() + 3.0;
    while (listener.dialNow(&why) == 0 && monoNow() < end) pause(20);
    CHECK_IN(car.trace.text(), countWithin(car.trace, "gap connect BtCtrl nbtapp", dials + 1, 3000));
    PhoneApp dialled(node);
    CHECK_IN(car.trace.text(), car.sessionsWithin(1, 3000));
    CHECK(car.agent.phones().find(id)->dialled && ours->dialledBefore(kDialled) && listener.phoneOpenedLeft(kDialled));
    CHECK_IN(testutil::readFile(file), testutil::readFile(file).find("\"dialled\":true") != std::string::npos);
    dialled.peer->close("done");
    node.vanish();
    car.gap.ended("nbtapp", kDialled, "NBTApp", 1);
    CHECK(car.sessionsWithin(0, 3000));
    CHECK(!car.trace.logged("00:11:22") && !car.trace.logged("001122") && !car.trace.logged("Test phone"));
    car.agent.setSerialDial(0);
    listener.stop();
    services.setRemover(0);
    CHECK(waitForSerialLinks(3000));
}

// ---- the node's reader ----------------------------------------------------------------------------------------------

// What a stand-in for the car's Bluetooth stack holds for a node: bytes the phone sent that wait ahead of the node,
// which only a read brings forward. The node itself, a terminal here, never has them, so a wait on it never says
// that there is something to read while a read would return them.
static pthread_mutex_t gAheadMu = PTHREAD_MUTEX_INITIALIZER;
static std::string gAhead;

static void sendAhead(const std::string& bytes) {
    pthread_mutex_lock(&gAheadMu);
    gAhead += bytes;
    pthread_mutex_unlock(&gAheadMu);
}

// The stand-in's timed read: what waits ahead of the node, in pieces of 1,700 bytes at most, at once; with
// nothing waiting, the node itself, for its end, and a short wait.
static long readAhead(int fd, void* buf, size_t len, int ms) {
    pthread_mutex_lock(&gAheadMu);
    size_t n = gAhead.size() < len ? gAhead.size() : len;
    if (n > 1700) n = 1700;
    memcpy(buf, gAhead.data(), n);
    gAhead.erase(0, n);
    pthread_mutex_unlock(&gAheadMu);
    if (n > 0) return (long)n;
    return timedNodeRead(fd, buf, len, ms < 20 ? ms : 20);
}

// A read that does not come back when its time is up: it waits until its node has been closed under it.
static long readStuck(int fd, void*, size_t, int) {
    while (fcntl(fd, F_GETFD) != -1) pause(5);
    errno = EBADF;
    return -1;
}

// What the reader gives until `want` bytes are there, or it says the link has ended. *ended: it said so.
static std::string takeFrom(NodeReader* reader, size_t want, int ms, bool* ended) {
    std::vector<uint8_t> got;
    *ended = false;
    double end = monoNow() + ms / 1000.0;
    while (got.size() < want && monoNow() < end) {
        if (reader->next(100, &got) == NodeReader::kEnded) {
            *ended = true;
            break;
        }
    }
    return std::string(got.begin(), got.end());
}

static void testReader() {
    std::string dir = freshFolder("reader");
    {   // what the phone sends comes in order; with nothing sent, nothing comes when the time is up; when the phone
        // ends the link, the reader says so after the last of its bytes
        Trace trace;
        Node node(dir + "/btspp", 0);
        NodeReader* reader = NodeReader::start(node.openNear(), node.path, &trace);
        CHECK(reader != 0);
        std::vector<uint8_t> got;
        double at = monoNow();
        CHECK(reader->next(150, &got) == NodeReader::kNothing && got.empty());
        CHECK(monoNow() - at >= 0.14 && monoNow() - at < 1.0);
        const std::string sent = pattern(100000, 7);
        node.say(sent);
        bool ended = false;
        CHECK(takeFrom(reader, sent.size(), 5000, &ended) == sent && !ended);
        node.say("last");
        CHECK(takeFrom(reader, 4, 3000, &ended) == "last" && !ended);
        node.hangUp();
        CHECK(takeFrom(reader, 1, 3000, &ended).empty() && ended && reader->ended() && reader->dropped() == 0);
        reader->finish();
        // Said once for the process, by the first reader whose read had its time run out.
        CHECK_IN(trace.text(), trace.saw("btspp0: a read with nothing to read came back when its time was up, on time"));
    }
    {   // stop() while a read waits: the thread has stopped within a tick, the node is handed back open, and what
        // the phone sends from then on stays for whoever reads the node next
        Trace trace;
        Node node(dir + "/btspp", 1);
        int fd = node.openNear();
        NodeReader* reader = NodeReader::start(fd, node.path, &trace);
        pause(50);
        double at = monoNow();
        CHECK(reader->stop() == fd);
        CHECK(monoNow() - at < 0.25 && fcntl(fd, F_GETFD) != -1);
        node.say("for the next reader");
        char buffer[64];
        CHECK(read(fd, buffer, sizeof buffer) == 19 && memcmp(buffer, "for the next reader", 19) == 0);
        close(fd);
    }
    {   // in drain mode what comes is counted and dropped, what waited in the queue too
        Trace trace;
        Node node(dir + "/btspp", 2);
        NodeReader* reader = NodeReader::start(node.openNear(), node.path, &trace);
        node.say("queued");
        pause(100);
        reader->drain();
        node.say("and dropped");
        std::vector<uint8_t> got;
        CHECK(reader->next(200, &got) == NodeReader::kNothing && got.empty());
        node.hangUp();
        bool ended = false;
        CHECK(takeFrom(reader, 1, 3000, &ended).empty() && ended && reader->dropped() == 17);
        reader->finish();
    }
    {   // a read that does not come back when its time is up: after three ticks the node is closed under it, the
        // log says so, and the thread frees itself once that read has returned
        Trace trace;
        Node node(dir + "/btspp", 3);
        NodeReader* reader = NodeReader::start(node.openNear(), node.path, &trace, &readStuck);
        pause(50);
        double at = monoNow();
        CHECK(reader->stop() == -1);
        double took = monoNow() - at;
        CHECK(took >= 0.29 && took < 1.0);
        CHECK_IN(trace.text(), trace.saw("btspp3: its read did not come back within 300 ms of being asked to stop, so "
                                         "the node is closed under it"));
        pause(50);
    }
    {   // a stand-in for the car's stack, on which the bytes wait ahead of the node and a wait on the node never
        // says that they are there: they come all the same, in order, because a read is always out
        Trace trace;
        Node node(dir + "/btspp", 4);
        int fd = node.openNear();
        NodeReader* reader = NodeReader::start(fd, node.path, &trace, &readAhead);
        const std::string sent = pattern(20000, 8);
        sendAhead(sent);
        bool ended = false;
        CHECK(takeFrom(reader, sent.size(), 5000, &ended) == sent && !ended);
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        CHECK(select(fd + 1, &rf, NULL, NULL, &tv) == 0);   // the node itself never had them
        node.hangUp();
        takeFrom(reader, 1, 3000, &ended);
        CHECK(ended);
        reader->finish();
    }
    {   // a time limit that could not be armed: the first reader whose read comes back after that says so, with the
        // error, and no reader says it again in the process
        noteNodeReadArmError(EINVAL);
        noteNodeReadArmError(EPERM);   // only the first is kept
        Trace trace;
        Node node(dir + "/btspp", 5), next(dir + "/btspp", 6);
        NodeReader* reader = NodeReader::start(node.openNear(), node.path, &trace);
        CHECK_IN(trace.text(), sawWithin(trace, std::string("btspp5: the time limit of a read could not be set (") +
                                                    strerror(EINVAL) + "): a read of the node waits until bytes come "
                                                    "or the link ends", 2000));
        NodeReader* second = NodeReader::start(next.openNear(), next.path, &trace);
        pause(300);
        CHECK(trace.count("could not be set") == 1);
        int error = 0;
        CHECK(!takeNodeReadArmError(&error));
        reader->finish();
        second->finish();
    }
}

// The primer by itself, on a node with the test's calls, and with the host's own.
static void testPrimer() {
    std::string dir = freshFolder("primer");
    NodePrimer primer(heldCalls());
    {   // a held node: the read takes nothing and brings what waits ahead of the node into it
        Node node(dir + "/btspp", 0);
        node.held = true;
        node.send("first");
        int fd = node.openNear();
        NodePrimer::Result r;
        std::string why;
        CHECK_IN(why, primer.prime(fd, &r, &why) && r.queued == 0 && r.taken == 0 && r.putBack == 0);
        OtherProgram other(node);
        CHECK(other.reads(5, 1000) == "first");
        // Once primed, the next prime finds the queue as it is: bytes there, nothing read.
        node.send("second");
        pause(20);
        CHECK_IN(why, primer.prime(fd, &r, &why) && r.queued == 6 && r.taken == 0);
        CHECK(other.reads(6, 1000) == "second");
        close(fd);
    }
    {   // a put-back goes in front of what came into the queue meanwhile, which is read out and put back behind it
        Node node(dir + "/btspp", 1);
        int fd = node.openNear();
        node.say("later");
        pause(20);
        std::vector<uint8_t> read;
        const std::string earlier = "earlier ";
        read.assign(earlier.begin(), earlier.end());
        NodePrimer::Result r;
        std::string why;
        CHECK_IN(why, primer.putBack(fd, read, &r, &why) && r.putBack == 13 && r.fromQueue == 5 && r.lost == 0);
        CHECK(OtherProgram(node).reads(13, 1000) == "earlier later");
        // Nothing to put back: no call is made, not even one that would fail.
        node.injectError = EIO;
        CHECK_IN(why, primer.putBack(fd, std::vector<uint8_t>(), &r, &why) && r.putBack == 0 && node.injects == 1);
        // Too much for the queue with what waits there: nothing is read out of it, and it stays for the next reader.
        node.injectError = 0;
        node.queueSize = 10;
        node.say("waits");
        pause(20);
        CHECK(!primer.putBack(fd, read, &r, &why) && why == "13 B would not fit in its queue of 10 B" && r.lost == 8);
        CHECK(OtherProgram(node).reads(5, 1000) == "waits" && node.injects == 1);
        close(fd);
    }
    {   // the host's own calls: the queue's fill, the terminal settings and the read that does not wait, but no size
        // and no put-back
        Node node(dir + "/btspp", 2);
        int fd = node.openNear();
        NodeCalls host = platformNodeCalls();
        CHECK(host.raw(fd) == 1 && host.queued(fd) == 0);
        char byte;
        CHECK(host.readNow(fd, &byte, 1) == -1 && errno == EAGAIN);
        CHECK(host.queueSize(fd) == -1 && errno == ENOSYS && host.inject(fd, "x", 1) == -1 && errno == ENOSYS);
        node.say("ab");
        pause(20);
        CHECK(host.queued(fd) == 2 && host.readNow(fd, &byte, 1) == 1 && byte == 'a');
        NodePrimer own;
        NodePrimer::Result r;
        std::string why;
        CHECK(!own.putBack(fd, std::vector<uint8_t>(1, 'a'), &r, &why) && has(why, "the size of its queue could not be told"));
        struct termios cooked;
        CHECK(tcgetattr(fd, &cooked) == 0);
        cooked.c_lflag |= ICANON;
        CHECK(tcsetattr(fd, TCSANOW, &cooked) == 0 && host.raw(fd) == 0);
        close(fd);
    }
}

// ---- what an earlier link left behind -------------------------------------------------------------------------------

// A frame of the protocol, as a phone app sends one: a MSG with this text.
static std::string frameOf(const std::string& text) {
    std::string out;
    encodeFrame(kFrameMsg, 0, 0, text.data(), text.size(), &out);
    return out;
}

// What a phone sent into a link that nobody read comes on the next link, ahead of what the phone sends on that
// one. Whole frames there are passed over, and the magic bytes looked for after them.
static void testLeftOvers() {
    std::string dir = freshFolder("left-over");
    const std::string old = frameOf("{\"id\":7,\"method\":\"ping\",\"params\":{}}") + frameOf("{\"id\":8,\"result\":{}}");
    char said[160], passed[160];
    snprintf(said, sizeof said, ": %u B left over from an earlier link came first, as whole frames, and were dropped",
             (unsigned)old.size());
    // Of a link that is let go: where they go is the let-go line's to say.
    snprintf(passed, sizeof passed, ": %u B left over from an earlier link came first, as whole frames, and were "
             "passed over", (unsigned)old.size());
    {
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));

        // Old frames, then this link's magic bytes: the agent's link in the same try, from the magic bytes on.
        Node first(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        first.appear();
        first.say(old + kMagic + "and the rest");
        Link* link = acceptWithin(listener, 3000);
        CHECK_IN(trace.text(), link != 0);
        CHECK(readFrom(link, 16, 3000) == kMagic + "and the rest");
        CHECK_IN(trace.text(), trace.saw(std::string("btspp0") + said));
        CHECK(trace.saw("btspp0: it opened with the magic bytes, so the link is the agent's"));
        CHECK(trace.count("left over from an earlier link") == 1);
        first.hangUp();
        first.vanish();
        CHECK(endsWithin(link, 3000));
        delete link;

        // The same in pieces: a frame cut in two, and the magic bytes too.
        Node pieces(dir + "/btspp", 1);
        gap.phone(kInfServiceRequest, kCarSerialService);
        pieces.appear();
        pieces.say(old.substr(0, 10));
        pause(40);
        pieces.say(old.substr(10) + "NB");
        pause(40);
        pieces.say("TL!");
        link = acceptWithin(listener, 3000);
        CHECK_IN(trace.text(), link != 0);
        CHECK(readFrom(link, 5, 3000) == "NBTL!" && trace.saw(std::string("btspp1") + said));
        pieces.hangUp();
        pieces.vanish();
        CHECK(endsWithin(link, 3000));
        delete link;

        // Old frames and nothing more in the time: let go, as any link that is not the agent's. No program is
        // declared for a serial service here, so its node is read, with what comes dropped, until the link ends.
        Node only(dir + "/btspp", 2);
        gap.phone(kInfServiceRequest, kCarSerialService);
        only.appear();
        only.say(old);
        CHECK_IN(trace.text(), sawWithin(trace, "btspp2 let go", 3000));
        CHECK_IN(trace.text(), trace.saw("(nothing more came after the leftovers in that time). No program is "
                                         "declared for a serial service, the gate has seen no other program open the "
                                         "node"));
        CHECK(trace.saw(std::string("btspp2") + passed));
        only.say("NBTL late");
        bool closed = false;
        CHECK(only.hear(1, 300, &closed).empty() && !closed && acceptWithin(listener, 50) == 0);
        only.hangUp();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp2: the link ended ", 3000));
        CHECK(trace.saw("s after it was let go; 9 B were dropped, and its node is closed now"));
        only.vanish();

        // Old frames and then something else: let go at once, and read the same way.
        Node other(dir + "/btspp", 3);
        gap.phone(kInfServiceRequest, kCarSerialService);
        other.appear();
        double at = monoNow();
        other.say(old + "\x10\x20\x30\x40 not a frame");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp3 let go", 3000) && monoNow() - at < 0.25);
        CHECK(trace.saw("(after the leftovers came bytes that are neither a frame nor the magic bytes). No program is "
                        "declared"));
        CHECK(other.hear(1, 100, &closed).empty() && !closed && acceptWithin(listener, 50) == 0);
        other.hangUp();
        CHECK_IN(trace.text(), sawWithin(trace, "btspp3: the link ended ", 3000));
        other.vanish();
        listener.stop();
    }
    {   // the same two with a program declared for a serial service that runs: the link may be its, with the
        // agent's old bytes in front, so its node is closed for it, with nothing written
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        Machine machine(dir + "/root");
        machine.running = "radiod";
        Store store(&machine);
        SerialServices services(&machine, &store, &trace);
        services.setDeclared(std::vector<SerialServiceOwner>(1, declares("TestRadio", "radiod")));
        SerialListener listener(&gap, &gate, &trace, configFor(dir));
        services.setRemover(&listener);
        listener.setServices(&services);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));

        Node only(dir + "/btspp", 0);
        gap.phone(kInfServiceRequest, kCarSerialService);
        only.appear();
        only.say(old);
        CHECK_IN(trace.text(), sawWithin(trace, "btspp0 let go", 3000));
        CHECK(trace.saw("(nothing more came after the leftovers in that time) and closed"));
        CHECK(trace.saw(std::string("btspp0") + passed) && closedUnwritten(only, 2000) && acceptWithin(listener, 50) == 0);
        only.vanish();

        Node other(dir + "/btspp", 1);
        gap.phone(kInfServiceRequest, kCarSerialService);
        other.appear();
        other.say(old + "\x10\x20\x30\x40 not a frame");
        CHECK_IN(trace.text(), sawWithin(trace, "btspp1 let go", 3000));
        CHECK(trace.saw("(after the leftovers came bytes that are neither a frame nor the magic bytes) and closed"));
        CHECK(closedUnwritten(other, 2000) && acceptWithin(listener, 50) == 0);
        other.vanish();
        listener.stop();
        services.setRemover(0);
    }
    {   // the whole way with the stand-in for the car's stack: nothing waits on the node, and the link works
        Trace trace;
        LinksGone gone;
        FakeGap gap(&trace);
        FakeGate gate(&trace);
        SerialConfig config = configFor(dir);
        config.readNode = &readAhead;
        SerialListener listener(&gap, &gate, &trace, config);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));
        Node node(dir + "/btspp", 4);
        gap.phone(kInfServiceRequest, kCarSerialService);
        node.appear();
        sendAhead(frameOf("{}") + kMagic + "up");
        Link* link = acceptWithin(listener, 3000);
        CHECK_IN(trace.text(), link != 0);
        CHECK(readFrom(link, 6, 3000) == kMagic + "up");
        const std::string more = pattern(5000, 9);
        sendAhead(more);
        CHECK(readFrom(link, more.size(), 3000) == more);
        CHECK(link->write("down", 4) == 4);
        bool closed = false;
        CHECK(node.hear(4, 3000, &closed) == "down");
        node.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
        listener.stop();
    }
}

// ---- serial.reset ---------------------------------------------------------------------------------------------------

// The listener's side of serial.reset: ServiceDisconnect for a link whose node is open, a little after it was asked
// for, and with none open, at the open of the next link's node; and in the log, what came of it.
static void testReset() {
    std::string dir = freshFolder("reset");
    Trace trace;
    LinksGone gone;
    FakeGap gap(&trace);
    FakeGate gate(&trace);
    SerialConfig config = configFor(dir);
    config.resetArmedMs = 1500;
    // The car answers each ServiceDisconnect, as it does: one at a time, so one with no answer would hold up the next.
    gap.answersDisconnect = true;
    std::string file = dir + "/serial-problems.log";
    ProblemLog problems(file, "1.0.0 (build test)");   // before the listener, which writes to it until it stops
    SerialListener listener(&gap, &gate, &trace, config);
    listener.setProblemLog(&problems);
    std::string why;
    int64_t forMs = 0;
    // Not set up: nothing can be asked.
    CHECK(listener.resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetFailed && !why.empty());
    CHECK_IN(why, listener.start(&why));
    gap.attach();
    CHECK_IN(trace.text(), sawWithin(trace, "the serial service 'NBTApp' is registered", 2000));

    // A link of the agent's is up: sent, and ServiceDisconnect goes out some 300 ms later, for the phone that asked
    // for the link last. The car ends the link, and the log says how soon its node went.
    Node first(dir + "/btspp", 0);
    gap.phone(kInfServiceRequest, kCarSerialService);
    first.appear();
    Link* link = dialled(listener, first, trace);
    double asked = monoNow();
    CHECK(listener.resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetSent);
    CHECK(!trace.saw("gap disconnect"));
    CHECK_IN(trace.text(), sawWithin(trace, "gap disconnect", 2000));
    CHECK(monoNow() - asked >= 0.29);
    CHECK(gap.disconnect == buildServiceDisconnect("BtCtrl", "ExtBox1", "AABBCCDDEEFF", kCarSerialService));
    CHECK_IN(trace.text(), trace.saw("serial.reset: ServiceDisconnect sent, "));
    first.hangUp();
    first.vanish();
    CHECK_IN(trace.text(), sawWithin(trace, "serial.reset: " + first.path + " went ", 2000));
    CHECK(trace.saw(" ms after ServiceDisconnect was sent"));
    CHECK(endsWithin(link, 3000));
    delete link;

    // Other values; and a link that stays up, which the log says 3 s after.
    Node second(dir + "/btspp", 1);
    gap.phone(kInfServiceRequest, kCarSerialService);
    second.appear();
    link = dialled(listener, second, trace);
    CHECK(listener.resetSerialLink("Dev2", "Inst2", "Other", &forMs, &why) == SerialReset::kResetSent);
    CHECK_IN(trace.text(), sawWithin(trace, "serial.reset: the link was still up 3 s after ServiceDisconnect was sent: " +
                                                second.path + " goes on being read until the link ends", 5000));
    CHECK(gap.disconnect == buildServiceDisconnect("Dev2", "Inst2", "AABBCCDDEEFF", "Other"));
    exchange(second, link, "still", "noch");
    second.hangUp();
    second.vanish();
    CHECK(endsWithin(link, 3000));
    delete link;
    pause(450);   // the listener sees that the node has gone

    // No node is open: armed. The next link is ended at the open of its node, with no decide step, and its node
    // read until the link ends; even with the magic bytes it is not the agent's.
    unsigned before = trace.count("gap disconnect");
    CHECK(listener.resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetArmed && forMs == 1500);
    CHECK(trace.count("gap disconnect") == before);
    Node third(dir + "/btspp", 2);
    gap.phone(kInfServiceRequest, kCarSerialService);
    third.appear();
    CHECK_IN(trace.text(), sawWithin(trace, "btspp2: serial.reset asked ", 3000));
    CHECK(trace.saw("ServiceDisconnect is sent at once, with no decide step, and its node is read"));
    CHECK(trace.count("gap disconnect") == before + 1 && !trace.saw("btspp2: its first message came"));
    third.say(kMagic);
    bool closed = false;
    CHECK(acceptWithin(listener, 300) == 0 && third.hear(1, 50, &closed).empty() && !closed);
    third.hangUp();
    third.vanish();
    CHECK_IN(trace.text(), sawWithin(trace, "serial.reset: " + third.path + " went ", 2000));
    CHECK_IN(trace.text(), sawWithin(trace, "btspp2: the link ended ", 2000));
    CHECK(trace.saw("s after it was ended at its open; 4 B were dropped"));
    pause(450);

    // Once: the link after it is decided on as ever.
    Node fourth(dir + "/btspp", 3);
    gap.phone(kInfServiceRequest, kCarSerialService);
    fourth.appear();
    link = dialled(listener, fourth, trace);
    CHECK(trace.count("gap disconnect") == before + 1);
    fourth.hangUp();
    fourth.vanish();
    CHECK(endsWithin(link, 3000));
    delete link;
    pause(450);

    // Armed, and no link comes in its time: nothing is asked any more, and the next link is decided on.
    CHECK(listener.resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetArmed);
    CHECK_IN(trace.text(), sawWithin(trace, "serial.reset: no serial link came within 1 s to be ended at its open", 3000));
    Node fifth(dir + "/btspp", 4);
    gap.phone(kInfServiceRequest, kCarSerialService);
    fifth.appear();
    link = dialled(listener, fifth, trace);
    CHECK(trace.count("gap disconnect") == before + 1);
    fifth.hangUp();
    fifth.vanish();
    CHECK(endsWithin(link, 3000));
    delete link;
    pause(450);

    // Sent, and the link ends before ServiceDisconnect is to go: nothing goes to the car, the log and the problem
    // file say so, and the reset is done. ServiceDisconnect for a phone-opened link the car has no live link for can
    // hold that instance up for well over a minute.
    Node sixth(dir + "/btspp", 5);
    gap.phone(kInfServiceRequest, kCarSerialService);
    sixth.appear();
    link = dialled(listener, sixth, trace);
    CHECK(listener.resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetSent);
    sixth.hangUp();
    sixth.vanish();
    CHECK_IN(trace.text(), sawWithin(trace, "serial.reset: no serial node of the listener's is there any more, ", 2000));
    CHECK(trace.saw(" ms after it was asked for: the link has ended, so nothing is sent to the car"));
    pause(300);
    CHECK_IN(trace.text(), trace.count("gap disconnect") == before + 1 && !trace.saw("btspp5 went "));
    CHECK(endsWithin(link, 3000));
    delete link;
    double end = monoNow() + 3.0;
    while (testutil::readFile(file).find("nothing was sent") == std::string::npos && monoNow() < end) pause(10);
    CHECK_IN(testutil::readFile(file), testutil::readFile(file).find(": serial.reset: the link had ended before "
                                                                     "ServiceDisconnect was to go, so nothing was "
                                                                     "sent\n") != std::string::npos);
    pause(450);
    // The next serial.reset with a node open is sent as ever.
    Node seventh(dir + "/btspp", 6);
    gap.phone(kInfServiceRequest, kCarSerialService);
    seventh.appear();
    link = dialled(listener, seventh, trace);
    CHECK(listener.resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetSent);
    CHECK_IN(trace.text(), countWithin(trace, "gap disconnect", before + 2, 2000));
    seventh.hangUp();
    seventh.vanish();
    CHECK(endsWithin(link, 3000));
    delete link;

    // Nothing of the phone's in the log.
    CHECK(!trace.saw("Test Phone") && !trace.saw("AABBCCDDEEFF"));
    listener.stop();
    CHECK(listener.resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetFailed);

    {   // The listener gives up and is set up again while a link of the agent's that the phone opened is up: its node
        // tracker starts afresh then, with that node as someone else's, and serial.reset still ends the link, which
        // the agent reads.
        std::string again = freshFolder("reset-again");
        Trace t;
        LinksGone linksGone;
        FakeGap g(&t);
        FakeGate gt(&t);
        SerialConfig c = configFor(again);
        c.registerMs = 300;
        c.retryMs = 500;
        g.answersDisconnect = true;
        SerialListener l(&g, &gt, &t, c);
        CHECK_IN(why, l.start(&why));
        g.attach();
        CHECK_IN(t.text(), sawWithin(t, "the serial service 'NBTApp' is registered", 2000));
        Node node(again + "/btspp", 0);
        g.phone(kInfServiceRequest, kCarSerialService);
        node.appear();
        Link* up = dialled(l, node, t);
        g.detach();
        CHECK_IN(t.text(), sawWithin(t, "went away and has not come back: Bluetooth serial is off", 3000));
        CHECK_IN(t.text(), sawWithin(t, "setting Bluetooth serial up again", 3000));
        g.attach();
        CHECK_IN(t.text(), countWithin(t, "the serial service 'NBTApp' is registered", 2, 3000));
        exchange(node, up, "still", "noch");
        CHECK(l.resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetSent);
        CHECK_IN(t.text(), sawWithin(t, "serial.reset: ServiceDisconnect sent, ", 2000));
        CHECK(t.count("gap disconnect") == 1 && !t.saw("no serial node of the listener's"));
        node.hangUp();
        node.vanish();
        CHECK_IN(t.text(), sawWithin(t, "serial.reset: " + node.path + " went ", 2000));
        CHECK(endsWithin(up, 3000));
        delete up;
        l.stop();
    }
    {   // serial.reset while a link is decided on, and the link is let go to another program in the delay: nothing
        // goes to the car, which would end that program's link, and the log and the problem file say so truly
        std::string pdir = freshFolder("reset-left-problems");
        ProblemLog leftProblems(pdir + "/serial-problems.log", "1.0.0 (build test)");
        Letting lt("reset-left");
        lt.config.decideMs = 3000;
        lt.listener = new SerialListener(&lt.gap, &lt.gate, &lt.trace, lt.config);
        lt.services.setRemover(lt.listener);
        lt.listener->setServices(&lt.services);
        lt.listener->setProblemLog(&leftProblems);
        CHECK_IN(why, lt.listener->start(&why));
        lt.gap.attach();
        CHECK_IN(lt.trace.text(), sawWithin(lt.trace, "the serial service 'NBTApp' is registered", 2000));
        Node node(lt.dir + "/btspp", 0);
        node.held = true;
        lt.gap.phone(kInfServiceRequest, kCarSerialService);
        node.appear();
        CHECK_IN(lt.trace.text(), sawWithin(lt.trace, "btspp0 opened", 3000));
        CHECK(lt.listener->resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetSent);
        node.send("\x10\x20\x30\x40 another app's opening");
        CHECK_IN(lt.trace.text(), sawWithin(lt.trace, "btspp0 let go ", 2000));
        CHECK_IN(lt.trace.text(), sawWithin(lt.trace, "serial.reset: the link was let go to another program ", 2000));
        CHECK(lt.trace.saw(" ms after serial.reset was asked for, and is that program's now: nothing is sent to the "
                           "car, which would end that program's link"));
        CHECK(!lt.trace.saw("gap disconnect") && !lt.trace.saw("no serial node of the listener's"));
        std::string pfile = pdir + "/serial-problems.log";
        double until = monoNow() + 3.0;
        while (testutil::readFile(pfile).find("nothing was sent") == std::string::npos && monoNow() < until) pause(10);
        CHECK_IN(testutil::readFile(pfile), testutil::readFile(pfile).find(
                                                ": serial.reset: the link was let go to another program before "
                                                "ServiceDisconnect was to go, so nothing was sent\n") != std::string::npos);
        node.vanish();
        CHECK_IN(lt.trace.text(), sawWithin(lt.trace, "btspp0: the link ended ", 2000));
    }
}

// What nbtagent's dial options are made of, and what the agent's first log line says of the dial.
static void testDialOptions() {
    {   // --dial-poll F,L,T
        DialConfig c;
        CHECK(parseDialPoll("5,60,20", &c) && c.pollFirstMs == 5000 && c.pollFirstForMs == 60000 && c.pollLaterMs == 20000);
        CHECK(parseDialPoll("1,0,3600", &c) && c.pollFirstForMs == 0 && c.pollLaterMs == 3600000);
        CHECK(parseDialPoll("3600,86400,1", &c));
        const char* bad[] = {"", "5", "5,60", "5,60,20,1", "0,60,20", "5,60,0", "3601,60,20", "5,86401,20", "5,60,3601",
                             "5,,20", ",60,20", "5,60,", "5 ,60,20", "-5,60,20", "5,60,2a", "a"};
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            DialConfig d;
            CHECK_IN(bad[i], !parseDialPoll(bad[i], &d) && d.pollFirstMs == 10000);
        }
    }
    {   // --dial-knock on|off
        bool on = false;
        CHECK(parseDialKnock("on", &on) && on && parseDialKnock("off", &on) && !on);
        on = true;
        CHECK(!parseDialKnock("", &on) && !parseDialKnock(0, &on) && !parseDialKnock("On", &on) &&
              !parseDialKnock("1", &on) && !parseDialKnock("off ", &on) && on);
    }
    {   // the start-up line with the knock, the default: the values in effect, and no poll or ask
        DialConfig c;
        CHECK(c.on && c.knock);
        std::string line = describeDial(c);
        CHECK_IN(line, has(line, "when the phone's app knocks, and otherwise only while the pairing window is open, at "
                                 "serial.dial and while this version of the agent is on trial (--dial-knock on)"));
        CHECK_IN(line, has(line, "a link the phone opens that starts with the knock's magic bytes within the decide "
                                 "time is that phone's knock, and a phone's request for a serial link is no knock, "
                                 "whatever becomes of it.") &&
                           has(line, "waits 4000 ms at most") && has(line, "ignored for 3000 ms") &&
                           has(line, "No poll, and no ask of a phone's services") &&
                           has(line, "every phone there is dialled every 5 s") &&
                           has(line, "every phone of the agent's there every 5 s, for 120 s from when the trial is "
                                     "known, the serial service registered and the car's device list come; after "
                                     "that, until the version is confirmed, a phone is dialled when its app knocks") &&
                           has(line, "magic bytes are waited for 3000 ms") && has(line, "given up after 95 s") &&
                           has(line, "/dev/nbtapp") && has(line, "one re-point request goes on the same instance") &&
                           !has(line, "polls every") && !has(line, "02:00"));
        c.trialDialMs = 7000;
        c.trialDialForMs = 300000;
        c.knockQuietMs = 2500;
        CHECK(has(describeDial(c), "every phone of the agent's there every 7 s, for 300 s from when") &&
              has(describeDial(c), "an answer other than 5 within 500 ms stops these requests for the rest of the run") &&
              has(describeDial(c), "ignored for 2500 ms"));
    }
    {   // the start-up line says the values in effect
        DialConfig c;
        c.knock = false;
        std::string line = describeDial(c);
        CHECK_IN(line, has(line, "with no knock (--dial-knock off)"));
        CHECK_IN(line, has(line, "polls every 10 s for 120 s after such a phone connects or a link of the agent's ends, "
                                 "then every 30 s; at once when the phone asks for a link and no link of the agent's "
                                 "comes in 1000 ms; every phone there every 5 s while the pairing window is open"));
        CHECK_IN(line, has(line, "magic bytes are waited for 3000 ms") && has(line, "given up after 95 s") &&
                           has(line, "tried again after 15 s") && has(line, "for 30 s, twice as long each time up to "
                                                                             "300 s") &&
                           has(line, "has 5000 ms to be ended by the phone") && has(line, "waited for 3000 ms") &&
                           has(line, "/dev/nbtapp"));
        CHECK_IN(line, has(line, "or up to 1000 ms after it was answered, is not counted as a refusal") &&
                           has(line, "the car's device list is waited for 3000 ms") && has(line, "15 s at most in all") &&
                           has(line, "registered again at once if it has gone"));
        // An ask that misses holds nothing back; a dial that fails backs the phone off; three take its mark away.
        CHECK_IN(line, has(line, "one with no answer or an error is as one that does not name it and holds nothing back") &&
                           has(line, "a phone whose dial the car answered with another failure or did not answer") &&
                           has(line, "After 3 dials of a phone in a row that gave the agent no link") &&
                           !has(line, "three in a row"));
        // The re-point's times; never its address.
        CHECK_IN(line, has(line, "one re-point request goes on the same instance") &&
                           has(line, "its answer is waited for 3000 ms, with no ask or dial meanwhile") &&
                           has(line, "an answer other than 5 within 500 ms stops these requests") && !has(line, "02:00"));
        CHECK(parseDialPoll("5,60,20", &c));
        CHECK(has(describeDial(c), "polls every 5 s for 60 s after"));
        c.on = false;
        CHECK(has(describeDial(c), "the car does not dial phones (--dial off)"));
    }
}

static void testOptions() {
    unsigned ms = 7;
    CHECK(parseSerialDecideMs("1", &ms) && ms == 1);
    CHECK(parseSerialDecideMs("500", &ms) && ms == 500);
    CHECK(parseSerialDecideMs("0800", &ms) && ms == 800);
    CHECK(parseSerialDecideMs("10000", &ms) && ms == kSerialDecideMostMs);
    ms = 7;
    CHECK(!parseSerialDecideMs("0", &ms) && !parseSerialDecideMs("000", &ms) && !parseSerialDecideMs("10001", &ms));
    CHECK(!parseSerialDecideMs("", &ms) && !parseSerialDecideMs(0, &ms) && !parseSerialDecideMs("-5", &ms));
    CHECK(!parseSerialDecideMs("5 ", &ms) && !parseSerialDecideMs(" 5", &ms) && !parseSerialDecideMs("5ms", &ms));
    CHECK(!parseSerialDecideMs("0.5", &ms) && !parseSerialDecideMs("+5", &ms) && !parseSerialDecideMs("0x10", &ms));
    // More digits than a number holds: refused, and not wrapped round into the range.
    CHECK(!parseSerialDecideMs("4294967297", &ms) && !parseSerialDecideMs("99999999999999999999", &ms));
    CHECK(ms == 7);   // a value that is refused changes nothing
    // The protocol's own time is one the option takes.
    CHECK((unsigned)kSerialDecideMs >= 1 && (unsigned)kSerialDecideMs <= kSerialDecideMostMs);

    SerialConfig config;
    CHECK(config.decideMs == (unsigned)kSerialDecideMs);
    CHECK(config.nodeCalls.queued == 0 && config.nodeCalls.inject == 0);   // the platform's
    // With the knock, the default: every link a phone opens is read, and the knock's bytes are its knock.
    CHECK_IN(describeSerialDecide(config),
             describeSerialDecide(config) ==
                 "a link is the agent's if it opens with the magic bytes within 500 ms of the open of its node, after "
                 "any whole frames left over from an earlier link. Any other is let go. If a program declared for a "
                 "serial service is running, the gate has seen another program open the node, or a program of software "
                 "put on by hand is running, what was read from it is put back into its node, which stays open, unread, "
                 "until the link ends, for the program it belongs to; where that cannot be done, its node is closed for "
                 "that program. Otherwise it is read until the link ends or one of those signs comes. A link that opens "
                 "with the knock's magic bytes there instead is the phone's knock: nothing is written into it, the car "
                 "is to dial the phone that opened it, and its node is read, with what comes dropped, until the link "
                 "ends. Every link a phone opens is read so, whoever the phone is");
    config.dial.knock = false;
    CHECK(describeSerialDecide(config) ==
          "a link is the agent's if it opens with the magic bytes within 500 ms of the open of its node, after any "
          "whole frames left over from an earlier link. Any other is let go. If a program declared for a serial "
          "service is running, the gate has seen another program open the node, or a program of software put on by "
          "hand is running, what was read from it is put back into its node, which stays open, unread, until the "
          "link ends, for the program it belongs to; where that cannot be done, its node is closed for that program. "
          "Otherwise it is read until the link ends or one of those signs comes. A link that a phone of the agent's "
          "opens, whose app a link the car dialled has reached, is not read at all while a program declared for a "
          "serial service is running, or may be: it is left to that program, its node primed for it with one read "
          "that does not wait and kept open, unread, until the link ends, and the app is reached by the dial");
    config.decideMs = 1200;
    config.dial.on = false;
    CHECK(describeSerialDecide(config) ==
          "a link is the agent's if it opens with the magic bytes within 1200 ms (--decide; the protocol's time is "
          "500 ms) of the open of its node, after any whole frames left over from an earlier link. Any other is let "
          "go. If a program declared for a serial service is running, the gate has seen another program open the "
          "node, or a program of software put on by hand is running, what was read from it is put back into its "
          "node, which stays open, unread, until the link ends, for the program it belongs to; where that cannot be "
          "done, its node is closed for that program. Otherwise it is read until the link ends or one of those signs "
          "comes");
    config.dial.on = true;
    CHECK(config.lingerMs == (unsigned)kSerialLingerMs &&
          describeSerialLinger(config) == "a link the agent ends keeps its node, read and with what comes dropped, until "
                                          "the phone ends the link; the log says so if it is still up 5000 ms after the "
                                          "agent's end closed");

    CHECK(config.registerMs == 20000 && config.retryMs == 30000);
    CHECK(describeSerialRetry(config) ==
          "a serial service that the car's Bluetooth service does not take, or that is not registered for 20 s, is "
          "given up: the gate is taken away, and both are set up again 30 s later, for as long as it takes");
    config.retryMs = 0;
    CHECK(describeSerialRetry(config) ==
          "a serial service that the car's Bluetooth service does not take, or that is not registered for 20 s, is "
          "given up: the gate is taken away, and Bluetooth serial stays off until the agent is started again");

    {   // what the log says of a refusal
        RefusalFacts f;
        CHECK(describeRefusal(1, f) ==
              "the car refused the phone's link (1 so far). The car's device list could not be asked for. No serial "
              "node is there. Other causes, in the order to look at them: the iDrive Bluetooth pairing menu is open; "
              "the ignition is off; the phone's pairing with the car is broken or missing; the car's coding does not "
              "allow serial links for phones");
        f.list = RefusalFacts::kListRead;
        f.phone.ok = true;
        f.phone.known = true;
        f.phone.listed = true;
        f.phone.connection = 2;
        f.phone.security = 7;
        f.node = RefusalFacts::kNodeOther;
        CHECK(refusalNoMediaAudio(f));
        CHECK_IN(describeRefusal(20, f),
                 describeRefusal(20, f) ==
                     "the car refused the phone's link (20 so far). In the car's device list the phone is connected "
                     "(connection 2), security 7. So the car has no media audio connection to the phone, and the car "
                     "refuses the phone's own serial links while it has none, so the phone's app cannot knock "
                     "meanwhile: the driver can connect it again by choosing the phone in the car's media menu. A "
                     "serial node is there already and is not the agent's: the car "
                     "takes one phone-opened serial link at a time. Other causes, in the order to look at them: the "
                     "iDrive Bluetooth pairing menu is open; the ignition is off; the phone's pairing with the car is "
                     "broken or missing; the car's coding does not allow serial links for phones");
        // A service connected (its media audio): no such sentence.
        f.phone.connection = 3;
        CHECK(!refusalNoMediaAudio(f) && !has(describeRefusal(20, f), "media audio") &&
              has(describeRefusal(20, f), "the phone is service connected (connection 3), security 7. A serial node"));
        f.phone.connection = 2;
        f.phone.listed = false;
        f.node = RefusalFacts::kNodeOurs;
        CHECK(describeRefusal(40, f) ==
              "the car refused the phone's link (40 so far). The phone is not in the car's device list: its pairing "
              "with the car is missing. A serial link of the agent's is up already: the car takes one phone-opened "
              "serial link at a time. Other causes, in the order to look at them: the iDrive Bluetooth pairing menu is "
              "open; the ignition is off; the car's coding does not allow serial links for phones");
        f.phone.error = 5;
        CHECK(has(describeRefusal(1, f), "The car did not give its device list (5 DEFAULT). "));
        f.phone.error = 0;
        f.phone.known = false;
        CHECK(has(describeRefusal(1, f), "The refusal named no phone to look for in the car's device list. "));
        f.phone.ok = false;
        CHECK(has(describeRefusal(1, f), "The car's device list could not be read. "));
        f.list = RefusalFacts::kListNoAnswer;
        CHECK(has(describeRefusal(1, f), "The car's device list did not come in 3 s. "));
    }
    {   // the broker's line in the system's log, read in pieces
        BrokerLineFinder finder("SGBluetoothGap");
        std::string log = "Nov 18 10:00:00    3    18     0 something else\n"
                          "Nov 18 10:00:01    3    18     0 Notification set on BSS_GAP.SGBluetoothGap 35.9, version "
                          "available: 35.1\r\n"
                          "Nov 18 10:00:02    3    18     0 Notification set on BSS_GAP.SGOther 2.0, version available: 1.0\n" +
                          std::string(5000, 'x') + " SGBluetoothGap version available: a line too long\n"
                          "Nov 18 10:00:03    3    18     0 Notification set on BSS_GAP.SGBluetoothGap 36.0, version "
                          "available: 35.1";
        for (size_t i = 0; i < log.size(); i += 7) finder.feed(log.data() + i, log.size() - i < 7 ? log.size() - i : 7);
        CHECK_IN(finder.line(), finder.line() == "Nov 18 10:00:03    3    18     0 Notification set on "
                                                 "BSS_GAP.SGBluetoothGap 36.0, version available: 35.1");
        BrokerLineFinder none("SGBluetoothGap");
        none.feed("nothing here\n", 13);
        CHECK(none.line().empty() && none.bytes() == 13 && finder.bytes() == log.size());
    }
    {   // why no broker line came: how sloginfo ended, and how much of the log it gave
        ProcessResult r;
        CHECK(brokerLookWhy(false, "fork: no memory", r, 4, 0) == "sloginfo could not be started: fork: no memory");
        r.exited = true;
        r.exitCode = 127;
        CHECK(brokerLookWhy(true, "", r, 4, 30) == "sloginfo is not there (the shell's status 127)");
        r.exitCode = 126;
        CHECK(brokerLookWhy(true, "", r, 4, 30) == "sloginfo is there but could not be run (the shell's status 126)");
        r.exitCode = 1;
        CHECK(brokerLookWhy(true, "", r, 4, 300) == "sloginfo ended with status 1 after giving 300 B, with no line of the "
                                                    "service broker's about it");
        r.exitCode = 0;
        CHECK(brokerLookWhy(true, "", r, 4, 0) == "sloginfo ran and gave nothing: the system log is empty, or could not "
                                                  "be read");
        CHECK(brokerLookWhy(true, "", r, 4, 1023) == "sloginfo read 1023 B of the system log, with no line of the service "
                                                     "broker's about it");
        CHECK(brokerLookWhy(true, "", r, 4, 200 * 1024 + 100) == "sloginfo read 200 KB of the system log, with no line of "
                                                                "the service broker's about it");
        r.exited = false;
        r.signal = 9;
        CHECK(brokerLookWhy(true, "", r, 4, 2048) == "sloginfo was ended by signal 9 after giving 2 KB, with no line of "
                                                     "the service broker's about it");
        r.signal = 0;
        r.timedOut = true;
        CHECK(brokerLookWhy(true, "", r, 4, 5000) == "sloginfo did not end in 4 s; it had given 5 KB by then, with no "
                                                     "line of the service broker's about it");
    }

    unsigned s = 7;
    CHECK(parseSerialCheckS("0", &s) && s == 0);
    CHECK(parseSerialCheckS("30", &s) && s == 30);
    CHECK(parseSerialCheckS("3600", &s) && s == kSerialCheckMostS);
    s = 7;
    CHECK(!parseSerialCheckS("3601", &s) && !parseSerialCheckS("", &s) && !parseSerialCheckS(0, &s));
    CHECK(!parseSerialCheckS("-1", &s) && !parseSerialCheckS("30s", &s) && !parseSerialCheckS("99999999999", &s));
    CHECK(s == 7);
    config = SerialConfig();
    CHECK(config.checkMs == 30000 && config.checkQuietMs == 30000);
    CHECK(describeSerialCheck(config) ==
          "while the service is registered, no link of the agent's is up and no phone has asked for a link for 30 s, "
          "the registration is checked every 30 s by registering it again: switching Bluetooth off and on drops it, "
          "and the car says nothing");
    config.checkMs = 0;
    CHECK(describeSerialCheck(config) ==
          "the registration is not checked (--register-check 0): a registration that switching Bluetooth off and on "
          "drops stays lost until the agent is started again");

    std::vector<std::string> programs;
    CHECK(describeSerialHold(programs) ==
          "no program is declared for a serial service, so the gate holds none off a link that is being decided on "
          "or is the agent's");
    programs.push_back("otherd");
    CHECK(describeSerialHold(programs) ==
          "while a link is being decided on or is the agent's, the gate also refuses opens by otherd");
    programs.push_back("radiod");
    CHECK(describeSerialHold(programs) ==
          "while a link is being decided on or is the agent's, the gate also refuses opens by otherd, radiod");
}

// ---- the knock ----------------------------------------------------------------------------------------------------

// A listener with the knock on, as it is by default, the tests' short times and the test's calls on a node; phone
// kDialled, one of the agent's that a dialled link has reached before (so that without the knock the rule would leave
// its own links unread while a program declared for a serial service runs), connected to the car and paired with
// it; the dialled node, which comes up when the car's dial is answered 0. `other`: radiod is declared for a serial
// service, and runs.
struct Knocker {
    Trace trace;
    LinksGone gone;
    FakeGap gap;
    FakeGate gate;
    std::string dir;
    Machine machine;
    Store store;
    SerialServices services;
    PhoneAddressList ours;
    Node dialNode;
    SerialConfig config;
    SerialListener* listener;

    explicit Knocker(const std::string& name, bool other = false)
        : gap(&trace), gate(&trace), dir(freshFolder(name)), machine(dir + "/root"), store(&machine),
          services(&machine, &store, &trace), dialNode(dir + "/nbtapp"), listener(0) {
        config = configFor(dir);
        config.dial.on = true;
        config.dial.knock = true;
        config.dial.node = dir + "/nbtapp";
        config.dial.magicWaitMs = 400;
        config.dial.disconnectWaitMs = 500;
        config.dial.replyLimitMs = 1000;
        config.dial.knockQuietMs = 2000;
        config.dial.knockWaitMs = 1500;
        config.dial.trialDialMs = 700;
        config.dial.pairingRoundMs = 500;
        config.nodeCalls = heldCalls();
        config.readNode = &heldRead;
        gap.phones.push_back(carPhone(kDialled, kConnConnected, 7));
        gap.dialNode = &dialNode;
        ours.set(std::vector<std::string>(1, kDialled), std::vector<std::string>(1, kDialled));
        if (other) {
            services.setDeclared(std::vector<SerialServiceOwner>(1, declares("TestRadio", "radiod")));
            machine.running = "radiod";
        }
    }
    ~Knocker() {
        if (listener != 0) {
            listener->stop();
            services.setRemover(0);
            delete listener;
        }
        if (getenv("NBT_TEST_TRACE") != 0) fprintf(stderr, "---- %s\n%s", dir.c_str(), trace.text().c_str());
    }
    void start() {
        listener = new SerialListener(&gap, &gate, &trace, config);
        listener->setPhoneAddresses(&ours);
        services.setRemover(listener);
        listener->setServices(&services);
        std::string why;
        CHECK_IN(why, listener->start(&why));
        gap.attach();
        CHECK_IN(trace.text(), sawWithin(trace, "the car's device list: phone 1 is connected", 2000));
    }
    // The phone asks for a serial link; `refused`: and the car refuses it.
    void request(bool refused) {
        gap.phone(kInfServiceRequest, kCarSerialService, "001122334455");
        if (refused) gap.phone(kInfServiceRequestError, kCarSerialService, "001122334455");
    }
    // The phone opens a link on `node` that says `bytes` first.
    void opens(Node& node, const std::string& bytes) {
        request(false);
        node.appear();
        if (!bytes.empty()) node.say(bytes);
    }
};

static const std::string kKnock("NBTK");

static void testKnock() {
    {   // by marker: a phone-opened link with the knock's bytes. Nothing is written into it, it is let go, read and
        // dropped until the phone ends it, and the car dials that phone at once, with no ask
        Knocker k("knock-marker");
        k.start();
        pause(300);
        CHECK(k.gap.connects().empty() && !k.trace.saw("gap query"));   // nothing by itself: no poll, no ask
        k.dialNode.say(kMagic);   // the app listens, and says the magic bytes once it has the car's link
        Node knock(k.dir + "/btspp", 0);
        k.opens(knock, kKnock);
        CHECK_IN(k.trace.text(), sawWithin(k.trace, "btspp0: it opened with the knock's magic bytes ", 3000));
        CHECK_IN(k.trace.text(), k.trace.saw("phone 1's knock, and the car dials it as soon as nothing else of the "
                                             "agent's is under way. Nothing is written into the link"));
        Link* link = acceptWithin(*k.listener, 3000);
        CHECK_IN(k.trace.text(), link != 0 && link->dialledByCar() && link->phoneAddress() == kDialled);
        CHECK(readFrom(link, kMagic.size(), 3000) == kMagic);
        CHECK(k.trace.logged("phone 1 knocked by the knock's magic bytes, and the car dials it now"));
        CHECK(k.trace.logged("phone 1 (the phone knocked): the car's dial answered 0 OK in "));
        CHECK(dialsOfPhone(k.gap) == 1 && k.gap.connects().size() == 1 && !k.trace.saw("gap query"));
        bool closed = false;
        CHECK(knock.hear(1, 200, &closed).empty() && !closed);   // not a byte into the knock, and not closed
        knock.say("whatever the phone sends on it");
        SerialStatus st = k.listener->serialStatus();
        CHECK(st.knock && st.knocksByMarker == 1 && st.knockDials == 1 &&
              st.knockDialsFailed == 0 && st.dials == 1 && st.asks == 0 && st.link == "dialled" && st.leftUnread == 0);
        CHECK(!k.listener->phoneOpenedLeft(kDialled));
        // The app ends the knock's link: its node goes, and was never the agent's link or another program's.
        knock.hangUp();
        knock.vanish();
        CHECK_IN(k.trace.text(), sawWithin(k.trace, "btspp0: the link ended ", 3000) &&
                                     k.trace.saw("s after it was let go as a knock; "));
        CHECK(acceptWithin(*k.listener, 100) == 0);
        k.dialNode.vanish();
        k.dialNode.hangUp();
        k.gap.ended("nbtapp", kDialled, "NBTApp", 1);
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK(!k.trace.logged("00:11:22") && !k.trace.logged("001122") && !k.trace.logged("Test Phone"));
    }
    {   // a knock from a phone that is not one of the agent's (one it has forgotten, say), with the pairing window
        // closed: the car dials it, once, and the link is handed over as any dialled link
        Knocker k("knock-not-ours");
        k.ours.set(std::vector<std::string>());
        k.start();
        k.dialNode.say(kMagic);
        Node knock(k.dir + "/btspp", 0);
        k.opens(knock, kKnock);
        Link* link = acceptWithin(*k.listener, 3000);
        CHECK_IN(k.trace.text(), link != 0 && link->dialledByCar() && link->phoneAddress() == kDialled);
        CHECK(k.trace.logged("phone 1 knocked by the knock's magic bytes, and the car dials it now"));
        pause(300);
        CHECK(dialsOfPhone(k.gap) == 1 && k.gap.connects().size() == 1 && !k.trace.saw("gap query"));
        SerialStatus st = k.listener->serialStatus();
        CHECK(st.knocksByMarker == 1 && st.knockDials == 1 && st.knocksIgnoredNotDialable == 0 &&
              st.knocksIgnoredBusy == 0 && st.knocksIgnoredQuiet == 0 && st.phonesEligible == 0);
        knock.hangUp();
        knock.vanish();
        k.dialNode.vanish();
        k.dialNode.hangUp();
        k.gap.ended("nbtapp", kDialled, "NBTApp", 1);
        CHECK(endsWithin(link, 3000));
        delete link;
    }
    {   // a knock's dial answered BUSY with no node there: the line says that the phone's knocks are ignored for its
        // quiet time, as they are
        Knocker k("knock-busy");
        k.gap.connectError = 2;
        k.start();
        Node knock(k.dir + "/btspp", 0);
        k.opens(knock, kKnock);
        const std::string busy = "phone 1 (the phone knocked): the car's dial answered 2 BUSY in ";
        CHECK_IN(k.trace.text(), sawWithin(k.trace, busy, 3000));
        CHECK_IN(k.trace.text(), k.trace.logged(" ms: a link on the instance is up already, and its node is not there; "
                                                "its knocks are ignored for 2000 ms. The same answer again is only "
                                                "counted"));
        CHECK(!k.trace.logged("dialled again when its app knocks") && k.listener->serialStatus().knockDialsFailed == 1);
        Node second(k.dir + "/btspp", 1);
        k.opens(second, kKnock);
        CHECK_IN(k.trace.text(), sawWithin(k.trace, "phone 1's knock, ignored: it came in the phone's quiet time", 3000));
        CHECK(dialsOfPhone(k.gap) == 1);
        knock.vanish();
        second.vanish();
    }
    {   // a link left behind on the dialled instance when the agent starts: ended, and the line says that the phones
        // are dialled when their app knocks, which is all that comes: no dial by itself
        Knocker k("knock-stale-start");
        k.dialNode.appear();
        k.gap.answersDisconnect = true;
        k.start();
        CHECK(k.trace.logged("nbtapp exists already: a link an earlier agent left behind"));
        const std::string ended = "the link left behind on " + k.dialNode.path + " is ended: the phones are dialled "
                                  "when their app knocks";
        CHECK_IN(k.trace.text(), sawWithin(k.trace, ended, 4000));
        pause(500);
        CHECK(!k.trace.logged("the car dials the phones afresh") && k.gap.connects().empty());
    }
    {   // the magic bytes on a phone-opened link: the agent's, as ever, with no dial; and every link a phone opens is
        // read, though without the knock this phone's would be left unread for radiod. Other bytes: put back for
        // radiod, as without the knock
        Knocker k("knock-read", true);
        k.start();
        CHECK(!k.listener->phoneOpenedLeft(kDialled));
        Node mine(k.dir + "/btspp", 0);
        k.request(false);
        mine.appear();
        Link* link = dialled(*k.listener, mine, k.trace);
        CHECK(!link->dialledByCar() && k.trace.saw("btspp0: it opened with the magic bytes, so the link is the agent's"));
        CHECK(!k.trace.saw("left to another program") && k.listener->serialStatus().leftUnread == 0);
        mine.hangUp();
        CHECK(endsWithin(link, 3000));
        delete link;
        mine.vanish();
        pause(450);
        const std::string opening = "\x10\x20\x30\x40 another app's opening";
        Node other(k.dir + "/btspp", 1);
        other.held = true;
        k.request(false);
        other.send(opening);
        other.appear();
        CHECK_IN(k.trace.text(), sawWithin(k.trace, "btspp1 let go ", 3000));
        CHECK_IN(k.trace.text(), k.trace.saw(" B read from it were put back, in the order they came") &&
                                     k.listener->serialStatus().putBack == 1);
        CHECK(OtherProgram(other).reads(opening.size(), 2000) == opening);
        pause(300);
        CHECK(k.gap.connects().empty() && !k.trace.saw("gap query") && k.listener->serialStatus().knocksByMarker == 0);
        other.vanish();
    }
    {   // requests the car refuses bring no byte, and so no knock: five from one phone 250 ms apart, the app's own
        // gap, bring no dial, no ask and no knock count; the refusal's line says that the app cannot knock meanwhile
        Knocker k("knock-refused");
        k.start();
        k.dialNode.say(kMagic);
        double next = monoNow();
        for (int i = 0; i < 5; ++i) {
            while (monoNow() < next) pause(5);
            next += 0.25;
            k.request(true);
        }
        const std::string noAudio = "So the car has no media audio connection to the phone, and the car refuses the "
                                    "phone's own serial links while it has none, so the phone's app cannot knock "
                                    "meanwhile: the driver can connect it again by choosing the phone in the car's "
                                    "media menu. ";
        CHECK_IN(k.trace.text(), sawWithin(k.trace, noAudio, 3000));
        pause(1000);
        SerialStatus st = k.listener->serialStatus();
        CHECK_IN(k.trace.text(), k.gap.connects().empty() && !k.trace.saw("gap query") && st.asks == 0 &&
                                     st.dials == 0 && st.knockDials == 0 && st.knocksByMarker == 0);
        CHECK(st.knocksIgnoredQuiet == 0 && st.knocksIgnoredBusy == 0 &&
              st.knocksIgnoredNotDialable == 0 && st.refusals == 5 && st.refusalsNoAudio == 1);
        CHECK(!k.trace.logged("knocked") && acceptWithin(*k.listener, 100) == 0);
    }
    {   // a storm of requests from one phone, 400 to 700 ms apart, some taken and ended with no byte, some refused,
        // and nothing knocks: no dial and no ask at all
        Knocker k("knock-storm");
        k.start();
        const int gaps[] = {400, 550, 700, 450, 600, 500, 650, 400, 700, 550, 450, 600};
        const int n = (int)(sizeof gaps / sizeof gaps[0]);
        double next = monoNow();
        unsigned taken = 0;
        for (int i = 0; i < n; ++i) {
            while (monoNow() < next) pause(5);
            next += gaps[i] / 1000.0;
            if (i % 3 == 0) {
                Node node(k.dir + "/btspp", taken++);
                k.opens(node, std::string());
                CHECK_IN(k.trace.text(), sawWithin(k.trace, node.path + " let go ", 3000));
                node.hangUp();
                node.vanish();
            } else {
                k.request(true);
            }
        }
        while (monoNow() < next) pause(5);
        SerialStatus st = k.listener->serialStatus();
        CHECK_IN(k.trace.text(), k.gap.connects().empty() && !k.trace.saw("gap query") && st.asks == 0 &&
                                     st.dials == 0 && st.knockDials == 0 && st.knocksByMarker == 0);
        CHECK(st.refusals == (unsigned)(n - (int)taken) && !k.trace.logged("knocked"));
    }
    {   // a knock's dial that fails: the re-point, as for any failed dial, and the quiet time; a knock in it is
        // ignored; after it a knock is dialled again
        Knocker k("knock-failed");
        k.gap.connectError = 5;
        k.start();
        Node first(k.dir + "/btspp", 0);
        k.opens(first, kKnock);
        CHECK_IN(k.trace.text(), sawWithin(k.trace, kRepointItem, 3000));
        CHECK_IN(k.trace.text(), sawWithin(k.trace, "phone 1 (the phone knocked): the car's dial answered 5 DEFAULT in ", 2000));
        CHECK(k.trace.logged("its app is not listening on the agent's service, or something else went wrong; its "
                             "knocks are ignored for ") && !k.trace.logged("by a poll"));
        // The line about the doubted record is longer than the first buffer a line is made in: it is logged whole.
        const std::string doubt =
            "log: phone 1: the car's dial came to 5 DEFAULT. Such a dial may leave the car's record of phone-opened "
            "serial links wrong, and the car may then refuse such links, another program's too, after one of them "
            "ends. That record may stay wrong; the agent has sent the car one re-point request, so that the end of "
            "such a link that the car books against the agent's instance is not counted against the phone's connected "
            "services; serial.status says the record is doubted until a dial is answered 0, the car says that such a "
            "link ended, or its Bluetooth service goes";
        CHECK_IN(k.trace.text(), sawWithin(k.trace, "the car says that such a link ended, or its Bluetooth", 2000));
        CHECK_IN(k.trace.text(), doubt.size() > 512 + 5 && k.trace.exactly(doubt) == 1);
        Node second(k.dir + "/btspp", 1);
        k.opens(second, kKnock);
        CHECK_IN(k.trace.text(), sawWithin(k.trace, "btspp1: it opened with the knock's magic bytes ", 3000) &&
                                     k.trace.saw("phone 1's knock, ignored: it came in the phone's quiet time"));
        pause(300);
        CHECK(dialsOfPhone(k.gap) == 1);
        SerialStatus st = k.listener->serialStatus();
        CHECK(st.knockDials == 1 && st.knockDialsFailed == 1 && st.knocksIgnoredQuiet == 1 && st.knocksByMarker == 2 &&
              st.repoints == 1 && st.recordDoubtful);
        // Its quiet time over, the next knock is dialled; the app listens now.
        pause(2000);
        k.gap.connectError = 0;
        k.dialNode.say(kMagic);
        Node third(k.dir + "/btspp", 2);
        k.opens(third, kKnock);
        CHECK_IN(k.trace.text(), dialsWithin(k.gap, 2, 3000));
        Link* link = acceptWithin(*k.listener, 3000);
        CHECK_IN(k.trace.text(), link != 0 && link->dialledByCar());
        CHECK(k.listener->serialStatus().knockDials == 2 && k.listener->serialStatus().knockDialsFailed == 1);
        k.dialNode.vanish();
        k.dialNode.hangUp();
        k.gap.ended("nbtapp", kDialled, "NBTApp", 1);
        CHECK(endsWithin(link, 3000));
        delete link;
        first.vanish();
        second.vanish();
        third.vanish();
    }
    {   // the pairing window's rounds: every phone there in turn, the agent's or not, with no ask
        Knocker k("knock-pairing");
        k.ours.set(std::vector<std::string>());
        k.gap.connectError = 5;
        k.start();
        pause(300);
        CHECK(k.gap.connects().empty());
        k.listener->pairingWindow(true);
        CHECK_IN(k.trace.text(), dialsWithin(k.gap, 2, 2500));
        CHECK(k.trace.logged("phone 1 (pairing window): the car's dial answered 5 DEFAULT") && !k.trace.saw("gap query"));
        k.listener->pairingWindow(false);
        pause(100);
        unsigned n = dialsOfPhone(k.gap);
        pause(1000);
        CHECK(dialsOfPhone(k.gap) == n && k.listener->serialStatus().knockDials == 0);
    }
    {   // on trial: the agent's phones that are there, every trialDialMs, with no ask, until the version is confirmed
        Knocker k("knock-trial");
        k.gap.connectError = 5;
        k.start();
        pause(300);
        CHECK(k.gap.connects().empty());
        k.listener->agentOnTrial(true);
        CHECK(k.trace.logged("this version of the agent is on trial: the car dials the agent's phones that are there, "
                             "every 700 ms while no link of the agent's is up"));
        CHECK_IN(k.trace.text(), dialsWithin(k.gap, 2, 2500));
        CHECK(k.trace.logged("phone 1 (the agent is on trial): the car's dial answered 5 DEFAULT") &&
              !k.trace.saw("gap query"));
        k.listener->agentOnTrial(false);
        CHECK(k.trace.logged("this version of the agent is confirmed"));
        pause(100);
        unsigned n = dialsOfPhone(k.gap);
        pause(1500);
        CHECK(dialsOfPhone(k.gap) == n);
    }
    {   // on trial for trialDialForMs only: dials that fail until then, one line when the time is up and no trial dial
        // after it, a knock dialled as at any other time; a new trial has its own time
        Knocker k("knock-trial-time");
        k.config.dial.trialDialForMs = 2000;
        // Not marked `dialled`: the failed dials then hand nothing to the agent (dialFailed()), and the knock's link
        // is the first thing the listener hands over.
        k.ours.set(std::vector<std::string>(1, kDialled));
        k.gap.connectError = 5;
        k.start();
        k.listener->agentOnTrial(true);
        CHECK(k.trace.logged("this version of the agent is on trial: the car dials the agent's phones that are there, "
                             "every 700 ms while no link of the agent's is up, for 2 s from when the serial service is "
                             "registered and the car's device list has come, or until it is confirmed if that is "
                             "sooner"));
        const std::string over = "this version of the agent is still on trial after 2 s of the trial's dials: the car no "
                                 "longer dials the agent's phones by itself for this trial, and dials a phone when its "
                                 "app knocks";
        CHECK_IN(k.trace.text(), sawWithin(k.trace, over, 6000));
        CHECK_IN(k.trace.text(), dialsOfPhone(k.gap) >= 2 &&
                                     k.trace.logged("phone 1 (the agent is on trial): the car's dial answered 5 DEFAULT"));
        unsigned n = dialsOfPhone(k.gap);
        pause(1500);
        CHECK(dialsOfPhone(k.gap) == n && k.trace.loggedCount(over) == 1);
        // Still on trial: the phone's knock is dialled.
        k.gap.connectError = 0;
        k.dialNode.say(kMagic);
        Node knock(k.dir + "/btspp", 0);
        k.opens(knock, kKnock);
        Link* link = acceptWithin(*k.listener, 3000);
        CHECK_IN(k.trace.text(), link != 0 && link->dialledByCar() &&
                                     k.trace.logged("phone 1 knocked by the knock's magic bytes, and the car dials it now"));
        CHECK(dialsOfPhone(k.gap) == n + 1);
        knock.hangUp();
        knock.vanish();
        k.dialNode.vanish();
        k.dialNode.hangUp();
        k.gap.ended("nbtapp", kDialled, "NBTApp", 1);
        CHECK(endsWithin(link, 3000));
        delete link;
        // Confirmed, and then a new trial: its own time, with trial dials again, and the line again once it is up.
        k.gap.connectError = 5;
        k.listener->agentOnTrial(false);
        k.listener->agentOnTrial(true);
        CHECK_IN(k.trace.text(), dialsWithin(k.gap, n + 2, 3000) && countWithin(k.trace, over, 2, 6000));
        CHECK(k.trace.loggedCount(over) == 2);
        CHECK(!k.trace.logged("00:11:22") && !k.trace.logged("001122"));
    }
    {   // on trial, while a link of the agent's is up: no trial dial
        Knocker k("knock-trial-up");
        k.start();
        k.dialNode.say(kMagic);
        k.listener->agentOnTrial(true);
        Link* link = acceptWithin(*k.listener, 3000);
        CHECK_IN(k.trace.text(), link != 0 && link->dialledByCar());
        pause(1600);
        CHECK(dialsOfPhone(k.gap) == 1);
        k.dialNode.vanish();
        k.dialNode.hangUp();
        k.gap.ended("nbtapp", kDialled, "NBTApp", 1);
        CHECK(endsWithin(link, 3000));
        delete link;
    }
    {   // --dial-knock off: the knock's bytes are no knock, and the link is let go as any other
        Knocker k("knock-off");
        k.config.dial.knock = false;
        k.ours.set(std::vector<std::string>());   // no poll dials it
        k.start();
        Node node(k.dir + "/btspp", 0);
        k.opens(node, kKnock);
        CHECK_IN(k.trace.text(), sawWithin(k.trace, "btspp0 let go ", 3000));
        CHECK(!k.trace.saw("knock's") && !k.trace.saw("knocked") && !k.listener->serialStatus().knock &&
              dialsOfPhone(k.gap) == 0);
        node.vanish();
    }
    {   // the agent behind it lists the knock's capability in hello, with the dial's
        Car car;
        car.listener.stop();
        SerialConfig config = configFor(car.dir);
        config.dial.on = true;
        config.dial.knock = true;
        config.dial.node = car.dir + "/nbtapp";
        Node node(config.dial.node);
        car.gap.dialNode = &node;
        car.gap.phones.push_back(carPhone(kDialled, kConnConnected, 7));
        SerialListener listener(&car.gap, &car.gate, &car.trace, config);
        PhoneAddressList* ours = car.agent.phoneAddresses();
        ours->set(std::vector<std::string>(1, kDialled));
        listener.setPhoneAddresses(ours);
        car.agent.addListener(&listener);
        car.agent.setSerialDial(&listener);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        car.gap.attach();
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "the car's device list: phone 1 is connected", 3000));
        Node knock(car.dir + "/btspp", 0);
        car.gap.phone(kInfServiceRequest, kCarSerialService, "001122334455");
        knock.appear();
        knock.say(kKnock);
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "gap connect BtCtrl nbtapp 00:11:22:33:44:55 NBTApp", 3000));
        PhoneApp app(node);
        CHECK(car.sessionsWithin(1, 3000));
        app.hello("1");
        std::vector<PhoneApp*> apps(1, &app);
        Answered answered(&app);
        CHECK_IN(car.trace.text(), turnsUntil(car.agent, apps, answered, 3000));
        CHECK_IN(app.answers.caps, has(app.answers.caps + " ", " serial-dial ") &&
                                       has(app.answers.caps + " ", " serial-knock "));
        app.peer->close("done");
        node.vanish();
        car.gap.ended("nbtapp", kDialled, "NBTApp", 1);
        CHECK(car.sessionsWithin(0, 3000));
        knock.hangUp();
        knock.vanish();
        car.agent.setSerialDial(0);
        listener.stop();
        CHECK(waitForSerialLinks(3000));
    }
    {   // the knock of a phone that the agent holds no pairing for: the car dials it, the agent takes the link and
        // refuses `auth` on it as on a link the phone opens, and the link ends; nothing of that holds the phone's
        // knocks back: the next is dialled at once
        Car car;
        car.listener.stop();
        SerialConfig config = configFor(car.dir);
        config.dial.on = true;
        config.dial.knock = true;
        config.dial.node = car.dir + "/nbtapp";
        Node node(config.dial.node);
        car.gap.dialNode = &node;
        car.gap.phones.push_back(carPhone(kDialled, kConnConnected, 7));
        SerialListener listener(&car.gap, &car.gate, &car.trace, config);
        listener.setPhoneAddresses(car.agent.phoneAddresses());   // the agent has no phones
        car.agent.addListener(&listener);
        car.agent.setSerialDial(&listener);
        std::string why;
        CHECK_IN(why, listener.start(&why));
        car.gap.attach();
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "the car's device list: phone 1 is connected", 3000));
        Node knock(car.dir + "/btspp", 0);
        car.gap.phone(kInfServiceRequest, kCarSerialService, "001122334455");
        knock.appear();
        knock.say(kKnock);
        CHECK_IN(car.trace.text(), dialsWithin(car.gap, 1, 3000));
        {
            PhoneApp app(node);
            CHECK(car.sessionsWithin(1, 3000));
            app.hello("3");
            std::vector<PhoneApp*> apps(1, &app);
            Answered answered(&app);
            CHECK_IN(car.trace.text(), turnsUntil(car.agent, apps, answered, 3000));
            CHECK(app.answers.got[app.helloId] == "ok role=agent maxFrame=4096");
            int64_t id = app.peer->request("auth", "{\"nonce\":\"" + std::string(32, '0') + "\"}");
            app.helloId = id;   // what Answered waits for
            CHECK_IN(car.trace.text(), turnsUntil(car.agent, apps, answered, 3000));
            CHECK(app.answers.got[id] == "auth-failed");
            CHECK(car.sessionsWithin(0, 3000));
            app.peer->close("auth failed");
        }
        node.vanish();
        car.gap.ended("nbtapp", kDialled, "NBTApp", 1);
        CHECK_IN(car.trace.text(), sawWithin(car.trace, "phone 1: the dialled link has ended", 3000));
        knock.hangUp();
        knock.vanish();
        SerialStatus st = listener.serialStatus();
        CHECK(st.knockDials == 1 && st.knockDialsFailed == 0 && st.notTheApp == 0);
        // The next knock: dialled at once, with no quiet time and no back-off.
        Node again(config.dial.node);
        car.gap.dialNode = &again;
        Node second(car.dir + "/btspp", 1);
        car.gap.phone(kInfServiceRequest, kCarSerialService, "001122334455");
        second.appear();
        second.say(kKnock);
        CHECK_IN(car.trace.text(), dialsWithin(car.gap, 2, 3000));
        CHECK(car.trace.loggedCount("phone 1 knocked by the knock's magic bytes, and the car dials it now") == 2);
        CHECK(listener.serialStatus().knockDials == 2 && listener.serialStatus().knocksIgnoredQuiet == 0);
        again.vanish();
        car.gap.ended("nbtapp", kDialled, "NBTApp", 1);
        second.hangUp();
        second.vanish();
        car.agent.setSerialDial(0);
        listener.stop();
        CHECK(waitForSerialLinks(3000));
    }
}

// The gate's hold with a program declared for a serial service (radiod): a phone's knock keeps it on from the
// phone's request until the phone ends the knock's link, with no time limit, whatever the car's dialled link does.
static void testKnockHold() {
    const std::string on = "gate: opens by radiod are refused as well from now on ";
    const std::string off = "gate: opens by radiod are passed on again ";
    const std::string ended = off + "(the phone's knock link has ended)";
    {   // a knock, the car's dialled link coming up while the knock's link is up, and a second knock right after
        Knocker k("knock-hold", true);
        k.start();
        CHECK(!k.gate.refuses("radiod") && !k.trace.saw("gate hold"));
        k.dialNode.say(kMagic);
        Node knock(k.dir + "/btspp", 0);
        k.opens(knock, kKnock);
        CHECK_IN(k.trace.text(), sawWithin(k.trace, on + "(a phone has asked for a link)", 2000));
        CHECK_IN(k.trace.text(), sawWithin(k.trace, "btspp0: it opened with the knock's magic bytes ", 3000));
        // The car's dialled link comes up while the knock's is up: the hold stays on.
        Link* link = acceptWithin(*k.listener, 3000);
        CHECK_IN(k.trace.text(), link != 0 && link->dialledByCar());
        CHECK(readFrom(link, kMagic.size(), 3000) == kMagic);
        // Well past the wait for a request's link: no time limit while the knock's link is up.
        pause(1500);
        CHECK_IN(k.trace.text(), k.gate.refuses("radiod") && k.trace.count("gate hold") == 1 &&
                                     k.trace.count("gate release") == 0 && !k.trace.logged("are passed on again"));
        bool closed = false;
        CHECK(knock.hear(1, 200, &closed).empty() && !closed);   // nothing came into the knock's link
        // The phone ends the knock's link: the hold goes off, though the dialled link stays up.
        knock.hangUp();
        knock.vanish();
        CHECK_IN(k.trace.text(), sawWithin(k.trace, ended, 1500));
        CHECK(!k.gate.refuses("radiod") && k.trace.count("gate release") == 1 &&
              k.trace.at("btspp0: the link ended ") < k.trace.at("gate release"));
        exchange(k.dialNode, link, "still up", "noch da");
        CHECK(k.trace.count("gate hold") == 1 && k.trace.count("gate release") == 1);

        // A second knock right after: held again throughout, and off again at its end.
        Node second(k.dir + "/btspp", 1);
        k.opens(second, kKnock);
        CHECK_IN(k.trace.text(), sawWithin(k.trace, "btspp1: it opened with the knock's magic bytes ", 3000));
        pause(700);
        CHECK_IN(k.trace.text(), k.gate.refuses("radiod") && k.trace.count("gate hold") == 2 &&
                                     k.trace.count("gate release") == 1 && k.trace.loggedCount(off) == 1);
        second.hangUp();
        second.vanish();
        CHECK_IN(k.trace.text(), countWithin(k.trace, ended, 2, 1500));
        pause(500);
        CHECK(!k.gate.refuses("radiod") && k.trace.count("gate hold") == 2 && k.trace.count("gate release") == 2);
        // Nothing of the two is left in the hold: a request the car refuses has it on and off again.
        k.request(true);
        CHECK_IN(k.trace.text(), sawWithin(k.trace, off + "(the car refused the phone's link)", 2000));
        CHECK(!k.gate.refuses("radiod") && k.trace.count("gate hold") == 3 && k.trace.count("gate release") == 3);

        k.dialNode.vanish();
        k.dialNode.hangUp();
        k.gap.ended("nbtapp", kDialled, "NBTApp", 1);
        CHECK(endsWithin(link, 3000));
        delete link;
        CHECK(!k.trace.logged("00:11:22") && !k.trace.logged("001122"));
    }
    {   // the listener stops with a knock's link up: the hold goes off, as for a link of the agent's, and then the gate
        Knocker k("knock-hold-stop", true);
        k.gap.phones[0] = carPhone(kDialled, kConnConnected, 0);   // not paired with the car: its knock brings no dial
        k.start();
        Node knock(k.dir + "/btspp", 0);
        k.opens(knock, kKnock);
        CHECK_IN(k.trace.text(), sawWithin(k.trace, "btspp0: it opened with the knock's magic bytes ", 3000));
        pause(300);
        CHECK(k.gate.refuses("radiod") && k.trace.count("gate release") == 0 && dialsOfPhone(k.gap) == 0);
        k.listener->stop();
        CHECK_IN(k.trace.text(), !k.gate.refuses("radiod") && k.trace.count("gate release") == 1 &&
                                     k.trace.saw("gate detach"));
        CHECK(k.trace.saw("btspp0: the link was still up ") &&
              k.trace.saw("s after it was let go as a knock, and its node is closed as the listener stops"));
        CHECK(k.trace.at("gate release") < k.trace.at("gate detach") && !k.trace.logged(ended));
        knock.hangUp();
        knock.vanish();
    }
}

int main(int argc, char** argv) {
    if (argc > 1) gWork = argv[1];
    // As the agent has it: a link whose other side has gone shows as an error, not as a signal.
    signal(SIGPIPE, SIG_IGN);
    testOptions();
    testDialOptions();
    testReader();   // first: the one line of the process on whether a read came back on time is its
    testPrimer();
    testLink();
    testListener();
    testOtherServices();
    testRegistrationCheck();
    testPresence();
    testDial();
    testDialAsk();
    testRepoint();
    testDialGivenUp();
    testRefusal();
    testAgent();
    testAgentDials();
    testLeftBehind();
    testDecide();
    testPutBack();
    testLeftOvers();
    testReset();
    testHold();
    testLeftUnread();
    testPhoneOpenedLeft();
    testUndialled();
    testKnock();
    testKnockHold();
    printf("test_serial: ok\n");
    return 0;
}
