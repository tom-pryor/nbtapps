// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/spp_service.hpp: the UUID and name checks, the SppUuidRegister / SppUuidDeregister payloads,
// the reply and inform readers, and the registration itself, run against a Bluetooth server that this test plays.
// All names, addresses and UUIDs are invented; only the payload layout is the Bluetooth server's.

#include "../../agent/spp_service.hpp"

#include <stdio.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "test_util.hpp"

using namespace nbt::btspp;

static const char* const kName = "Test_Service";
static const char* const kUuid = "0123ABCD-89EF-4A5B-8C7D-0123456789AB";
static const char* const kPhoneName = "Test Phone";
static const char* const kPhoneAddress = "AABBCCDDEEFF";

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

    FakeGap() : events(0), up(false), canOpen(true), takes(true), opens(0) {}

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
        if (!up) return false;
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
    bool up, canOpen, takes;
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
    };

    virtual void sppSay(const std::string& line) { lines.push_back(line); }
    virtual void sppState(SppService::State now) { states.push_back(now); }
    virtual void sppPhone(SppService::Phone what, const std::string& service, bool parsed) {
        PhoneEvent e;
        e.what = what;
        e.service = service;
        e.parsed = parsed;
        phone.push_back(e);
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
    CHECK(std::string(gapErrorName(0)) == "OK" && std::string(gapErrorName(4)) == "REFUSED");
    CHECK(std::string(gapErrorName(99)) == "FAILED");

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
        CHECK(gap.informs.size() == 3 && gap.informs[0] == kInfServiceRequest &&
              gap.informs[1] == kInfServiceRequestComplete && gap.informs[2] == kInfServiceRequestError);
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
        CHECK(r.gap.informs.size() == 6);                 // the informs are asked for again
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
        CHECK(r.gap.sent.empty() && r.gap.informs.size() == 3 && r.spp.state() == SppService::kRemoved);
    }
    {   // the server says no
        Registered r;
        CHECK(r.spp.remove("leaving"));
        r.gap.took(kReqSppUuidDeregister);
        r.gap.reply(kRspSppUuidDeregister, 4);
        CHECK(r.spp.state() == SppService::kRemoveFailed);
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
        CHECK(r.seen.passed() == path(SppService::kWaiting, SppService::kRegSent) && r.gap.informs.size() == 3);
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
        CHECK(r.spp.state() == SppService::kRegSent && r.gap.informs.size() == 6);
    }
    {   // a server that came back while the service was away was not asked for the phones' requests: now it is
        Registered r;
        CHECK(r.spp.remove("leaving"));
        r.gap.took(kReqSppUuidDeregister);
        r.gap.reply(kRspSppUuidDeregister, 0);
        r.gap.detach();
        r.gap.attach();
        CHECK(r.gap.sent.empty() && r.gap.informs.size() == 3);
        r.spp.again();
        isRegister(r.gap.took(kReqSppUuidRegister), 0);
        CHECK(r.gap.informs.size() == 6);
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
        CHECK(spp.state() == SppService::kRegistered && gap.opens == 1 && gap.informs.size() == 3);
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
        r.gap.reply(kRspSppUuidDeregister, 5);                                    // whatever the server makes of a name
        CHECK(r.seen.removed() == "Second=5");
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

    // 15. The real client is the head unit's: anywhere else it does not open, and nothing can be sent through it.
#ifndef __QNXNTO__
    {
        DsiGapClient real;
        CHECK(real.describe() == "BSS_GAP.SGBluetoothGap 35.0");
        real.configure(36, 2, false);
        CHECK(real.describe() == "BSS_GAP.SGBluetoothGap 36.2" && !real.attached());
        CHECK(!real.request(kReqSppUuidDeregister, buildDeregister(kName)) && !real.subscribe(kInfServiceRequest));
        Seen seen;
        SppService spp(&real, &seen);
        spp.configure(kName, kUuid, 0);
        if (sizeof(void*) != 4) CHECK(!spp.start());
    }
#endif

    printf("test_spp_service: ok\n");
    return 0;
}
