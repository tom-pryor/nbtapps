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

// The defaults, with no settle time: the tests of what came before the ask leave it out.
static DialConfig plain() {
    DialConfig c;
    c.settleMs = 0;
    return c;
}

// The tests of the answers and the rules leave polls out: none comes for a day.
static DialConfig config() {
    DialConfig c = plain();
    c.pollFirstMs = c.pollLaterMs = 86400000;
    return c;
}

struct Quiet : DialRules {
    Quiet() : DialRules(config()) { linkEnded(0); }
};

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

// The ask next() gives, answered at once with the agent's service listed: the dial is out. `d` is the ask's.
static bool dialNext(DialRules& r, int64_t now, const DialRules::Facts& f, DialRules::Dial* d) {
    if (!r.next(now, f, d)) return false;
    CHECK(r.state() == DialRules::kAsking);
    return r.listing(d->address, 0, true, f, now) == DialRules::kDialNow && r.state() == DialRules::kDialling;
}

// A forced dial of phone one, out at `now`.
static DialRules::Dial dialOne(DialRules& r, int64_t now) {
    CHECK(r.force() >= 1);
    DialRules::Dial d;
    CHECK(dialNext(r, now, facts(), &d));
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
        Quiet r;
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
        Quiet r;
        present(r, 1000);
        dialOne(r, 2000);
        CHECK(r.reply(kOne, kService, kService, 1, 3000) == DialRules::kRetryLater);
        CHECK(r.heldBack(kOne, 3000 + 15000 - 1) && !r.heldBack(kOne, 3000 + 15000));
    }
    {   // 14: held back until the phone's connection changes in the device list
        Quiet r;
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
        Quiet r;
        present(r, 1000);
        dialOne(r, 2000);
        CHECK(!r.overdue(2000 + 95000 - 1) && r.overdue(2000 + 95000));
        r.gaveUp(97000);
        CHECK(r.state() == DialRules::kIdle && r.backedOffUntil(kOne) == 97000 + 30000);
        CHECK(r.reply(kOne, kService, kService, 0, 98000) == DialRules::kNotThis);   // too late
    }
    {   // 2 and no node: as an answer not understood; 2 with the node: stale, ended, and then dialled again at once
        Quiet r;
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
        CHECK(dialNext(r, 20600, facts(), &d) && d.address == kOne && d.why == DialRules::kAfresh);
        CHECK(r.reply(kOne, kService, kService, 5, 20700) == DialRules::kNotListening);
        CHECK(!r.next(20800, facts(), &d));   // once only
    }
    {   // 2 with the node there, and the car does not end that link: not dialled again at once, and backed off,
        // twice as long each time; one that does end is dialled again at once
        Quiet r;
        present(r, 1000);
        dialOne(r, 2000);
        CHECK(r.reply(kOne, kService, kService, 2, 2100) == DialRules::kLookForNode);
        r.stale();
        r.notEnded(5100);
        CHECK(r.state() == DialRules::kIdle && r.backedOffUntil(kOne) == 5100 + 30000 && r.heldBack(kOne, 5100));
        DialRules::Dial d;
        CHECK(!r.next(5200, facts(), &d) && !r.next(40000, facts(), &d));
        dialOne(r, 50000);
        CHECK(r.reply(kOne, kService, kService, 2, 50100) == DialRules::kLookForNode);
        r.stale();
        r.notEnded(53000);
        CHECK(r.backedOffUntil(kOne) == 53000 + 60000 && !r.next(53100, facts(), &d));
        dialOne(r, 60000);
        CHECK(r.reply(kOne, kService, kService, 2, 60100) == DialRules::kLookForNode);
        r.stale();
        r.ended(61000);
        CHECK(dialNext(r, 61100, facts(), &d) && d.why == DialRules::kAfresh);
    }
}

static void testOvertaken() {
    // The car ends a phone's request for a link while the agent's dial is out, or within overtakeMs of its answer 0:
    // the dial overtook it.
    Quiet r;
    present(r, 1000);
    CHECK(!r.overtakes(1000));
    dialOne(r, 2000);
    CHECK(r.overtakes(2050) && r.overtakes(90000));
    CHECK(r.reply(kOne, kService, kService, 0, 2500) == DialRules::kOpenNode);
    CHECK(r.overtakes(2500) && r.overtakes(3499) && !r.overtakes(3500));
    r.linkUp(2600);
    r.ending();
    r.ended(5000);
    CHECK(!r.overtakes(5000));
    // A dial answered otherwise: only while it was out.
    dialOne(r, 6000);
    CHECK(r.overtakes(6050));
    CHECK(r.reply(kOne, kService, kService, 5, 6100) == DialRules::kNotListening);
    CHECK(!r.overtakes(6100) && !r.overtakes(6200));
}

