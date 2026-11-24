// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SPP_SERVICE_HPP
#define NBT_AGENT_SPP_SERVICE_HPP

// A Bluetooth serial (SPP) service of our own on the car: its registration with the Bluetooth stack, and the
// stack's word that a phone has asked for a serial link.
//
// The stack's server "BSS_GAP.SGBluetoothGap" takes a request SppUuidRegister(name, UUID, UUID, type) and adds an
// SDP record for the UUID, which a phone app then connects to. Nothing is written to disk: a reboot clears the
// registration, and so does a restart of the stack's process, and so does switching the car's Bluetooth off. A
// program that ends without SppUuidDeregister leaves the registration behind until then, with nothing answering it.
//
// What the stack does with the registrations of all its clients, which the agent goes by:
//   * it advertises the first four registrations of this kind that it holds, in the order they were made, each in a
//     place of its own. A fifth is taken, answered 0 and can be dialled from (the car's dial takes the UUID from the
//     request), but is in no service record, so a phone never finds it; a sixth is answered 5, as a name that is
//     registered already is. A registration that is removed frees its place, and the next one made takes the first
//     free place. The car's own software holds one to three of them (one at least on every car), made within about a
//     second of the stack's opening, in answer to that opening
//   * a registration whose advertised UUID is empty ("dial-only") is mishandled at its removal: the stack frees the
//     wrong entry of a table it keeps of them, one entry stays taken until the stack's process restarts, and the
//     registration that held the entry freed can no longer be removed, by anyone. The car's own software makes one
//     such registration. Registrations with an advertised UUID can be made and removed any number of times. So the
//     agent never makes a dial-only registration, and makes none of its own before the car's are there
//   * a registration sent before the stack says it is open (attribute Status, below) is carried out, and still
//     answered 1 (TIMEOUT) some 10 s later; one sent while it is closed is answered 5 at once. Switching the car's
//     Bluetooth off closes the stack and drops every registration; switching it on opens it again
//   * nothing tells a client which names are registered, nor when another client registers or removes one
//
// Three layers, each usable without the one below:
//   * pure helpers (no devices, plain C++98): the UUID and name checks, the payloads of the requests, readers for
//     the replies and informs
//   * GapClient: the server as its client sees it. DsiGapClient is the real one, for the head unit only (its library
//     is the head unit's). A test puts its own in its place
//   * SppService: the registration itself, on any GapClient
//
// DSI payload encoding (little-endian, every item 4-byte aligned from the payload start):
//   u32      4 bytes
//   string   u32 length INCLUDING the NUL, the bytes, padding to 4: zeros in what we send. What the car sends may
//            have other bytes there, and leaves the padding out after a message's last string (Reader)

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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
// Ends a link of a phone's to one of the car's services: closing the link's node does not. Which values a link that
// the phone opened needs is not settled; kDisconnectDevice and kDisconnectInstance are the first ones tried.
const uint32_t kReqServiceDisconnect = 0x1b;         // device, instance, address, service
const char* const kDisconnectDevice = "BtCtrl";
const char* const kDisconnectInstance = "ExtBox1";
// The car opens a link to a service on a phone: the service's name, which the stack turns into its UUID, on an
// instance of the device's (whose node is /dev/<instance>). The address is in the colon form, 00:11:22:33:44:55.
const uint32_t kReqServiceConnect = 0x1a;            // device, instance, address, service
// The car asks a phone for its services afresh (an SDP search over the phone's link), by the phone's address in the
// colon form. The answer lists the services by name, the agent's own among them while the phone's app listens on
// its UUID. One at a time in the car, for every client: a query that meets another client's is answered BUSY, and
// that answer does not reach the client; the one good answer may reach both. Each query also sends every client
// three ServiceListChanged informs. A query to a phone that has no link to the car pages it.
const uint32_t kReqServiceListQuery = 0x19;          // address
// The car's own services (A2DP_SOURCE, SPP, DUN, ...), each by its name, and whether each is on. SupportedServicesGet
// gives every one the stack has, in an order of its own, by which nothing is to be found but the name.
// EnabledServicesSet switches the ones it names on or off and leaves the rest as they are: its first list is the
// values (1 on, 0 off), its second the names, in the same order. One name is enough; the answer to several says
// nothing of each.
const uint32_t kReqSupportedServicesGet = 0x24;      // nothing
const uint32_t kReqEnabledServicesSet = 0x23;        // u32 values[], names[]
const uint32_t kRspSupportedServicesGet = 0x80000039;   // u32 GapErrorCode, u32 values[], names[]
const uint32_t kRspEnabledServicesSet = 0x80000038;     // u32 GapErrorCode
// Refuses a phone's pending request for a service (kInfServiceRequest), which then ends with kInfServiceRequestError.
// The stack takes one at a time from all its clients and answers one that comes while another is out BUSY (2). For
// the car's dial-up networking service it answers 5 whether or not the refusal worked.
const uint32_t kReqServiceReject = 0x1c;             // address, service
const uint32_t kRspServiceReject = 0x80000031;       // u32 GapErrorCode
const uint32_t kReqVersionGet = 0x03;                // nothing
const uint32_t kReqDeviceListGet = 0x13;             // nothing
const uint32_t kRspVersionGet = 0x80000019;          // u32 GapErrorCode, five strings
const uint32_t kRspDeviceListGet = 0x80000028;       // u32 GapErrorCode, then the device lists (DeviceList)
// u32 GapErrorCode, address, services[], states u32[], profiles[], versions u32[] (ServiceList)
const uint32_t kRspServiceListQuery = 0x8000002e;
const uint32_t kRspServiceConnect = 0x8000002f;      // address, service, u32 GapErrorCode
const uint32_t kRspServiceDisconnect = 0x80000030;   // address, service, u32 GapErrorCode, u32 reason
const uint32_t kRspSppUuidRegister = 0x80000043;     // u32 GapErrorCode
const uint32_t kRspSppUuidDeregister = 0x80000044;   // u32 GapErrorCode
// Informs about a link the PHONE opens to one of the car's services, each naming the service. The request comes
// before the car accepts the link and the /dev/btsppN node appears; a link the car opens itself has none.
const uint32_t kInfServiceRequest = 0x80000009;          // name, address, service
const uint32_t kInfServiceRequestComplete = 0x8000000a;  // address, service
const uint32_t kInfServiceRequestError = 0x8000000b;     // address, service
// Informs about the car's phones and their links, kept as state.
const uint32_t kInfDeviceListChanged = 0x80000001;   // the device lists (DeviceList), then a u32
const uint32_t kInfServiceDisconnected = 0x80000007; // device, instance, address, service, u32 reason (1 remote, 0 local)
const uint32_t kInfServiceListChanged = 0x80000008;  // address, services[], states u32[], profiles[], versions u32[],
                                                     // u32 reason
// Informs about a pairing of the car's with a phone. What follows the address is not settled, and the address may
// not be first either: read leniently (pairingInformAddress()). A pairing that failed or is under way stops the
// phone's dials (dial_rules.hpp); one that worked does not, so PairingComplete is not asked for.
const uint32_t kInfPairingComplete = 0x80000004;                 // address, ...
const uint32_t kInfPairingError = 0x80000005;                    // address, u32 ...
const uint32_t kInfSimplePairingConfirmationRequired = 0x8000000c;   // address, name, the number shown ...
// The pairing informs that are asked for and told (sppPairing()): the ones that stop a phone's dials.
inline bool isPairingInform(uint32_t id) {
    return id == kInfPairingError || id == kInfSimplePairingConfirmationRequired;
}
// A list in a payload is a u32 count, then the elements.

