// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SPP_SERVICE_HPP
#define NBT_AGENT_SPP_SERVICE_HPP

// A Bluetooth serial (SPP) service of our own on the car: its registration with the Bluetooth stack, and the
// stack's word that a phone has asked for a serial link.
//
// The stack's server "BSS_GAP.SGBluetoothGap" takes a request SppUuidRegister(name, UUID, UUID, type) and adds an
// SDP record for the UUID, which a phone app then connects to. Nothing is written to disk: a reboot clears the
// registration, and so does a restart of the stack's process. A program that ends without SppUuidDeregister leaves
// the registration behind until then, with nothing answering it.
//
// Three layers, each usable without the one below:
//   * pure helpers (no devices, plain C++98): the UUID and name checks, the payloads of the two requests, readers
//     for the replies and informs
//   * GapClient: the server as its client sees it. DsiGapClient is the real one, for the head unit only (its library
//     is the head unit's). A test puts its own in its place
//   * SppService: the registration itself, on any GapClient
//
// DSI payload encoding (little-endian, every item 4-byte aligned from the payload start):
//   u32      4 bytes
//   string   u32 length INCLUDING the NUL, the bytes, zero padding to 4

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "dsi_client.hpp"

namespace nbt {
namespace btspp {

// The Bluetooth stack's DSI server "BSS_GAP.SGBluetoothGap".
const char* const kGapInterface = "SGBluetoothGap";
const char* const kGapRole = "BSS_GAP";
const int kGapMajor = 35;
const int kGapMinor = 0;
const uint32_t kReqSppUuidRegister = 0x2e;           // name, service UUID, connect UUID, u32 type
const uint32_t kReqSppUuidDeregister = 0x2f;         // name
const uint32_t kRspSppUuidRegister = 0x80000043;     // u32 GapErrorCode
const uint32_t kRspSppUuidDeregister = 0x80000044;   // u32 GapErrorCode
// Informs about a link the PHONE opens to one of the car's services, each naming the service. The request comes
// before the car accepts the link and the /dev/btsppN node appears; a link the car opens itself has none.
const uint32_t kInfServiceRequest = 0x80000009;          // name, address, service
const uint32_t kInfServiceRequestComplete = 0x8000000a;  // address, service
const uint32_t kInfServiceRequestError = 0x8000000b;     // address, service
const uint32_t kTypeResponse = 0x200;
const uint32_t kTypeRejectedA = 0x202, kTypeRejectedB = 0x204;   // the stub or server refused the request

// ---- UUID and name -------------------------------------------------------------------------------------------

inline bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// 8-4-4-4-12 hex digits; *out is the upper-case form the stack is given.
inline bool normaliseUuid(const std::string& in, std::string* out) {
    if (in.size() != 36) return false;
    std::string u;
    for (size_t i = 0; i < 36; ++i) {
        char c = in[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else {
            if (!isHex(c)) return false;
            if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
        }
        u += c;
    }
    if (out) *out = u;
    return true;
}

// xxxxxxxx-0000-1000-8000-00805F9B34FB: a Bluetooth SIG profile UUID (Serial Port is 00001101-...). Those belong to
// the car's own services; registering one here could collide with them. Takes a normalised UUID.
inline bool isSigBaseUuid(const std::string& uuid) {
    return uuid.size() == 36 && uuid.compare(8, 28, "-0000-1000-8000-00805F9B34FB") == 0;
}

// 1..32 of [A-Za-z0-9_-]: the name is the key the stack files the registration under.
inline bool isServiceName(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// ---- request payloads ----------------------------------------------------------------------------------------

inline void putU32(std::vector<uint8_t>& v, uint32_t x) {
    while (v.size() % 4) v.push_back(0);
    v.push_back((uint8_t)(x & 0xff));
    v.push_back((uint8_t)((x >> 8) & 0xff));
    v.push_back((uint8_t)((x >> 16) & 0xff));
    v.push_back((uint8_t)((x >> 24) & 0xff));
}

inline void putString(std::vector<uint8_t>& v, const std::string& s) {
    putU32(v, (uint32_t)s.size() + 1);
    v.insert(v.end(), s.begin(), s.end());
    v.push_back(0);
    while (v.size() % 4) v.push_back(0);
}

// type 0 is what a phone-opened link needs (the value in use on cars; the other value of the stack's
// [DevB, DevA] pair is untried).
inline std::vector<uint8_t> buildRegister(const std::string& name, const std::string& uuid,
                                          const std::string& connectUuid, uint32_t type) {
    std::vector<uint8_t> p;
    putString(p, name);
    putString(p, uuid);
    putString(p, connectUuid);
    putU32(p, type);
    return p;
}

inline std::vector<uint8_t> buildDeregister(const std::string& name) {
    std::vector<uint8_t> p;
    putString(p, name);
    return p;
}

// ---- replies and informs -------------------------------------------------------------------------------------

class Reader {
public:
    Reader(const uint8_t* p, uint32_t n) : p_(p), n_(p ? n : 0), off_(0), ok_(true) {}
    bool ok() const { return ok_; }
    uint32_t left() const { return n_ - off_; }

    uint32_t u32() {
        if (!ok_ || left() < 4) { ok_ = false; return 0; }
        uint32_t v = (uint32_t)p_[off_] | ((uint32_t)p_[off_ + 1] << 8) | ((uint32_t)p_[off_ + 2] << 16) |
                     ((uint32_t)p_[off_ + 3] << 24);
        off_ += 4;
        return v;
    }
    std::string str() {
        uint32_t len = u32();
        if (!ok_) return std::string();
        uint32_t padded = (len + 3u) & ~3u;
        if (len > 4096 || padded > left()) { ok_ = false; return std::string(); }
        std::string s;
        for (uint32_t i = 0; i < len && p_[off_ + i]; ++i) s += (char)p_[off_ + i];
        off_ += padded;
        return s;
    }

private:
    const uint8_t* p_;
    uint32_t n_, off_;
    bool ok_;
};

const uint32_t kNoError = 0xffffffffu;   // the reply held no error code

inline uint32_t readError(const uint8_t* p, uint32_t n) {
    Reader r(p, n);
    uint32_t e = r.u32();
    return r.ok() ? e : kNoError;
}

// GapErrorCode. DEFAULT is what the stack returns when the name or UUID is already registered.
inline const char* gapErrorName(uint32_t e) {
    switch (e) {
    case 0: return "OK";
    case 1: return "TIMEOUT";
    case 2: return "BUSY";
    case 3: return "STATE";
    case 4: return "REFUSED";
    case 5: return "DEFAULT (name or UUID already registered?)";
    case 6: return "INVALID_PARAM";
    default: return "FAILED";
    }
}

struct ServiceRequest {
    std::string name;      // the phone's name (ServiceRequest only)
    std::string address;   // the phone's Bluetooth address
    std::string service;   // the service it asked for
    bool ok;               // the payload parsed completely
};

inline bool isServiceRequestInform(uint32_t id) {
    return id == kInfServiceRequest || id == kInfServiceRequestComplete || id == kInfServiceRequestError;
}

inline ServiceRequest parseServiceRequest(uint32_t id, const uint8_t* p, uint32_t n) {
    ServiceRequest q;
    Reader r(p, n);
    if (id == kInfServiceRequest) q.name = r.str();
    q.address = r.str();
    q.service = r.str();
    q.ok = r.ok();
    return q;
}

// ---- the Bluetooth server, as its client sees it ---------------------------------------------------------------

class GapClient {
public:
    // What the server says. All of it arrives on a thread of the client's own, never on the caller's.
    class Events {
    public:
        virtual ~Events() {}
        // The server has answered: requests can be sent from here on. Again each time it comes back.
        virtual void gapAttached() = 0;
        // The server's process stopped or restarted.
        virtual void gapDetached() = 0;
        // A reply or an inform. `type` is kTypeResponse or a rejection for a reply.
        virtual void gapMessage(uint32_t type, uint32_t id, const uint8_t* payload, uint32_t len) = 0;
    };

    virtual ~GapClient() {}

    // Starts connecting, once; gapAttached() follows when the server answers. False if it cannot start at all.
    virtual bool open(Events* events) = 0;
    virtual bool attached() const = 0;
    // False if the server is not attached. From any thread, and from inside an event.
    virtual bool request(uint32_t id, const std::vector<uint8_t>& payload) = 0;
    // Asks for one of the server's informs. The same rules as request().
    virtual bool subscribe(uint32_t informId) = 0;
    // For the log: the server's name and the version of its interface.
    virtual std::string describe() const = 0;
};

// The real one: the head unit's own library, through DsiClient. On any other system open() fails and says why.
// It lives as long as its process: the library's thread goes on calling it.
class DsiGapClient : public GapClient {
public:
    DsiGapClient();
    virtual ~DsiGapClient();

    // Before open(): the interface version the service broker is asked for (the defaults are kGapMajor and
    // kGapMinor), and whether every message is logged.
    void configure(int verMajor, int verMinor, bool verbose);
    // Before open(): where the client's own lines go (DsiClient::setLog). Without it, to standard output.
    void setLog(DsiClient::LogFn log, void* user) { dsi_.setLog(log, user); }

    virtual bool open(Events* events);
    virtual bool attached() const { return dsi_.attached(); }
    virtual bool request(uint32_t id, const std::vector<uint8_t>& payload);
    virtual bool subscribe(uint32_t informId);
    virtual std::string describe() const;

private:
    DsiGapClient(const DsiGapClient&);
    DsiGapClient& operator=(const DsiGapClient&);
    static void onAttached(DsiClient* c, void* self);
    static void onData(DsiClient* c, uint32_t type, uint32_t id, uint32_t seq, const uint8_t* payload, uint32_t len,
                       void* self);
    static void onDetached(DsiClient* c, void* self);

    DsiClient dsi_;
    Events* events_;
    int verMajor_, verMinor_;   // not "major" / "minor": those are macros in the QNX headers
    pthread_mutex_t tx_;        // one send at a time: the library's thread and the caller's
};

// ---- the registration ------------------------------------------------------------------------------------------
// start() registers the service as soon as the server is attached, and again each time the server comes back,
// because its restart drops the registration. A first refusal is taken for a registration that an earlier run
// left behind under the same name: that one is removed and the registration tried once more. remove() takes the
// service away again; after it a server that comes back is not given the service a second time.
//
// The server's requests for a link do not say which serial node the link gets, and name the service the phone
// asked for. Which link is whose is for the caller to work out.
const double kOtherRemovalReplyS = 5.0;   // see SppService::setOtherReplyTime()

class SppService : private GapClient::Events {
public:
    enum State {
        kIdle,           // not started
        kWaiting,        // started, and the server is not attached: not yet, or it went away
        kRegSent,        // first SppUuidRegister
        kClearSent,      // it failed: SppUuidDeregister to clear a registration left behind
        kRetrySent,      // second SppUuidRegister
        kRegistered,
        kFailed,         // the server did not take the registration
        kRemoveSent,     // SppUuidDeregister by remove() or startRemoval()
        kRemoved,
        kRemoveFailed
    };

    // What a phone's request for a serial link came to.
    enum Phone {
        kPhoneAsked,     // it asked; if the car accepts, the link's node appears some tens of milliseconds later
        kPhoneLinked,    // the car accepted. Follows kPhoneAsked for the same link, sometimes after the node is there
        kPhoneRefused    // the car refused: no link comes of the request
    };

    // On the client's thread, except what start(), startRemoval() and remove() cause themselves, which is on the
    // thread that called them.
    class Events {
    public:
        virtual ~Events() {}
        // A line for the log. It never holds a phone's name or address.
        virtual void sppSay(const std::string& line) = 0;
        virtual void sppState(State now) = 0;
        // `service` is the service the phone asked for, which may be any of the car's, not only this one.
        // `parsed`: the server's message was read whole.
        virtual void sppPhone(Phone what, const std::string& service, bool parsed) = 0;
        // What removeOther(name) came to: the server's error code, 0 for done, or kNoError if the server rejected
        // the request, gave no answer in time, or could not be sent it.
        virtual void sppOtherRemoved(const std::string& /*name*/, uint32_t /*error*/) {}
    };

    // Both outlive this object.
    SppService(GapClient* gap, Events* events);
    virtual ~SppService();

    // Before start(). The caller checks the name (isServiceName) and the UUID (normaliseUuid) first.
    void configure(const std::string& name, const std::string& uuid, uint32_t type);

    // False if the client cannot start connecting. Otherwise kWaiting, and the rest follows as events.
    bool start();
    // In place of start(): only removes the registration under the name, once the server is attached. For a
    // registration that a run which was killed left behind.
    bool startRemoval();
    // Sends the removal: kRemoveSent, then kRemoved or kRemoveFailed with the server's reply. False, and nothing
    // sent, if the server is not attached: the registration, if there is one, stays until the stack restarts.
    // Either way the service is not registered again by this object, until again().
    bool remove(const char* why);
    // After remove(), or once the registration has failed: registers once more, on the client that start()
    // opened. kWaiting until the server is attached, and from there as after start().
    void again();

    // Asks the server to drop the registration filed under `name`, which is not this object's own: one that a
    // program which has ended left behind. This object's own registration and state stay as they are. The
    // server's reply does not say which name it is for, so these requests go out one at a time, in the order they
    // were asked for, and none while a removal of this object's own waits for its reply; what each came to is
    // told through sppOtherRemoved(). A name that is waiting already is not taken twice. False, with nothing
    // kept, if the server is not attached, or if `name` is this object's own or not a service name at all.
    // From any thread.
    bool removeOther(const std::string& name);
    // How long the reply to removeOther()'s request is waited for before the next request may go out and the
    // unanswered one is told as such. kOtherRemovalReplyS unless a test sets it.
    void setOtherReplyTime(double seconds) { otherReplyS_ = seconds; }

    State state();
    const std::string& name() const { return name_; }
    const std::string& uuid() const { return uuid_; }

private:
    SppService(const SppService&);
    SppService& operator=(const SppService&);

    virtual void gapAttached();
    virtual void gapDetached();
    virtual void gapMessage(uint32_t type, uint32_t id, const uint8_t* payload, uint32_t len);

    enum Mode { kModeOff, kModeKeep, kModeRemoveOnly };

    void say(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void set(State s);
    Mode mode();
    void setMode(Mode m);
    bool send(uint32_t id, const std::vector<uint8_t>& payload);
    void subscribe();
    void sendRegister();
    bool sendDeregister(const char* why);
    void sendNextOther();

    GapClient* gap_;
    Events* events_;
    std::string name_, uuid_;
    uint32_t type_;
    pthread_mutex_t mu_;   // state_, mode_, subscribed_, and the three below
    State state_;
    Mode mode_;
    bool subscribed_;      // the phones' requests are asked for of the server that is attached now
    std::vector<std::string> otherWaiting_;   // removeOther(): not yet sent
    std::string otherSent_;                   // sent, and waiting for its reply; empty for none
    double otherSentAt_;
    double otherReplyS_;
};

}  // namespace btspp
}  // namespace nbt

#endif
