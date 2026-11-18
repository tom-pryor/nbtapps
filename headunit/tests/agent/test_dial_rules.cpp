// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/dial_rules.cpp: when the car dials a phone of the agent's, and what comes of each answer. The
// time is the test's own, in milliseconds. All addresses are made up.

#include "../../agent/dial_rules.hpp"

#include "../../agent/spp_service.hpp"
#include "test_util.hpp"

using namespace nbt;
using namespace nbt::btspp;

static const char* const kOne = "00:11:22:33:44:55";
static const char* const kTwo = "66:77:88:99:AA:BB";
static const char* const kService = "NBTApp";

static std::string num(size_t n) {
    char b[32];
    snprintf(b, sizeof b, "%u", (unsigned)n);
    return b;
}

static BtDevice device(const char* address, uint32_t connection, uint32_t security) {
    BtDevice d;
    d.address = address;
    d.connection = connection;
    d.security = security;
    return d;
}

static DialConfig config() {
    DialConfig c;
    return c;
}

// Facts with the service registered, no link up, and `ours` as the agent's phones.
static DialRules::Facts facts(const char* ours1 = kOne, const char* ours2 = 0) {
    DialRules::Facts f;
    f.registered = true;
    if (ours1 != 0) f.ours.push_back(ours1);
    if (ours2 != 0) f.ours.push_back(ours2);
    return f;
}

// Rules with phone one, and phone two if asked, connected and paired with the car.
static void present(DialRules& r, int64_t now, bool two = false, uint32_t connection = kConnConnected, uint32_t security = 7) {
    std::vector<BtDevice> list;
    list.push_back(device(kOne, connection, security));
    if (two) list.push_back(device(kTwo, kConnConnected, 8));
    r.devices(list, now, 0, 0);
}

// A forced dial of phone one, out at `now`.
static DialRules::Dial dialOne(DialRules& r, int64_t now) {
    CHECK(r.force() >= 1);
    DialRules::Dial d;
    CHECK(r.next(now, facts(), &d));
    CHECK(d.address == kOne && r.state() == DialRules::kDialling);
    return d;
}

static void testAnswers() {
    // What each answer asks for, and what it leaves of the phone.
    struct Row {
        uint32_t error;
        DialRules::Reply reply;
        DialRules::State after;
        bool heldBack;       // polls leave the phone alone afterwards
    } rows[] = {
        {0, DialRules::kOpenNode, DialRules::kUp, false},
        {2, DialRules::kLookForNode, DialRules::kIdle, false},
        {5, DialRules::kNotListening, DialRules::kIdle, false},
        {14, DialRules::kUnreachable, DialRules::kIdle, true},
        {1, DialRules::kRetryLater, DialRules::kIdle, true},
        {3, DialRules::kRetryLater, DialRules::kIdle, true},
        {15, DialRules::kRetryLater, DialRules::kIdle, true},
        {kNoError, DialRules::kRetryLater, DialRules::kIdle, true},   // a rejection
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
        char what[32];
        snprintf(what, sizeof what, "answer %u", rows[i].error);
        DialRules r(config());
        present(r, 1000);
        DialRules::Dial d = dialOne(r, 2000);
        // Not this dial's answer: another service, another phone.
        CHECK_IN(what, r.reply(kOne, "SPP", kService, rows[i].error, 2100) == DialRules::kNotThis);
        CHECK_IN(what, r.reply(kTwo, kService, kService, rows[i].error, 2100) == DialRules::kNotThis);
        CHECK_IN(what, r.state() == DialRules::kDialling);
        // The address in another form is the same phone.
        CHECK_IN(what, r.reply("001122334455", kService, kService, rows[i].error, 2500) == rows[i].reply);
        CHECK_IN(what, r.state() == rows[i].after && r.heldBack(kOne, 2500) == rows[i].heldBack);
        // A second answer to the same dial counts no more.
        CHECK_IN(what, r.reply(kOne, kService, kService, rows[i].error, 2600) == DialRules::kNotThis);
    }
    {   // an answer not understood holds the phone back for retryMs
        DialRules r(config());
        present(r, 1000);
        dialOne(r, 2000);
        CHECK(r.reply(kOne, kService, kService, 1, 3000) == DialRules::kRetryLater);
        CHECK(r.heldBack(kOne, 3000 + 15000 - 1) && !r.heldBack(kOne, 3000 + 15000));
    }
    {   // 14: held back until the phone's connection changes in the device list
        DialRules r(config());
        present(r, 1000);
        dialOne(r, 2000);
        CHECK(r.reply(kOne, kService, kService, 14, 7000) == DialRules::kUnreachable);
        CHECK(r.heldBack(kOne, 1000000));
        present(r, 8000);   // the same list: no change
        CHECK(r.heldBack(kOne, 1000000));
        present(r, 9000, false, kConnInRange);
        present(r, 9500, false, kConnConnected);
        CHECK(!r.heldBack(kOne, 9500));
    }
    {   // no answer at all: overdue after replyLimitMs, and given up with a back-off
        DialRules r(config());
        present(r, 1000);
        dialOne(r, 2000);
        CHECK(!r.overdue(2000 + 95000 - 1) && r.overdue(2000 + 95000));
        r.gaveUp(97000);
        CHECK(r.state() == DialRules::kIdle && r.backedOffUntil(kOne) == 97000 + 30000);
        CHECK(r.reply(kOne, kService, kService, 0, 98000) == DialRules::kNotThis);   // too late
    }
    {   // 2 and no node: as an answer not understood; 2 with the node: stale, ended, and then dialled again at once
        DialRules r(config());
        present(r, 1000);
        dialOne(r, 2000);
        CHECK(r.reply(kOne, kService, kService, 2, 2100) == DialRules::kLookForNode);
        r.notThere(2200);
        CHECK(r.state() == DialRules::kIdle && r.heldBack(kOne, 2200) && !r.heldBack(kOne, 2200 + 15000));
        DialRules::Dial d;
        CHECK(!r.next(2300, facts(), &d));
        dialOne(r, 20000);
        CHECK(r.reply(kOne, kService, kService, 2, 20100) == DialRules::kLookForNode);
        r.stale();
        CHECK(r.state() == DialRules::kEnding);
        CHECK(!r.next(20200, facts(), &d));   // nothing while it is being ended
        r.ended(20500);
        CHECK(r.next(20600, facts(), &d) && d.address == kOne && d.why == DialRules::kAfresh);
        CHECK(r.reply(kOne, kService, kService, 5, 20700) == DialRules::kNotListening);
        CHECK(!r.next(20800, facts(), &d));   // once only
    }
}