// The ids of the server's replies: request N's is 0x80000000 + N + 0x16 for N up to 0x11, + 0x15 for 0x13 to 0x3c
// and + 0x14 for 0x3e to 0x4a. A reply is never asked for as an inform: a client that does gets every other
// client's replies of that id as well.
inline bool isResponseId(uint32_t id) { return id >= 0x80000016u && id <= 0x8000005eu; }
const uint32_t kTypeResponse = 0x200;
const uint32_t kTypeRejectedA = 0x202, kTypeRejectedB = 0x204;   // the stub or server refused the request

// Two attributes of the server's, asked for as the informs are. The current value comes a few milliseconds after it
// is asked for, and again at every change. Each of their messages has the type 0x202, which for a reply would be
// kTypeRejectedA, so they are told apart by their id before anything else.
//   Status     u32: 1 the stack is open (the car's Bluetooth is on), 0 it is closed. 1 comes shortly before the answer
//              to the Open that switches Bluetooth on, 0 just before the answer to the Close that switches it off
//   LocalName  string: the car's Bluetooth name. Sent at the Open, as '' at a Close, and just before the answer to any
//              client's LocalNameSet, whether or not the name changed: the car's own software sets it at every Open,
//              and makes its own registrations right after that answer
const uint32_t kAttrStatus = 0xc0000000u;
const uint32_t kAttrLocalName = 0xc0000001u;
inline bool isAttributeId(uint32_t id) { return id == kAttrStatus || id == kAttrLocalName; }

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

// The UUID of the agent's stand-in for the serial service `name` of another program (SppService::setStandIns()):
// made up from the name, the same for the same name in every run and on every car, and another for every other name.
// It is the first 16 bytes of the SHA-256 of kStandInSeed followed by the name, with the version bits set to 8 and
// the variant bits to 10 (RFC 9562, a UUID of the maker's own), in the form normaliseUuid() gives. So it is never the
// agent's own UUID, which is a version 4 one, never a Bluetooth SIG profile UUID (isSigBaseUuid()), and never empty.
const char* const kStandInSeed = "NBTApps stand-in serial service:";
std::string standInUuid(const std::string& name);

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

inline std::vector<uint8_t> buildServiceDisconnect(const std::string& device, const std::string& instance,
                                                   const std::string& address, const std::string& service) {
    std::vector<uint8_t> p;
    putString(p, device);
    putString(p, instance);
    putString(p, address);
    putString(p, service);
    return p;
}

// The same four strings as ServiceDisconnect.
inline std::vector<uint8_t> buildServiceConnect(const std::string& device, const std::string& instance,
                                                const std::string& address, const std::string& service) {
    return buildServiceDisconnect(device, instance, address, service);
}

// EnabledServicesSet for one service: on (1) or off (0).
inline std::vector<uint8_t> buildEnabledServicesSet(const std::string& name, bool on) {
    std::vector<uint8_t> p;
    putU32(p, 1);
    putU32(p, on ? 1 : 0);
    putU32(p, 1);
    putString(p, name);
    return p;
}

inline std::vector<uint8_t> buildServiceReject(const std::string& address, const std::string& service) {
    std::vector<uint8_t> p;
    putString(p, address);
    putString(p, service);
    return p;
}

inline std::vector<uint8_t> buildServiceListQuery(const std::string& address) {
    std::vector<uint8_t> p;
    putString(p, address);
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
    // The car leaves out the padding after the last string of a message, and padding it does send holds whatever
    // was there before, not zeros. So a string needs only its own bytes, the padding may be cut off at the end,
    // and the padding's bytes are never looked at.
    std::string str() {
        uint32_t len = u32();
        if (!ok_) return std::string();
        if (len > 4096 || len > left()) { ok_ = false; return std::string(); }
        std::string s;
        for (uint32_t i = 0; i < len && p_[off_ + i]; ++i) s += (char)p_[off_ + i];
        uint32_t padded = (len + 3u) & ~3u;
        off_ += padded < left() ? padded : left();
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

// GapErrorCode, in the stack's own order. 0 is NONE there, which is "OK" here.
inline const char* gapErrorName(uint32_t e) {
    switch (e) {
    case 0: return "OK";
    case 1: return "TIMEOUT";
    case 2: return "BUSY";
    case 3: return "STATE";
    case 4: return "LINKKEY";
    case 5: return "DEFAULT";
    case 6: return "REFUSED";
    case 7: return "RFCOMM";
    case 8: return "SECURITY";
    case 9: return "MISSING_KEY";
    case 10: return "INVALID_PARAM";
    case 11: return "ALREADY_CONNECTED";
    case 12: return "HW_NOT_READY";
    case 13: return "NO_SLC";
    case 14: return "PAGE_TIMEOUT";
    case 15: return "DEVICE_NOT_IN_LIST";
    default: return "UNKNOWN";
    }
}

// What a code means in the answer to SppUuidRegister and to SppUuidDeregister, where that is known; empty where
// it is not. While the stack is making a connection, a registration or a removal waits; one that has waited 10 s
// is dropped with TIMEOUT, and one that comes while another waits gets BUSY.
inline const char* registerErrorMeaning(uint32_t e) {
    switch (e) {
    case 1: return "the stack was busy for too long and dropped the request";
    case 2: return "the stack was busy with another request";
    case 5: return "the name or the UUID is registered already, the stack has no place left for it, or the car's "
                   "Bluetooth is switched off";
    default: return "";
    }
}

inline const char* deregisterErrorMeaning(uint32_t e) {
    switch (e) {
    case 1: return "the stack was busy for too long and dropped the request";
    case 2: return "the stack was busy with another request";
    case 5: return "a link on that service is up";
    case 10: return "no such name is registered";
    default: return "";
    }
}

// "5 DEFAULT (what it means)", or "no error code" for kNoError.
inline std::string gapErrorText(uint32_t e, const char* meaning) {
    if (e == kNoError) return "no error code";
    char b[200];
    if (meaning != 0 && *meaning != 0) snprintf(b, sizeof(b), "%u %s (%s)", e, gapErrorName(e), meaning);
    else snprintf(b, sizeof(b), "%u %s", e, gapErrorName(e));
    return b;
}
inline std::string registerErrorText(uint32_t e) { return gapErrorText(e, registerErrorMeaning(e)); }
inline std::string deregisterErrorText(uint32_t e) { return gapErrorText(e, deregisterErrorMeaning(e)); }

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

// The most elements a list is read with: more is taken for a message that is not what it seems.
const uint32_t kMostListed = 64;

// A list of strings into *out; with no `out`, only read past, and *count, if given, says how many there were.
inline bool readStrings(Reader& r, std::vector<std::string>* out, uint32_t* count = 0) {
    uint32_t n = r.u32();
    if (!r.ok() || n > kMostListed) return false;
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        std::string s = r.str();
        if (out) out->push_back(s);
    }
    if (count) *count = n;
    return r.ok();
}

inline bool readValues(Reader& r, std::vector<uint32_t>* out) {
    uint32_t n = r.u32();
    if (!r.ok() || n > kMostListed) return false;
    for (uint32_t i = 0; i < n && r.ok(); ++i) out->push_back(r.u32());
    return r.ok();
}

// 0x8000002f: what came of a ServiceConnect.
struct ServiceConnectReply {
    std::string address, service;
    uint32_t error;
    bool ok;
};

inline ServiceConnectReply parseServiceConnectReply(const uint8_t* p, uint32_t n) {
    ServiceConnectReply c;
    Reader r(p, n);
    c.address = r.str();
    c.service = r.str();
    c.error = r.u32();
    c.ok = r.ok();
    return c;
}

// 0x80000030: what came of a ServiceDisconnect.
struct ServiceDisconnectReply {
    std::string address, service;
    uint32_t error, reason;
    bool ok;
};

inline ServiceDisconnectReply parseServiceDisconnectReply(const uint8_t* p, uint32_t n) {
    ServiceDisconnectReply d;
    Reader r(p, n);
    d.address = r.str();
    d.service = r.str();
    d.error = r.u32();
    d.reason = r.u32();
    d.ok = r.ok();
    return d;
}

// A phone in the car's device list. Its name is not kept: it is read past and dropped.
struct BtDevice {
    std::string address;
    uint32_t deviceClass, security, connection;
    BtDevice() : deviceClass(0), security(0), connection(0) {}
};

// BtDevice::connection.
enum { kConnNotInRange = 0, kConnInRange = 1, kConnConnected = 2, kConnServiceConnected = 3 };

inline const char* connectionName(uint32_t c) {
    switch (c) {
    case kConnNotInRange: return "not in range";
    case kConnInRange: return "in range";
    case kConnConnected: return "connected";
    case kConnServiceConnected: return "service connected";
    default: return "unknown";
    }
}

// The device lists: names[], addresses[], class u32[], security u32[], connection u32[], one element in each for a
// device. DeviceListGet's reply has the error code before them; DeviceListChanged has a u32 after them.
struct DeviceList {
    uint32_t error;                 // the reply's; 0 for an inform
    std::vector<BtDevice> devices;
    uint32_t after;                 // the inform's last value
    bool ok;
    DeviceList() : error(0), after(0), ok(false) {}
};

inline bool readDeviceLists(Reader& r, std::vector<BtDevice>* out) {
    std::vector<std::string> addresses;
    std::vector<uint32_t> classes, security, connection;
    uint32_t n = 0;
    if (!readStrings(r, 0, &n) || !readStrings(r, &addresses) || !readValues(r, &classes) ||
        !readValues(r, &security) || !readValues(r, &connection))
        return false;
    if (addresses.size() != n || classes.size() != n || security.size() != n || connection.size() != n) return false;
    for (uint32_t i = 0; i < n; ++i) {
        BtDevice d;
        d.address = addresses[i];
        d.deviceClass = classes[i];
        d.security = security[i];
        d.connection = connection[i];
        out->push_back(d);
    }
    return true;
}

// kRspDeviceListGet, or kInfDeviceListChanged.
inline DeviceList parseDeviceList(uint32_t id, const uint8_t* p, uint32_t n) {
    DeviceList l;
    Reader r(p, n);
    if (id == kRspDeviceListGet) l.error = r.u32();
    // A reply with an error may hold nothing after it.
    if (id == kRspDeviceListGet && r.ok() && l.error != 0 && r.left() == 0) {
        l.ok = true;
        return l;
    }
    bool lists = r.ok() && readDeviceLists(r, &l.devices);
    if (id == kInfDeviceListChanged) l.after = r.u32();
    l.ok = lists && r.ok();
    return l;
}

// Whether `s` is a Bluetooth address: 00:11:22:33:44:55 in either case, or its twelve digits alone.
inline bool isBtAddress(const std::string& s) {
    if (s.size() == 12) {
        for (size_t i = 0; i < s.size(); ++i)
            if (!isHex(s[i])) return false;
        return true;
    }
    if (s.size() != 17) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i % 3 == 2 ? s[i] != ':' : !isHex(s[i])) return false;
    }
    return true;
}