static void testBackOff() {
    Quiet r;
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
        Quiet r;
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
        CHECK(r.force() == 2);   // that ended the round
        // One at a time, in the order of their numbers, the address always in the colon form.
        CHECK(dialNext(r, 1200, facts(), &d) && d.address == kOne && d.number == 1 && d.why == DialRules::kForced && d.at == 1200);
        CHECK(!r.next(1300, facts(), &d) && r.current().address == kOne);
        CHECK(r.reply(kOne, kService, kService, 5, 1400) == DialRules::kNotListening);
        CHECK(dialNext(r, 1500, facts(), &d) && d.address == kTwo && d.number == 2);
        CHECK(r.reply(kTwo, kService, kService, 5, 1600) == DialRules::kNotListening);
        CHECK(!r.next(1700, facts(), &d));
    }
    {   // a link of the agent's ends a round: one that the dial brought, or one the phone opened
        Quiet r;
        present(r, 1000, true);
        DialRules::Dial d;
        CHECK(r.force() == 2 && dialNext(r, 1100, facts(), &d) && d.address == kOne);
        CHECK(r.reply(kOne, kService, kService, 0, 1200) == DialRules::kOpenNode);
        r.linkUp(1300);
        r.ending();
        r.ended(2000);
        CHECK(!r.next(2100, facts(), &d));
        CHECK(r.force() == 2);
        DialRules::Facts f = facts();
        f.linkOfOurs = true;
        CHECK(!r.next(2200, f, &d) && !r.next(2300, facts(), &d));
    }
    {   // afresh: a round of the eligible phones only
        Quiet r;
        present(r, 1000, true);
        r.afresh();
        DialRules::Dial d;
        CHECK(dialNext(r, 1100, facts(kTwo), &d) && d.address == kTwo && d.why == DialRules::kAfresh);
        r.reply(kTwo, kService, kService, 5, 1200);
        CHECK(!r.next(1300, facts(kTwo), &d));
    }
    {   // the colon form, from a device list that gives the address otherwise
        Quiet r;
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
            Quiet r;
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
        Quiet r;
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
        Quiet r;
        present(r, 1000, true);
        CHECK(r.pairingTrouble(kTwo) == 2 && r.troubled(kTwo));
        CHECK(r.pairingTrouble("66778899aabb") == 0);   // said once
        CHECK(r.force() == 1);
        DialRules::Dial d;
        CHECK(dialNext(r, 1100, facts(kOne, kTwo), &d) && d.address == kOne && !r.next(1200, facts(kOne, kTwo), &d));
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
        Quiet r;
        present(r, 1000);
        CHECK(r.pairingTrouble("") == 0 && r.pairingTrouble("not an address") == 0 && !r.troubled(kOne));
        dialOne(r, 2000);
        CHECK(r.pairingTrouble("") == 1 && r.troubled(kOne));
        // The dial's answer still comes, after which nobody is dialled.
        CHECK(r.reply(kOne, kService, kService, 5, 84000) == DialRules::kNotListening);
        CHECK(r.force() == 0);
    }
    {   // security 9 in the device list: the same, said once; back at 7 after a change, dialled again
        Quiet r;
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

// Answers the dial that is out with 5, as a phone that does not take the link.
static void notListening(DialRules& r, const DialRules::Dial& d, int64_t now) {
    CHECK(r.reply(d.address, kService, kService, 5, now) == DialRules::kNotListening);
}

static void testTriggers() {
    {   // polls: the first at the first chance, then every 30 s; every 10 s for 2 minutes after a phone connects or
        // a link of the agent's ends
        DialRules r((plain()));
        DialRules::Dial d;
        CHECK(!r.next(0, facts(), &d));   // no device list yet
        present(r, 1000);
        CHECK(dialNext(r, 1000, facts(), &d) && d.why == DialRules::kPoll && d.address == kOne);
        notListening(r, d, 1200);
        CHECK(r.nextPollAt() == 1000 + 30000);
        CHECK(!r.next(30999, facts(), &d) && dialNext(r, 31000, facts(), &d) && d.why == DialRules::kPoll);
        notListening(r, d, 31100);
        // The phone leaves and connects again: every 10 s from then, for 2 minutes.
        present(r, 40000, false, kConnInRange);
        present(r, 41000, false, kConnConnected);
        CHECK(r.nextPollAt() == 51000);
        int64_t now = 51000;
        while (now < 41000 + 120000) {
            CHECK_IN(num((size_t)now), dialNext(r, now, facts(), &d) && d.why == DialRules::kPoll);
            notListening(r, d, now + 100);
            CHECK(!r.next(now + 9999, facts(), &d));
            now += 10000;
        }
        CHECK(dialNext(r, now, facts(), &d));
        notListening(r, d, now + 100);
        CHECK(r.nextPollAt() == now + 30000);
        // A link of the agent's ends: the same.
        r.linkEnded(500000);
        CHECK(r.nextPollAt() == 510000);
        // Polls leave out a phone that is held back, and one that is not the agent's.
        CHECK(dialNext(r, 510000, facts(), &d));
        CHECK(r.reply(kOne, kService, kService, 14, 515000) == DialRules::kUnreachable);
        CHECK(!r.next(520000, facts(), &d) && !r.next(700000, facts(), &d));
        present(r, 700000, false, kConnServiceConnected);
        CHECK(dialNext(r, 730000, facts(), &d) && d.address == kOne);
    }
    {   // the phone's own request: dialled once its time has run with no link of the agent's, whatever holds it back
        DialRules r((plain()));
        present(r, 1000);
        DialRules::Dial d;
        CHECK(dialNext(r, 1000, facts(), &d));
        CHECK(r.reply(kOne, kService, kService, 0, 1500) == DialRules::kOpenNode);
        r.notTheApp(4500);
        r.ended(5000);
        CHECK(r.heldBack(kOne, 6000));
        r.phoneAsked("001122334455", 6000);
        CHECK(!r.next(6999, facts(), &d));
        DialRules::Facts busy = facts();
        busy.linkOfOurs = true;           // its own link being decided on: no dial meanwhile
        CHECK(!r.next(7000, busy, &d));
        CHECK(dialNext(r, 7100, facts(), &d) && d.why == DialRules::kSignal && d.address == kOne);
        notListening(r, d, 7200);
        CHECK(!r.next(7300, facts(), &d));   // once
        // A link of the agent's that comes answers it.
        r.phoneAsked(kOne, 8000);
        r.linkCame();
        CHECK(!r.next(9000, facts(), &d));
        // Not for a phone that is not eligible.
        r.phoneAsked(kOne, 10000);
        CHECK(!r.next(11000, facts(kTwo), &d));
        // Nor for a request with no address.
        r.phoneAsked("", 12000);
        CHECK(!r.next(13000, facts(), &d));
    }
    {   // a request that is still pending holds back polls, retries, the pairing window and a round, and not the dial
        // of the phone that asked, which goes signalWaitMs after it
        DialRules r((plain()));
        present(r, 1000, true);
        DialRules::Facts pending = facts(kOne, kTwo);
        pending.requestPending = true;
        pending.pairingOpen = true;
        DialRules::Dial d;
        CHECK(r.force() == 2);
        CHECK(!r.next(1000, pending, &d));   // the round, the first poll and the pairing window wait
        r.phoneAsked(kTwo, 2000);
        CHECK(!r.next(2999, pending, &d));
        CHECK(dialNext(r, 3000, pending, &d) && d.why == DialRules::kSignal && d.address == kTwo);
        notListening(r, d, 3100);
        CHECK(!r.next(3200, pending, &d));
        // Once it is not pending, the rest go; the forced round was ended by it.
        DialRules::Facts f = facts(kOne, kTwo);
        f.pairingOpen = true;
        CHECK(dialNext(r, 3300, f, &d) && d.why == DialRules::kPairing && d.address == kOne);
        // A link of the agent's being decided on holds back that dial too.
        notListening(r, d, 3400);
        r.phoneAsked(kOne, 4000);
        pending.linkOfOurs = true;
        CHECK(!r.next(5000, pending, &d));
    }
    {   // the pairing window: every phone there, the agent's or not, every 5 s, until it closes
        DialRules r((plain()));
        present(r, 1000, true);
        DialRules::Dial d;
        DialRules::Facts f = facts(0);
        CHECK(dialNext(r, 1000, f, &d) == false);   // a poll: none of them is the agent's
        f.pairingOpen = true;
        CHECK(dialNext(r, 2000, f, &d) && d.why == DialRules::kPairing && d.address == kOne);
        notListening(r, d, 2100);
        CHECK(dialNext(r, 2200, f, &d) && d.why == DialRules::kPairing && d.address == kTwo);
        notListening(r, d, 2300);
        CHECK(!r.next(6999, f, &d));
        CHECK(dialNext(r, 7000, f, &d) && d.address == kOne);
        notListening(r, d, 7100);
        f.pairingOpen = false;
        CHECK(!r.next(7200, f, &d) && !r.next(13000, f, &d));
        // It opens again: at once.
        f.pairingOpen = true;
        CHECK(dialNext(r, 13100, f, &d) && d.why == DialRules::kPairing);
    }
    {   // a retry, retryMs after an answer not understood, before the next poll
        DialRules r((plain()));
        present(r, 1000);
        DialRules::Dial d;
        CHECK(dialNext(r, 1000, facts(), &d));
        CHECK(r.reply(kOne, kService, kService, 1, 2000) == DialRules::kRetryLater);
        CHECK(!r.next(16999, facts(), &d));
        CHECK(dialNext(r, 17000, facts(), &d) && d.why == DialRules::kRetry);
        notListening(r, d, 17100);
        CHECK(!r.next(18000, facts(), &d));
    }
}

static void testAsk() {
    {   // every dial is asked first: listed, the dial goes at once; the ask's time and the dial's are kept apart
        Quiet r;
        present(r, 1000);
        DialRules::Dial d;
        CHECK(r.force() == 1 && r.next(2000, facts(), &d));
        CHECK(r.state() == DialRules::kAsking && d.address == kOne && d.askedAt == 2000 && d.why == DialRules::kForced);
        // Not an answer to this ask: another phone, an empty address; a dial's answer is none either.
        CHECK(r.listing(kTwo, 0, true, facts(), 2100) == DialRules::kNotThisAsk && r.listing("", 0, true, facts(), 2100) == DialRules::kNotThisAsk);
        CHECK(r.reply(kOne, kService, kService, 0, 2100) == DialRules::kNotThis && r.state() == DialRules::kAsking);
        CHECK(!r.overtakes(2100));   // only a dial overtakes a phone's request
        // The address in another form is the same phone; listed in any state.
        CHECK(r.listing("001122334455", 0, true, facts(), 3400) == DialRules::kDialNow && r.state() == DialRules::kDialling);
        CHECK(r.current().askedAt == 2000 && r.current().at == 3400 && r.overtakes(3500));
        CHECK(r.listing(kOne, 0, true, facts(), 3500) == DialRules::kNotThisAsk);   // once
        CHECK(r.reply(kOne, kService, kService, 5, 3600) == DialRules::kNotListening);
    }
    {   // not listed: no dial, nothing held against the phone, and the next poll asks again
        DialConfig c = plain();
        DialRules r(c);
        present(r, 1000);
        DialRules::Dial d;
        CHECK(r.next(1000, facts(), &d) && d.why == DialRules::kPoll);
        CHECK(r.listing(kOne, 0, false, facts(), 2400) == DialRules::kNotListed && r.state() == DialRules::kIdle);
        CHECK(!r.heldBack(kOne, 2400) && r.backedOffUntil(kOne) == 0 && r.askMisses(kOne) == 0);
        CHECK(!r.next(30999, facts(), &d));
        CHECK(r.next(31000, facts(), &d) && d.why == DialRules::kPoll && r.state() == DialRules::kAsking);
    }
    {   // an error or no answer: as not listed; three in a row back the phone off as a dial with no answer does,
        // and a good answer between them starts the run again
        Quiet r;
        present(r, 1000);
        DialRules::Dial d;
        int64_t now = 2000;
        uint32_t errors[] = {2, kNoError};
        for (int i = 0; i < 2; ++i) {
            CHECK(r.force() == 1 && r.next(now, facts(), &d));
            CHECK(r.listing(kOne, errors[i], true, facts(), now + 100) == DialRules::kNoListing && r.state() == DialRules::kIdle);
            CHECK(r.askMisses(kOne) == (unsigned)i + 1 && !r.heldBack(kOne, now + 100));
            now += 1000;
        }
        CHECK(r.force() == 1 && r.next(now, facts(), &d));
        CHECK(r.listing(kOne, 0, false, facts(), now + 100) == DialRules::kNotListed && r.askMisses(kOne) == 0);
        now += 1000;
        for (int i = 0; i < 2; ++i) {
            CHECK(r.force() == 1 && r.next(now, facts(), &d));
            CHECK(r.listing(kOne, 1, true, facts(), now + 100) == DialRules::kNoListing);
            now += 1000;
        }
        // No answer: overdue after askLimitMs; the third in a row.
        CHECK(r.force() == 1 && r.next(now, facts(), &d));
        CHECK(!r.askOverdue(now + 5999) && r.askOverdue(now + 6000));
        CHECK(r.listingLost(now + 6000) == DialRules::kAskBackedOff && r.state() == DialRules::kIdle);
        CHECK(r.backedOffUntil(kOne) == now + 6000 + 30000 && r.heldBack(kOne, now + 6000) && r.askMisses(kOne) == 0);
        CHECK(r.nextBackOffMs(kOne) == 60000);
        // Its answer after that is too late.
        CHECK(r.listing(kOne, 0, true, facts(), now + 6500) == DialRules::kNotThisAsk && r.state() == DialRules::kIdle);
        CHECK(r.listingLost(now + 6500) == DialRules::kNotThisAsk && !r.askOverdue(now + 100000));
        // A forced round still asks it; a change of its connection takes the run and the back-off away.
        CHECK(r.force() == 1 && r.next(now + 7000, facts(), &d));
        CHECK(r.listingLost(now + 13000) == DialRules::kNoListing && r.askMisses(kOne) == 1);
        present(r, now + 14000, false, kConnServiceConnected);
        CHECK(r.askMisses(kOne) == 0 && !r.heldBack(kOne, now + 14000));
    }
    {   // one ask or dial at a time; an ask that a link of the agent's ends is passed over when its answer comes, and
        // no other goes until it has come
        Quiet r;
        present(r, 1000, true);
        DialRules::Dial d;
        CHECK(r.force() == 2 && r.next(2000, facts(), &d) && d.address == kOne);
        CHECK(!r.next(2100, facts(), &d) && r.state() == DialRules::kAsking);
        DialRules::Facts busy = facts();
        busy.linkOfOurs = true;
        CHECK(!r.next(2200, busy, &d) && r.state() == DialRules::kIdle);
        CHECK(r.force() == 2 && !r.next(2300, facts(), &d));   // the first may still be out in the car
        CHECK(r.listing(kOne, 0, true, facts(), 3300) == DialRules::kNotThisAsk && r.state() == DialRules::kIdle);
        CHECK(r.next(3400, facts(), &d) && d.address == kOne);   // its answer came: the car is free
        // keepAsking() does the same, and the time of the ask ends the wait.
        CHECK(!r.keepAsking(busy, 3500) && r.state() == DialRules::kIdle);
        CHECK(!r.next(3400 + 5999, facts(), &d) && r.next(3400 + 6000, facts(), &d) && d.address == kTwo);
        // The registration's going ends it too.
        DialRules::Facts gone = facts();
        gone.registered = false;
        CHECK(r.keepAsking(facts(), 9500) && !r.keepAsking(gone, 9500) && r.state() == DialRules::kIdle);
        CHECK(!r.keepAsking(facts(), 9600));   // nothing out
        CHECK(r.listing(kTwo, 0, true, facts(), 9700) == DialRules::kNotThisAsk);
    }
    {   // a phone's request that becomes pending ends the ask of a round; not the ask its own request brought,
        // unless another phone's request comes
        DialRules r((plain()));
        present(r, 1000, true);
        DialRules::Facts f = facts(kOne, kTwo);
        DialRules::Facts pending = f;
        pending.requestPending = true;
        DialRules::Dial d;
        CHECK(r.next(1000, f, &d) && d.why == DialRules::kPoll && d.address == kOne);
        r.phoneAsked(kTwo, 1100);
        CHECK(!r.next(1200, pending, &d) && r.state() == DialRules::kIdle);
        CHECK(r.listing(kOne, 0, true, facts(), 1500) == DialRules::kNotThisAsk);
        CHECK(r.next(2100, pending, &d) && d.why == DialRules::kSignal && d.address == kTwo);
        CHECK(r.keepAsking(pending, 2200) && !r.next(2300, pending, &d) && r.state() == DialRules::kAsking);
        r.phoneAsked(kTwo, 2400);   // the same phone again: the ask goes on
        CHECK(r.keepAsking(pending, 2500));
        r.phoneAsked(kOne, 2600);   // another phone: it ends
        CHECK(!r.keepAsking(pending, 2700) && r.state() == DialRules::kIdle);
    }
    {   // never a phone below connection 2: not asked, and one that goes below 2, out of the list or into pairing
        // trouble during its ask is not dialled when its service list comes
        Quiet r;
        present(r, 1000, false, kConnInRange);
        DialRules::Dial d;
        DialRules::Facts f = facts();
        f.pairingOpen = true;
        CHECK(r.force() == 0 && !r.next(1000, f, &d));
        r.phoneAsked(kOne, 1000);
        CHECK(!r.next(5000, f, &d));
        present(r, 6000);
        CHECK(r.force() == 1 && r.next(6000, facts(), &d));
        present(r, 6500, false, kConnInRange);
        CHECK(r.listing(kOne, 0, true, facts(), 7000) == DialRules::kNoLongerDue && r.state() == DialRules::kIdle);
        r.devices(std::vector<BtDevice>(), 7100, 0, 0);
        present(r, 8000);
        CHECK(r.force() == 1 && r.next(8000, facts(), &d));
        r.devices(std::vector<BtDevice>(), 8100, 0, 0);
        CHECK(r.listing(kOne, 0, true, facts(), 8200) == DialRules::kNoLongerDue);
        present(r, 9000);
        CHECK(r.force() == 1 && r.next(9000, facts(), &d));
        CHECK(r.pairingTrouble("") == 1);   // stands for the phone being asked
        CHECK(r.listing(kOne, 0, true, facts(), 9200) == DialRules::kNoLongerDue && r.force() == 0);
    }
    {   // what brought the ask must still hold when its answer lists the service: a poll's phone that the agent has
        // forgotten since is not dialled; one it still has is
        DialRules r((plain()));
        present(r, 1000);
        DialRules::Dial d;
        CHECK(r.next(1000, facts(), &d) && d.why == DialRules::kPoll);
        CHECK(r.listing(kOne, 0, true, facts(0), 2000) == DialRules::kNoLongerDue && r.state() == DialRules::kIdle);
        CHECK(!r.heldBack(kOne, 2000) && r.askMisses(kOne) == 0);   // nothing held against it
        CHECK(r.next(31000, facts(), &d) && d.why == DialRules::kPoll);
        CHECK(r.listing(kOne, 0, true, facts(), 32000) == DialRules::kDialNow);
    }
    {   // the pairing window closes between the ask and its answer, for a phone that is not the agent's: not dialled;
        // while it is still open, dialled
        Quiet r;
        present(r, 1000);
        DialRules::Facts open = facts(0);
        open.pairingOpen = true;
        DialRules::Dial d;
        CHECK(r.next(2000, open, &d) && d.why == DialRules::kPairing && d.address == kOne);
        CHECK(r.listing(kOne, 0, true, facts(0), 3000) == DialRules::kNoLongerDue && r.state() == DialRules::kIdle);
        CHECK(r.next(7000, open, &d) && d.why == DialRules::kPairing);
        CHECK(r.listing(kOne, 0, true, open, 8000) == DialRules::kDialNow);
    }
    {   // a forced round's phone is dialled whether or not it is the agent's, and so is the afresh ask after a link
        // left behind by a dial a forced round brought; it still has to be connected and paired
        Quiet r;
        present(r, 1000);
        DialRules::Dial d;
        CHECK(r.force() == 1 && r.next(2000, facts(0), &d) && d.why == DialRules::kForced);
        CHECK(r.listing(kOne, 0, true, facts(0), 3000) == DialRules::kDialNow && r.state() == DialRules::kDialling);
        CHECK(r.reply(kOne, kService, kService, 2, 3100) == DialRules::kLookForNode);
        r.stale();
        r.ended(3500);
        CHECK(r.next(3600, facts(0), &d) && d.why == DialRules::kAfresh);
        CHECK(r.listing(kOne, 0, true, facts(0), 4000) == DialRules::kDialNow);
        r.reply(kOne, kService, kService, 5, 4100);
        CHECK(r.force() == 1 && r.next(5000, facts(0), &d) && d.why == DialRules::kForced);
        present(r, 5500, false, kConnInRange);
        CHECK(r.listing(kOne, 0, true, facts(0), 6000) == DialRules::kNoLongerDue);
        // The afresh ask after the pairing window's dial is not: with the window closed since, not dialled.
        present(r, 7000);
        DialRules::Facts open = facts(0);
        open.pairingOpen = true;
        CHECK(dialNext(r, 7000, open, &d) && d.why == DialRules::kPairing);
        CHECK(r.reply(kOne, kService, kService, 2, 7100) == DialRules::kLookForNode);
        r.stale();
        r.ended(7500);
        CHECK(r.next(7600, facts(0), &d) && d.why == DialRules::kAfresh);
        CHECK(r.listing(kOne, 0, true, facts(0), 8000) == DialRules::kNoLongerDue);
    }
    {   // the service goes while asking: nothing out, and a late answer passed over
        Quiet r;
        present(r, 1000);
        DialRules::Dial d;
        CHECK(r.force() == 1 && r.next(2000, facts(), &d));
        r.serviceGone();
        CHECK(r.state() == DialRules::kIdle && r.listing(kOne, 0, true, facts(), 2500) == DialRules::kNotThisAsk);
        present(r, 3000);
        CHECK(r.force() == 1 && r.next(3000, facts(), &d));   // no wait for the lost ask
        // The listener drops one as it stops: passed over, and waited for as one that was ended.
        r.askDropped(3100);
        CHECK(r.state() == DialRules::kIdle && r.force() == 1 && !r.next(3200, facts(), &d));
        CHECK(r.next(3000 + 6000, facts(), &d));
    }
}

static void testSettle() {
    {   // a phone in the first list: no poll asks it until settleMs after that list, and the poll comes then; a
        // forced round and the pairing window ask at once
        DialConfig c = plain();
        c.settleMs = 8000;
        DialRules r(c);
        present(r, 1000);
        DialRules::Dial d;
        CHECK(r.settling(kOne, 1000) && r.settling(kOne, 8999) && !r.settling(kOne, 9000));
        CHECK(!r.heldBack(kOne, 1000));   // the settle time is not a hold of its own
        CHECK(!r.next(1000, facts(), &d) && r.nextPollAt() == 9000 && !r.next(8999, facts(), &d));
        CHECK(r.next(9000, facts(), &d) && d.why == DialRules::kPoll);
        CHECK(r.listing(kOne, 0, false, facts(), 10000) == DialRules::kNotListed);
        CHECK(r.nextPollAt() == 9000 + 30000);
        // It leaves and connects again: settling again, from that list; a change from 2 to 3 is no new connect.
        present(r, 50000, false, kConnInRange);
        present(r, 51000);
        CHECK(r.settling(kOne, 58999) && !r.settling(kOne, 59000));
        present(r, 60000, false, kConnServiceConnected);
        CHECK(!r.settling(kOne, 60000));
        CHECK(r.nextPollAt() == 51000 + 10000 && r.next(61000, facts(), &d) && d.why == DialRules::kPoll);
        r.listing(kOne, 0, false, facts(), 61500);
        // A phone that comes into a later list connected settles too.
        present(r, 70000, true);
        CHECK(r.settling(kTwo, 70000) && !r.settling(kOne, 70000));
        // Forced and the pairing window: at once.
        present(r, 80000, false, kConnInRange);
        present(r, 81000);
        CHECK(r.force() == 1 && r.next(81000, facts(), &d) && d.why == DialRules::kForced);
        r.listing(kOne, 0, false, facts(), 81500);
        DialRules::Facts f = facts(0);
        f.pairingOpen = true;
        CHECK(r.next(81600, f, &d) && d.why == DialRules::kPairing && d.address == kOne);
    }
    {   // the ask a phone's request brings waits for the phone's settle time, and goes then, before the poll
        DialConfig c = plain();
        c.settleMs = 8000;
        DialRules r(c);
        present(r, 1000);
        DialRules::Dial d;
        r.phoneAsked(kOne, 2000);
        CHECK(!r.next(3000, facts(), &d) && !r.next(8999, facts(), &d));
        CHECK(r.next(9000, facts(), &d) && d.why == DialRules::kSignal);
    }
    {   // a retry waits for it; an afresh round leaves the settling phone out
        DialConfig c = config();
        c.settleMs = 8000;
        DialRules r(c);
        r.linkEnded(0);
        present(r, 1000);
        DialRules::Dial d;
        CHECK(r.force() == 1 && dialNext(r, 1000, facts(), &d));
        CHECK(r.reply(kOne, kService, kService, 1, 1100) == DialRules::kRetryLater);
        present(r, 5000, false, kConnInRange);
        present(r, 10000);   // settling until 18000; the retry was due at 16100
        CHECK(!r.next(16100, facts(), &d) && !r.next(17999, facts(), &d));
        CHECK(r.next(18000, facts(), &d) && d.why == DialRules::kRetry);
        r.listing(kOne, 0, false, facts(), 18100);
        present(r, 20000, false, kConnInRange);
        present(r, 21000);
        r.afresh();
        CHECK(!r.next(21000, facts(), &d) && !r.next(29000, facts(), &d));
    }
}

static void testSignalAsks() {
    // A phone's requests bring one ask of it every signalAskMs at most; a request whose time runs out sooner brings
    // none.
    DialRules r((config()));
    r.linkEnded(0);
    present(r, 1000);
    DialRules::Dial d;
    r.phoneAsked(kOne, 2000);
    CHECK(r.next(3000, facts(), &d) && d.why == DialRules::kSignal);
    CHECK(r.listing(kOne, 0, false, facts(), 4400) == DialRules::kNotListed);
    r.phoneAsked(kOne, 5000);
    CHECK(!r.next(6000, facts(), &d) && !r.next(12999, facts(), &d));   // 3 s after the last: dropped
    r.phoneAsked(kOne, 12000);
    CHECK(r.next(13000, facts(), &d) && d.why == DialRules::kSignal);   // 10 s after the last
    r.listing(kOne, 0, false, facts(), 13500);
    // Polls, forced rounds and the pairing window are not held by it.
    CHECK(r.force() == 1 && r.next(13600, facts(), &d) && d.why == DialRules::kForced);
}

static void testDoubt() {
    // A dial that the car answers with a failure, or does not answer, after the phone listed the service: the car's
    // record of phone-opened links may be wrong, until a dial is answered 0, the end of a phone-opened link is said,
    // or the service goes. 2 and 10, and a rejection, are not such answers.
    struct Row {
        uint32_t error;
        bool doubt;
    } rows[] = {{5, true}, {14, true}, {1, true}, {3, true}, {2, false}, {10, false}, {kNoError, false}, {0, false}};
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
        Quiet r;
        present(r, 1000);
        dialOne(r, 2000);
        r.reply(kOne, kService, kService, rows[i].error, 2500);
        CHECK_IN(num(i), r.recordDoubtful() == rows[i].doubt);
    }
    Quiet r;
    present(r, 1000);
    CHECK(!r.recordDoubtful());
    dialOne(r, 2000);
    r.reply(kOne, kService, kService, 5, 2500);
    CHECK(r.recordDoubtful());
    // An ask that does not list the service, and one with an error, change nothing.
    DialRules::Dial d;
    CHECK(r.force() == 1 && r.next(3000, facts(), &d) && r.listing(kOne, 0, false, facts(), 3100) == DialRules::kNotListed);
    CHECK(r.force() == 1 && r.next(3200, facts(), &d) && r.listing(kOne, 2, false, facts(), 3300) == DialRules::kNoListing);
    CHECK(r.recordDoubtful());
    // A dial answered 2 neither.
    dialOne(r, 4000);
    CHECK(r.reply(kOne, kService, kService, 2, 4100) == DialRules::kLookForNode && r.recordDoubtful());
    r.notThere(4200);
    // Answered 0: gone.
    dialOne(r, 5000);
    CHECK(r.reply(kOne, kService, kService, 0, 5100) == DialRules::kOpenNode && !r.recordDoubtful());
    r.linkUp(5200);
    r.ending();
    r.ended(6000);
    // A dial with no answer: doubted; the end of a phone-opened link said: gone.
    dialOne(r, 7000);
    CHECK(r.overdue(7000 + 95000));
    r.gaveUp(7000 + 95000);
    CHECK(r.recordDoubtful());
    r.phoneLinkEnded();
    CHECK(!r.recordDoubtful());
    // The service goes: gone.
    dialOne(r, 200000);
    r.reply(kOne, kService, kService, 14, 200100);
    CHECK(r.recordDoubtful());
    r.serviceGone();
    CHECK(!r.recordDoubtful());
}

int main() {
    testAnswers();
    testAsk();
    testSettle();
    testSignalAsks();
    testDoubt();
    testTriggers();
    testBackOff();
    testWhoAndWhen();
    testPairingTrouble();
    testOvertaken();
    printf("test_dial_rules: ok\n");
    return 0;
}