static void testBackOff() {
    DialRules r(config());
    present(r, 1000);
    // Something answers on the agent's UUID that is not the app: 30 s, then doubled each time, up to 5 minutes.
    int64_t expect[] = {30000, 60000, 120000, 240000, 300000, 300000};
    int64_t now = 2000;
    for (size_t i = 0; i < sizeof expect / sizeof expect[0]; ++i) {
        dialOne(r, now);
        CHECK(r.reply(kOne, kService, kService, 0, now + 500) == DialRules::kOpenNode);
        CHECK(r.nextBackOffMs(kOne) == expect[i]);
        r.notTheApp(now + 3500);
        CHECK(r.state() == DialRules::kEnding);
        CHECK_IN(num(i), r.backedOffUntil(kOne) == now + 3500 + expect[i]);
        CHECK(r.heldBack(kOne, now + 3500 + expect[i] - 1) && !r.heldBack(kOne, now + 3500 + expect[i]));
        r.ended(now + 4000);
        now += 1000000;
    }
    // A link that works takes it away.
    dialOne(r, now);
    CHECK(r.reply(kOne, kService, kService, 0, now + 500) == DialRules::kOpenNode);
    r.linkUp(now + 600);
    CHECK(r.state() == DialRules::kUp && r.backedOffUntil(kOne) == 0 && r.nextBackOffMs(kOne) == 30000);
    r.ending();
    CHECK(r.state() == DialRules::kEnding);
    r.ended(now + 10000);
    // So does the phone's connection changing.
    now += 100000;
    dialOne(r, now);
    r.reply(kOne, kService, kService, 0, now + 500);
    r.notTheApp(now + 3500);
    r.ended(now + 4000);
    dialOne(r, now + 100000);
    r.reply(kOne, kService, kService, 0, now + 100500);
    r.notTheApp(now + 103500);
    r.ended(now + 104000);
    CHECK(r.nextBackOffMs(kOne) == 120000 && r.heldBack(kOne, now + 104000));
    present(r, now + 105000, false, kConnServiceConnected);
    CHECK(!r.heldBack(kOne, now + 105000) && r.nextBackOffMs(kOne) == 30000);
    // A back-off holds back polls only: a forced dial goes all the same.
    dialOne(r, now + 106000);
    r.reply(kOne, kService, kService, 0, now + 106500);
    r.notTheApp(now + 109500);
    r.ended(now + 110000);
    CHECK(r.heldBack(kOne, now + 110000));
    dialOne(r, now + 110001);
}