// The phone a pairing inform is about, if its payload begins with a string that is a Bluetooth address; empty if
// it does not.
inline std::string pairingInformAddress(const uint8_t* p, uint32_t n) {
    Reader r(p, n);
    std::string s = r.str();
    return r.ok() && isBtAddress(s) ? s : std::string();
}

// Two addresses are the same phone's whatever their form: the hex digits are compared, upper-cased.
inline std::string addressKey(const std::string& address) {
    std::string k;
    for (size_t i = 0; i < address.size(); ++i) {
        char c = address[i];
        if (!isHex(c)) continue;
        if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
        k += c;
    }
    return k;
}

// kInfServiceDisconnected: a link to a service has ended. reason 1: the far end ended it; 0: this end did.
struct ServiceDisconnected {
    std::string device, instance, address, service;
    uint32_t reason;
    bool ok;
};

inline ServiceDisconnected parseServiceDisconnected(const uint8_t* p, uint32_t n) {
    ServiceDisconnected d;
    Reader r(p, n);
    d.device = r.str();
    d.instance = r.str();
    d.address = r.str();
    d.service = r.str();
    d.reason = r.u32();
    d.ok = r.ok();
    return d;
}

// kInfServiceListChanged: the services of a phone and the state of each.
struct ServiceListChanged {
    std::string address;
    std::vector<std::string> services, profiles;
    std::vector<uint32_t> states, versions;
    uint32_t reason;
    bool ok;
};

inline ServiceListChanged parseServiceListChanged(const uint8_t* p, uint32_t n) {
    ServiceListChanged c;
    Reader r(p, n);
    c.address = r.str();
    bool lists = readStrings(r, &c.services) && readValues(r, &c.states) && readStrings(r, &c.profiles) &&
                 readValues(r, &c.versions);
    c.reason = r.u32();
    c.ok = lists && r.ok();
    return c;
}

// kRspServiceListQuery: a phone's services, by name, and the state of each (0 for a service with no link up), as
// the phone's SDP records gave them just now. The car names a service of a phone's that it knows by its registered
// name, the agent's own among them, and another by its UUID.
struct ServiceList {
    uint32_t error;
    std::string address;
    std::vector<std::string> services, profiles;
    std::vector<uint32_t> states, versions;
    bool ok;
    bool rejected;   // no reply: the server rejected the request (SppService::Events::sppServiceList())
    ServiceList() : error(0), ok(false), rejected(false) {}
};

// The same layout as ServiceListGet's reply (0x8000002d), which this reads as well. A reply with an error may hold
// nothing after it: that is read whole, with no address.
inline ServiceList parseServiceList(const uint8_t* p, uint32_t n) {
    ServiceList l;
    Reader r(p, n);
    l.error = r.u32();
    if (r.ok() && l.error != 0 && r.left() == 0) {
        l.ok = true;
        return l;
    }
    l.address = r.str();
    bool lists = r.ok() && readStrings(r, &l.services) && readValues(r, &l.states) && readStrings(r, &l.profiles) &&
                 readValues(r, &l.versions);
    l.ok = lists && r.ok();
    return l;
}

// Whether a service list names `name`, whatever its state.
inline bool listsService(const ServiceList& list, const std::string& name) {
    for (size_t i = 0; i < list.services.size(); ++i)
        if (list.services[i] == name) return true;
    return false;
}

// kRspSupportedServicesGet: the car's services and whether each is on. A reply with an error may hold nothing after
// it: that is read whole.
struct SupportedServices {
    uint32_t error;
    std::vector<uint32_t> values;
    std::vector<std::string> names;
    bool ok;   // read whole, with as many values as names
    SupportedServices() : error(0), ok(false) {}
    // 1 on, 0 off, -1 not listed.
    int state(const std::string& name) const {
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == name) return values[i] != 0 ? 1 : 0;
        return -1;
    }
};

