// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/spp_service.hpp: the UUID and name checks, the SppUuidRegister / SppUuidDeregister /
// ServiceDisconnect payloads, the reply and inform readers, the registration itself and ServiceDisconnect, run
// against a Bluetooth server that this test plays.
// All names, addresses and UUIDs are invented; only the payload layout is the Bluetooth server's.

#include "../../agent/spp_service.hpp"

#include <stdio.h>
#include <unistd.h>

#include <string>
#include <utility>
#include <vector>

#include "test_util.hpp"

using namespace nbt::btspp;

static const char* const kName = "Test_Service";
static const char* const kUuid = "0123ABCD-89EF-4A5B-8C7D-0123456789AB";
static const char* const kPhoneName = "Test Phone";
static const char* const kPhoneAddress = "AABBCCDDEEFF";
static const size_t kInforms = 6;   // the informs a registration asks for

static std::vector<uint8_t> bytes(const char* s, size_t n) {
    return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(s), reinterpret_cast<const uint8_t*>(s) + n);
}

// The Bluetooth server, played by the test on the test's own thread: it keeps what the registration sends, and
// says what the test tells it to.
class FakeGap : public GapClient {
public:
    struct Sent {
        uint32_t id;
        std::vector<uint8_t> payload;
    };

    FakeGap() : events(0), up(false), canOpen(true), takes(true), refuseSubscribe(false), opens(0) {}

    virtual bool open(Events* e) {
        ++opens;
        events = e;
        return canOpen;
    }
    virtual bool attached() const { return up; }
    virtual bool request(uint32_t id, const std::vector<uint8_t>& payload) {
        if (!up || !takes) return false;
        Sent s;
        s.id = id;
        s.payload = payload;
        sent.push_back(s);
        return true;
    }
    virtual bool subscribe(uint32_t informId) {
        if (!up || refuseSubscribe) return false;
        informs.push_back(informId);
        return true;
    }
    virtual std::string describe() const { return "Test.Server 1.0"; }

    void attach() {
        up = true;
        events->gapAttached();
    }
    void detach() {
        up = false;
        events->gapDetached();
    }
    void reply(uint32_t id, uint32_t error) {
        std::vector<uint8_t> p;
        putU32(p, error);
        events->gapMessage(kTypeResponse, id, &p[0], (uint32_t)p.size());
    }
    void reject(uint32_t type, uint32_t id) { events->gapMessage(type, id, 0, 0); }
    // An inform about a phone's request, as the server builds it; `cut` bytes are left off its end.
    void inform(uint32_t id, const char* service, size_t cut = 0) {
        std::vector<uint8_t> p;
        if (id == kInfServiceRequest) putString(p, kPhoneName);
        putString(p, kPhoneAddress);
        putString(p, service);
        events->gapMessage(0x400, id, &p[0], (uint32_t)(p.size() - cut));
    }

    // The one request sent since the last look, which must be `id`; its payload.
    std::vector<uint8_t> took(uint32_t id) {
        CHECK(sent.size() == 1 && sent[0].id == id);
        std::vector<uint8_t> p = sent[0].payload;
        sent.clear();
        return p;
    }

    Events* events;
    bool up, canOpen, takes, refuseSubscribe;
    int opens;
    std::vector<Sent> sent;
    std::vector<uint32_t> informs;
};

class Seen : public SppService::Events {
public:
    struct PhoneEvent {
        SppService::Phone what;
        std::string service;
        bool parsed;
        std::string address;
    };

    virtual void sppSay(const std::string& line) { lines.push_back(line); }
    virtual void sppState(SppService::State now) { states.push_back(now); }
    virtual void sppPhone(SppService::Phone what, const std::string& service, bool parsed, const std::string& address) {
        PhoneEvent e;
        e.what = what;
        e.service = service;
        e.parsed = parsed;
        e.address = address;
        phone.push_back(e);
    }
    virtual void sppRefusedPhone(const PhoneInList& phone) { refused.push_back(phone); }
    virtual void sppConnectReply(const ServiceConnectReply& reply) { connects.push_back(reply); }
    virtual void sppDisconnectReply(const ServiceDisconnectReply& reply) { disconnects.push_back(reply); }
    virtual void sppServiceList(const ServiceList& list) { lists.push_back(list); }
    virtual void sppPairing(uint32_t informId, const std::string& address) {
        pairings.push_back(std::make_pair(informId, address));
    }
    virtual void sppOtherRemoved(const std::string& name, uint32_t error) {
        char b[32];
        snprintf(b, sizeof(b), "=%d", error == kNoError ? -1 : (int)error);
        others += (others.empty() ? "" : " ") + name + b;
    }
    // What the removals of other programs' registrations came to since the last look, as "Name=0 Other=-1".
    std::string removed() {
        std::string s = others;
        others.clear();
        return s;
    }

    bool said(const char* part) const {
        for (size_t i = 0; i < lines.size(); ++i)
            if (lines[i].find(part) != std::string::npos) return true;
        return false;
    }
    // The states passed through since the last look, as "2 3 5".
    std::string passed() {
        std::string s;
        for (size_t i = 0; i < states.size(); ++i) {
            char b[16];
            snprintf(b, sizeof(b), "%s%d", i ? " " : "", (int)states[i]);
            s += b;
        }
        states.clear();
        return s;
    }

    std::vector<std::string> lines;
    std::vector<SppService::State> states;
    std::vector<PhoneEvent> phone;
    std::vector<PhoneInList> refused;
    std::vector<ServiceConnectReply> connects;
    std::vector<ServiceDisconnectReply> disconnects;
    std::vector<ServiceList> lists;
    std::vector<std::pair<uint32_t, std::string> > pairings;
    std::string others;
};

// The name a removal request carries.
static std::string deregisters(const std::vector<uint8_t>& p) {
    Reader r(&p[0], (uint32_t)p.size());
    std::string name = r.str();
    CHECK(r.ok() && r.left() == 0);
    return name;
}

static std::string path(SppService::State a) {
    char b[16];
    snprintf(b, sizeof(b), "%d", (int)a);
    return b;
}
static std::string path(SppService::State a, SppService::State b) { return path(a) + " " + path(b); }
static std::string path(SppService::State a, SppService::State b, SppService::State c) {
    return path(a, b) + " " + path(c);
}

static void isRegister(const std::vector<uint8_t>& p, uint32_t type) {
    Reader r(&p[0], (uint32_t)p.size());
    CHECK(r.str() == kName);
    CHECK(r.str() == kUuid && r.str() == kUuid);   // the service's UUID, and the same one to connect to
    CHECK(r.u32() == type && r.ok() && r.left() == 0);
}

// A ServiceDisconnect reply: address, service, error, reason.
static std::vector<uint8_t> disconnectReply(const char* address, const char* service, uint32_t error,
                                            uint32_t reason = 0) {
    std::vector<uint8_t> p;
    putString(p, address);
    putString(p, service);
    putU32(p, error);
    putU32(p, reason);
    return p;
}

static void isDeregister(const std::vector<uint8_t>& p) {
    Reader r(&p[0], (uint32_t)p.size());
    CHECK(r.str() == kName && r.ok() && r.left() == 0);
}

// A started registration that the server has just taken.
struct Registered {
    FakeGap gap;
    Seen seen;
    SppService spp;
    Registered() : spp(&gap, &seen) {
        spp.configure(kName, kUuid, 0);
        CHECK(spp.start());
        gap.attach();
        isRegister(gap.took(kReqSppUuidRegister), 0);
        gap.reply(kRspSppUuidRegister, 0);
        CHECK(spp.state() == SppService::kRegistered);
        seen.passed();
    }
};