static void testWhoAndWhen() {
    {   // nothing before the device list, while unregistered, while a link of ours is up, or while a dial is out
        DialRules r(config());
        DialRules::Dial d;
        CHECK(r.force() == 0 && !r.next(1000, facts(), &d));
        present(r, 1000, true);
        CHECK(r.force() == 2);
        DialRules::Facts f = facts();
        f.registered = false;
        CHECK(!r.next(1100, f, &d));
        f = facts();
        f.linkOfOurs = true;
        CHECK(!r.next(1100, f, &d));
        // One at a time, in the order of their numbers, the address always in the colon form.
        CHECK(r.next(1200, facts(), &d) && d.address == kOne && d.number == 1 && d.why == DialRules::kForced && d.at == 1200);
        CHECK(!r.next(1300, facts(), &d) && r.current().address == kOne);
        CHECK(r.reply(kOne, kService, kService, 5, 1400) == DialRules::kNotListening);
        CHECK(r.next(1500, facts(), &d) && d.address == kTwo && d.number == 2);
        CHECK(r.reply(kTwo, kService, kService, 5, 1600) == DialRules::kNotListening);
        CHECK(!r.next(1700, facts(), &d));
    }
    {   // the colon form, from a device list that gives the address otherwise
        DialRules r(config());
        std::vector<BtDevice> list(1, device("aabbccddeeff", kConnConnected, 7));
        r.devices(list, 1000, 0, 0);
        DialRules::Dial d;
        CHECK(r.force() == 1 && r.next(1000, facts("AA:BB:CC:DD:EE:FF"), &d) && d.address == "AA:BB:CC:DD:EE:FF");
    }
    {   // who may be dialled: connected, paired with the car with its key, and the agent's or the pairing window open
        struct Row {
            uint32_t connection, security;
            bool ours, pairingOpen, eligible, forced;
        } rows[] = {
            {kConnConnected, 7, true, false, true, true},
            {kConnServiceConnected, 8, true, false, true, true},
            {kConnInRange, 7, true, false, false, false},
            {kConnNotInRange, 7, true, true, false, false},
            {kConnConnected, 0, true, false, false, false},
            {kConnConnected, 9, true, true, false, false},
            {kConnConnected, 6, true, false, false, false},
            {kConnConnected, 7, false, false, false, true},
            {kConnConnected, 7, false, true, true, true},
        };
        for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            DialRules r(config());
            std::vector<BtDevice> list(1, device(kOne, rows[i].connection, rows[i].security));
            r.devices(list, 1000, 0, 0);
            DialRules::Facts f = facts(rows[i].ours ? kOne : kTwo);
            f.pairingOpen = rows[i].pairingOpen;
            CHECK_IN(num(i), r.eligible(r.presence().phones()[0], f) == rows[i].eligible);
            CHECK_IN(num(i), r.eligibleCount(f) == (rows[i].eligible ? 1u : 0u));
            CHECK_IN(num(i), (r.force() == 1) == rows[i].forced);
        }
    }
    {   // the service goes: nothing known, and a dial that was out is lost
        DialRules r(config());
        present(r, 1000);
        dialOne(r, 2000);
        r.serviceGone();
        CHECK(r.state() == DialRules::kIdle && !r.presence().known());
        DialRules::Dial d;
        CHECK(r.force() == 0 && !r.next(3000, facts(), &d));
        CHECK(r.reply(kOne, kService, kService, 0, 3000) == DialRules::kNotThis);
    }
}

static void testPairingTrouble() {
    {   // an inform that names the phone; the phone is not dialled again until it is at 7 or 8 after a change
        DialRules r(config());
        present(r, 1000, true);
        CHECK(r.pairingTrouble(kTwo) == 2 && r.troubled(kTwo));
        CHECK(r.pairingTrouble("66778899aabb") == 0);   // said once
        CHECK(r.force() == 1);
        DialRules::Dial d;
        CHECK(r.next(1100, facts(kOne, kTwo), &d) && d.address == kOne && !r.next(1200, facts(kOne, kTwo), &d));
        r.reply(kOne, kService, kService, 5, 1300);
        CHECK(!r.eligible(*r.presence().find(kTwo), facts(kOne, kTwo)) && r.eligibleCount(facts(kOne, kTwo)) == 1);
        present(r, 2000, true);   // the same list: no change
        CHECK(r.troubled(kTwo));
        std::vector<BtDevice> list;
        list.push_back(device(kOne, kConnConnected, 7));
        list.push_back(device(kTwo, kConnServiceConnected, 8));
        r.devices(list, 3000, 0, 0);
        CHECK(!r.troubled(kTwo) && r.force() == 2);
    }
    {   // an inform that names no phone stands for the one being dialled; none being dialled: nobody
        DialRules r(config());
        present(r, 1000);
        CHECK(r.pairingTrouble("") == 0 && r.pairingTrouble("not an address") == 0 && !r.troubled(kOne));
        dialOne(r, 2000);
        CHECK(r.pairingTrouble("") == 1 && r.troubled(kOne));
        // The dial's answer still comes, after which nobody is dialled.
        CHECK(r.reply(kOne, kService, kService, 5, 84000) == DialRules::kNotListening);
        CHECK(r.force() == 0);
    }
    {   // security 9 in the device list: the same, said once; back at 7 after a change, dialled again
        DialRules r(config());
        present(r, 1000);
        std::vector<unsigned> troubled;
        std::vector<BtDevice> list(1, device(kOne, kConnServiceConnected, 9));
        r.devices(list, 2000, 0, &troubled);
        CHECK(troubled.size() == 1 && troubled[0] == 1 && r.troubled(kOne) && r.force() == 0);
        troubled.clear();
        list[0].connection = kConnConnected;
        r.devices(list, 3000, 0, &troubled);
        CHECK(troubled.empty() && r.troubled(kOne));
        list[0].security = 7;
        r.devices(list, 4000, 0, &troubled);
        CHECK(!r.troubled(kOne) && r.force() == 1);
    }
}

int main() {
    testAnswers();
    testBackOff();
    testWhoAndWhen();
    testPairingTrouble();
    printf("test_dial_rules: ok\n");
    return 0;
}