inline SupportedServices parseSupportedServices(const uint8_t* p, uint32_t n) {
    SupportedServices s;
    Reader r(p, n);
    s.error = r.u32();
    if (r.ok() && s.error != 0 && r.left() == 0) {
        s.ok = true;
        return s;
    }
    bool lists = r.ok() && readValues(r, &s.values) && readStrings(r, &s.names);
    s.ok = lists && r.ok() && s.values.size() == s.names.size();
    return s;
}

// kRspVersionGet: the stack's version strings.
struct StackVersion {
    uint32_t error;
    std::vector<std::string> parts;
    bool ok;
};

inline StackVersion parseStackVersion(const uint8_t* p, uint32_t n) {
    StackVersion v;
    Reader r(p, n);
    v.error = r.u32();
    for (int i = 0; i < 5 && r.ok() && r.left() > 0; ++i) {
        std::string s = r.str();
        if (r.ok()) v.parts.push_back(s);
    }
    v.ok = r.ok() && (v.parts.size() == 5 || v.error != 0);
    return v;
}

// A string of the server's as a log may show it: printable ASCII, `most` characters at most.
inline std::string shownText(const std::string& s, size_t most) {
    std::string t;
    for (size_t i = 0; i < s.size() && t.size() < most; ++i) t += (s[i] >= 0x20 && s[i] < 0x7f) ? s[i] : '?';
    return t;
}

// What the car's device list says of one phone, without its address.
struct PhoneInList {
    bool ok;               // the list was read; if not, nothing below is known
    uint32_t error;        // the list's error code; 0 for a list that came
    bool known;            // there was a phone to look for
    bool listed;           // it is in the list
    uint32_t connection;   // kConnNotInRange ... kConnServiceConnected
    uint32_t security;
    PhoneInList() : ok(false), error(0), known(false), listed(false), connection(0), security(0) {}
};