int main() {
    // 1. UUIDs: shape, upper-casing, SIG profile UUIDs.
    std::string u;
    CHECK(normaliseUuid("0123abcd-89ef-4a5b-8c7d-0123456789ab", &u) && u == "0123ABCD-89EF-4A5B-8C7D-0123456789AB");
    CHECK(normaliseUuid(kUuid, &u) && u == kUuid);                            // upper case already
    CHECK(!normaliseUuid("", &u));
    CHECK(!normaliseUuid("0123abcd89ef4a5b8c7d0123456789ab", &u));           // no dashes
    CHECK(!normaliseUuid("0123abcd-89ef-4a5b-8c7d-0123456789a", &u));        // one short
    CHECK(!normaliseUuid("0123abcd-89ef-4a5b-8c7d-0123456789abc", &u));      // one long
    CHECK(!normaliseUuid("0123abcd-89ef-4a5b-8c7d-0123456789ag", &u));       // not hex
    CHECK(!normaliseUuid("0123abcd_89ef-4a5b-8c7d-0123456789ab", &u));       // wrong separator
    CHECK(!normaliseUuid("0123abcd-89ef-4a5b-8c7d-012345678'ab", &u));       // a quote never gets through
    CHECK(isSigBaseUuid("00001101-0000-1000-8000-00805F9B34FB"));            // Serial Port
    CHECK(isSigBaseUuid("0000110A-0000-1000-8000-00805F9B34FB"));
    CHECK(!isSigBaseUuid(kUuid));
    CHECK(!isSigBaseUuid("00001101-0000-1000-8000-00805F9B34FC"));
    CHECK(!isSigBaseUuid("short"));

    // 2. Service names.
    CHECK(isServiceName(kName) && isServiceName("a") && isServiceName("Test_Service-2"));
    CHECK(!isServiceName("") && !isServiceName("two words") && !isServiceName("semi;colon") && !isServiceName("q'"));
    CHECK(isServiceName(std::string(32, 'x')) && !isServiceName(std::string(33, 'x')));

    // 3. Payloads: strings carry their length including the NUL and are padded to 4.
    std::vector<uint8_t> p;
    putString(p, "abc");                                   // 4 + "abc\0"
    CHECK(p == bytes("\x04\x00\x00\x00" "abc\x00", 8));
    p.clear();
    putString(p, "abcd");                                  // 5 + "abcd\0" + 3 padding
    CHECK(p == bytes("\x05\x00\x00\x00" "abcd\x00\x00\x00\x00", 12));
    p.clear();
    putString(p, "");
    CHECK(p == bytes("\x01\x00\x00\x00\x00\x00\x00\x00", 8));
    p.clear();
    putU32(p, 0x11223344u);
    CHECK(p == bytes("\x44\x33\x22\x11", 4));

    const std::string uuid = kUuid;                                        // 36 chars: length 37, padded to 40
    std::vector<uint8_t> reg = buildRegister("Echo", uuid, uuid, 0);
    CHECK(reg.size() == (4 + 8) + (4 + 40) + (4 + 40) + 4);
    Reader rr(&reg[0], (uint32_t)reg.size());
    CHECK(rr.str() == "Echo" && rr.str() == uuid && rr.str() == uuid && rr.u32() == 0 && rr.ok() && rr.left() == 0);
    std::vector<uint8_t> reg1 = buildRegister("Echo", uuid, uuid, 1);
    CHECK(reg1[reg1.size() - 4] == 1);
    std::vector<uint8_t> dereg = buildDeregister("Echo");
    CHECK(dereg == bytes("\x05\x00\x00\x00" "Echo\x00\x00\x00\x00", 12));

    // 4. Replies: the error code, and replies too short to hold one.
    CHECK(readError(reinterpret_cast<const uint8_t*>("\x05\x00\x00\x00"), 4) == 5);
    CHECK(readError(reinterpret_cast<const uint8_t*>("\x00\x00\x00\x00"), 4) == 0);
    CHECK(readError(reinterpret_cast<const uint8_t*>("\x05\x00"), 2) == kNoError);
    CHECK(readError(NULL, 0) == kNoError);
    // The stack's own table, in its order.
    {
        const char* const names[] = {"OK", "TIMEOUT", "BUSY", "STATE", "LINKKEY", "DEFAULT", "REFUSED", "RFCOMM",
                                     "SECURITY", "MISSING_KEY", "INVALID_PARAM", "ALREADY_CONNECTED", "HW_NOT_READY",
                                     "NO_SLC", "PAGE_TIMEOUT", "DEVICE_NOT_IN_LIST"};
        for (uint32_t e = 0; e < 16; ++e) CHECK_IN(gapErrorName(e), std::string(gapErrorName(e)) == names[e]);
        CHECK(std::string(gapErrorName(16)) == "UNKNOWN" && std::string(gapErrorName(kNoError)) == "UNKNOWN");
        // What a code means depends on the request it answers.
        CHECK(registerErrorText(0) == "0 OK");
        CHECK(registerErrorText(5) == "5 DEFAULT (the name or the UUID is registered already, or the car's Bluetooth "
                                      "is switched off)");
        CHECK(deregisterErrorText(10) == "10 INVALID_PARAM (no such name is registered)");
        CHECK(deregisterErrorText(5) == "5 DEFAULT (a link on that service is up)");
        CHECK(registerErrorText(10) == "10 INVALID_PARAM" && deregisterErrorText(14) == "14 PAGE_TIMEOUT");
        CHECK(registerErrorText(1) == "1 TIMEOUT (the stack was busy for too long and dropped the request)");
        CHECK(deregisterErrorText(2) == "2 BUSY (the stack was busy with another request)");
        CHECK(registerErrorText(kNoError) == "no error code" && deregisterErrorText(99) == "99 UNKNOWN");
    }

    // 5. Reader: truncated and oversized strings fail without reading past the end.
    {
        std::vector<uint8_t> t = bytes("\x09\x00\x00\x00" "abc", 7);       // claims 9 bytes, has 3
        Reader r(&t[0], (uint32_t)t.size());
        CHECK(r.str().empty() && !r.ok());
        std::vector<uint8_t> big = bytes("\xff\xff\xff\x7f", 4);
        Reader r2(&big[0], 4);
        CHECK(r2.str().empty() && !r2.ok());
        Reader r3(NULL, 12);
        CHECK(r3.u32() == 0 && !r3.ok());
    }

    // 5b. What the car sends: no padding after a message's last string, and padding that is not zeros. Made-up
    //     payloads; only the layout is the car's.
    {
        // "Echo" (length 5) as the last string, its three bytes of padding left out.
        std::vector<uint8_t> t = bytes("\x05\x00\x00\x00" "Echo\x00", 9);
        Reader r(&t[0], (uint32_t)t.size());
        CHECK(r.str() == "Echo" && r.ok() && r.left() == 0);
        // Only part of the padding there.
        t = bytes("\x05\x00\x00\x00" "Echo\x00\x7e", 10);
        Reader r1(&t[0], (uint32_t)t.size());
        CHECK(r1.str() == "Echo" && r1.ok() && r1.left() == 0);
        // Padding with leftovers in it, then a value: the value is read where it is, the padding never looked at.
        t = bytes("\x05\x00\x00\x00" "Echo\x00\xa5\x5a\xff" "\x0e\x00\x00\x00", 16);
        Reader r2(&t[0], (uint32_t)t.size());
        CHECK(r2.str() == "Echo" && r2.u32() == 14 && r2.ok() && r2.left() == 0);
        // A string followed by leftover bytes rather than zeros, past its NUL.
        t = bytes("\x03\x00\x00\x00" "ab\x00\x41" "\x02\x00\x00\x00" "z\x00", 14);
        Reader r3(&t[0], (uint32_t)t.size());
        CHECK(r3.str() == "ab" && r3.str() == "z" && r3.ok() && r3.left() == 0);
        // A string that is not the last one, cut short of its padding: what follows it is missing, and that fails.
        t = bytes("\x05\x00\x00\x00" "Echo\x00", 9);
        Reader r4(&t[0], (uint32_t)t.size());
        CHECK(r4.str() == "Echo" && r4.ok() && r4.u32() == 0 && !r4.ok());
        // The string's own bytes must all be there.
        t = bytes("\x05\x00\x00\x00" "Ech", 7);
        Reader r5(&t[0], (uint32_t)t.size());
        CHECK(r5.str().empty() && !r5.ok());
        // A phone's request whose service, the last string, comes without its padding and the one before with junk
        // in it.
        std::vector<uint8_t> q = bytes("\x0b\x00\x00\x00" "Test Phone\x00\xee"
                                       "\x0d\x00\x00\x00" "AABBCCDDEEFF\x00\x01\x02\x03"
                                       "\x05\x00\x00\x00" "Echo\x00", 45);
        ServiceRequest s = parseServiceRequest(kInfServiceRequest, &q[0], (uint32_t)q.size());
        CHECK(s.ok && s.name == kPhoneName && s.address == kPhoneAddress && s.service == "Echo");
    }

    // 6. ServiceRequest informs: name + address + service, or address + service.
    {
        std::vector<uint8_t> q;
        putString(q, kPhoneName);
        putString(q, kPhoneAddress);
        putString(q, "Echo");
        ServiceRequest s = parseServiceRequest(kInfServiceRequest, &q[0], (uint32_t)q.size());
        CHECK(s.ok && s.name == kPhoneName && s.address == kPhoneAddress && s.service == "Echo");

        std::vector<uint8_t> c;
        putString(c, kPhoneAddress);
        putString(c, "Echo");
        s = parseServiceRequest(kInfServiceRequestComplete, &c[0], (uint32_t)c.size());
        CHECK(s.ok && s.name.empty() && s.address == kPhoneAddress && s.service == "Echo");
        s = parseServiceRequest(kInfServiceRequestError, &c[0], (uint32_t)c.size());
        CHECK(s.ok && s.service == "Echo");
        s = parseServiceRequest(kInfServiceRequestError, &c[0], 6);         // cut short
        CHECK(!s.ok);
        CHECK(isServiceRequestInform(kInfServiceRequest) && isServiceRequestInform(kInfServiceRequestError));
        CHECK(!isServiceRequestInform(kRspSppUuidRegister));
    }

    // 7. The registration, when all goes well: nothing is sent before the server is attached, then the three
    //    informs are asked for and the service registered.
    {
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        CHECK(spp.state() == SppService::kIdle);
        spp.configure(kName, kUuid, 1);
        CHECK(spp.name() == kName && spp.uuid() == kUuid);
        CHECK(spp.start() && gap.opens == 1);
        CHECK(spp.state() == SppService::kWaiting && gap.sent.empty() && gap.informs.empty());
        gap.attach();
        CHECK(gap.informs.size() == kInforms && gap.informs[0] == kInfServiceRequest &&
              gap.informs[1] == kInfServiceRequestComplete && gap.informs[2] == kInfServiceRequestError);
        CHECK(gap.informs[3] == kInfDeviceListChanged && gap.informs[4] == kInfServiceDisconnected &&
              gap.informs[5] == kInfServiceListChanged);
        for (size_t i = 0; i < gap.informs.size(); ++i) CHECK(!isResponseId(gap.informs[i]));
        CHECK(!seen.said("could not be asked for inform"));
        isRegister(gap.took(kReqSppUuidRegister), 1);
        CHECK(spp.state() == SppService::kRegSent);
        gap.reply(kRspSppUuidRegister, 0);
        CHECK(spp.state() == SppService::kRegistered && gap.sent.empty());
        CHECK(seen.passed() == path(SppService::kWaiting, SppService::kRegSent, SppService::kRegistered));
        CHECK(seen.said("attached to the Bluetooth server Test.Server 1.0"));
        CHECK(seen.said("SppUuidRegister name 'Test_Service', UUID 0123ABCD-89EF-4A5B-8C7D-0123456789AB, type 1"));
        CHECK(seen.said("SppUuidRegister reply: 0 OK"));
    }
    {   // a client that cannot start connecting at all
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        gap.canOpen = false;
        spp.configure(kName, kUuid, 0);
        CHECK(!spp.start());
    }

    // 8. A registration left behind under the same name: removed, and the registration tried once more.
    {
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        CHECK(spp.start());
        gap.attach();
        isRegister(gap.took(kReqSppUuidRegister), 0);
        gap.reply(kRspSppUuidRegister, 5);
        isDeregister(gap.took(kReqSppUuidDeregister));
        CHECK(spp.state() == SppService::kClearSent && seen.said("left behind"));
        gap.reply(kRspSppUuidDeregister, 0);
        isRegister(gap.took(kReqSppUuidRegister), 0);
        CHECK(spp.state() == SppService::kRetrySent);
        gap.reply(kRspSppUuidRegister, 0);
        CHECK(spp.state() == SppService::kRegistered && gap.sent.empty());
    }
    {   // nothing was filed under the name, and the stack still says DEFAULT: the UUID is taken by another name, or
        // the car's Bluetooth is off
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        CHECK(spp.start());
        gap.attach();
        gap.took(kReqSppUuidRegister);
        gap.reply(kRspSppUuidRegister, 5);
        gap.took(kReqSppUuidDeregister);
        gap.reply(kRspSppUuidDeregister, 10);
        CHECK(seen.said("SppUuidDeregister reply (id 0x80000044): 10 INVALID_PARAM (no such name is registered)"));
        gap.took(kReqSppUuidRegister);
        gap.reply(kRspSppUuidRegister, 5);
        CHECK(spp.state() == SppService::kFailed);
        CHECK(seen.said("nothing was registered under the name 'Test_Service', so its UUID "
                        "0123ABCD-89EF-4A5B-8C7D-0123456789AB is registered under another name, or the car's Bluetooth "
                        "is switched off"));
    }
    {   // refused the second time too: failed, and nothing more is sent
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        CHECK(spp.start());
        gap.attach();
        gap.took(kReqSppUuidRegister);
        gap.reply(kRspSppUuidRegister, 5);
        gap.took(kReqSppUuidDeregister);
        gap.reply(kRspSppUuidDeregister, 4);              // whatever the removal came to, the second try follows
        gap.took(kReqSppUuidRegister);
        gap.reply(kRspSppUuidRegister, 5);
        CHECK(spp.state() == SppService::kFailed && gap.sent.empty());
        gap.reply(kRspSppUuidRegister, 0);                // a reply nothing waits for
        CHECK(spp.state() == SppService::kFailed);
    }

    // 9. Requests the server rejects outright.
    {
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        CHECK(spp.start());
        gap.attach();
        gap.took(kReqSppUuidRegister);
        gap.reject(kTypeRejectedA, kReqSppUuidRegister);
        CHECK(spp.state() == SppService::kFailed && gap.sent.empty() && seen.said("rejected the request"));
    }
    {   // the removal of a registration left behind is rejected: the second try is still made
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        CHECK(spp.start());
        gap.attach();
        gap.took(kReqSppUuidRegister);
        gap.reply(kRspSppUuidRegister, 5);
        gap.took(kReqSppUuidDeregister);
        gap.reject(kTypeRejectedB, kReqSppUuidDeregister);
        isRegister(gap.took(kReqSppUuidRegister), 0);
        gap.reject(kTypeRejectedB, kReqSppUuidRegister);
        CHECK(spp.state() == SppService::kFailed && gap.sent.empty());
    }

    // 10. A phone's requests: what it asked for goes on, its name and address do not.
    {
        Registered r;
        r.gap.inform(kInfServiceRequest, kName);
        r.gap.inform(kInfServiceRequestComplete, kName);
        r.gap.inform(kInfServiceRequest, "Another");
        r.gap.inform(kInfServiceRequestError, "Another");
        r.gap.inform(kInfServiceRequestError, kName, 6);  // cut short
        CHECK(r.seen.phone.size() == 5);
        CHECK(r.seen.phone[0].what == SppService::kPhoneAsked && r.seen.phone[0].service == kName &&
              r.seen.phone[0].parsed);
        // The address goes with a request, for the node it brings, and with nothing else.
        CHECK(r.seen.phone[0].address == kPhoneAddress && r.seen.phone[2].address == kPhoneAddress);
        CHECK(r.seen.phone[1].address.empty() && r.seen.phone[3].address.empty() && r.seen.phone[4].address.empty());
        CHECK(r.seen.phone[1].what == SppService::kPhoneLinked && r.seen.phone[1].service == kName);
        CHECK(r.seen.phone[2].what == SppService::kPhoneAsked && r.seen.phone[2].service == "Another");
        CHECK(r.seen.phone[3].what == SppService::kPhoneRefused && r.seen.phone[3].service == "Another" &&
              r.seen.phone[3].parsed);
        CHECK(r.seen.phone[4].what == SppService::kPhoneRefused && !r.seen.phone[4].parsed);
        CHECK(!r.seen.said(kPhoneName) && !r.seen.said(kPhoneAddress));
        CHECK(r.spp.state() == SppService::kRegistered && r.gap.sent.empty() && r.seen.passed().empty());
    }

    // 11. The server goes away and comes back: its restart drops the registration, so it is made again.
    {
        Registered r;
        r.gap.detach();
        CHECK(r.spp.state() == SppService::kWaiting && r.seen.said("the registration is gone with it"));
        r.gap.attach();
        CHECK(r.gap.informs.size() == 2 * kInforms);                 // the informs are asked for again
        isRegister(r.gap.took(kReqSppUuidRegister), 0);
        r.gap.reply(kRspSppUuidRegister, 0);
        CHECK(r.seen.passed() == path(SppService::kWaiting, SppService::kRegSent, SppService::kRegistered));
    }
    {   // a registration that failed is tried again when the server comes back, and not before
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        CHECK(spp.start());
        gap.attach();
        gap.took(kReqSppUuidRegister);
        gap.reject(kTypeRejectedA, kReqSppUuidRegister);
        gap.detach();
        CHECK(spp.state() == SppService::kFailed && gap.sent.empty());
        gap.attach();
        isRegister(gap.took(kReqSppUuidRegister), 0);
        gap.reply(kRspSppUuidRegister, 0);
        CHECK(spp.state() == SppService::kRegistered);
    }

    // 12. Taking the service away.
    {
        Registered r;
        CHECK(r.spp.remove("leaving"));
        isDeregister(r.gap.took(kReqSppUuidDeregister));
        CHECK(r.spp.state() == SppService::kRemoveSent);
        CHECK(r.seen.said("SppUuidDeregister name 'Test_Service': leaving"));
        r.gap.reply(kRspSppUuidDeregister, 0);
        CHECK(r.seen.passed() == path(SppService::kRemoveSent, SppService::kRemoved));
        r.gap.detach();                                   // a server that comes back is not given the service again
        r.gap.attach();
        CHECK(r.gap.sent.empty() && r.gap.informs.size() == kInforms && r.spp.state() == SppService::kRemoved);
    }
    {   // the server says no
        Registered r;
        CHECK(r.spp.remove("leaving"));
        r.gap.took(kReqSppUuidDeregister);
        r.gap.reply(kRspSppUuidDeregister, 5);
        CHECK(r.spp.state() == SppService::kRemoveFailed && r.spp.removeError() == 5);
        CHECK(r.seen.said("5 DEFAULT (a link on that service is up)"));
    }
    {   // the stack had dropped the registration already (Bluetooth off and on): nothing is filed under the name,
        // which is as good as removed
        Registered r;
        CHECK(r.spp.removeError() == kNoError);
        CHECK(r.spp.remove("leaving"));
        r.gap.took(kReqSppUuidDeregister);
        r.gap.reply(kRspSppUuidDeregister, 10);
        CHECK(r.spp.state() == SppService::kRemoved && r.spp.removeError() == 10);
    }
    {   // or rejects the request
        Registered r;
        CHECK(r.spp.remove("leaving"));
        r.gap.took(kReqSppUuidDeregister);
        r.gap.reject(kTypeRejectedA, kReqSppUuidDeregister);
        CHECK(r.spp.state() == SppService::kRemoveFailed);
    }
    {   // the reply to the removal may carry an id of its own: the first plain response counts
        Registered r;
        CHECK(r.spp.remove("leaving"));
        r.gap.took(kReqSppUuidDeregister);
        r.gap.reply(0x80000099u, 0);
        CHECK(r.spp.state() == SppService::kRemoved);
    }
    {   // a registration's reply that comes after the removal went out is not taken for the removal's
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        CHECK(spp.start());
        gap.attach();
        gap.took(kReqSppUuidRegister);
        CHECK(spp.remove("leaving"));
        gap.took(kReqSppUuidDeregister);
        gap.reply(kRspSppUuidRegister, 0);
        CHECK(spp.state() == SppService::kRemoveSent);
        gap.reply(kRspSppUuidDeregister, 0);
        CHECK(spp.state() == SppService::kRemoved);
    }
    {   // with the server gone there is nothing to send, and the service is not registered when it comes back
        Registered r;
        r.gap.detach();
        CHECK(!r.spp.remove("leaving") && r.gap.sent.empty() && r.spp.state() == SppService::kWaiting);
        r.gap.attach();
        CHECK(r.gap.sent.empty() && r.spp.state() == SppService::kWaiting);
    }
    {   // again(): the service is asked for once more, on the client that is open. With the server there, at
        // once, and the phones' requests are not asked for a second time
        Registered r;
        CHECK(r.spp.remove("leaving"));
        r.gap.took(kReqSppUuidDeregister);
        r.gap.reply(kRspSppUuidDeregister, 0);
        r.seen.passed();
        r.spp.again();
        isRegister(r.gap.took(kReqSppUuidRegister), 0);
        CHECK(r.seen.passed() == path(SppService::kWaiting, SppService::kRegSent) && r.gap.informs.size() == kInforms);
        r.gap.reply(kRspSppUuidRegister, 0);
        CHECK(r.spp.state() == SppService::kRegistered && r.gap.opens == 1);
    }
    {   // with the server gone, when it comes back
        Registered r;
        r.gap.detach();
        CHECK(!r.spp.remove("leaving"));
        r.spp.again();
        CHECK(r.gap.sent.empty() && r.spp.state() == SppService::kWaiting);
        r.gap.attach();
        isRegister(r.gap.took(kReqSppUuidRegister), 0);
        CHECK(r.spp.state() == SppService::kRegSent && r.gap.informs.size() == 2 * kInforms);
    }
    {   // a server that came back while the service was away was not asked for the phones' requests: now it is
        Registered r;
        CHECK(r.spp.remove("leaving"));
        r.gap.took(kReqSppUuidDeregister);
        r.gap.reply(kRspSppUuidDeregister, 0);
        r.gap.detach();
        r.gap.attach();
        CHECK(r.gap.sent.empty() && r.gap.informs.size() == kInforms);
        r.spp.again();
        isRegister(r.gap.took(kReqSppUuidRegister), 0);
        CHECK(r.gap.informs.size() == 2 * kInforms);
    }
    {   // and after a registration that the server did not take
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        CHECK(spp.start());
        gap.attach();
        gap.took(kReqSppUuidRegister);
        gap.reply(kRspSppUuidRegister, 5);
        gap.took(kReqSppUuidDeregister);
        gap.reply(kRspSppUuidDeregister, 0);
        gap.took(kReqSppUuidRegister);
        gap.reply(kRspSppUuidRegister, 5);
        CHECK(spp.state() == SppService::kFailed);
        spp.again();
        isRegister(gap.took(kReqSppUuidRegister), 0);
        gap.reply(kRspSppUuidRegister, 0);
        CHECK(spp.state() == SppService::kRegistered && gap.opens == 1 && gap.informs.size() == kInforms);
    }
    {   // a removal the client does not take
        Registered r;
        r.gap.takes = false;
        CHECK(!r.spp.remove("leaving") && r.spp.state() == SppService::kRemoveFailed && r.seen.said("not sent"));
    }

    // 13. Only removing, for a registration that a killed run left behind: no informs, no registration.
    {
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        CHECK(spp.startRemoval() && spp.state() == SppService::kWaiting && gap.sent.empty());
        gap.attach();
        isDeregister(gap.took(kReqSppUuidDeregister));
        CHECK(gap.informs.empty() && spp.state() == SppService::kRemoveSent && seen.said(": requested"));
        gap.reply(kRspSppUuidDeregister, 0);
        CHECK(spp.state() == SppService::kRemoved && gap.sent.empty());
    }
    {   // nothing was registered under the name
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        CHECK(spp.startRemoval());
        gap.attach();
        gap.took(kReqSppUuidDeregister);
        gap.reply(kRspSppUuidDeregister, 5);
        CHECK(spp.state() == SppService::kRemoveFailed);
    }

    // 14. Removing another program's registration: our own stays as it is, and the requests go out one at a time.
    {
        Registered r;
        CHECK(r.spp.removeOther("OtherRadio"));
        CHECK(deregisters(r.gap.took(kReqSppUuidDeregister)) == "OtherRadio");
        CHECK(r.seen.said("SppUuidDeregister name 'OtherRadio': another program's registration, left behind"));
        CHECK(r.spp.removeOther("Second") && r.spp.removeOther("Third"));
        CHECK(r.spp.removeOther("Second") && r.spp.removeOther("OtherRadio"));    // waiting or out already: not twice
        CHECK(r.gap.sent.empty() && r.seen.removed().empty());                    // the first has not been answered
        r.gap.reply(kRspSppUuidDeregister, 0);
        CHECK(r.seen.removed() == "OtherRadio=0" && r.seen.said("SppUuidDeregister reply for 'OtherRadio': 0 OK"));
        CHECK(deregisters(r.gap.took(kReqSppUuidDeregister)) == "Second");
        r.gap.reply(kRspSppUuidDeregister, 10);                                   // whatever the server makes of a name
        CHECK(r.seen.removed() == "Second=10");
        CHECK(r.seen.said("SppUuidDeregister reply for 'Second': 10 INVALID_PARAM (no such name is registered)"));
        CHECK(deregisters(r.gap.took(kReqSppUuidDeregister)) == "Third");
        r.gap.reject(kTypeRejectedA, kReqSppUuidDeregister);
        CHECK(r.seen.removed() == "Third=-1" && r.seen.said("rejected the removal of 'Third'") && r.gap.sent.empty());
        CHECK(r.spp.state() == SppService::kRegistered && r.seen.passed().empty());
        // Answered, so the name can be asked for again.
        CHECK(r.spp.removeOther("OtherRadio"));
        CHECK(deregisters(r.gap.took(kReqSppUuidDeregister)) == "OtherRadio");
        // A reply to a registration, and a phone's request, are not the removal's answer.
        r.gap.reply(kRspSppUuidRegister, 0);
        r.gap.inform(kInfServiceRequest, "OtherRadio");
        CHECK(r.seen.removed().empty());
        r.gap.reply(kRspSppUuidDeregister, 0);
        CHECK(r.seen.removed() == "OtherRadio=0" && r.spp.state() == SppService::kRegistered);
        r.gap.reply(kRspSppUuidDeregister, 0);                                    // an answer nothing waits for
        CHECK(r.seen.removed().empty() && r.spp.state() == SppService::kRegistered && r.gap.sent.empty());
    }
    {   // what is not taken: our own name, a name the stack would not take, and anything with the server gone
        Registered r;
        CHECK(!r.spp.removeOther(kName) && !r.spp.removeOther("") && !r.spp.removeOther("two words"));
        CHECK(!r.spp.removeOther(std::string(33, 'x')) && r.gap.sent.empty());
        r.gap.detach();
        CHECK(!r.spp.removeOther("OtherRadio") && r.gap.sent.empty() && r.seen.removed().empty());
    }
    {   // a request the client does not take is told at once, and the next one is not held up by it
        Registered r;
        r.gap.takes = false;
        CHECK(r.spp.removeOther("OtherRadio") && r.seen.removed() == "OtherRadio=-1");
        r.gap.takes = true;
        CHECK(r.spp.removeOther("Second"));
        CHECK(deregisters(r.gap.took(kReqSppUuidDeregister)) == "Second");
    }
    {   // a removal that is never answered holds the next one back only for so long
        Registered r;
        r.spp.setOtherReplyTime(0.05);
        CHECK(r.spp.removeOther("OtherRadio") && r.spp.removeOther("Second"));
        CHECK(deregisters(r.gap.took(kReqSppUuidDeregister)) == "OtherRadio");
        usleep(100000);
        CHECK(r.spp.removeOther("Third"));                                        // any later request moves it on
        CHECK(r.seen.removed() == "OtherRadio=-1" && r.seen.said("no reply to the removal of 'OtherRadio'"));
        CHECK(deregisters(r.gap.took(kReqSppUuidDeregister)) == "Second");
        r.gap.reply(kRspSppUuidDeregister, 0);
        CHECK(r.seen.removed() == "Second=0" && deregisters(r.gap.took(kReqSppUuidDeregister)) == "Third");
    }
    {   // the server goes away with a removal out and one waiting: both went with it, and nothing is sent later
        Registered r;
        CHECK(r.spp.removeOther("OtherRadio") && r.spp.removeOther("Second"));
        r.gap.took(kReqSppUuidDeregister);
        r.gap.detach();
        r.gap.attach();
        isRegister(r.gap.took(kReqSppUuidRegister), 0);
        r.gap.reply(kRspSppUuidRegister, 0);
        CHECK(r.gap.sent.empty() && r.seen.removed().empty() && r.spp.state() == SppService::kRegistered);
        r.gap.reply(kRspSppUuidDeregister, 0);
        CHECK(r.seen.removed().empty());
    }
    {   // beside a removal of our own. Ours goes out second, so the first answer is the other program's
        Registered r;
        CHECK(r.spp.removeOther("OtherRadio"));
        r.gap.took(kReqSppUuidDeregister);
        CHECK(r.spp.remove("leaving"));
        isDeregister(r.gap.took(kReqSppUuidDeregister));
        r.gap.reply(kRspSppUuidDeregister, 4);
        CHECK(r.seen.removed() == "OtherRadio=4" && r.spp.state() == SppService::kRemoveSent);
        r.gap.reply(kRspSppUuidDeregister, 0);
        CHECK(r.spp.state() == SppService::kRemoved && r.seen.removed().empty());
    }
    {   // and the other way round: while ours waits for its answer, the other program's is held back
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        CHECK(spp.start());
        gap.attach();
        gap.took(kReqSppUuidRegister);
        gap.reply(kRspSppUuidRegister, 5);                                        // ours was left behind too
        isDeregister(gap.took(kReqSppUuidDeregister));
        CHECK(spp.removeOther("OtherRadio") && gap.sent.empty());
        gap.reply(kRspSppUuidDeregister, 0);                                      // ours: the registration is tried again
        CHECK(seen.removed().empty() && gap.sent.size() == 2 && gap.sent[0].id == kReqSppUuidRegister);
        CHECK(gap.sent[1].id == kReqSppUuidDeregister && deregisters(gap.sent[1].payload) == "OtherRadio");
        gap.sent.clear();
        gap.reply(kRspSppUuidRegister, 0);
        gap.reply(kRspSppUuidDeregister, 0);
        CHECK(spp.state() == SppService::kRegistered && seen.removed() == "OtherRadio=0");
    }
    {   // before the service is registered at all, with the server attached: nothing of ours stands in the way
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        CHECK(spp.start());
        gap.attach();
        isRegister(gap.took(kReqSppUuidRegister), 0);
        CHECK(spp.removeOther("OtherRadio"));
        CHECK(deregisters(gap.took(kReqSppUuidDeregister)) == "OtherRadio");
        gap.reply(kRspSppUuidRegister, 0);
        gap.reply(kRspSppUuidDeregister, 0);
        CHECK(spp.state() == SppService::kRegistered && seen.removed() == "OtherRadio=0");
    }

    // 15. ServiceDisconnect: for the phone of the last request for a link that the server told. Its address goes
    //     into the request, and into nothing that is said.
    {
        // Four strings, byte for byte.
        std::vector<uint8_t> d = buildServiceDisconnect("BtCtrl", "ExtBox1", kPhoneAddress, "SPP");
        CHECK(d == bytes("\x07\x00\x00\x00" "BtCtrl\x00\x00"
                         "\x08\x00\x00\x00" "ExtBox1\x00"
                         "\x0d\x00\x00\x00" "AABBCCDDEEFF\x00\x00\x00\x00"
                         "\x04\x00\x00\x00" "SPP\x00", 52));
        Registered r;
        CHECK(!r.spp.knowsPhone() && !r.spp.disconnectPhone("", "", "") && r.gap.sent.empty());   // no phone yet
        r.gap.inform(kInfServiceRequest, "SPP");
        r.gap.inform(kInfServiceRequestComplete, "Another");   // not a request: it changes nothing
        CHECK(r.spp.knowsPhone() && r.spp.disconnectPhone("", "", ""));
        CHECK(r.gap.took(kReqServiceDisconnect) == d);
        CHECK(r.seen.said("ServiceDisconnect device 'BtCtrl', instance 'ExtBox1', service 'SPP', for the phone of the "
                          "last request for a link"));
        // One at a time: the next waits for the answer to this one.
        bool waits = false;
        CHECK(!r.spp.disconnectPhone("Dev2", "Inst2", "Other", &waits) && waits && r.gap.sent.empty());
        std::vector<uint8_t> dr = disconnectReply(kPhoneAddress, "SPP", 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &dr[0], (uint32_t)dr.size());
        CHECK(r.seen.said("ServiceDisconnect reply: 0 OK, reason 0"));
        // Other values, to try them without a new build.
        CHECK(r.spp.disconnectPhone("Dev2", "Inst2", "Other", &waits) && !waits);
        CHECK(r.gap.took(kReqServiceDisconnect) == buildServiceDisconnect("Dev2", "Inst2", kPhoneAddress, "Other"));
        // What comes back that this class does not know: its id, its type and its first value, never its strings.
        std::vector<uint8_t> answer;
        putU32(answer, 3);
        putString(answer, kPhoneAddress);
        putString(answer, kPhoneName);
        r.gap.events->gapMessage(kTypeResponse, 0x80000031u, &answer[0], (uint32_t)answer.size());
        CHECK(r.seen.said("after ServiceDisconnect, a message from the Bluetooth server that the agent does not know: "
                          "id 0x80000031, type 0x200, first value 3 (0x3)"));
        r.gap.events->gapMessage(kTypeResponse, 0x80000032u, &answer[0], 2);
        CHECK(r.seen.said("id 0x80000032, type 0x200, 2 bytes"));
        // A rejection of it, which changes nothing of the registration's.
        r.gap.reject(kTypeRejectedA, kReqServiceDisconnect);
        CHECK(r.seen.said("the Bluetooth server rejected ServiceDisconnect (type 0x202)"));
        CHECK(r.spp.state() == SppService::kRegistered && r.seen.passed().empty());
        // What it knows is not among them.
        r.gap.reply(kRspSppUuidRegister, 0);
        r.gap.reply(kRspSppUuidDeregister, 0);
        size_t unknown = 0;
        for (size_t i = 0; i < r.seen.lines.size(); ++i)
            if (r.seen.lines[i].find("does not know") != std::string::npos) ++unknown;
        CHECK(unknown == 2);
        CHECK(!r.seen.said(kPhoneName) && !r.seen.said(kPhoneAddress));
        // A later phone's request is the one named.
        r.gap.inform(kInfServiceRequest, "Echo");
        CHECK(r.spp.disconnectPhone("", "", ""));
        CHECK(r.gap.took(kReqServiceDisconnect) == buildServiceDisconnect("BtCtrl", "ExtBox1", kPhoneAddress, "Echo"));
        // With the server gone nothing is sent.
        r.gap.detach();
        CHECK(!r.spp.disconnectPhone("", "", "") && r.gap.sent.empty());
    }
    {   // before any ServiceDisconnect, an answer nobody knows is not logged
        Registered r;
        r.gap.reply(0x80000031u, 3);
        CHECK(!r.seen.said("does not know") && r.spp.state() == SppService::kRegistered);
    }
    {   // a request the client does not take
        Registered r;
        r.gap.inform(kInfServiceRequest, "SPP");
        r.gap.takes = false;
        CHECK(!r.spp.disconnectPhone("", "", "") && r.seen.said("not sent"));
    }

    // 17. The ids of replies, which are never asked for as informs.
    CHECK(isResponseId(kRspSppUuidRegister) && isResponseId(kRspSppUuidDeregister) && isResponseId(kRspVersionGet));
    CHECK(isResponseId(kRspDeviceListGet) && isResponseId(kRspServiceConnect) && isResponseId(kRspServiceDisconnect));
    CHECK(isResponseId(0x80000016u) && isResponseId(0x8000005eu));
    CHECK(!isResponseId(0x80000015u) && !isResponseId(0x8000005fu) && !isResponseId(kInfServiceRequest));
    CHECK(!isResponseId(kInfDeviceListChanged) && !isResponseId(kInfServiceDisconnected) &&
          !isResponseId(kInfServiceListChanged) && !isResponseId(0x8000000fu) && !isResponseId(0x2e));

    // 18. The other messages, built and read. Made-up payloads: only the layouts are the server's. Where the server
    //     pads, it pads with leftovers here, and it leaves the padding out after a message's last string.
    {
        std::vector<uint8_t> c = buildServiceConnect("BtCtrl", "testinst", "00:11:22:33:44:55", "Test_Service");
        CHECK(c == bytes("\x07\x00\x00\x00" "BtCtrl\x00\x00"
                         "\x09\x00\x00\x00" "testinst\x00\x00\x00\x00"
                         "\x12\x00\x00\x00" "00:11:22:33:44:55\x00\x00\x00"
                         "\x0d\x00\x00\x00" "Test_Service\x00\x00\x00\x00", 72));

        // ServiceConnect's reply: address, service, error.
        std::vector<uint8_t> r = bytes("\x12\x00\x00\x00" "00:11:22:33:44:55\x00\xa1\xb2"
                                       "\x0d\x00\x00\x00" "Test_Service\x00\xc3\xd4\xe5"
                                       "\x0e\x00\x00\x00", 48);
        ServiceConnectReply cr = parseServiceConnectReply(&r[0], (uint32_t)r.size());
        CHECK(cr.ok && cr.address == "00:11:22:33:44:55" && cr.service == "Test_Service" && cr.error == 14);
        CHECK(!parseServiceConnectReply(&r[0], 44).ok);

        // ServiceDisconnect's reply: address, service, error, reason.
        r = bytes("\x0d\x00\x00\x00" "AABBCCDDEEFF\x00\x99\x99\x99"
                  "\x04\x00\x00\x00" "SPP\x00"
                  "\x00\x00\x00\x00" "\x01\x00\x00\x00", 36);
        ServiceDisconnectReply dr = parseServiceDisconnectReply(&r[0], (uint32_t)r.size());
        CHECK(dr.ok && dr.address == kPhoneAddress && dr.service == "SPP" && dr.error == 0 && dr.reason == 1);
        CHECK(!parseServiceDisconnectReply(&r[0], 32).ok);

        // The device lists of two phones: names (dropped), addresses, class, security, connection.
        std::vector<uint8_t> lists;
        putU32(lists, 2);
        putString(lists, "Test Phone A");
        putString(lists, "Test Phone B");
        putU32(lists, 2);
        putString(lists, "00:11:22:33:44:55");
        putString(lists, "66:77:88:99:AA:BB");
        putU32(lists, 2); putU32(lists, 0x5a020c); putU32(lists, 0x5a020c);
        putU32(lists, 2); putU32(lists, 7); putU32(lists, 9);
        putU32(lists, 2); putU32(lists, kConnConnected); putU32(lists, kConnNotInRange);
        std::vector<uint8_t> reply;
        putU32(reply, 0);
        reply.insert(reply.end(), lists.begin(), lists.end());
        DeviceList l = parseDeviceList(kRspDeviceListGet, &reply[0], (uint32_t)reply.size());
        CHECK(l.ok && l.error == 0 && l.devices.size() == 2);
        CHECK(l.devices[0].address == "00:11:22:33:44:55" && l.devices[0].security == 7 &&
              l.devices[0].connection == kConnConnected && l.devices[0].deviceClass == 0x5a020c);
        CHECK(l.devices[1].address == "66:77:88:99:AA:BB" && l.devices[1].security == 9 &&
              l.devices[1].connection == kConnNotInRange);
        CHECK(!parseDeviceList(kRspDeviceListGet, &reply[0], (uint32_t)reply.size() - 1).ok);
        std::vector<uint8_t> changed = lists;
        putU32(changed, 3);
        l = parseDeviceList(kInfDeviceListChanged, &changed[0], (uint32_t)changed.size());
        CHECK(l.ok && l.devices.size() == 2 && l.after == 3);
        CHECK(!parseDeviceList(kInfDeviceListChanged, &lists[0], (uint32_t)lists.size()).ok);   // its last value missing
        // An error with nothing after it; lists whose counts differ.
        std::vector<uint8_t> e;
        putU32(e, 3);
        l = parseDeviceList(kRspDeviceListGet, &e[0], (uint32_t)e.size());
        CHECK(l.ok && l.error == 3 && l.devices.empty());
        std::vector<uint8_t> odd;
        putU32(odd, 0);
        putU32(odd, 1); putString(odd, "Test Phone A");
        putU32(odd, 1); putString(odd, "00:11:22:33:44:55");
        putU32(odd, 1); putU32(odd, 0);
        putU32(odd, 0);
        putU32(odd, 1); putU32(odd, 2);
        CHECK(!parseDeviceList(kRspDeviceListGet, &odd[0], (uint32_t)odd.size()).ok);
        std::vector<uint8_t> many;
        putU32(many, 0);
        putU32(many, 100000);
        CHECK(!parseDeviceList(kRspDeviceListGet, &many[0], (uint32_t)many.size()).ok);
        CHECK(std::string(connectionName(3)) == "service connected" && std::string(connectionName(7)) == "unknown");
        CHECK(addressKey("00:11:22:aa:BB:cc") == "001122AABBCC" && addressKey("001122AABBCC") == "001122AABBCC");

        // ServiceDisconnected: device, instance, address, service, reason.
        std::vector<uint8_t> d;
        putString(d, "BtCtrl");
        putString(d, "testinst");
        putString(d, "00:11:22:33:44:55");
        putString(d, "Test_Service");
        putU32(d, 1);
        ServiceDisconnected sd = parseServiceDisconnected(&d[0], (uint32_t)d.size());
        CHECK(sd.ok && sd.device == "BtCtrl" && sd.instance == "testinst" && sd.service == "Test_Service" &&
              sd.reason == 1);
        CHECK(!parseServiceDisconnected(&d[0], (uint32_t)d.size() - 4).ok);

        // ServiceListChanged: address, services[], states[], profiles[], versions[], reason.
        std::vector<uint8_t> s;
        putString(s, "00:11:22:33:44:55");
        putU32(s, 2); putString(s, "SPP"); putString(s, "HFP");
        putU32(s, 2); putU32(s, 1); putU32(s, 0);
        putU32(s, 1); putString(s, "Profile");
        putU32(s, 1); putU32(s, 0x0102);
        putU32(s, 4);
        ServiceListChanged sl = parseServiceListChanged(&s[0], (uint32_t)s.size());
        CHECK(sl.ok && sl.services.size() == 2 && sl.services[1] == "HFP" && sl.states[0] == 1 &&
              sl.profiles[0] == "Profile" && sl.versions[0] == 0x0102 && sl.reason == 4);
        CHECK(!parseServiceListChanged(&s[0], (uint32_t)s.size() - 4).ok);

        // ServiceListQuery: the address; its reply: error, address, services[], states[], profiles[], versions[].
        CHECK(kReqServiceListQuery == 0x19 && kRspServiceListQuery == 0x8000002e && isResponseId(kRspServiceListQuery));
        CHECK(buildServiceListQuery("00:11:22:33:44:55") == bytes("\x12\x00\x00\x00" "00:11:22:33:44:55\x00\x00\x00", 24));
        std::vector<uint8_t> q = bytes("\x00\x00\x00\x00"
                                       "\x12\x00\x00\x00" "00:11:22:33:44:55\x00\x5a\x5b"
                                       "\x03\x00\x00\x00"
                                       "\x05\x00\x00\x00" "GATT\x00\x61\x62\x63"
                                       "\x0d\x00\x00\x00" "Test_Service\x00\x71\x72\x73"
                                       "\x04\x00\x00\x00" "SPP\x00"
                                       "\x03\x00\x00\x00" "\x00\x00\x00\x00" "\x00\x00\x00\x00" "\x01\x00\x00\x00"
                                       "\x01\x00\x00\x00" "\x05\x00\x00\x00" "A2DP\x00\x44\x55\x66"
                                       "\x01\x00\x00\x00" "\x04\x01\x00\x00", 112);
        ServiceList ql = parseServiceList(&q[0], (uint32_t)q.size());
        CHECK(ql.ok && ql.error == 0 && ql.address == "00:11:22:33:44:55" && ql.services.size() == 3);
        CHECK(ql.services[1] == kName && ql.states.size() == 3 && ql.states[2] == 1 && ql.profiles.size() == 1 &&
              ql.profiles[0] == "A2DP" && ql.versions.size() == 1 && ql.versions[0] == 0x104);
        CHECK(listsService(ql, kName) && listsService(ql, "SPP") && !listsService(ql, "Test_Servic") &&
              !listsService(ql, "Other_Service"));
        CHECK(!parseServiceList(&q[0], (uint32_t)q.size() - 4).ok && !parseServiceList(&q[0], 30).ok);
        // A last string with its padding left out reads whole.
        std::vector<uint8_t> cut;
        putU32(cut, 0);
        putString(cut, "00:11:22:33:44:55");
        putU32(cut, 1); putString(cut, kName);
        putU32(cut, 1); putU32(cut, 0);
        putU32(cut, 1); putString(cut, "HFP");
        putU32(cut, 0);
        ql = parseServiceList(&cut[0], (uint32_t)cut.size());
        CHECK(ql.ok && listsService(ql, kName) && ql.profiles[0] == "HFP" && ql.versions.empty());
        std::vector<uint8_t> empty;
        putU32(empty, 0);
        putString(empty, "00:11:22:33:44:55");
        putU32(empty, 0); putU32(empty, 0); putU32(empty, 0); putU32(empty, 0);
        ql = parseServiceList(&empty[0], (uint32_t)empty.size());
        CHECK(ql.ok && ql.services.empty() && !listsService(ql, kName));
        // An error with nothing after it: read whole, no address; with lists after it, as any other.
        std::vector<uint8_t> busy;
        putU32(busy, 2);
        ql = parseServiceList(&busy[0], (uint32_t)busy.size());
        CHECK(ql.ok && ql.error == 2 && ql.address.empty() && ql.services.empty());
        busy.insert(busy.end(), empty.begin() + 4, empty.end());
        ql = parseServiceList(&busy[0], (uint32_t)busy.size());
        CHECK(ql.ok && ql.error == 2 && ql.address == "00:11:22:33:44:55");
        // Too many elements is not a list.
        std::vector<uint8_t> huge;
        putU32(huge, 0);
        putString(huge, "00:11:22:33:44:55");
        putU32(huge, 100000);
        CHECK(!parseServiceList(&huge[0], (uint32_t)huge.size()).ok);

        // VersionGet's reply: error, five strings, the last with no padding.
        std::vector<uint8_t> v;
        putU32(v, 0);
        putString(v, "TEST_STACK_1");
        putString(v, "Vendor");
        putString(v, "1.2.3");
        putString(v, "four");
        putString(v, "five5");
        v.resize(v.size() - 2);
        StackVersion sv = parseStackVersion(&v[0], (uint32_t)v.size());
        CHECK(sv.ok && sv.error == 0 && sv.parts.size() == 5 && sv.parts[0] == "TEST_STACK_1" && sv.parts[4] == "five5");
        CHECK(!parseStackVersion(&v[0], 20).ok);
        CHECK(shownText(std::string("ab\x01" "cd", 5), 4) == "ab?c");
    }

    // 19. The new informs and replies on a registration: state kept, sparing lines, never an address or a name.
    {
        Registered r;
        std::vector<uint8_t> lists;
        putU32(lists, 1); putString(lists, kPhoneName);
        putU32(lists, 1); putString(lists, "00:11:22:33:44:55");
        putU32(lists, 1); putU32(lists, 0);
        putU32(lists, 1); putU32(lists, 8);
        putU32(lists, 1); putU32(lists, kConnServiceConnected);
        putU32(lists, 0);
        CHECK(!r.spp.heard().devicesKnown);
        r.gap.events->gapMessage(0x400, kInfDeviceListChanged, &lists[0], (uint32_t)lists.size());
        SppService::Heard h = r.spp.heard();
        CHECK(h.devicesKnown && h.devices.size() == 1 && h.devices[0].security == 8 && h.deviceListChanges == 1);
        CHECK(h.devices[0].connection == kConnServiceConnected);
        // A device list asked for: DeviceListGet, no payload.
        CHECK(r.spp.askDeviceList() && r.gap.took(kReqDeviceListGet).empty());
        std::vector<uint8_t> reply;
        putU32(reply, 0);
        putU32(reply, 0); putU32(reply, 0); putU32(reply, 0); putU32(reply, 0); putU32(reply, 0);
        r.gap.events->gapMessage(kTypeResponse, kRspDeviceListGet, &reply[0], (uint32_t)reply.size());
        h = r.spp.heard();
        CHECK(h.devicesKnown && h.devices.empty() && h.deviceListChanges == 1);
        // A link ended, logged without the address.
        std::vector<uint8_t> d;
        putString(d, "BtCtrl");
        putString(d, "ExtBox1");
        putString(d, kPhoneAddress);
        putString(d, "SPP");
        putU32(d, 1);
        for (int i = 0; i < 10; ++i) r.gap.events->gapMessage(0x400, kInfServiceDisconnected, &d[0], (uint32_t)d.size());
        CHECK(r.seen.said("the Bluetooth stack says a link ended: device 'BtCtrl', instance 'ExtBox1', service 'SPP', "
                          "ended by the phone"));
        CHECK(r.seen.said("Further ends of links are counted and not logged") && r.spp.heard().linksEnded == 10);
        size_t ends = 0;
        for (size_t i = 0; i < r.seen.lines.size(); ++i) ends += r.seen.lines[i].find("a link ended") != std::string::npos;
        CHECK(ends == 8);
        // A service list changed: counted only.
        std::vector<uint8_t> s;
        putString(s, kPhoneAddress);
        putU32(s, 0); putU32(s, 0); putU32(s, 0); putU32(s, 0);
        putU32(s, 0);
        size_t before = r.seen.lines.size();
        r.gap.events->gapMessage(0x400, kInfServiceListChanged, &s[0], (uint32_t)s.size());
        CHECK(r.spp.heard().serviceListChanges == 1 && r.seen.lines.size() == before);
        // One that cannot be read is said once.
        r.gap.events->gapMessage(0x400, kInfServiceListChanged, &s[0], 6);
        r.gap.events->gapMessage(0x400, kInfServiceListChanged, &s[0], 6);
        CHECK(r.seen.lines.size() == before + 1 && r.seen.said("ServiceListChanged from the Bluetooth server could not "
                                                               "be read (id 0x80000008, 6 bytes)"));
        // VersionGet: no payload; its reply is logged when it was asked for, and not when another client's.
        CHECK(r.spp.askVersion() && r.gap.took(kReqVersionGet).empty());
        std::vector<uint8_t> v;
        putU32(v, 0);
        putString(v, "TEST_STACK_1");
        putString(v, "Vendor");
        putString(v, "1.2.3");
        putString(v, "four");
        putString(v, "five");
        r.gap.events->gapMessage(kTypeResponse, kRspVersionGet, &v[0], (uint32_t)v.size());
        CHECK(r.seen.said("the Bluetooth stack's version: 'TEST_STACK_1', 'Vendor', '1.2.3', 'four', 'five'"));
        before = r.seen.lines.size();
        r.gap.events->gapMessage(kTypeResponse, kRspVersionGet, &v[0], (uint32_t)v.size());
        CHECK(r.seen.lines.size() == before);
        // ServiceDisconnect's reply: known now, so logged with its code and reason, and not as unknown.
        r.gap.inform(kInfServiceRequest, "SPP");
        CHECK(r.spp.disconnectPhone("", "", ""));
        r.gap.took(kReqServiceDisconnect);
        std::vector<uint8_t> dr;
        putString(dr, kPhoneAddress);
        putString(dr, "SPP");
        putU32(dr, 0);
        putU32(dr, 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &dr[0], (uint32_t)dr.size());
        CHECK(r.seen.said("ServiceDisconnect reply: 0 OK, reason 0") && !r.seen.said("does not know"));
        // A ServiceConnect reply is another client's: nothing in this agent asks for one.
        before = r.seen.lines.size();
        std::vector<uint8_t> cr;
        putString(cr, kPhoneAddress);
        putString(cr, "Test_Service");
        putU32(cr, 14);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceConnect, &cr[0], (uint32_t)cr.size());
        CHECK(r.seen.lines.size() == before);
        CHECK(!r.seen.said(kPhoneName) && !r.seen.said(kPhoneAddress) && !r.seen.said("00:11:22"));
        CHECK(r.spp.state() == SppService::kRegistered && r.gap.sent.empty());
        // The server goes: what it said of the phones goes with it.
        r.gap.detach();
        CHECK(!r.spp.heard().devicesKnown && !r.spp.askVersion() && !r.spp.askDeviceList());
    }
    {   // a client that cannot send: the subscription's failure is said
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        CHECK(spp.start());
        gap.refuseSubscribe = true;
        gap.attach();
        CHECK(seen.said("the Bluetooth server could not be asked for inform 0x80000009: the request could not be sent"));
        gap.takes = false;
        CHECK(!spp.askVersion() && seen.said("DSI request 0x3 not sent: the send to the Bluetooth server failed"));
    }

    // 20. The registration check: SppUuidRegister for our own name once more. It changes no state, never removes
    //     anything, and says only what is news.
    {
        Registered r;
        size_t lines = r.seen.lines.size();
        // Still there: 5, counted only.
        CHECK(r.spp.checkRegistration(30));
        isRegister(r.gap.took(kReqSppUuidRegister), 0);
        CHECK(!r.spp.checkRegistration(30) && r.gap.sent.empty());   // one at a time
        r.gap.reply(kRspSppUuidRegister, 5);
        CHECK(r.spp.state() == SppService::kRegistered && r.gap.sent.empty() && r.seen.lines.size() == lines);
        CHECK(r.seen.passed().empty());
        // Busy, both ways: counted, and the next round asks again.
        CHECK(r.spp.checkRegistration(30));
        r.gap.took(kReqSppUuidRegister);
        r.gap.reply(kRspSppUuidRegister, 1);
        CHECK(r.spp.checkRegistration(30));
        r.gap.took(kReqSppUuidRegister);
        r.gap.reply(kRspSppUuidRegister, 2);
        CHECK(r.gap.sent.empty() && r.seen.lines.size() == lines && r.spp.state() == SppService::kRegistered);
        // Lost and registered again: logged.
        CHECK(r.spp.checkRegistration(30));
        r.gap.took(kReqSppUuidRegister);
        r.gap.reply(kRspSppUuidRegister, 0);
        CHECK(r.seen.said("the car's Bluetooth had dropped the registration of 'Test_Service', as switching Bluetooth "
                          "off and on in iDrive does, with no word of it: the registration check found it gone and has "
                          "registered it again"));
        CHECK(r.gap.sent.empty() && r.spp.state() == SppService::kRegistered && r.seen.passed().empty());
        // Anything else: logged, and still nothing removed.
        CHECK(r.spp.checkRegistration(30));
        r.gap.took(kReqSppUuidRegister);
        r.gap.reply(kRspSppUuidRegister, 12);
        CHECK(r.seen.said("the registration check of 'Test_Service' was answered 12 HW_NOT_READY"));
        CHECK(r.spp.checkRegistration(30));
        r.gap.took(kReqSppUuidRegister);
        r.gap.reject(kTypeRejectedA, kReqSppUuidRegister);
        CHECK(r.seen.said("the Bluetooth server rejected the registration check (type 0x202)"));
        CHECK(r.gap.sent.empty() && r.spp.state() == SppService::kRegistered && r.seen.passed().empty());
        // No reply: after the time given, counted, said once, and the next check goes.
        CHECK(r.spp.checkRegistration(0.05));
        r.gap.took(kReqSppUuidRegister);
        usleep(80000);
        CHECK(r.spp.checkRegistration(0.05));
        r.gap.took(kReqSppUuidRegister);
        usleep(80000);
        CHECK(r.spp.checkRegistration(0.05));
        r.gap.took(kReqSppUuidRegister);
        size_t silent = 0;
        for (size_t i = 0; i < r.seen.lines.size(); ++i) silent += r.seen.lines[i].find("had no reply") != std::string::npos;
        CHECK(silent == 1);
        r.gap.reply(kRspSppUuidRegister, 5);
        SppService::CheckCounts c = r.spp.checkCounts();
        CHECK(c.sent == 9 && c.present == 2 && c.busy == 2 && c.restored == 1 && c.other == 2 && c.unanswered == 2);
        // The registration log line of the start-up path is not among what a check says.
        size_t registers = 0;
        for (size_t i = 0; i < r.seen.lines.size(); ++i) registers += r.seen.lines[i].find("SppUuidRegister") != std::string::npos;
        CHECK(registers == 2);   // the start-up's request and its reply
        // The server goes with a check out: the check goes with it, and none goes while it is away.
        CHECK(r.spp.checkRegistration(30));
        r.gap.took(kReqSppUuidRegister);
        r.gap.detach();
        CHECK(!r.spp.checkRegistration(30) && r.gap.sent.empty());
        // It comes back: the start-up path registers, and a 5 there is still the start-up path's.
        r.gap.attach();
        r.gap.took(kReqSppUuidRegister);
        r.gap.reply(kRspSppUuidRegister, 0);
        CHECK(r.spp.state() == SppService::kRegistered && r.spp.checkRegistration(30));
        r.gap.took(kReqSppUuidRegister);
        r.gap.reply(kRspSppUuidRegister, 5);
        CHECK(r.gap.sent.empty() && r.spp.state() == SppService::kRegistered);
    }
    // 20a. An answer that keeps coming is said once: a stack that gives it every round writes no line every 30 s.
    {
        struct Count {
            static size_t of(const Seen& seen, const char* part) {
                size_t n = 0;
                for (size_t i = 0; i < seen.lines.size(); ++i) n += seen.lines[i].find(part) != std::string::npos;
                return n;
            }
        };
        Registered r;
        // The same odd answer three times: one line.
        for (int i = 0; i < 3; ++i) {
            CHECK(r.spp.checkRegistration(30));
            r.gap.took(kReqSppUuidRegister);
            r.gap.reply(kRspSppUuidRegister, 12);
        }
        CHECK(Count::of(r.seen, "was answered 12 HW_NOT_READY") == 1);
        CHECK(r.seen.said("the registration check of 'Test_Service' was answered 12 HW_NOT_READY. The same answer "
                          "again is only counted"));
        SppService::CheckCounts c = r.spp.checkCounts();
        CHECK(c.other == 3 && c.lastOdd == 12 && c.lastOddRejected == 0 && c.lastOddTimes == 3);
        // Another odd answer is news; the same rejection twice is said once.
        for (int i = 0; i < 2; ++i) {
            CHECK(r.spp.checkRegistration(30));
            r.gap.took(kReqSppUuidRegister);
            r.gap.reject(kTypeRejectedA, kReqSppUuidRegister);
        }
        CHECK(Count::of(r.seen, "rejected the registration check (type 0x202)") == 1);
        c = r.spp.checkCounts();
        CHECK(c.other == 5 && c.lastOddRejected == kTypeRejectedA && c.lastOddTimes == 2);
    }
    {   // odd, present, odd: two lines
        Registered r;
        const uint32_t answers[] = { 12, 5, 12 };
        for (int i = 0; i < 3; ++i) {
            CHECK(r.spp.checkRegistration(30));
            r.gap.took(kReqSppUuidRegister);
            r.gap.reply(kRspSppUuidRegister, answers[i]);
        }
        size_t n = 0;
        for (size_t i = 0; i < r.seen.lines.size(); ++i)
            n += r.seen.lines[i].find("was answered 12 HW_NOT_READY") != std::string::npos;
        CHECK(n == 2);
        SppService::CheckCounts c = r.spp.checkCounts();
        CHECK(c.other == 2 && c.present == 1 && c.lastOdd == 12 && c.lastOddTimes == 1);
    }
    {   // four restores in a row: the restored line, then once that it keeps being dropped; a 5 starts it afresh
        Registered r;
        size_t lines = r.seen.lines.size();
        for (int i = 0; i < 4; ++i) {
            CHECK(r.spp.checkRegistration(30));
            r.gap.took(kReqSppUuidRegister);
            r.gap.reply(kRspSppUuidRegister, 0);
        }
        CHECK(r.seen.lines.size() == lines + 2);
        CHECK(r.seen.lines[lines].find("had dropped the registration of 'Test_Service'") != std::string::npos);
        CHECK(r.seen.lines[lines + 1] == "the registration of 'Test_Service' keeps being dropped: the check has found it "
                                         "gone and registered it again 2 times with no check finding it there in between. "
                                         "From now on this is only counted");
        CHECK(r.spp.checkCounts().restored == 4);
        // Found there, then dropped again: news once more.
        CHECK(r.spp.checkRegistration(30));
        r.gap.took(kReqSppUuidRegister);
        r.gap.reply(kRspSppUuidRegister, 5);
        CHECK(r.spp.checkRegistration(30));
        r.gap.took(kReqSppUuidRegister);
        r.gap.reply(kRspSppUuidRegister, 0);
        CHECK(r.seen.lines.size() == lines + 3 && r.seen.lines[lines + 2].find("had dropped the registration") != std::string::npos);
        // Busy between restores also starts it afresh.
        CHECK(r.spp.checkRegistration(30));
        r.gap.took(kReqSppUuidRegister);
        r.gap.reply(kRspSppUuidRegister, 2);
        CHECK(r.spp.checkRegistration(30));
        r.gap.took(kReqSppUuidRegister);
        r.gap.reply(kRspSppUuidRegister, 0);
        CHECK(r.seen.lines.size() == lines + 4 && r.seen.lines[lines + 3].find("had dropped the registration") != std::string::npos);
        CHECK(r.spp.checkCounts().lastOddTimes == 0);
    }
    {   // only while registered: never while the start-up's registration waits, and never after a removal
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        CHECK(!spp.checkRegistration(30));
        CHECK(spp.start());
        gap.attach();
        gap.took(kReqSppUuidRegister);
        CHECK(!spp.checkRegistration(30) && gap.sent.empty());
        gap.reply(kRspSppUuidRegister, 5);                 // the start-up path: cleared, then tried again
        isDeregister(gap.took(kReqSppUuidDeregister));
        CHECK(!spp.checkRegistration(30) && gap.sent.empty());
        gap.reply(kRspSppUuidDeregister, 0);
        gap.took(kReqSppUuidRegister);
        gap.reply(kRspSppUuidRegister, 0);
        CHECK(spp.state() == SppService::kRegistered);
        CHECK(spp.remove("leaving"));
        gap.took(kReqSppUuidDeregister);
        CHECK(!spp.checkRegistration(30));
        gap.reply(kRspSppUuidDeregister, 0);
        CHECK(!spp.checkRegistration(30) && gap.sent.empty());
    }
    {   // a check whose reply comes after a removal went out does not take the removal's place
        Registered r;
        CHECK(r.spp.checkRegistration(30));
        r.gap.took(kReqSppUuidRegister);
        CHECK(r.spp.remove("leaving"));
        r.gap.took(kReqSppUuidDeregister);
        r.gap.reply(kRspSppUuidRegister, 5);
        CHECK(r.spp.state() == SppService::kRemoveSent);
        r.gap.reply(kRspSppUuidDeregister, 0);
        CHECK(r.spp.state() == SppService::kRemoved);
    }

    // 21. A refusal of a link: what the car's device list says of that phone, asked for when the listener wants it.
    {
        Registered r;
        // The device list of two phones, the refused one second and in another form of its address.
        std::vector<uint8_t> reply;
        putU32(reply, 0);
        putU32(reply, 2); putString(reply, "Test Phone A"); putString(reply, kPhoneName);
        putU32(reply, 2); putString(reply, "66:77:88:99:AA:BB"); putString(reply, "aa:bb:cc:dd:ee:ff");
        putU32(reply, 2); putU32(reply, 0); putU32(reply, 0);
        putU32(reply, 2); putU32(reply, 7); putU32(reply, 8);
        putU32(reply, 2); putU32(reply, kConnInRange); putU32(reply, kConnConnected);
        r.gap.inform(kInfServiceRequestError, "SPP");
        CHECK(r.spp.askAboutRefused() && r.gap.took(kReqDeviceListGet).empty());
        // A device list that changed is not the answer.
        std::vector<uint8_t> changed(reply.begin() + 4, reply.end());
        putU32(changed, 0);
        r.gap.events->gapMessage(0x400, kInfDeviceListChanged, &changed[0], (uint32_t)changed.size());
        CHECK(r.seen.refused.empty());
        r.gap.events->gapMessage(kTypeResponse, kRspDeviceListGet, &reply[0], (uint32_t)reply.size());
        CHECK(r.seen.refused.size() == 1);
        PhoneInList p = r.seen.refused[0];
        CHECK(p.ok && p.error == 0 && p.known && p.listed && p.connection == kConnConnected && p.security == 8);
        // Once: a later list is not for it.
        r.gap.events->gapMessage(kTypeResponse, kRspDeviceListGet, &reply[0], (uint32_t)reply.size());
        CHECK(r.seen.refused.size() == 1);
        // A phone that is not in the list.
        std::vector<uint8_t> empty;
        putU32(empty, 0);
        putU32(empty, 0); putU32(empty, 0); putU32(empty, 0); putU32(empty, 0); putU32(empty, 0);
        CHECK(r.spp.askAboutRefused());
        r.gap.took(kReqDeviceListGet);
        r.gap.events->gapMessage(kTypeResponse, kRspDeviceListGet, &empty[0], (uint32_t)empty.size());
        CHECK(r.seen.refused.size() == 2 && r.seen.refused[1].ok && !r.seen.refused[1].listed);
        // The list refused, or rejected.
        std::vector<uint8_t> error;
        putU32(error, 5);
        CHECK(r.spp.askAboutRefused());
        r.gap.took(kReqDeviceListGet);
        r.gap.events->gapMessage(kTypeResponse, kRspDeviceListGet, &error[0], (uint32_t)error.size());
        CHECK(r.seen.refused.size() == 3 && r.seen.refused[2].ok && r.seen.refused[2].error == 5);
        CHECK(r.spp.askAboutRefused());
        r.gap.took(kReqDeviceListGet);
        r.gap.reject(kTypeRejectedA, kReqDeviceListGet);
        CHECK(r.seen.refused.size() == 4 && !r.seen.refused[3].ok);
        CHECK(!r.seen.said(kPhoneName) && !r.seen.said(kPhoneAddress) && !r.seen.said("aa:bb"));
        // phoneInList on its own: the forms of an address are one phone.
        DeviceList l = parseDeviceList(kRspDeviceListGet, &reply[0], (uint32_t)reply.size());
        CHECK(phoneInList(l, "66778899AABB").listed && phoneInList(l, "66778899AABB").security == 7);
        CHECK(!phoneInList(l, "00:00:00:00:00:01").listed && !phoneInList(l, "").known);
        // With the server gone, nothing is asked.
        r.gap.detach();
        CHECK(!r.spp.askAboutRefused());
    }

    // 22. The car dials: ServiceConnect, and its answer. A reply that names another service or another phone is
    //     another client's, even while ours waits: ours stays out, and its own answer is still taken.
    {
        struct Reply {
            static std::vector<uint8_t> of(const char* address, const char* service, uint32_t error) {
                std::vector<uint8_t> p;
                putString(p, address);
                putString(p, service);
                putU32(p, error);
                return p;
            }
        };
        Registered r;
        CHECK(r.spp.connectService("BtCtrl", "testinst", "00:11:22:33:44:55", kName));
        CHECK(r.gap.took(kReqServiceConnect) == buildServiceConnect("BtCtrl", "testinst", "00:11:22:33:44:55", kName));
        CHECK(!r.spp.connectService("BtCtrl", "testinst", "00:11:22:33:44:55", kName) && r.gap.sent.empty());
        size_t before = r.seen.lines.size();
        // Another service, then another phone: not passed on, and the dial is still out.
        std::vector<uint8_t> p = Reply::of("00:11:22:33:44:55", "Other_Service", 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceConnect, &p[0], (uint32_t)p.size());
        p = Reply::of("66:77:88:99:AA:BB", kName, 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceConnect, &p[0], (uint32_t)p.size());
        CHECK(r.seen.connects.empty() && r.spp.heard().othersConnectReplies == 2);
        CHECK(!r.spp.connectService("BtCtrl", "testinst", "00:11:22:33:44:55", kName) && r.gap.sent.empty());
        CHECK(r.seen.lines.size() == before + 2 &&
              r.seen.lines[before] == "a ServiceConnect reply for another client came while the agent's dial waited for "
                                      "its answer, and was left: the dial waits on");
        // Ours, with the address in another form: passed on, and the dial is answered.
        p = Reply::of("001122334455", kName, 5);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceConnect, &p[0], (uint32_t)p.size());
        CHECK(r.seen.connects.size() == 1 && r.seen.connects[0].ok && r.seen.connects[0].error == 5 &&
              r.seen.connects[0].service == kName);
        // With no dial out, a reply is another client's, as before: ignored, and not logged.
        before = r.seen.lines.size();
        r.gap.events->gapMessage(kTypeResponse, kRspServiceConnect, &p[0], (uint32_t)p.size());
        p = Reply::of("66:77:88:99:AA:BB", kName, 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceConnect, &p[0], (uint32_t)p.size());
        CHECK(r.seen.connects.size() == 1 && r.seen.lines.size() == before && r.spp.heard().othersConnectReplies == 2);
        // Another client's that keep coming: the first few are said, then they are only counted.
        CHECK(r.spp.connectService("BtCtrl", "testinst", "00:11:22:33:44:55", kName));
        r.gap.took(kReqServiceConnect);
        for (int i = 0; i < 10; ++i)
            r.gap.events->gapMessage(kTypeResponse, kRspServiceConnect, &p[0], (uint32_t)p.size());
        CHECK(r.seen.connects.size() == 1 && r.spp.heard().othersConnectReplies == 12);
        size_t said = 0;
        for (size_t i = 0; i < r.seen.lines.size(); ++i)
            said += r.seen.lines[i].find("for another client") != std::string::npos;
        CHECK(said == 4 && r.seen.said("and was left: the dial waits on. Further ones are counted and not logged"));
        // One that cannot be read is the answer, as before: passed on with what was asked for, and kNoError.
        p = Reply::of("00:11:22:33:44:55", kName, 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceConnect, &p[0], (uint32_t)p.size() - 2);
        CHECK(r.seen.connects.size() == 2 && !r.seen.connects[1].ok && r.seen.connects[1].error == kNoError &&
              r.seen.connects[1].address == "00:11:22:33:44:55" && r.seen.connects[1].service == kName);
        CHECK(r.seen.said("ServiceConnect's reply from the Bluetooth server could not be read"));
        CHECK(r.spp.connectService("BtCtrl", "testinst", "00:11:22:33:44:55", kName));
        r.gap.took(kReqServiceConnect);
        // A rejection is the answer, as before.
        r.gap.reject(kTypeRejectedA, kReqServiceConnect);
        CHECK(r.seen.connects.size() == 3 && !r.seen.connects[2].ok && r.seen.connects[2].error == kNoError &&
              r.seen.connects[2].address == "00:11:22:33:44:55");
        CHECK(r.seen.said("the Bluetooth server rejected ServiceConnect (type 0x202)"));
        CHECK(r.spp.connectService("BtCtrl", "testinst", "00:11:22:33:44:55", kName));
        CHECK(!r.seen.said("00:11:22") && !r.seen.said("66:77:88") && !r.seen.said("001122"));
    }

    // 23. The pairing informs: asked for with setPairingInforms(), the two that stop dials only. PairingComplete,
    //     a pairing that worked, is not asked for, and one that comes all the same is neither told nor logged.
    {
        FakeGap gap;
        Seen seen;
        SppService spp(&gap, &seen);
        spp.configure(kName, kUuid, 0);
        spp.setPairingInforms(true);
        CHECK(spp.start());
        gap.attach();
        CHECK(gap.informs.size() == kInforms + 2 && gap.informs[kInforms] == kInfPairingError &&
              gap.informs[kInforms + 1] == kInfSimplePairingConfirmationRequired);
        gap.took(kReqSppUuidRegister);
        gap.reply(kRspSppUuidRegister, 0);
        std::vector<uint8_t> p;
        putString(p, "00:11:22:33:44:55");
        putString(p, kPhoneName);
        size_t before = seen.lines.size();
        gap.events->gapMessage(0x400, kInfPairingComplete, &p[0], (uint32_t)p.size());
        CHECK(seen.pairings.empty());
        gap.events->gapMessage(0x400, kInfPairingError, &p[0], (uint32_t)p.size());
        gap.events->gapMessage(0x400, kInfSimplePairingConfirmationRequired, &p[0], (uint32_t)p.size());
        gap.events->gapMessage(0x400, kInfPairingError, 0, 0);
        CHECK(seen.pairings.size() == 3 && seen.pairings[0].first == kInfPairingError &&
              seen.pairings[0].second == "00:11:22:33:44:55");
        CHECK(seen.pairings[1].first == kInfSimplePairingConfirmationRequired && seen.pairings[2].second.empty());
        // Not as an unknown message after a ServiceDisconnect either, whatever its type.
        gap.inform(kInfServiceRequest, "SPP");
        CHECK(spp.disconnectPhone("", "", ""));
        gap.took(kReqServiceDisconnect);
        gap.events->gapMessage(kTypeResponse, kInfPairingComplete, &p[0], (uint32_t)p.size());
        gap.events->gapMessage(0x400, kInfPairingComplete, &p[0], (uint32_t)p.size());
        CHECK(seen.pairings.size() == 3 && !seen.said("does not know"));
        CHECK(!seen.said(kPhoneName) && !seen.said("00:11:22"));
        CHECK(seen.lines.size() == before + 1);   // the ServiceDisconnect's own line
    }
    {   // without setPairingInforms(): none is asked for
        Registered r;
        CHECK(r.gap.informs.size() == kInforms);
    }

    // 24. ServiceDisconnect's answer. One at a time, of either kind; a reply that names another phone, or for
    //     disconnectService() another service, is another client's, even while ours waits: ours stays out, and its
    //     own answer is still taken. One with no answer in its time is given up when the next is asked for.
    {   // disconnectService()'s
        Registered r;
        CHECK(r.spp.disconnectService("BtCtrl", "nbtapp", "00:11:22:33:44:55", kName));
        CHECK(r.gap.took(kReqServiceDisconnect) == buildServiceDisconnect("BtCtrl", "nbtapp", "00:11:22:33:44:55", kName));
        CHECK(!r.spp.disconnectService("BtCtrl", "nbtapp", "66:77:88:99:AA:BB", kName) && r.gap.sent.empty());
        size_t before = r.seen.lines.size();
        // Another phone, then another service: not passed on, not logged as ours, and ours is still out.
        std::vector<uint8_t> p = disconnectReply("66:77:88:99:AA:BB", kName, 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &p[0], (uint32_t)p.size());
        p = disconnectReply("00:11:22:33:44:55", "Other_Service", 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &p[0], (uint32_t)p.size());
        CHECK(r.seen.disconnects.empty() && r.spp.heard().othersDisconnectReplies == 2);
        CHECK(r.seen.lines.size() == before + 2 &&
              r.seen.lines[before] == "a ServiceDisconnect reply for another client came while the agent's "
                                      "ServiceDisconnect waited for its answer, and was left: the agent's waits on");
        CHECK(!r.seen.said("ServiceDisconnect reply:"));
        CHECK(!r.spp.disconnectService("BtCtrl", "nbtapp", "66:77:88:99:AA:BB", kName) && r.gap.sent.empty());
        // Ours, with the address in another form: passed on and logged, and the next may go.
        p = disconnectReply("001122334455", kName, 5, 6);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &p[0], (uint32_t)p.size());
        CHECK(r.seen.disconnects.size() == 1 && r.seen.disconnects[0].ok && r.seen.disconnects[0].error == 5 &&
              r.seen.disconnects[0].service == kName);
        CHECK(r.seen.said("ServiceDisconnect reply: 5 DEFAULT, reason 6"));
        // With none out, a reply is another client's, as before: ignored, and not logged.
        before = r.seen.lines.size();
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &p[0], (uint32_t)p.size());
        p = disconnectReply("66:77:88:99:AA:BB", kName, 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &p[0], (uint32_t)p.size());
        CHECK(r.seen.disconnects.size() == 1 && r.seen.lines.size() == before &&
              r.spp.heard().othersDisconnectReplies == 2);
        // Another client's that keep coming: the first few are said, then they are only counted.
        CHECK(r.spp.disconnectService("BtCtrl", "nbtapp", "00:11:22:33:44:55", kName));
        r.gap.took(kReqServiceDisconnect);
        for (int i = 0; i < 10; ++i)
            r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &p[0], (uint32_t)p.size());
        CHECK(r.seen.disconnects.size() == 1 && r.spp.heard().othersDisconnectReplies == 12);
        size_t said = 0;
        for (size_t i = 0; i < r.seen.lines.size(); ++i)
            said += r.seen.lines[i].find("for another client") != std::string::npos;
        CHECK(said == 4 && r.seen.said("and was left: the agent's waits on. Further ones are counted and not logged"));
        // One that cannot be read is the answer, as before: passed on with what was asked for, and kNoError.
        p = disconnectReply("00:11:22:33:44:55", kName, 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &p[0], (uint32_t)p.size() - 2);
        CHECK(r.seen.disconnects.size() == 2 && !r.seen.disconnects[1].ok &&
              r.seen.disconnects[1].error == kNoError && r.seen.disconnects[1].address == "00:11:22:33:44:55" &&
              r.seen.disconnects[1].service == kName);
        CHECK(r.seen.said("ServiceDisconnect's reply from the Bluetooth server could not be read"));
        CHECK(r.spp.disconnectService("BtCtrl", "nbtapp", "00:11:22:33:44:55", kName));
        r.gap.took(kReqServiceDisconnect);
        // A rejection is the answer, as before.
        r.gap.reject(kTypeRejectedA, kReqServiceDisconnect);
        CHECK(r.seen.disconnects.size() == 3 && !r.seen.disconnects[2].ok &&
              r.seen.disconnects[2].error == kNoError && r.seen.disconnects[2].address == "00:11:22:33:44:55");
        CHECK(r.seen.said("the Bluetooth server rejected ServiceDisconnect (type 0x202)"));
        CHECK(r.spp.disconnectService("BtCtrl", "nbtapp", "00:11:22:33:44:55", kName));
        CHECK(!r.seen.said("00:11:22") && !r.seen.said("66:77:88") && !r.seen.said("001122"));
    }
    {   // disconnectPhone()'s: another phone's reply is not its answer; the service is not compared
        Registered r;
        r.gap.inform(kInfServiceRequest, "SPP");
        CHECK(r.spp.disconnectPhone("", "", ""));
        r.gap.took(kReqServiceDisconnect);
        std::vector<uint8_t> p = disconnectReply("66:77:88:99:AA:BB", "SPP", 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &p[0], (uint32_t)p.size());
        CHECK(!r.seen.said("ServiceDisconnect reply:") && r.seen.said("a ServiceDisconnect reply for another client"));
        CHECK(r.spp.heard().othersDisconnectReplies == 1);
        bool waits = false;
        CHECK(!r.spp.disconnectPhone("", "", "", &waits) && waits && r.gap.sent.empty());
        p = disconnectReply("AA:BB:CC:DD:EE:FF", "Another_Service", 10, 3);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &p[0], (uint32_t)p.size());
        CHECK(r.seen.said("ServiceDisconnect reply: 10 ") && r.seen.said(", reason 3"));
        CHECK(r.seen.disconnects.empty());   // never told: only disconnectService()'s is
        CHECK(r.spp.disconnectPhone("", "", "", &waits) && !waits);
        CHECK(!r.seen.said(kPhoneAddress) && !r.seen.said("AA:BB:CC") && !r.seen.said("66:77:88"));
    }
    {   // the two kinds wait for each other, and neither takes the other's answer
        Registered r;
        r.gap.inform(kInfServiceRequest, "SPP");
        CHECK(r.spp.disconnectService("BtCtrl", "nbtapp", "00:11:22:33:44:55", kName));
        r.gap.took(kReqServiceDisconnect);
        size_t before = r.seen.lines.size();
        bool waits = false;
        CHECK(!r.spp.disconnectPhone("", "", "", &waits) && waits && r.gap.sent.empty());
        CHECK(r.seen.lines.size() == before);   // the caller says what it does
        std::vector<uint8_t> p = disconnectReply("00:11:22:33:44:55", kName, 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &p[0], (uint32_t)p.size());
        CHECK(r.seen.disconnects.size() == 1 && r.seen.disconnects[0].error == 0);
        // And the other way round.
        CHECK(r.spp.disconnectPhone("", "", "", &waits) && !waits);
        r.gap.took(kReqServiceDisconnect);
        CHECK(!r.spp.disconnectService("BtCtrl", "nbtapp", "00:11:22:33:44:55", kName) && r.gap.sent.empty());
        p = disconnectReply(kPhoneAddress, "SPP", 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &p[0], (uint32_t)p.size());
        CHECK(r.seen.disconnects.size() == 1);
        CHECK(r.spp.disconnectService("BtCtrl", "nbtapp", "00:11:22:33:44:55", kName));
        r.gap.took(kReqServiceDisconnect);
        // The server goes: no answer comes from it, and none waits.
        r.gap.detach();
        r.gap.attach();
        r.gap.took(kReqSppUuidRegister);
        r.gap.reply(kRspSppUuidRegister, 0);
        CHECK(r.spp.disconnectPhone("", "", ""));
    }
    {   // no answer: given up when the next is asked for after its time; a late answer for another phone is not
        // taken for the next one's
        Registered r;
        r.spp.setDisconnectReplyTime(0.1);
        r.gap.inform(kInfServiceRequest, "SPP");
        CHECK(r.spp.disconnectService("BtCtrl", "nbtapp", "00:11:22:33:44:55", kName));
        r.gap.took(kReqServiceDisconnect);
        bool waits = false;
        CHECK(!r.spp.disconnectPhone("", "", "", &waits) && waits);
        usleep(150000);
        CHECK(r.spp.disconnectPhone("", "", "", &waits) && !waits);
        r.gap.took(kReqServiceDisconnect);
        CHECK(!r.seen.said("given up"));   // the dial's own wait says so of disconnectService()'s
        std::vector<uint8_t> p = disconnectReply("00:11:22:33:44:55", kName, 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &p[0], (uint32_t)p.size());
        CHECK(r.seen.disconnects.empty() && !r.seen.said("ServiceDisconnect reply:") &&
              r.spp.heard().othersDisconnectReplies == 1);
        CHECK(!r.spp.disconnectService("BtCtrl", "nbtapp", "00:11:22:33:44:55", kName) && r.gap.sent.empty());
        // disconnectPhone()'s with no answer: given up the same way, and said, since nothing else says it.
        usleep(150000);
        CHECK(r.spp.disconnectService("BtCtrl", "nbtapp", "00:11:22:33:44:55", kName));
        r.gap.took(kReqServiceDisconnect);
        CHECK(r.seen.said("ServiceDisconnect had no reply in 0.1 s, and is given up"));
        p = disconnectReply(kPhoneAddress, "SPP", 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &p[0], (uint32_t)p.size());
        CHECK(r.seen.disconnects.empty() && r.spp.heard().othersDisconnectReplies == 2);
        // The answer of the one out is taken.
        p = disconnectReply("00:11:22:33:44:55", kName, 0);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceDisconnect, &p[0], (uint32_t)p.size());
        CHECK(r.seen.disconnects.size() == 1);
        // Before its time, nothing is given up.
        r.spp.setDisconnectReplyTime(30);
        CHECK(r.spp.disconnectService("BtCtrl", "nbtapp", "00:11:22:33:44:55", kName));
        r.gap.took(kReqServiceDisconnect);
        CHECK(!r.spp.disconnectPhone("", "", "", &waits) && waits && r.gap.sent.empty());
    }

    // 25. ServiceListQuery: sent with the phone's address; every reply of its id is told, whoever's query it answers;
    //     one that names no phone, cannot be read, or a rejection names the phone of ours that waits, and a rejection
    //     says that it is one. Nothing is logged but a rejection and the first reply that cannot be read, and never an
    //     address.
    {
        struct Reply {
            static std::vector<uint8_t> of(const char* address, uint32_t error, const char* service) {
                std::vector<uint8_t> p;
                putU32(p, error);
                putString(p, address);
                putU32(p, service ? 1 : 0);
                if (service) putString(p, service);
                putU32(p, service ? 1 : 0);
                if (service) putU32(p, 0);
                putU32(p, 0);
                putU32(p, 0);
                return p;
            }
        };
        Registered r;
        size_t before = r.seen.lines.size();
        CHECK(r.spp.queryServices("00:11:22:33:44:55"));
        CHECK(r.gap.took(kReqServiceListQuery) == buildServiceListQuery("00:11:22:33:44:55"));
        CHECK(r.seen.lines.size() == before);
        // Another phone's, then ours with the address in another form: both told, as they came.
        std::vector<uint8_t> p = Reply::of("66:77:88:99:AA:BB", 0, kName);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceListQuery, &p[0], (uint32_t)p.size());
        p = Reply::of("001122334455", 0, kName);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceListQuery, &p[0], (uint32_t)p.size());
        CHECK(r.seen.lists.size() == 2 && r.seen.lists[0].address == "66:77:88:99:AA:BB" && r.seen.lists[1].ok &&
              r.seen.lists[1].error == 0 && listsService(r.seen.lists[1], kName));
        // With none of ours out, one that names no phone names nobody.
        std::vector<uint8_t> e;
        putU32(e, 2);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceListQuery, &e[0], (uint32_t)e.size());
        CHECK(r.seen.lists.size() == 3 && r.seen.lists[2].ok && r.seen.lists[2].error == 2 && r.seen.lists[2].address.empty());
        // With ours out: an error with no address names ours.
        CHECK(r.spp.queryServices("00:11:22:33:44:55"));
        r.gap.took(kReqServiceListQuery);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceListQuery, &e[0], (uint32_t)e.size());
        CHECK(r.seen.lists.size() == 4 && r.seen.lists[3].error == 2 && r.seen.lists[3].address == "00:11:22:33:44:55");
        // One that cannot be read: kNoError, ours, said once.
        CHECK(r.spp.queryServices("00:11:22:33:44:55"));
        r.gap.took(kReqServiceListQuery);
        p = Reply::of("00:11:22:33:44:55", 0, kName);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceListQuery, &p[0], 10);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceListQuery, &p[0], 10);
        CHECK(r.seen.lists.size() == 6 && !r.seen.lists[4].ok && r.seen.lists[4].error == kNoError &&
              r.seen.lists[4].address == "00:11:22:33:44:55" && r.seen.lists[4].services.empty() &&
              !r.seen.lists[4].rejected);
        CHECK(r.seen.lists[5].address.empty());   // ours was answered by the one before
        CHECK(r.seen.said("ServiceListQuery's reply from the Bluetooth server could not be read"));
        // A rejection: kNoError, ours, and told apart from a reply that cannot be read.
        CHECK(r.spp.queryServices("00:11:22:33:44:55"));
        r.gap.took(kReqServiceListQuery);
        r.gap.reject(kTypeRejectedA, kReqServiceListQuery);
        CHECK(r.seen.lists.size() == 7 && r.seen.lists[6].error == kNoError && r.seen.lists[6].address == "00:11:22:33:44:55");
        CHECK(r.seen.lists[6].rejected && !r.seen.lists[6].ok);
        CHECK(!r.seen.lists[0].rejected && !r.seen.lists[2].rejected && !r.seen.lists[3].rejected);
        CHECK(r.seen.said("the Bluetooth server rejected ServiceListQuery (type 0x202)"));
        // Not as an unknown answer after a ServiceDisconnect.
        r.gap.inform(kInfServiceRequest, "SPP");
        CHECK(r.spp.disconnectPhone("", "", ""));
        r.gap.took(kReqServiceDisconnect);
        p = Reply::of("66:77:88:99:AA:BB", 0, kName);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceListQuery, &p[0], (uint32_t)p.size());
        CHECK(!r.seen.said("does not know") && r.seen.lists.size() == 8);
        // The server goes: what was out goes with it; a request is not sent while it is away.
        CHECK(r.spp.queryServices("00:11:22:33:44:55"));
        r.gap.took(kReqServiceListQuery);
        r.gap.detach();
        CHECK(!r.spp.queryServices("00:11:22:33:44:55") && r.gap.sent.empty());
        r.gap.up = true;
        r.gap.events->gapMessage(kTypeResponse, kRspServiceListQuery, &e[0], (uint32_t)e.size());
        CHECK(r.seen.lists.size() == 9 && r.seen.lists[8].address.empty());
        // A request the client does not take.
        r.gap.takes = false;
        CHECK(!r.spp.queryServices("00:11:22:33:44:55") && r.seen.said("DSI request 0x19 not sent"));
        CHECK(!r.seen.said("00:11:22") && !r.seen.said("66:77:88") && !r.seen.said("001122"));
    }

    // 26. A ServiceConnect that is never answered is given up with giveUpConnect(), for what it named only, and the
    //     next may go; an answer to it that comes after is another client's.
    {
        struct Reply {
            static std::vector<uint8_t> of(const char* address, const char* service, uint32_t error) {
                std::vector<uint8_t> p;
                putString(p, address);
                putString(p, service);
                putU32(p, error);
                return p;
            }
        };
        Registered r;
        CHECK(!r.spp.giveUpConnect("02:00:00:00:00:01", "Test_Other"));   // none waits
        CHECK(r.spp.connectService("BtCtrl", "testinst", "02:00:00:00:00:01", "Test_Other"));
        r.gap.took(kReqServiceConnect);
        CHECK(!r.spp.connectService("BtCtrl", "testinst", "00:11:22:33:44:55", kName) && r.gap.sent.empty());
        // Not for another phone or service than the one that waits: that one stays out.
        CHECK(!r.spp.giveUpConnect("00:11:22:33:44:55", "Test_Other"));
        CHECK(!r.spp.giveUpConnect("02:00:00:00:00:01", kName));
        CHECK(!r.spp.connectService("BtCtrl", "testinst", "00:11:22:33:44:55", kName) && r.gap.sent.empty());
        // The address in another form is the same.
        size_t lines = r.seen.lines.size();
        CHECK(r.spp.giveUpConnect("020000000001", "Test_Other"));
        CHECK(!r.spp.giveUpConnect("02:00:00:00:00:01", "Test_Other") && r.seen.lines.size() == lines);
        // Its late answer with none of ours out: dropped, unsaid and untold.
        std::vector<uint8_t> late = Reply::of("02:00:00:00:00:01", "Test_Other", 5);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceConnect, &late[0], (uint32_t)late.size());
        CHECK(r.seen.connects.empty() && r.seen.lines.size() == lines && r.spp.heard().othersConnectReplies == 0);
        // The next goes; the late answer that comes while it waits is another client's, and that one is still out.
        CHECK(r.spp.connectService("BtCtrl", "testinst", "00:11:22:33:44:55", kName));
        r.gap.took(kReqServiceConnect);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceConnect, &late[0], (uint32_t)late.size());
        CHECK(r.seen.connects.empty() && r.spp.heard().othersConnectReplies == 1);
        CHECK(!r.spp.connectService("BtCtrl", "testinst", "02:00:00:00:00:01", "Test_Other") && r.gap.sent.empty());
        std::vector<uint8_t> own = Reply::of("00:11:22:33:44:55", kName, 14);
        r.gap.events->gapMessage(kTypeResponse, kRspServiceConnect, &own[0], (uint32_t)own.size());
        CHECK(r.seen.connects.size() == 1 && r.seen.connects[0].error == 14 && r.seen.connects[0].service == kName);
        CHECK(!r.seen.said("00:11:22") && !r.seen.said("02:00:00"));
    }

    // 16. The real client is the head unit's: anywhere else it does not open, and nothing can be sent through it.
#ifndef __QNXNTO__
    {
        DsiGapClient real;
        CHECK(real.describe() == "BSS_GAP.SGBluetoothGap 35.0");
        real.configure(36, 2, false);
        CHECK(real.describe() == "BSS_GAP.SGBluetoothGap 36.2" && !real.attached());
        CHECK(!real.request(kReqSppUuidDeregister, buildDeregister(kName)) && !real.subscribe(kInfServiceRequest));
        CHECK(!real.subscribe(kRspSppUuidRegister));
        Seen seen;
        SppService spp(&real, &seen);
        spp.configure(kName, kUuid, 0);
        if (sizeof(void*) != 4) CHECK(!spp.start());
    }
#endif

    printf("test_spp_service: ok\n");
    return 0;
}