inline PhoneInList phoneInList(const DeviceList& list, const std::string& address) {
    PhoneInList p;
    p.ok = list.ok;
    p.error = list.error;
    p.known = !addressKey(address).empty();
    if (!p.ok || p.error != 0 || !p.known) return p;
    for (size_t i = 0; i < list.devices.size(); ++i) {
        if (addressKey(list.devices[i].address) != addressKey(address)) continue;
        p.listed = true;
        p.connection = list.devices[i].connection;
        p.security = list.devices[i].security;
        break;
    }
    return p;
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
    // After open() has returned false: true if no later open() in this process can work, so another try is no use.
    virtual bool openFinal() const { return false; }
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
    virtual bool openFinal() const { return dsi_.openFinal(); }
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
// start() registers the service once the server is attached and its registrations may go (below), and again each
// time the server comes back, because its restart drops the registration, and each time the car's Bluetooth has
// been switched on again, because switching it off drops it too. A first refusal is taken for a registration that an
// earlier run left behind under the same name: that one is removed and the registration tried once more. remove()
// takes the service away again; after it a server that comes back is not given the service a second time.
//
// When registrations may go. With the server attached, Status (kAttrStatus) and LocalName are asked for with the
// informs. No SppUuidRegister of this object's goes out, of any kind, unless Status says the stack is open and has
// said so for the hold time (setHold()): the car's own registrations are made within about a second of the stack's
// opening, and one of them is dial-only, so a registration made before it costs the stack a table entry at the next
// switch-off (the top of this file). The hold counts from when Status said open; at an attach that finds the stack
// open already, from the attach, since how long it has been open cannot be told. Status closed: every registration
// is gone, this object's and its stand-ins', and none goes out, nor a check, until it is open again and the hold has
// passed; then the whole round goes as at the start. A server that says nothing of Status within setStatusWait() of
// the attach (a stack that has no such attribute, or a subscription that could not be sent) has the registrations
// go then, as with no hold, and the log says so. Until they may go the state is kHeld. The hold's end, and the
// stale answers below, are seen by tend(), which the caller makes often; a server whose Status comes open with a
// hold of 0 has the round go at once.
//
// The stand-ins (setStandIns()). The places the stack advertises are given in the order registrations come, so a
// program that registers after the agent may land in the fifth place, where no phone finds it. So for each serial
// service of another program that the agent is told of, a registration is made under that service's name, with the
// UUID standInUuid() makes for it as both its advertised and its dialled UUID and the agent's own type, ahead of the
// agent's own: it holds the name, and with it a place below the agent's, until the program registers. The program's
// own registration is then answered 5 (the name is there): the program removes it by name, and registers again, and
// so takes that place. A round is the stand-ins, one at a time in the order they were given, each waiting for its
// answer (the answers name nothing, and are told apart only by their order), then this object's own registration or
// its check. A stand-in answered 0 holds the name; 5 means the name is there already (the program's own
// registration, a stand-in from before, or no place left), and is left so; any other answer is logged, and the round
// goes on. Only one SppUuidRegister of this object's is out at a time, of any kind.
//
// What a name holds. The stack cannot be asked what is registered under a name, so each stand-in is kept as "the
// name holds the agent's stand-in" (ours) from an answer 0 to its registration until its program is seen running
// (standInRuns()), the server goes, Status goes to closed, or its name is removed; at any other time what it holds
// is not known. replaceWithStandIn() is for a service whose program is not running: a stand-in that is ours is left
// alone, and what the name holds otherwise (a registration the program left behind, which phones would open links
// to with nobody reading them) is removed by name and the stand-in registered in its place at once.
//
// No place left. A 5 to this object's own registration means "the name is there" or "no place": the removal by name
// that follows tells the two apart, as its answer 10 says that nothing was there. When the second registration is
// refused too and stand-ins that are ours stand ahead of it, the agent's own takes the place of the last of them, in
// the order of the round, and of no more than it needs: that stand-in is removed (an ordinary removal, which the stack
// makes safely) and the registration made once more; refused again, the next-to-last that is ours is given up the
// same way, and so on, one removal and one registration at a time, until the registration is taken or no stand-in that
// is ours is left (kFailed then). The agent's own needs no advertised place (the app reaches it through the car's own
// serial service, and the car dials from any place), while a declared program needs one, so the places the first
// stand-ins hold, the advertised ones, stay theirs. A removal answered anything but 0 or 10 leaves that stand-in not
// known to be ours, and the registration goes once more all the same: refused, the next stand-in is given up, so the
// loop ends by itself. A stand-in given up is not registered again in that round; it stays given (setStandIns()) and
// goes first in the next round as always. In a registration check's round it is then answered 5 every time while no
// place is free, which is only counted; once a place has come free in between (a registration of the car's or of a
// program went) it takes that one, which is after the agent's own and may be the one the stack does not advertise.
// Nothing better can be known, and nothing is done about it. Status closed or the server going in the middle ends it:
// everything is forgotten, and the next round starts afresh.
//
// The server's requests for a link do not say which serial node the link gets, and name the service the phone
// asked for. Which link is whose is for the caller to work out.
const double kOtherRemovalReplyS = 5.0;   // see SppService::setOtherReplyTime()
const double kDisconnectReplyS = 3.0;     // see SppService::setDisconnectReplyTime()
const double kStatusWaitS = 10.0;         // see SppService::setStatusWait()
// A registration sent before the server went or Status went to closed is answered all the same, within some 10 s
// (TIMEOUT at the latest), and the next one waits for that answer, which is then passed over. With none in this long
// it is passed over all the same, and the next goes.
const double kStaleReplyS = 15.0;

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
        kRemoved,        // the server removed it, or had nothing under the name (removeError() says which)
        kRemoveFailed,
        kHeld            // started and attached, and no registration may go yet: the car's Bluetooth is off, or has
                         // not been on for the hold time (setHold()), or the server has not said which yet
    };

    // What the server's Status says of the car's Bluetooth.
    enum Bluetooth { kBtUnknown, kBtOff, kBtOn };
    // What replaceWithStandIn() came to.
    enum Replace {
        kReplacing,      // the name's registration is being removed, and the stand-in follows it; or was already
        kKeptOurs,       // the name holds the agent's stand-in: nothing was done
        kNotNow          // nothing could be done now: the server is not attached, no registration may go, or the
                         // name is not one to take
    };
    // One stand-in, as standIns() tells it: for the log and the tests.
    struct StandInState {
        std::string name, uuid;
        bool ours;                    // the name holds it, as far as is known
        unsigned sent, placed, there, other;   // its registrations sent, and answered 0, 5 and anything else
    };
    // The time this object goes by, in seconds; monotonic. A test's own, in place of the system's (setClock()).
    typedef double (*Clock)();

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
        // `parsed`: the server's message was read whole. `address` is the phone's, for kPhoneAsked, as the
        // request gave it: for the caller to keep with the request, and never to log.
        virtual void sppPhone(Phone what, const std::string& service, bool parsed, const std::string& address) = 0;
        // What removeOther(name) came to: the server's error code, 0 for done, or kNoError if the server rejected
        // the request, gave no answer in time, or could not be sent it.
        virtual void sppOtherRemoved(const std::string& /*name*/, uint32_t /*error*/) {}
        // What the car's device list says of the phone of the last refusal, after askAboutRefused().
        virtual void sppRefusedPhone(const PhoneInList& /*phone*/) {}
        // A whole device list has come, the answer to DeviceListGet or the inform DeviceListChanged: every phone the
        // car knows, with its address, connection and security. For the caller to keep, and never to log.
        virtual void sppDevices(const std::vector<BtDevice>& /*devices*/) {}
        // The answer to connectService(), with what it named; for a rejection, or a send that failed, error is
        // kNoError and the address and service are the ones asked for.
        virtual void sppConnectReply(const ServiceConnectReply& /*reply*/) {}
        // A late answer: a reply read whole that names the ServiceConnect last given up with kLateTold
        // (giveUpConnect()), which came while no ServiceConnect of ours waited. It answers nothing that is out, and is
        // never told through sppConnectReply(); a client that does not take this event drops it, unsaid.
        virtual void sppLateConnectReply(const ServiceConnectReply& /*reply*/) {}
        // The answer to disconnectService(), the same way.
        virtual void sppDisconnectReply(const ServiceDisconnectReply& /*reply*/) {}
        // Every ServiceListQuery reply that comes, whoever's query it answers: the car may give the one answer to
        // two clients whose queries met. One that cannot be read, or a rejection of queryServices()'s request, has
        // the error kNoError, and the rejection has `rejected` too; they and an error reply with no address name the
        // phone of queryServices() if that waits for its answer, and nobody otherwise. For the caller to keep, and
        // never to log the address.
        virtual void sppServiceList(const ServiceList& /*list*/) {}
        // The inform ServiceDisconnected: a link of the car's ended.
        virtual void sppLinkEnded(const ServiceDisconnected& /*ended*/) {}
        // A pairing inform that stops dials, PairingError or SimplePairingConfirmationRequired
        // (setPairingInforms()), and the address it begins with, or empty. PairingComplete is never told.
        virtual void sppPairing(uint32_t /*informId*/, const std::string& /*address*/) {}
        // A phone's request for the service setKnockService() names, in place of sppPhone(): asked (kPhoneAsked), or
        // ended, refused (kPhoneRefused) or come up (kPhoneLinked), with the phone's address as the inform gave it,
        // for the caller to keep and never to log.
        virtual void sppKnockRequest(Phone /*what*/, const std::string& /*address*/, bool /*parsed*/) {}
        // The answer to rejectRequest(): the stack's error code, or kNoError for a rejection.
        virtual void sppRejectAnswered(uint32_t /*error*/) {}
    };

    // The service setKeepOn() keeps on, as the last look found it.
    enum Kept {
        kKeptNotLooked,   // no look has been answered since the server attached or the car's Bluetooth came on
        kKeptOn,          // listed, and on
        kKeptOff,         // listed, and off: being switched on, or that failed and is tried again at the next check
        kKeptAbsent       // not in the stack's list of services: nothing to switch on
    };
    struct KeptState {
        Kept state;
        bool absentSeen;          // a look has found it not listed, in this object's life
        bool foundOffFirst;       // the first look of this object's life found it off
        bool switchedOn;          // this object has switched it on (and not off again since)
        unsigned switches;        // switched on by this object, all told
        unsigned offAgain;        // found off again after it had been on: switched off behind this object's back
        KeptState() : state(kKeptNotLooked), absentSeen(false), foundOffFirst(false), switchedOn(false), switches(0),
                      offAgain(0) {}
    };

    // Both outlive this object.
    SppService(GapClient* gap, Events* events);
    virtual ~SppService();

    // Before start(). The caller checks the name (isServiceName) and the UUID (normaliseUuid) first.
    void configure(const std::string& name, const std::string& uuid, uint32_t type);
    // Before start(): how long Status must have said open before a registration goes (the top of this section); 0,
    // which is how it starts, for none.
    void setHold(double seconds) { holdS_ = seconds; }
    // Before start(): how long after an attach a server that says nothing of Status is waited for before the
    // registrations go all the same. kStatusWaitS unless a test sets it.
    void setStatusWait(double seconds) { statusWaitS_ = seconds; }
    // Test only, before start(): the clock for the hold, the waits for answers and the stale answers.
    void setClock(Clock clock) { clock_ = clock; }
    // The names of the serial services of other programs that the agent has a stand-in for: from here on, each round
    // registers these, in this order, ahead of this object's own registration or check. The agent's own name, and a
    // name the stack would not take, are passed over; so is a name given twice. A name that is no longer given is
    // dropped: a stand-in that is ours under it is removed by name, one whose registration is out is removed once it is
    // answered 0, and one that is not known to be ours is left as it is. From any thread.
    void setStandIns(const std::vector<std::string>& names);
    // The program of stand-in `name` is running: from here the name is not known to hold the stand-in, so that nothing
    // takes what it holds for the agent's. From any thread.
    void standInRuns(const std::string& name);
    // For a serial service of another program's whose program is not running (serial_services.hpp): kKeptOurs if the
    // name holds the agent's stand-in; otherwise what is registered under it is removed by name (removeOther()'s
    // queue), and on an answer 0 or 10 the stand-in is registered at once, ahead of anything but a round under way;
    // kReplacing then, and also while that is under way already. kNotNow, with nothing done, if the server is not
    // attached, the service is not started or no registration may go now (kHeld), or the name is this object's own or
    // not a service name. A name not given to setStandIns() is taken as one, and dropped again at its next call. From
    // any thread.
    Replace replaceWithStandIn(const std::string& name);
    // Removes by name every stand-in that is ours, through removeOther()'s queue, as the agent stops; how many. Its
    // program is the caller's to look at first (standInRuns()): a name whose program runs holds that program's
    // registration, and is left alone. From any thread.
    unsigned removeStandIns();
    // Whether a removal of another program's registration or of a stand-in waits to go out or for its answer.
    bool othersPending();
    std::vector<StandInState> standIns();
    Bluetooth bluetooth();
    // At every pass of the caller's: the registrations go when they may (the hold has passed, or the server has said
    // nothing of Status for setStatusWait()), and an answer that a registration sent before the server went or
    // Status went to closed was waiting for is passed over after kStaleReplyS. From any thread; never while the caller
    // holds a lock the events take.
    void tend();

    // False if the client cannot start connecting. Otherwise kWaiting, and the rest follows as events.
    bool start();
    // After start() or startRemoval() has returned false: no later one can work in this process.
    bool startFinal() const { return gap_->openFinal(); }
    // In place of start(): only removes the registration under the name, once the server is attached. For a
    // registration that a run which was killed left behind.
    bool startRemoval();
    // Sends the removal: kRemoveSent, then kRemoved or kRemoveFailed with the server's reply. False, and nothing
    // sent, if the server is not attached: the registration, if there is one, stays until the stack restarts.
    // Either way the service is not registered again by this object, until again().
    bool remove(const char* why);
    // After remove(), or once the registration has failed: registers once more, on the client that start()
    // opened. kWaiting until the server is attached, and from there as after start(): the round at once if
    // registrations may go, kHeld until they may otherwise.
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

    // Asks the server to end the phone's link: ServiceDisconnect for the phone of the last request for a link
    // that the server told (kInfServiceRequest), whatever service it was for. `device`, `instance` and `service`
    // may be given; empty is kDisconnectDevice, kDisconnectInstance and the service named in that request. The
    // phone's address goes into the request and nowhere else. False, with nothing sent, if the server is not
    // attached or no phone has asked for a link since this object was made, or while a ServiceDisconnect waits
    // for its answer (disconnectService()'s or one of its own): then `*waits`, if given, is true, and the caller
    // asks again later. Its reply (kRspServiceDisconnect) is logged with its error code and reason, and a reply or a
    // rejection that comes after it and that this object does not know with its id, its type and its first 32-bit
    // value. A reply read whole that names another phone is another client's, as for disconnectService(); the
    // service is not compared, since which one the car wants here is not settled. From any thread.
    bool disconnectPhone(const std::string& device, const std::string& instance, const std::string& service,
                         bool* waits = 0);
    // Whether a phone has asked for a link since this object was made: disconnectPhone() has someone to name.
    bool knowsPhone();
    // ServiceConnect: the car opens a link to `service` on the phone at `address` (the colon form), on its instance
    // `instance` of `device`. One at a time: false, with nothing sent, while one waits for its answer, or if the
    // server is not attached or did not take the request. The answer goes to sppConnectReply(). A reply that is
    // read whole and names another service or another phone is another client's: it is not the answer, which is
    // still waited for. Nothing is logged but the first few of those: the caller says what it does. From any
    // thread.
    //
    // One that names the same phone and service as the connect kept by giveUpConnect(kLateTold) takes that one's
    // place: the kept one is forgotten as this one goes out (and kept again if it could not be sent), and its late
    // answer, should it come now, cannot be told from this one's: the first reply that names them is taken as this
    // one's answer.
    bool connectService(const std::string& device, const std::string& instance, const std::string& address,
                        const std::string& service);
    // What becomes of an answer to a ServiceConnect that giveUpConnect() gave up, should one come after all.
    enum LateAnswer {
        kLateDropped,   // another client's, as below. For a request whose late answer the caller passes over anyway
                        // (the listener's re-point, dial_rules.hpp)
        kLateTold       // the connect is kept, and a reply read whole that names its phone and service and comes while
                        // no ServiceConnect of ours waits is told through sppLateConnectReply() (a dial's: its late
                        // answer still brings the re-point)
    };
    // Gives up the ServiceConnect that waits for its answer, if it is the one connectService() was given `address`
    // and `service` for, so that the next may go: nothing else ends one that is never answered but a rejection, a
    // failed send or the server's going. True if it was given up; false, with nothing changed, if none waits or the
    // one that waits names another phone or service. Nothing is logged here: the caller says what it gives up. From
    // any thread.
    //
    // An answer to it that comes after all, with kLateDropped, is another client's: dropped unsaid while no
    // ServiceConnect of ours waits, and counted and left, as any other client's, while one that names another phone or
    // service waits; one that names the same is taken as that one's answer.
    //
    // With kLateTold the one given up is kept: one at a time, the last given up so, in memory only. Its late answer,
    // read whole and with none of ours waiting, goes to sppLateConnectReply(), once: the connect is forgotten as it is
    // told. It is also forgotten when a reply naming it comes while another of ours waits (which is then counted as
    // another client's, as above), when connectService() sends one naming the same phone and service (see there), and
    // when the server goes. A reply that cannot be read, and a rejection, name nothing, and are never told as late.
    // A later kLateDropped give-up of another connect leaves it kept.
    bool giveUpConnect(const std::string& address, const std::string& service, LateAnswer late);
    // ServiceListQuery: the car asks the phone at `address` (the colon form) for its services. False, with nothing
    // sent, if the server is not attached or did not take the request. Its answer, and any other reply of that id,
    // goes to sppServiceList(); one at a time is the caller's to keep to (dial_rules.hpp), since the car's answer
    // may be another client's. Nothing is logged here: the caller says what it does. From any thread.
    bool queryServices(const std::string& address);
    // ServiceDisconnect with all four values given: the answer goes to sppDisconnectReply() as well as to the log.
    // False, with nothing sent, while a ServiceDisconnect waits for its answer (this one's or disconnectPhone()'s),
    // or as connectService(). A reply that is read whole and names another phone or another service (the car's
    // reply repeats the service it was sent) is another client's: it is not the answer, which is still waited for,
    // and it is neither told nor logged as ours; the first few of them are logged as such, and the rest counted.
    // `quiet`: neither the request nor its reply is logged here, for a caller that only counts what it repeats.
    //
    // One ServiceDisconnect at a time, of either kind: the replies say nothing else of which request they answer.
    // One that has had no answer for the time setDisconnectReplyTime() sets is given up when the next is asked for,
    // so that a car that never answers cannot hold up every later one; a late answer to it is then another
    // client's, unless it names the same phone as the one out by then, from which it cannot be told apart and is
    // taken as that one's answer.
    bool disconnectService(const std::string& device, const std::string& instance, const std::string& address,
                           const std::string& service, bool quiet = false);
    // How long a ServiceDisconnect waits for its answer before the next may go out: kDisconnectReplyS unless the
    // caller sets it. The listener sets the time its dial waits for one (DialConfig::disconnectWaitMs).
    void setDisconnectReplyTime(double seconds) { disconnectReplyS_ = seconds; }
    // Before start(): the pairing informs that stop dials are asked for too (sppPairing()).
    void setPairingInforms(bool on) { pairingInforms_ = on; }
    // Before start(): a phone's requests for the service `name` (the car's dial-up networking service, for the knock
    // by dial-up) go to sppKnockRequest() and not to sppPhone(), and are not the request disconnectPhone() names, nor
    // the refusal askAboutRefused() asks about. Empty, which is how it starts, for none.
    void setKnockService(const std::string& name) { knockService_ = name; }
    // ServiceReject for the phone's pending request for `service`: the answer goes to sppRejectAnswered(). One at a
    // time: false, with nothing sent, while one waits for its answer, or if the server is not attached or did not
    // take the request. Nothing is logged here. From any thread.
    bool rejectRequest(const std::string& address, const std::string& service);

    // Before start(): the car's service `name` (the dial-up networking service, for the knock by dial-up) is kept on
    // while this object keeps its registration. Each time this object's own registration has been made, and in every
    // registration check's round, SupportedServicesGet goes (one more request; never before registrations may go,
    // When registrations may go, above), and a service listed and off is switched on with EnabledServicesSet, for
    // that name alone. A service not listed is said once and left. The first time it is found off is said; found off
    // again after that (the car's own software switches it off on some cars at some moments, with no word to other
    // clients), it is switched on again, with a line the first time and counted after. One request of each kind out at
    // a time; an answer to one sent before the server went or Status went to closed is passed over (kStaleReplyS at
    // most), and after that nothing is known of it until the next look. A request answered with an error or rejected
    // is said when that differs from the last, and asked again at the next check, never sooner. Empty, which is how
    // it starts, for none.
    void setKeepOn(const std::string& name) { keptName_ = name; }
    KeptState kept();
    // As the caller stops: switches the kept service off again, only where this object's first look found it off and
    // this object switched it on; a service found on is left as it is. True if the request went; its answer is
    // waited for with keptSetOut(). From any thread.
    bool switchKeptOff();
    bool keptSetOut();
    bool attached() const { return gap_->attached(); }
    // The server's name and the version of its interface that is asked for, for the log.
    std::string server() const { return gap_->describe(); }

    // VersionGet: the stack's version strings, which the log gets when the reply comes. False, with nothing sent, if
    // the server is not attached or the request could not be sent. From any thread.
    bool askVersion();
    // DeviceListGet: the car's device list is read anew, into heard(). The same rules as askVersion().
    bool askDeviceList();
    // The same, for the phone of the last refusal of a link (kInfServiceRequestError): what the list says of it
    // goes to sppRefusedPhone() when the reply comes, its address to nobody.
    bool askAboutRefused();

    // What the server's informs and replies have told about the car's phones and their links: kept, and not acted
    // on. A phone's address is in memory only, and never in the log.
    struct Heard {
        bool devicesKnown;                 // a device list has come
        std::vector<BtDevice> devices;
        unsigned deviceListChanges;        // DeviceListChanged informs
        unsigned linksEnded;               // ServiceDisconnected informs
        unsigned serviceListChanges;       // ServiceListChanged informs: only counted. Every ServiceListQuery, the
                                           // agent's or another client's, brings three, and they change nothing
        std::vector<std::string> version;  // the stack's version strings, from VersionGet, as the log shows them
        unsigned othersConnectReplies;     // ServiceConnect replies for another client that came while ours waited
        unsigned othersDisconnectReplies;  // the same for ServiceDisconnect
        unsigned statusChanges;            // Status values that differed from the one before, the first among them
        unsigned names;                    // LocalName values
        Heard() : devicesKnown(false), deviceListChanges(0), linksEnded(0), serviceListChanges(0),
                  othersConnectReplies(0), othersDisconnectReplies(0), statusChanges(0), names(0) {}
    };
    Heard heard();

    // The registration check. Switching Bluetooth off and on in iDrive drops every registration. Status says so
    // (above), and then the registration is no longer kRegistered here; but a stack with no Status, or a drop of
    // another kind, says nothing, and the registration stays kRegistered. So, while registered, the caller has
    // SppUuidRegister for this object's own name sent now and then, as the end of a round: the stand-ins go first,
    // each counted, with a line only for an answer other than the stand-in's last, and then the check, whose answer
    // says:
    //   0       the stack had lost the registration, and has it again now: logged the first time; if the next
    //           check that is not answered 5, 1 or 2 finds it gone again, one line says it keeps being dropped,
    //           and after that it is counted only, until a check finds it there (5) or the stack busy (1, 2)
    //   5       it is still there (as it answers a name that is registered already): counted only
    //   1 or 2  the stack was busy: counted, and the next check asks again
    //   other   (or a rejection of the request): logged when it differs from the last such answer, or when a
    //           check answered 5, 1 or 2 came between them; else counted only
    // A check changes no state. It is never the start-up path, which clears a refused registration by removing
    // it: a check answered 5 has found what it hoped to find. A check answered 1 (TIMEOUT) may have been carried out:
    // it counts as busy, and the next check, a registration of the same name, finds it there (5) or makes it (0).
    // False, with nothing sent, unless the registration is kRegistered, the server attached and no check waits for
    // its reply; one whose round has waited `replyS` counts as unanswered, and the next may go. From any thread.
    bool checkRegistration(double replyS);
    struct CheckCounts {
        unsigned sent, present, restored, busy, other, unanswered;
        // The last answer counted in `other`: a GapErrorCode, or kNoError with the type of a rejection; and how
        // many times it came in its last run. lastOddTimes 0: none came.
        uint32_t lastOdd, lastOddRejected;
        unsigned lastOddTimes;
        CheckCounts()
            : sent(0), present(0), restored(0), busy(0), other(0), unanswered(0), lastOdd(0), lastOddRejected(0),
              lastOddTimes(0) {}
    };
    CheckCounts checkCounts();

    State state();
    // How many times this object's own registration has become kRegistered: a caller that looks now and then sees by
    // it that the registration was made anew, however quickly it was lost and made again in between.
    unsigned timesRegistered();
    // The server's answer to the last removal of this object's own registration: 0, a GapErrorCode, or kNoError
    // for none yet. 10 (no such name is registered) counts as removed: the stack had dropped the registration.
    uint32_t removeError();
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
    void subscribeTo(uint32_t informId);
    bool handleKnown(uint32_t type, uint32_t id, const uint8_t* payload, uint32_t len);
    void checkAnswered(uint32_t err, uint32_t rejectedType);
    bool sendDeregister(const char* why);
    void sendNextOther();
    void noteUnknown(uint32_t type, uint32_t id, const uint8_t* payload, uint32_t len);
    bool takeDisconnect(const std::string& address, const std::string& service, bool told, bool quiet = false);

    // What a removal through removeOther()'s queue is for.
    enum OtherWhy {
        kOtherLeftBehind,   // removeOther(): another program's registration, left behind
        kOtherReplace,      // replaceWithStandIn(): what a name holds, for the stand-in to follow
        kOtherYield,        // a stand-in given up for the agent's own registration (No place left, above)
        kOtherStop,         // removeStandIns()
        kOtherDropped       // a stand-in whose name setStandIns() no longer gives
    };
    struct Other {
        std::string name;
        OtherWhy why;
        Other(const std::string& n, OtherWhy w) : name(n), why(w) {}
    };
    // What the one SppUuidRegister that is out is for.
    enum RegFor { kForNone, kForStandIn, kForOwn, kForCheck };
    struct StandIn {
        std::string name, uuid;
        bool wanted;      // setStandIns() gives it now
        bool ours;        // the name holds it (What a name holds, above)
        bool replacing;   // replaceWithStandIn(): the removal of what the name holds is under way, or its stand-in
                          // waits to go or for its answer
        bool queued;      // in pending_
        uint32_t last;    // its last answer, kNoError with lastRejected for a rejection; lastKnown: one came
        uint32_t lastRejected;
        bool lastKnown;
        unsigned sent, placed, there, other;
        StandIn() : wanted(false), ours(false), replacing(false), queued(false), last(kNoError), lastRejected(0),
                    lastKnown(false), sent(0), placed(0), there(0), other(0) {}
    };

    double now() const { return clock_(); }
    // Whether registrations may go now (When registrations may go, above). mu_ held.
    bool mayRegisterLocked(double at) const;
    // From kHeld: the round of this object's own registration begins, if registrations may go now. True if it began.
    bool advance();
    // The same with mu_ held, and *line the log's line for it: kRegSent, and the round set out; begun() then says the
    // line, tells kRegSent and sends, with mu_ let go. In one step with what sets kHeld, so that no other thread
    // begins the round in between.
    bool beginLocked(std::string* line);
    void begun(const std::string& line);
    // Sends the next SppUuidRegister that is due, if none is out and they may go: this object's second registration,
    // the next of a round, then a stand-in that waits. Never with mu_ held.
    void pump();
    // The answer to the SppUuidRegister that was out.
    void registerAnswered(uint32_t err, uint32_t rejectedType);
    // `waited`: this object's own registration was the one out; otherwise the answer is only said.
    void ownAnswered(uint32_t err, uint32_t rejectedType, bool waited);
    void standInAnswered(const std::string& name, uint32_t err, uint32_t rejectedType, bool quiet);
    // The answer to a removal through removeOther()'s queue that was for a stand-in.
    void otherAnswered(const Other& other, uint32_t err);
    void statusTold(const uint8_t* payload, uint32_t len);
    void nameTold(const uint8_t* payload, uint32_t len);
    // Every registration is gone (Status closed, or the server went): mu_ held.
    void forgetRegistrationsLocked();
    StandIn* standInLocked(const std::string& name);
    // "'A', 'B'", for the log.
    static std::string namesText(const std::vector<std::string>& names);
    // Whether a line about Status or LocalName is said: the first kAttributeLines are, and *last is true for the
    // last of them. mu_ held.
    bool attributeLineLocked(bool* last);
    void queueOtherLocked(const std::string& name, OtherWhy why);

    GapClient* gap_;
    Events* events_;
    std::string name_, uuid_;
    uint32_t type_;
    Clock clock_;
    double holdS_, statusWaitS_;
    pthread_mutex_t mu_;   // state_, mode_, subscribed_, and everything below
    State state_;
    Mode mode_;
    bool subscribed_;      // the phones' requests are asked for of the server that is attached now
    std::vector<Other> otherWaiting_;         // removeOther()'s queue: not yet sent
    std::string otherSent_;                   // sent, and waiting for its reply; empty for none
    OtherWhy otherSentWhy_;
    double otherSentAt_;
    double otherReplyS_;
    // Status, since the attach (When registrations may go, above).
    Bluetooth bt_;
    bool statusHeard_;                        // a Status has come since the attach: the first is its current value
    double attachedAt_, openSince_;
    unsigned attributeLines_;                 // lines said about Status and LocalName
    // The stand-ins, in the order setStandIns() gave them, and what goes out of SppUuidRegister.
    std::vector<StandIn> standIns_;
    RegFor regFor_;                           // the one SppUuidRegister out; kForNone: none
    std::string regName_;                     // kForStandIn: whose
    double regAt_;
    bool regStale_;                           // sent before the server went or Status went to closed
    bool regQuiet_;                           // a stand-in of a check's round: a line only for news
    bool roundOn_, roundCheck_;               // a round goes, for the check (or for this object's own)
    std::vector<std::string> roundLeft_;      // its stand-ins still to go
    bool retryDue_;                           // this object's second (or a later) registration is to go next
    std::vector<std::string> pending_;        // stand-ins to register when nothing else is due: replaceWithStandIn()'s
    bool yieldTried_;                         // this round has given a stand-in up (No place left, above)
    bool yielding_;                           // one is being given up: yieldLeft_ (1) removal still to answer
    unsigned yieldLeft_;
    std::vector<std::string> yielded_;        // the stand-ins this round has given up, in that order, for the log
    unsigned checkFails_;                     // checks whose registration could not be sent
    unsigned timesRegistered_;
    // The phones' requests for this service go to sppKnockRequest() (setKnockService()).
    std::string knockService_;
    bool rejectOut_;                          // a ServiceReject of ours waits for its answer
    // The kept service (setKeepOn()).
    std::string keptName_;
    KeptState kept_;
    bool keptLooked_;                         // a look of this object's life has been answered
    bool keptWasOn_;                          // since the last look that could not tell, it has been seen on
    bool askOut_, askStale_;                  // SupportedServicesGet out, and sent before the server went or Status
    double askAt_;                            // went to closed
    bool setOut_, setStale_, setOn_;          // EnabledServicesSet out, and what it asks for
    double setAt_;
    // For the look [0] and the switch [1]: the last answer said, which is not news when it comes again.
    uint32_t keptOdd_[2], keptOddRejected_[2];
    bool keptOddKnown_[2];
    // The kept service's look: SupportedServicesGet, if it may go (When registrations may go). `quiet`: in a check's
    // round, where only news is said.
    void lookAtKept(bool quiet);
    void keptAnswered(uint32_t id, uint32_t type, const uint8_t* payload, uint32_t len);
    void sendKeptSet(bool on);
    bool keptOddNews(int which, uint32_t err, uint32_t rejectedType);
    bool lookQuiet_;
    // The last phone's request for a link: its address and the service it asked for. In memory only; neither
    // reaches the log or leaves this class.
    std::string phoneAddress_, phoneService_;
    unsigned unknownLeft_;                    // after a ServiceDisconnect: how many more unknown answers are logged
    uint32_t removeError_;                    // see removeError()
    uint32_t clearError_;                     // the answer to kClearSent's removal
    bool disconnectOut_;                      // a ServiceDisconnect of ours waits for its reply
    bool disconnectTold_;                     // and its answer goes to sppDisconnectReply(): disconnectService()'s
    bool disconnectQuiet_;                    // and its reply is not logged (disconnectService()'s `quiet`)
    std::string disconnectAddress_, disconnectService_;   // what it named; in memory only
    double disconnectSentAt_;
    double disconnectReplyS_;                 // see setDisconnectReplyTime()
    bool connectOut_;                         // a ServiceConnect of ours waits for its reply
    std::string connectAddress_, connectService_;
    bool lateKept_;                           // giveUpConnect(kLateTold): what the one given up named, for its late
    std::string lateAddress_, lateService_;   // answer; in memory only
    std::string queryAddress_;                // the phone of the last ServiceListQuery of ours, until a reply that
                                              // names it, a rejection or the server's going; in memory only
    bool pairingInforms_;
    std::string refusedAddress_;              // the phone of the last refusal; in memory only
    bool refusalAsked_;                       // askAboutRefused() waits for the device list
    bool versionAsked_;                       // a VersionGet of ours waits for its reply
    Heard heard_;
    unsigned endsLogged_;                     // ServiceDisconnected informs said in the log so far
    unsigned unreadSaid_;                     // the kinds of message logged once as not read
    bool checkOut_;                           // a registration check waits for its reply
    double checkSentAt_;
    bool unansweredSaid_;                     // since the last answer to a check
    CheckCounts checks_;
    unsigned restoredRun_;                    // checks answered 0 since the last one answered 5, 1 or 2
    bool oddRun_;                             // checks_.lastOdd* is the last other answer, with no 5, 1 or 2 since
};

}  // namespace btspp
}  // namespace nbt

#endif
