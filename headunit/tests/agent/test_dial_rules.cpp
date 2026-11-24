// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/dial_rules.cpp: when the car dials a phone of the agent's, and what comes of each answer; and
// for agent/dialup_requests.hpp, the requests of the knock by dial-up and the agent's refusals of them. The time is
// the test's own, in milliseconds. All addresses are made up.

#include "../../agent/dial_rules.hpp"

#include "../../agent/dialup_requests.hpp"
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

// The defaults with the knock off (`--dial-knock off`), as the rules were before the knock, which the tests up to
// testKnock() are about; and with no settle time: the tests of what came before the ask leave it out.
static DialConfig plain() {
    DialConfig c;
    c.knock = false;
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
        bool backedOff;      // for the back-off time, rather than until the retry
    } rows[] = {
        {0, DialRules::kOpenNode, DialRules::kUp, false, false},
        {2, DialRules::kLookForNode, DialRules::kIdle, false, false},
        {5, DialRules::kNotListening, DialRules::kIdle, true, true},
        {14, DialRules::kUnreachable, DialRules::kIdle, true, true},
        {1, DialRules::kFailed, DialRules::kIdle, true, true},
        {3, DialRules::kFailed, DialRules::kIdle, true, true},
        {15, DialRules::kFailed, DialRules::kIdle, true, true},
        {10, DialRules::kRetryLater, DialRules::kIdle, true, false},
        {kNoError, DialRules::kRetryLater, DialRules::kIdle, true, false},   // a rejection
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
        CHECK_IN(what, r.backedOffUntil(kOne) == (rows[i].backedOff ? 2500 + 30000 : 0));
        // A second answer to the same dial counts no more.
        CHECK_IN(what, r.reply(kOne, kService, kService, rows[i].error, 2600) == DialRules::kNotThis);
    }
    {   // 10, and a rejection, hold the phone back for retryMs
        uint32_t errors[] = {10, kNoError};
        for (int i = 0; i < 2; ++i) {
            Quiet r;
            present(r, 1000);
            dialOne(r, 2000);
            CHECK(r.reply(kOne, kService, kService, errors[i], 3000) == DialRules::kRetryLater);
            CHECK(r.heldBack(kOne, 3000 + 15000 - 1) && !r.heldBack(kOne, 3000 + 15000));
        }
    }
    {   // 14: backed off as any failed dial, for the back-off time and no longer, and sooner gone if the phone's
        // connection changes in the device list
        Quiet r;
        present(r, 1000);
        dialOne(r, 2000);
        CHECK(r.reply(kOne, kService, kService, 14, 7000) == DialRules::kUnreachable);
        CHECK(r.heldBack(kOne, 7000 + 30000 - 1) && !r.heldBack(kOne, 7000 + 30000));
        present(r, 8000);   // the same list: no change
        CHECK(r.heldBack(kOne, 8000));
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
    {   // 2 and no node: as 10, retried; 2 with the node: stale, ended, and then dialled again at once
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

// Answers the dial that is out with 5, as a phone that does not take the link: the phone is backed off.
static void notListening(DialRules& r, const DialRules::Dial& d, int64_t now) {
    CHECK(r.reply(d.address, kService, kService, 5, now) == DialRules::kNotListening);
}

// The ask next() gives, out: `d` is its.
static bool askNext(DialRules& r, int64_t now, const DialRules::Facts& f, DialRules::Dial* d) {
    return r.next(now, f, d) && r.state() == DialRules::kAsking;
}

// Answers the ask that is out without the agent's service, as a phone whose app does not listen: nothing is held
// against it.
static void notListed(DialRules& r, const DialRules::Dial& d, int64_t now) {
    CHECK(r.listing(d.address, 0, false, facts(), now) == DialRules::kNotListed);
}

static void testTriggers() {
    {   // polls: the first at the first chance, then every 30 s; every 10 s for 2 minutes after a phone connects or
        // a link of the agent's ends
        DialRules r((plain()));
        DialRules::Dial d;
        CHECK(!r.next(0, facts(), &d));   // no device list yet
        present(r, 1000);
        CHECK(askNext(r, 1000, facts(), &d) && d.why == DialRules::kPoll && d.address == kOne);
        notListed(r, d, 1200);
        CHECK(r.nextPollAt() == 1000 + 30000);
        CHECK(!r.next(30999, facts(), &d) && askNext(r, 31000, facts(), &d) && d.why == DialRules::kPoll);
        notListed(r, d, 31100);
        // The phone leaves and connects again: every 10 s from then, for 2 minutes.
        present(r, 40000, false, kConnInRange);
        present(r, 41000, false, kConnConnected);
        CHECK(r.nextPollAt() == 51000);
        int64_t now = 51000;
        while (now < 41000 + 120000) {
            CHECK_IN(num((size_t)now), askNext(r, now, facts(), &d) && d.why == DialRules::kPoll);
            notListed(r, d, now + 100);
            CHECK(!r.next(now + 9999, facts(), &d));
            now += 10000;
        }
        CHECK(askNext(r, now, facts(), &d));
        notListed(r, d, now + 100);
        CHECK(r.nextPollAt() == now + 30000);
        // A link of the agent's ends: the same.
        r.linkEnded(500000);
        CHECK(r.nextPollAt() == 510000);
        // Polls leave out a phone that is held back, and one that is not the agent's: a dial answered 14 backs it
        // off for 30 s, and polls ask it again after that; a change of its connection takes the next back-off away.
        CHECK(dialNext(r, 510000, facts(), &d));
        CHECK(r.reply(kOne, kService, kService, 14, 515000) == DialRules::kUnreachable);
        CHECK(!r.next(520000, facts(), &d) && !r.next(540000, facts(), &d));
        CHECK(dialNext(r, 550000, facts(), &d) && d.address == kOne);
        CHECK(r.reply(kOne, kService, kService, 14, 551000) == DialRules::kUnreachable);
        CHECK(r.backedOffUntil(kOne) == 551000 + 60000 && !r.next(560000, facts(), &d));
        present(r, 565000, false, kConnServiceConnected);
        CHECK(dialNext(r, 570000, facts(), &d) && d.address == kOne);
    }
    {   // the phone's own request: dialled once its time has run with no link of the agent's, a retry that is due
        // later notwithstanding; a back-off after a failed dial holds it back, and the ask goes once that is over
        Quiet r;
        present(r, 1000);
        DialRules::Dial d;
        dialOne(r, 1000);
        CHECK(r.reply(kOne, kService, kService, 0, 1500) == DialRules::kOpenNode);
        r.notTheApp(4500);
        r.ended(5000);
        CHECK(r.heldBack(kOne, 6000) && r.backedOffUntil(kOne) == 4500 + 30000);
        r.phoneAsked("001122334455", 6000);
        CHECK(!r.next(6999, facts(), &d) && !r.next(7100, facts(), &d) && !r.next(34499, facts(), &d));
        DialRules::Facts busy = facts();
        busy.linkOfOurs = true;           // its own link being decided on: no dial meanwhile
        CHECK(!r.next(34500, busy, &d));
        CHECK(dialNext(r, 34600, facts(), &d) && d.why == DialRules::kSignal && d.address == kOne);
        notListening(r, d, 34700);
        CHECK(r.backedOffUntil(kOne) == 34700 + 60000);
        CHECK(!r.next(34800, facts(), &d));   // once
        // Its requests in the next back-off bring no ask sooner; the one that waited goes as soon as it is over.
        r.phoneAsked(kOne, 50000);
        CHECK(!r.next(51000, facts(), &d) && !r.next(94699, facts(), &d));
        CHECK(askNext(r, 94700, facts(), &d) && d.why == DialRules::kSignal);
        notListed(r, d, 94800);
        // A retry that is due later is no hold for it: a dial answered 10, and a request before the retry is due.
        dialOne(r, 110000);
        CHECK(r.reply(kOne, kService, kService, 10, 110100) == DialRules::kRetryLater && r.heldBack(kOne, 110200));
        r.phoneAsked(kOne, 111000);
        CHECK(askNext(r, 112000, facts(), &d) && d.why == DialRules::kSignal);
        notListed(r, d, 112100);
        // A link of the agent's that comes answers it.
        r.phoneAsked(kOne, 130000);
        r.linkCame();
        CHECK(!r.next(131000, facts(), &d));
        // Not for a phone that is not eligible.
        r.phoneAsked(kOne, 150000);
        CHECK(!r.next(151000, facts(kTwo), &d));
        // Nor for a request with no address.
        r.phoneAsked("", 170000);
        CHECK(!r.next(171000, facts(), &d));
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
    {   // a retry, retryMs after an answer 10, before the next poll
        DialRules r((plain()));
        present(r, 1000);
        DialRules::Dial d;
        CHECK(dialNext(r, 1000, facts(), &d));
        CHECK(r.reply(kOne, kService, kService, 10, 2000) == DialRules::kRetryLater);
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
    {   // an error or no answer: as not listed, only counted, and a good answer between them starts the run again.
        // Nothing holds the phone back for them, however many come: the next poll asks again
        DialRules r((plain()));
        present(r, 1000);
        DialRules::Dial d;
        int64_t now = 1000;   // the first poll
        uint32_t errors[] = {2, kNoError};
        for (int i = 0; i < 2; ++i) {
            CHECK_IN(num(i), askNext(r, now, facts(), &d) && d.why == DialRules::kPoll);
            CHECK(r.listing(kOne, errors[i], true, facts(), now + 100) == DialRules::kNoListing && r.state() == DialRules::kIdle);
            CHECK(r.askMisses(kOne) == (unsigned)i + 1 && !r.heldBack(kOne, now + 100));
            now += 30000;
        }
        CHECK(askNext(r, now, facts(), &d));
        CHECK(r.listing(kOne, 0, false, facts(), now + 100) == DialRules::kNotListed && r.askMisses(kOne) == 0);
        now += 30000;
        for (int i = 0; i < 2; ++i) {
            CHECK(askNext(r, now, facts(), &d));
            CHECK(r.listing(kOne, 1, true, facts(), now + 100) == DialRules::kNoListing);
            now += 30000;
        }
        // No answer: overdue after askLimitMs; the third in a row, which holds nothing back either.
        CHECK(askNext(r, now, facts(), &d));
        CHECK(!r.askOverdue(now + 5999) && r.askOverdue(now + 6000));
        CHECK(r.listingLost(now + 6000) == DialRules::kNoListing && r.state() == DialRules::kIdle);
        CHECK(r.askMisses(kOne) == 3 && r.backedOffUntil(kOne) == 0 && !r.heldBack(kOne, now + 6000));
        CHECK(r.nextBackOffMs(kOne) == 30000);
        // Its answer after that is too late.
        CHECK(r.listing(kOne, 0, true, facts(), now + 6500) == DialRules::kNotThisAsk && r.state() == DialRules::kIdle);
        CHECK(r.listingLost(now + 6500) == DialRules::kNotThisAsk && !r.askOverdue(now + 100000));
        // The next poll asks again, and so does each after it.
        CHECK(!r.next(now + 29999, facts(), &d));
        now += 30000;
        for (int i = 0; i < 3; ++i) {
            CHECK_IN(num(i), askNext(r, now, facts(), &d) && d.why == DialRules::kPoll);
            CHECK(r.listingLost(now + 6000) == DialRules::kNoListing && r.askMisses(kOne) == 4u + (unsigned)i);
            now += 30000;
        }
        // A forced round asks too; a change of its connection takes the run away.
        CHECK(r.force() == 1 && r.next(now, facts(), &d) && d.why == DialRules::kForced);
        CHECK(r.listingLost(now + 6000) == DialRules::kNoListing && r.askMisses(kOne) == 7);
        present(r, now + 7000, false, kConnServiceConnected);
        CHECK(r.askMisses(kOne) == 0 && !r.heldBack(kOne, now + 7000));
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
    {   // a pending request that names no phone is not the asked phone's either: it ends the ask another phone's
        // request brought, and brings none itself
        DialRules r((plain()));
        present(r, 1000, true);
        DialRules::Facts f = facts(kOne, kTwo);
        DialRules::Facts pending = f;
        pending.requestPending = true;
        DialRules::Dial d;
        CHECK(r.next(1000, f, &d) && d.why == DialRules::kPoll && d.address == kOne);
        r.listing(kOne, 0, false, f, 1100);
        r.phoneAsked(kTwo, 2000);
        CHECK(r.next(3000, pending, &d) && d.why == DialRules::kSignal && d.address == kTwo);
        CHECK(r.keepAsking(pending, 3100) && r.state() == DialRules::kAsking);
        r.phoneAsked("", 3200);
        CHECK(!r.keepAsking(pending, 3300) && r.state() == DialRules::kIdle);
        CHECK(r.listing(kTwo, 0, true, f, 3400) == DialRules::kNotThisAsk);   // passed over
        CHECK(!r.next(4500, pending, &d));
        // Nor does one that names something that is not an address.
        r.phoneAsked(kTwo, 15000);
        CHECK(r.next(16000, pending, &d) && d.why == DialRules::kSignal && d.address == kTwo);
        r.phoneAsked("not an address", 16100);
        CHECK(!r.keepAsking(pending, 16200) && r.state() == DialRules::kIdle);
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
        CHECK(r.reply(kOne, kService, kService, 10, 1100) == DialRules::kRetryLater);
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
    r.listing(kOne, 0, false, facts(), 13700);
    // An ask that could not be sent cost the car nothing, and does not count: the next request is asked at once.
    CHECK(r.notSent(30000) == DialRules::kNotThisAsk);   // none out
    r.phoneAsked(kOne, 30000);
    CHECK(r.next(31000, facts(), &d) && d.why == DialRules::kSignal);
    CHECK(r.notSent(31000) == DialRules::kNoListing && r.state() == DialRules::kIdle && r.askMisses(kOne) == 1);
    r.phoneAsked(kOne, 32000);
    CHECK(r.next(33000, facts(), &d) && d.why == DialRules::kSignal);
    r.listing(kOne, 0, false, facts(), 33500);
    r.phoneAsked(kOne, 35000);
    CHECK(!r.next(36000, facts(), &d));   // that one went: it counts
    // One that a poll or a forced round brought and that could not be sent changes nothing of it either.
    CHECK(r.force() == 1 && r.next(36100, facts(), &d) && d.why == DialRules::kForced);
    CHECK(r.notSent(36100) == DialRules::kNoListing);
    r.phoneAsked(kOne, 37000);
    CHECK(!r.next(38000, facts(), &d));
}

static void testEndedSignalAsks() {
    // An ask a phone's request brought that another request ends before its answer came cost the car a query, and
    // counts; but the phone's requests may bring one more ask in that time, once. So whatever ends them, they bring
    // two asks in signalAskMs at most.
    Quiet r;
    present(r, 1000);
    DialRules::Facts pending = facts();
    pending.requestPending = true;
    DialRules::Dial d;
    r.phoneAsked(kOne, 2000);
    CHECK(r.next(3000, pending, &d) && d.why == DialRules::kSignal);
    r.phoneAsked("", 3100);   // a request that names no phone ends it
    CHECK(!r.keepAsking(pending, 3200) && r.state() == DialRules::kIdle);
    CHECK(r.listing(kOne, 0, false, pending, 4400) == DialRules::kNotThisAsk);   // its answer: the car is free
    r.phoneAsked(kOne, 5000);
    CHECK(r.next(6000, pending, &d) && d.why == DialRules::kSignal);   // the one more
    r.phoneAsked("", 6100);
    CHECK(!r.keepAsking(pending, 6200));
    r.listing(kOne, 0, false, pending, 7400);
    r.phoneAsked(kOne, 8000);
    CHECK(!r.next(9000, pending, &d));   // no more
    r.phoneAsked(kOne, 12500);
    CHECK(r.next(13500, pending, &d) && d.why == DialRules::kSignal);   // 10 s after the first
    // One that was answered gives no more.
    CHECK(r.listing(kOne, 0, false, pending, 14000) == DialRules::kNotListed);
    r.phoneAsked(kOne, 15000);
    CHECK(!r.next(16000, pending, &d));
    // Nor does one that was ended once the one more has gone in that time: here the second of two.
    r.phoneAsked(kOne, 24000);
    CHECK(r.next(25000, pending, &d) && d.why == DialRules::kSignal);
    r.phoneAsked("", 25100);
    CHECK(!r.keepAsking(pending, 25200));
    r.listing(kOne, 0, false, pending, 26000);
    r.phoneAsked(kOne, 27000);
    CHECK(r.next(28000, pending, &d) && d.why == DialRules::kSignal);
    r.phoneAsked("", 28100);
    CHECK(!r.keepAsking(pending, 28200));
    r.listing(kOne, 0, false, pending, 29000);
    r.phoneAsked(kOne, 30000);
    CHECK(!r.next(31000, pending, &d) && !r.next(34999, pending, &d));
}

static void testFailedDials() {
    // A dial the car answers with a failure, 5 and 14 among them, or does not answer, backs the phone off: polls and
    // its own requests leave it alone for the back-off time and no longer, and the pairing window and a forced round
    // still ask it meanwhile. The ask a request in that time brings waits for the end of the back-off, and goes then.
    struct Row {
        uint32_t error;   // kNoError here: no answer
        const char* what;
    } rows[] = {{5, "5"}, {14, "14"}, {1, "1"}, {kNoError, "no answer"}};
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
        DialConfig c = plain();
        c.pollFirstMs = c.pollLaterMs = 1000;   // a poll every second, to see where the back-off ends
        DialRules r(c);
        present(r, 1000);
        DialRules::Dial d;
        CHECK_IN(rows[i].what, dialNext(r, 1000, facts(), &d) && d.why == DialRules::kPoll);
        int64_t at = 1500;
        if (rows[i].error == kNoError) {
            at = 1000 + 95000;
            CHECK(r.overdue(at));
            r.gaveUp(at);
        } else {
            r.reply(kOne, kService, kService, rows[i].error, at);
        }
        CHECK_IN(rows[i].what, r.state() == DialRules::kIdle && r.backedOffUntil(kOne) == at + 30000);
        CHECK_IN(rows[i].what, r.heldBack(kOne, at + 29999) && !r.heldBack(kOne, at + 30000));
        // Its own request does not ask it: it waits.
        r.phoneAsked(kOne, at + 100);
        CHECK_IN(rows[i].what, !r.next(at + 1100, facts(), &d) && r.state() == DialRules::kIdle);
        CHECK(r.heldBack(kOne, at + 1200));
        // The pairing window and a forced round do; an answer that does not list it takes no back-off away.
        DialRules::Facts open = facts();
        open.pairingOpen = true;
        CHECK_IN(rows[i].what, askNext(r, at + 2000, open, &d) && d.why == DialRules::kPairing);
        notListed(r, d, at + 2100);
        CHECK(r.force() == 1 && askNext(r, at + 3000, facts(), &d) && d.why == DialRules::kForced);
        notListed(r, d, at + 3100);
        CHECK(r.heldBack(kOne, at + 3100));
        // The polls in the back-off time leave it out, and the request's ask still waits.
        int64_t t = at + 3200;
        int64_t poll = 0;
        for (;;) {
            poll = r.nextPollAt() > t ? r.nextPollAt() : t;
            if (poll >= at + 30000) break;
            CHECK_IN(rows[i].what, !r.next(poll, facts(), &d));
            t = poll + 1;
        }
        // The first chance after it asks it: the ask the request brought, which waited, ahead of the poll.
        CHECK_IN(rows[i].what, askNext(r, poll, facts(), &d) && d.why == DialRules::kSignal && poll < at + 31000);
        // And the next failure backs it off twice as long.
        CHECK(r.listing(kOne, 0, true, facts(), poll + 100) == DialRules::kDialNow);
        if (rows[i].error == kNoError) {
            r.gaveUp(poll + 95100);
            CHECK(r.backedOffUntil(kOne) == poll + 95100 + 60000);
        } else {
            r.reply(kOne, kService, kService, rows[i].error, poll + 200);
            CHECK(r.backedOffUntil(kOne) == poll + 200 + 60000);
        }
        CHECK_IN(rows[i].what, r.nextBackOffMs(kOne) == 120000);
    }
    {   // a dial whose request was rejected (never sent, or refused by the car's Bluetooth service), and one answered
        // 10: no back-off, the retry as before
        uint32_t errors[] = {kNoError, 10};
        for (int i = 0; i < 2; ++i) {
            Quiet r;
            present(r, 1000);
            dialOne(r, 2000);
            CHECK(r.reply(kOne, kService, kService, errors[i], 2100) == DialRules::kRetryLater);
            CHECK(r.backedOffUntil(kOne) == 0 && !r.recordDoubtful() && r.dialFailures(kOne) == 0);
            DialRules::Dial d;
            CHECK(!r.next(2100 + 14999, facts(), &d) && askNext(r, 2100 + 15000, facts(), &d) && d.why == DialRules::kRetry);
        }
    }
}

static void testDialFailures() {
    // The dials of a phone in a row that gave the agent no link: an answer that sets the doubt, no answer, a link that
    // was not the app's. 2, 10 and a rejection neither count nor start the run again; a link of the app's does.
    Quiet r;
    present(r, 1000, true);
    CHECK(r.dialFailures(kOne) == 0);
    int64_t now = 2000;
    dialOne(r, now);
    r.reply(kOne, kService, kService, 5, now + 100);
    CHECK(r.dialFailures(kOne) == 1);
    dialOne(r, now += 1000);
    r.reply(kOne, kService, kService, 14, now + 100);
    dialOne(r, now += 1000);
    r.reply(kOne, kService, kService, 10, now + 100);
    dialOne(r, now += 1000);
    r.reply(kOne, kService, kService, kNoError, now + 100);
    dialOne(r, now += 1000);
    r.reply(kOne, kService, kService, 2, now + 100);
    r.notThere(now + 200);
    CHECK(r.dialFailures(kOne) == 2);
    dialOne(r, now += 1000);
    CHECK(r.overdue(now + 95000));
    r.gaveUp(now + 95000);
    CHECK(r.dialFailures(kOne) == 3);
    now += 100000;
    dialOne(r, now);
    r.reply(kOne, kService, kService, 0, now + 100);
    r.notTheApp(now + 3000);
    r.ended(now + 4000);
    CHECK(r.dialFailures(kOne) == 4 && r.dialFailures(kTwo) == 0);
    // A connection that changes leaves the run as it is.
    present(r, now + 5000, true, kConnServiceConnected);
    CHECK(r.dialFailures(kOne) == 4);
    // The app's link: the run is over.
    dialOne(r, now += 10000);
    r.reply(kOne, kService, kService, 0, now + 100);
    r.linkUp(now + 200);
    CHECK(r.dialFailures(kOne) == 0);
    r.ending();
    r.ended(now + 1000);
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

// The re-point: which answers bring it, the same rule as the doubt; while it waits nothing starts; its limit.
static void testRepoint() {
    {   // the rule: every answer but 0, 2 and 10, and never kNoError (a rejection, a reply not read, a failed send)
        struct Row {
            uint32_t error;
            bool brings;
        } rows[] = {
            {0, false}, {2, false}, {10, false}, {kNoError, false},
            {1, true}, {3, true}, {4, true}, {5, true}, {6, true}, {7, true}, {8, true}, {9, true}, {11, true},
            {12, true}, {13, true}, {14, true}, {15, true}, {16, true}, {255, true},
        };
        for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            std::string what = "answer " + num(rows[i].error);
            CHECK_IN(what, DialRules::doubtingAnswer(rows[i].error) == rows[i].brings);
            // reply() goes by the same rule: the record is doubted after exactly these.
            Quiet r;
            present(r, 1000);
            dialOne(r, 2000);
            CHECK_IN(what, !r.recordDoubtful());
            r.reply(kOne, kService, kService, rows[i].error, 2100);
            CHECK_IN(what, r.recordDoubtful() == rows[i].brings);
        }
    }
    {   // while it waits, no ask starts, forced or not, and the ask that is out is ended; its limit lets them go on
        DialConfig c = config();
        c.repointLimitMs = 3000;
        DialRules r(c);
        r.linkEnded(0);
        present(r, 1000);
        DialRules::Dial d = dialOne(r, 2000);
        CHECK(r.reply(kOne, kService, kService, 5, 2100) == DialRules::kNotListening && r.recordDoubtful());
        CHECK(!r.repointOut() && !r.repointWaits(2100) && !r.repointOverdue(2100));
        r.repointSent(2100);
        CHECK(r.repointOut() && r.repointAt() == 2100);
        CHECK(r.repointWaits(2100) && r.repointWaits(5099) && !r.repointOverdue(5099));
        CHECK(!r.repointWaits(5100) && r.repointOverdue(5100));
        CHECK(r.force() == 1);
        CHECK(!r.next(2200, facts(), &d) && !r.next(5099, facts(), &d) && r.state() == DialRules::kIdle);
        // Its time over, the forced round goes on, given up or not.
        CHECK(r.next(5100, facts(), &d) && r.state() == DialRules::kAsking && d.why == DialRules::kForced);
        r.repointDone();
        CHECK(!r.repointOut() && !r.repointOverdue(9000) && r.recordDoubtful());
        // The next one, sent while that ask is out: it ends it, and its answer is passed over.
        r.repointSent(5200);
        CHECK(!r.keepAsking(facts(), 5200) && r.state() == DialRules::kIdle);
        CHECK(r.listing(kOne, 0, true, facts(), 5300) == DialRules::kNotThisAsk && r.state() == DialRules::kIdle);
        // Its answer: asks go on at once. The doubt stays as it was.
        r.repointDone();
        CHECK(r.force() == 1 && r.next(5400, facts(), &d) && r.state() == DialRules::kAsking);
        CHECK(r.keepAsking(facts(), 5400));
        CHECK(r.recordDoubtful());
    }
    {   // a poll waits for it too, and goes once it is done
        DialRules r(plain());
        present(r, 1000);
        r.repointSent(1000);
        DialRules::Dial d;
        CHECK(!r.next(1000, facts(), &d));
        r.repointDone();
        CHECK(r.next(1000, facts(), &d) && d.why == DialRules::kPoll);
    }
}

// ---- the knock ----------------------------------------------------------------------------------------------------

// The knock's rules: the defaults, with the knock on.
static DialConfig knocks() {
    DialConfig c;
    CHECK(c.knock);
    return c;
}

// The knock's dial next() gives: out at once, with no ask.
static DialRules::Dial knockDial(DialRules& r, int64_t now, const DialRules::Facts& f, DialRules::Why why) {
    DialRules::Dial d;
    CHECK(r.next(now, f, &d));
    CHECK(r.state() == DialRules::kDialling && d.why == why);
    return d;
}

// The news of next() since the last look: one knock, ended so.
static bool newsIs(DialRules& r, DialRules::KnockEnd end, unsigned number) {
    std::vector<DialRules::KnockNews> n = r.takeKnockNews();
    return n.size() == 1 && n[0].end == end && n[0].number == number;
}

// The knock by dial-up: the same rules as a knock by its magic bytes, counted apart, and told as such.
static void testKnockByDialup() {
    {   // dialled as any knock; counted as one by dial-up
        DialRules r(knocks());
        present(r, 1000);
        CHECK(r.knocked(kOne, 2000, true) == DialRules::kKnockWaits);
        DialRules::Dial d = knockDial(r, 2000, facts(), DialRules::kKnock);
        std::vector<DialRules::KnockNews> n = r.takeKnockNews();
        CHECK(d.address == kOne && n.size() == 1 && n[0].dialup && n[0].end == DialRules::kKnockDialled);
        CHECK(r.knockCounts().byDialup == 1 && r.knockCounts().byMarker == 0 && r.knockCounts().dials == 1);
        // A knock of either kind while its dial is out adds nothing.
        CHECK(r.knocked(kOne, 2100, true) == DialRules::kKnockBusy && r.knocked(kOne, 2100) == DialRules::kKnockBusy);
        CHECK(r.knockCounts().byDialup == 2 && r.knockCounts().byMarker == 1 && r.knockCounts().ignoredBusy == 2);
        // Failed: the quiet time, for knocks of both kinds.
        CHECK(r.reply(kOne, kService, kService, 5, 2200) == DialRules::kNotListening);
        CHECK(r.knocked(kOne, 3000, true) == DialRules::kKnockQuiet && r.knocked(kOne, 3000) == DialRules::kKnockQuiet);
    }
    {   // not dialable, and the knock's wait while the service is not registered
        DialRules r(knocks());
        present(r, 1000);
        CHECK(r.knocked(kTwo, 2000, true) == DialRules::kKnockWaits);   // not in the car's list
        DialRules::Dial d;
        CHECK(!r.next(2000, facts(), &d));
        std::vector<DialRules::KnockNews> n = r.takeKnockNews();
        CHECK(n.size() == 1 && n[0].dialup && n[0].end == DialRules::kKnockNotDialable);
        DialRules::Facts held = facts();
        held.registered = false;
        CHECK(r.knocked(kOne, 3000, true) == DialRules::kKnockWaits);
        CHECK(!r.next(3000, held, &d) && !r.next(3000 + 3999, held, &d) && r.takeKnockNews().empty());
        knockDial(r, 3000 + 3999, facts(), DialRules::kKnock);   // registered within its wait: dialled
        CHECK(r.takeKnockNews()[0].dialup);
    }
    {   // a wait that runs out while not registered: ignored, said as a knock by dial-up
        DialRules r(knocks());
        present(r, 1000);
        DialRules::Facts held = facts();
        held.registered = false;
        CHECK(r.knocked(kOne, 2000, true) == DialRules::kKnockWaits);
        DialRules::Dial d;
        CHECK(!r.next(2000 + 4000, held, &d));
        std::vector<DialRules::KnockNews> n = r.takeKnockNews();
        CHECK(n.size() == 1 && n[0].dialup && n[0].end == DialRules::kKnockBusy && n[0].waitedMs == 4000);
    }
}

// The pending requests for the car's dial-up service, and the agent's refusals of them, by the test's clock.
static void testDialupRequests() {
    DialupRequests::Timing t;
    CHECK(t.refuseAfterMs == 2000 && t.retryMs == 100 && t.retries == 5 && t.replyMs == 3000);
    std::string a;
    int64_t heard = 0;
    {   // the car ends it first: nothing is ever sent for it
        DialupRequests q(t);
        CHECK(q.heard(kOne, 1000) && !q.heard(kOne, 1500) && q.pending() == 1);   // a second while pending: the same
        CHECK(!q.refusalDue(1000, &a) && !q.refusalDue(2999, &a));                 // never at once
        CHECK(q.ended(kOne, &heard) && heard == 1000 && !q.busy());
        CHECK(!q.refusalDue(3000, &a) && !q.refusalDue(99999, &a) && q.refusalsSent() == 0);   // never after its end
        CHECK(!q.ended(kOne, &heard));                                                          // ended once
        CHECK(q.answered(5, 3000, &a, &heard) == DialupRequests::kNoneOut);
    }
    {   // not ended in 2 s: the agent's refusal, answered 5: the request ends
        DialupRequests q(t);
        q.heard(kOne, 1000);
        CHECK(q.refusalDue(3000, &a) && a == kOne && q.refusalOut() && q.refusalsSent() == 1);
        CHECK(!q.refusalDue(3000, &a));                                       // one at a time
        CHECK(q.answered(5, 3040, &a, &heard) == DialupRequests::kEnded && a == kOne && heard == 1000 && !q.busy());
    }
    {   // answered BUSY, then 5: sent again 100 ms later
        DialupRequests q(t);
        q.heard(kOne, 1000);
        CHECK(q.refusalDue(3000, &a));
        CHECK(q.answered(2, 3010, &a, &heard) == DialupRequests::kRetry && q.busy() && !q.refusalOut());
        CHECK(!q.refusalDue(3109, &a) && q.refusalDue(3110, &a) && q.refusalsSent() == 2);
        CHECK(q.answered(5, 3150, &a, &heard) == DialupRequests::kEnded);
    }
    {   // BUSY every time: given up after retries more, and its knock goes all the same
        DialupRequests q(t);
        q.heard(kOne, 1000);
        int64_t now = 3000;
        DialupRequests::Answer last = DialupRequests::kRetry;
        unsigned sends = 0;
        while (last == DialupRequests::kRetry) {
            CHECK(q.refusalDue(now, &a));
            ++sends;
            last = q.answered(2, now + 5, &a, &heard);
            now += 5 + 100;
        }
        CHECK(last == DialupRequests::kGaveUp && sends == t.retries + 1 && q.givenUp() == 1 && !q.busy());
    }
    {   // whether a refusal of the agent's had gone for a request the stack says has ended
        DialupRequests q(t);
        bool after = true;
        q.heard(kOne, 1000);
        CHECK(q.ended(kOne, &heard, &after) && !after);                // the car ended it
        q.heard(kOne, 2000);
        CHECK(q.refusalDue(4000, &a) && q.ended(kOne, &heard, &after) && after);
        CHECK(q.answered(5, 4030, &a, &heard) == DialupRequests::kAfterEnd);
        q.heard(kOne, 5000);                                           // BUSY, then the word before the next try
        CHECK(q.refusalDue(7000, &a) && q.answered(2, 7005, &a, &heard) == DialupRequests::kRetry);
        CHECK(q.ended(kOne, &heard, &after) && after && !q.busy());
    }
    {   // the car's end comes while the agent's refusal is out: the request ends then, and the answer says nothing more
        DialupRequests q(t);
        q.heard(kOne, 1000);
        CHECK(q.refusalDue(3000, &a));
        CHECK(q.ended(kOne, &heard) && q.refusalOut());
        CHECK(q.answered(5, 3030, &a, &heard) == DialupRequests::kAfterEnd && !q.busy());
        // A rejection or no answer counts as BUSY.
        q.heard(kOne, 4000);
        CHECK(q.refusalDue(6000, &a) && q.answered(kNoError, 6001, &a, &heard) == DialupRequests::kRetry);
        CHECK(q.refusalDue(6101, &a));
        DialupRequests::Answer w;
        CHECK(!q.refusalLost(6101 + 2999, &w, &a, &heard) && q.refusalLost(6101 + 3000, &w, &a, &heard));
        CHECK(w == DialupRequests::kRetry && q.answered(5, 9200, &a, &heard) == DialupRequests::kNoneOut);
    }
    {   // two phones: each its own time, one refusal at a time, the earlier first; clear() forgets them all
        DialupRequests q(t);
        q.heard(kTwo, 1000);
        q.heard(kOne, 1200);
        CHECK(q.refusalDue(3300, &a) && a == kTwo);
        CHECK(!q.refusalDue(3300, &a));
        CHECK(q.answered(5, 3320, &a, &heard) == DialupRequests::kEnded && a == kTwo);
        CHECK(q.refusalDue(3320, &a) && a == kOne);
        q.clear();
        CHECK(!q.busy() && q.answered(5, 3400, &a, &heard) == DialupRequests::kNoneOut && !q.ended(kOne, &heard));
        CHECK(!q.heard("", 5000) && !q.busy());   // a request that names no phone is nothing to follow
    }
}

static void testKnock() {
    {   // no ask and no poll: a phone of the agent's that is there is never dialled by itself, nor by its requests
        DialRules r(knocks());
        present(r, 1000, true);
        DialRules::Dial d;
        for (int64_t t = 1000; t < 1000 + 600000; t += 5000) {
            r.phoneAsked(kOne, t);
            CHECK(!r.next(t, facts(kOne, kTwo), &d) && r.state() == DialRules::kIdle);
        }
        r.afresh();   // nor afresh after a link left behind at the start
        CHECK(!r.next(700000, facts(kOne, kTwo), &d));
        CHECK(r.knockCounts().dials == 0 && r.knocksWaiting() == 0);
    }
    {   // a knock gives a dial at once, with no ask; a failed one brings the quiet time and no back-off, and the
        // car's record is doubted as for any dial that failed (the re-point follows it in the listener)
        DialRules r(knocks());
        present(r, 1000);
        CHECK(r.knocked(kOne, 2000) == DialRules::kKnockWaits && r.knocksWaiting() == 1);
        DialRules::Dial d = knockDial(r, 2000, facts(), DialRules::kKnock);
        CHECK(d.address == kOne && newsIs(r, DialRules::kKnockDialled, 1));
        CHECK(r.reply(kOne, kService, kService, 5, 2100) == DialRules::kNotListening);
        CHECK(r.state() == DialRules::kIdle && r.recordDoubtful() && r.backedOffUntil(kOne) == 0 && !r.heldBack(kOne, 2100));
        CHECK(r.quietUntil(kOne) == 2100 + 3000 && r.dialFailures(kOne) == 1);
        // In the quiet time, to its last moment: ignored.
        CHECK(r.knocked(kOne, 3000) == DialRules::kKnockQuiet);
        CHECK(r.knocked(kOne, 2100 + 3000 - 1) == DialRules::kKnockQuiet);
        CHECK(!r.next(5150, facts(), &d));
        // After it, a knock goes.
        CHECK(r.knocked(kOne, 2100 + 3000) == DialRules::kKnockWaits);
        knockDial(r, 6500, facts(), DialRules::kKnock);
        CHECK(r.reply(kOne, kService, kService, 0, 6600) == DialRules::kOpenNode);
        r.linkUp(6700);
        const DialRules::KnockCounts& k = r.knockCounts();
        CHECK(k.byMarker == 4 && k.ignoredQuiet == 2 && k.dials == 2 && k.dialsFailed == 1);
        // Its link up: its knocks add nothing.
        CHECK(r.knocked(kOne, 7000) == DialRules::kKnockBusy && r.knockCounts().ignoredBusy == 1);
    }
    {   // every way a knock's dial gives no link brings the quiet time, never the back-off or a retry
        for (int way = 0; way < 6; ++way) {
            std::string what = "way " + num((size_t)way);
            DialRules r(knocks());
            present(r, 1000);
            r.knocked(kOne, 2000);
            knockDial(r, 2000, facts(), DialRules::kKnock);
            int64_t at = 3000;
            if (way == 0) CHECK_IN(what, r.reply(kOne, kService, kService, 14, at) == DialRules::kUnreachable);
            if (way == 1) CHECK_IN(what, r.reply(kOne, kService, kService, 10, at) == DialRules::kRetryLater);
            if (way == 2) CHECK_IN(what, r.reply(kOne, kService, kService, kNoError, at) == DialRules::kRetryLater);
            if (way == 3) r.gaveUp(at);
            if (way == 4) {
                CHECK_IN(what, r.reply(kOne, kService, kService, 0, 2500) == DialRules::kOpenNode);
                r.notTheApp(at);
                r.ended(at);
            }
            if (way == 5) {
                CHECK_IN(what, r.reply(kOne, kService, kService, 2, 2500) == DialRules::kLookForNode);
                r.notThere(at);
            }
            CHECK_IN(what, r.state() == DialRules::kIdle && r.quietUntil(kOne) == at + 3000);
            CHECK_IN(what, r.backedOffUntil(kOne) == 0 && r.knockCounts().dialsFailed == 1);
            DialRules::Dial d;
            CHECK_IN(what, !r.next(at + 15000, facts(), &d) && !r.next(at + 600000, facts(), &d));
        }
    }
    {   // a link left behind: ended, and the phone dialled again at once for its knock; that dial failing is the
        // knock's one failure
        DialRules r(knocks());
        present(r, 1000);
        r.knocked(kOne, 2000);
        knockDial(r, 2000, facts(), DialRules::kKnock);
        CHECK(r.reply(kOne, kService, kService, 2, 2100) == DialRules::kLookForNode);
        r.stale();
        r.ended(2500);
        knockDial(r, 2600, facts(), DialRules::kAfresh);
        CHECK(r.reply(kOne, kService, kService, 5, 2700) == DialRules::kNotListening);
        CHECK(r.quietUntil(kOne) == 2700 + 3000 && r.knockCounts().dials == 1 && r.knockCounts().dialsFailed == 1);
    }
    {   // whose knock: any phone's that is there to dial, one of the agent's or not, with the pairing window open or
        // closed; not there to dial, the agent's or not; no phone at all
        DialRules r(knocks());
        present(r, 1000, true);
        DialRules::Dial d;
        // Phone two is not the agent's (facts(): phone one is), and the pairing window is closed: dialled.
        CHECK(r.knocked(kTwo, 2000) == DialRules::kKnockWaits);
        d = knockDial(r, 2000, facts(), DialRules::kKnock);
        CHECK(d.address == kTwo && newsIs(r, DialRules::kKnockDialled, 2));
        CHECK(r.reply(kTwo, kService, kService, 0, 2100) == DialRules::kOpenNode);
        r.linkUp(2200);
        r.ending();
        r.ended(2300);
        // No phone is the agent's at all: the same.
        CHECK(r.knocked(kOne, 3000) == DialRules::kKnockWaits);
        d = knockDial(r, 3000, facts(0), DialRules::kKnock);
        CHECK(d.address == kOne && newsIs(r, DialRules::kKnockDialled, 1));
        CHECK(r.reply(kOne, kService, kService, 0, 3100) == DialRules::kOpenNode);
        r.linkUp(3200);
        r.ending();
        r.ended(3300);
        DialRules::Facts open = facts(0);
        open.pairingOpen = true;
        r.knocked(kTwo, 4000);
        // The pairing window's round would come to it too; the knock goes first.
        d = knockDial(r, 4000, open, DialRules::kKnock);
        CHECK(d.address == kTwo);
        CHECK(r.knocked("", 5000) == DialRules::kKnockNotDialable);
        CHECK(r.knockCounts().dials == 3 && r.knockCounts().ignoredNotDialable == 1 &&
              r.knockCounts().ignoredBusy == 0 && r.knockCounts().ignoredQuiet == 0);
        // A phone that is not connected to the car, not paired with it with its key, or that has lost its pairing:
        // ignored as one that cannot be dialled, whether it is the agent's (i < 3) or not.
        uint32_t lists[][2] = {{kConnInRange, 7}, {kConnConnected, kSecurityKeyMissing}, {kConnConnected, 0}};
        for (int i = 0; i < 6; ++i) {
            DialRules s(knocks());
            present(s, 1000, false, lists[i % 3][0], lists[i % 3][1]);
            CHECK(s.knocked(kOne, 2000) == DialRules::kKnockWaits);
            CHECK_IN(num((size_t)i), !s.next(2000, i < 3 ? facts() : facts(kTwo), &d) &&
                                         newsIs(s, DialRules::kKnockNotDialable, 1) && s.state() == DialRules::kIdle);
        }
        DialRules s(knocks());
        present(s, 1000);
        s.knocked(kTwo, 2000);   // not in the car's list at all
        CHECK(!s.next(2000, facts(kOne, kTwo), &d) && newsIs(s, DialRules::kKnockNotDialable, 2));
    }
    {   // what it waits for, and for how long: a phone's request that is pending, a link being decided on, another
        // phone's dial, the service, the device list; a link of the agent's that is up has it ignored
        DialRules r(knocks());
        present(r, 1000, true);
        DialRules::Dial d;
        r.knocked(kOne, 2000);
        DialRules::Facts f = facts();
        f.requestPending = true;
        CHECK(!r.next(2000, f, &d) && r.knocksWaiting() == 1);
        f.requestPending = false;
        f.deciding = f.linkOfOurs = true;
        CHECK(!r.next(2500, f, &d) && r.knocksWaiting() == 1);
        f.deciding = f.linkOfOurs = false;
        f.registered = false;
        CHECK(!r.next(3000, f, &d) && r.knocksWaiting() == 1);
        d = knockDial(r, 3100, facts(), DialRules::kKnock);
        CHECK(newsIs(r, DialRules::kKnockDialled, 1));
        // While its dial is out: the other phone's knock waits, and goes once it has failed.
        r.knocked(kTwo, 3200);
        CHECK(!r.next(3300, facts(kOne, kTwo), &d) && r.knocksWaiting() == 1);
        r.reply(kOne, kService, kService, 5, 3400);
        d = knockDial(r, 3500, facts(kOne, kTwo), DialRules::kKnock);
        CHECK(d.address == kTwo && newsIs(r, DialRules::kKnockDialled, 2));
        // Up: a knock from the other phone is ignored once the link counts as the agent's.
        r.reply(kTwo, kService, kService, 0, 3600);
        r.linkUp(3700);
        r.knocked(kOne, 9000);   // its quiet time is over
        f = facts(kOne, kTwo);
        f.linkUp = f.linkOfOurs = true;
        CHECK(!r.next(9000, f, &d) && newsIs(r, DialRules::kKnockBusy, 1));
        r.ending();
        r.ended(9500);
        // One that waits too long is ignored.
        r.knocked(kOne, 10000);
        f = facts(kOne, kTwo);
        f.requestPending = true;
        CHECK(!r.next(10000 + 4000 - 1, f, &d) && r.knocksWaiting() == 1);
        CHECK(!r.next(10000 + 4000, f, &d) && newsIs(r, DialRules::kKnockBusy, 1) && r.knocksWaiting() == 0);
        // No device list yet: it waits for one.
        DialRules s(knocks());
        s.knocked(kOne, 1000);
        CHECK(!s.next(1000, facts(), &d) && s.knocksWaiting() == 1);
        present(s, 1500);
        knockDial(s, 1500, facts(), DialRules::kKnock);
    }
    {   // the connects of one burst that come up one after another bring one dial: the second finds the first
        // waiting, or its dial out
        DialRules r(knocks());
        present(r, 1000);
        DialRules::Dial d;
        CHECK(r.knocked(kOne, 2000) == DialRules::kKnockWaits);
        DialRules::Facts f = facts();
        f.requestPending = true;   // the next connect's request
        CHECK(!r.next(2010, f, &d));
        CHECK(r.knocked(kOne, 2300) == DialRules::kKnockBusy);
        knockDial(r, 2400, facts(), DialRules::kKnock);
        CHECK(r.knocked(kOne, 3100) == DialRules::kKnockBusy);
        CHECK(!r.next(3200, facts(), &d) && r.knockCounts().dials == 1 && r.knockCounts().ignoredBusy == 2);
    }
    {   // the pairing window's rounds and serial.dial, with no ask: every phone in turn
        DialRules r(knocks());
        present(r, 1000, true);
        DialRules::Facts f = facts();
        f.pairingOpen = true;
        DialRules::Dial d = knockDial(r, 2000, f, DialRules::kPairing);
        CHECK(d.address == kOne);
        r.reply(kOne, kService, kService, 5, 2100);
        d = knockDial(r, 2200, f, DialRules::kPairing);
        CHECK(d.address == kTwo && r.quietUntil(kOne) == 0);   // no knock's dial: no quiet time
        r.reply(kTwo, kService, kService, 5, 2300);
        CHECK(!r.next(2400, f, &d));
        knockDial(r, 2000 + 5000, f, DialRules::kPairing);
        r.reply(kOne, kService, kService, 14, 7100);
        CHECK(!r.next(7200, facts(), &d));   // closed: the rest of the round is not the agent's
        CHECK(r.force() == 2);
        d = knockDial(r, 8000, facts(), DialRules::kForced);
        CHECK(r.knockCounts().dials == 0);
    }
    {   // on trial: the agent's phones that are there, every trialDialMs, while no link of the agent's is up, and
        // no more once the version is confirmed
        DialRules r(knocks());
        present(r, 1000, true);
        DialRules::Facts f = facts(kOne, kTwo);
        f.onTrial = true;
        DialRules::Dial d = knockDial(r, 2000, f, DialRules::kTrial);
        CHECK(d.address == kOne);
        r.reply(kOne, kService, kService, 5, 2100);
        d = knockDial(r, 2200, f, DialRules::kTrial);
        CHECK(d.address == kTwo);
        r.reply(kTwo, kService, kService, 5, 2300);
        CHECK(!r.next(2400, f, &d) && !r.next(6999, f, &d));
        d = knockDial(r, 7000, f, DialRules::kTrial);
        CHECK(r.reply(kOne, kService, kService, 0, 7100) == DialRules::kOpenNode);
        r.linkUp(7200);
        f.linkUp = f.linkOfOurs = true;
        CHECK(!r.next(13000, f, &d));
        r.ending();
        r.ended(14000);
        f.linkUp = f.linkOfOurs = false;
        knockDial(r, 14000, f, DialRules::kTrial);   // overdue since the link came: at once
        r.reply(kOne, kService, kService, 5, 14100);
        f.onTrial = false;   // confirmed: the rest of the round goes too
        CHECK(!r.next(14200, f, &d) && !r.next(30000, f, &d));
        // Only the agent's phones.
        DialRules s(knocks());
        present(s, 1000, true);
        DialRules::Facts g = facts(kTwo);
        g.onTrial = true;
        d = knockDial(s, 2000, g, DialRules::kTrial);
        CHECK(d.address == kTwo);
        s.reply(kTwo, kService, kService, 5, 2100);
        CHECK(!s.next(2200, g, &d));
    }
}

// The trial's rounds for trialDialForMs only (dial_rules.hpp, "The trial's rounds go on for trialDialForMs only").
static void testTrialTime() {
    CHECK(DialConfig().trialDialForMs == 120000);
    DialConfig c = knocks();
    c.trialDialForMs = 20000;
    {   // rounds every trialDialMs, each dial failing, and none once the time is up: the rest of a round under way
        // is dropped, the log is told once, and a knock is dialled as when the agent is not on trial
        DialRules r(c);
        present(r, 1000, true);
        DialRules::Facts f = facts(kOne, kTwo);
        f.onTrial = true;
        DialRules::Dial d;
        for (int64_t round = 2000; round < 22000; round += 5000) {
            d = knockDial(r, round, f, DialRules::kTrial);
            CHECK(d.address == kOne && r.trialDialsFrom() == 2000 && !r.trialDialsOver());
            if (round == 17000) {
                // Phone two is still to come in this round when the time runs out.
                CHECK(r.reply(kOne, kService, kService, 5, 21999) == DialRules::kNotListening);
                break;
            }
            CHECK(r.reply(kOne, kService, kService, 5, round + 100) == DialRules::kNotListening);
            d = knockDial(r, round + 200, f, DialRules::kTrial);
            CHECK(d.address == kTwo);
            CHECK(r.reply(kTwo, kService, kService, 14, round + 300) == DialRules::kUnreachable);
            CHECK(!r.next(round + 400, f, &d) && !r.next(round + 4999, f, &d) && !r.takeTrialOver());
        }
        CHECK(r.dialFailures(kOne) == 4 && r.dialFailures(kTwo) == 3 && r.recordDoubtful());
        CHECK(!r.next(22000, f, &d) && r.state() == DialRules::kIdle);
        CHECK(r.trialDialsOver() && r.takeTrialOver() && !r.takeTrialOver());
        for (int64_t t = 22000; t < 22000 + 600000; t += 1000) CHECK(!r.next(t, f, &d));
        CHECK(!r.takeTrialOver());
        // Still on trial: a knock is dialled as at any other time.
        CHECK(r.knocked(kOne, 700000) == DialRules::kKnockWaits);
        d = knockDial(r, 700000, f, DialRules::kKnock);
        CHECK(d.address == kOne);
        CHECK(r.reply(kOne, kService, kService, 5, 700100) == DialRules::kNotListening);
        CHECK(!r.next(700200, f, &d) && !r.next(800000, f, &d));
        // A dial of the trial's that is out when the time runs out runs to its answer; the round goes no further.
        DialRules s(c);
        present(s, 1000, true);
        d = knockDial(s, 2000, f, DialRules::kTrial);
        CHECK(!s.next(30000, f, &d) && s.state() == DialRules::kDialling && s.trialDialsOver() && s.takeTrialOver());
        CHECK(s.reply(kOne, kService, kService, 5, 30100) == DialRules::kNotListening);
        CHECK(!s.next(30200, f, &d) && !s.next(60000, f, &d));
    }
    {   // the clock starts at the first next() that finds the trial, the service registered and the car's device
        // list come, whatever else holds a round back then; not at the rules' start
        DialRules r(c);
        DialRules::Facts f = facts();
        f.onTrial = true;
        f.registered = false;
        DialRules::Dial d;
        CHECK(!r.next(1000, f, &d) && r.trialDialsFrom() == 0);
        present(r, 50000);
        CHECK(!r.next(60000, f, &d) && r.trialDialsFrom() == 0);   // no service yet
        f.registered = true;
        DialRules::Facts noList = f;
        DialRules s(c);
        CHECK(!s.next(70000, noList, &d) && s.trialDialsFrom() == 0);   // no device list yet
        f.linkOfOurs = f.linkUp = true;   // a link of the agent's holds the round back, not the clock
        CHECK(!r.next(100000, f, &d) && r.trialDialsFrom() == 100000);
        f.linkOfOurs = f.linkUp = false;
        d = knockDial(r, 110000, f, DialRules::kTrial);
        r.reply(kOne, kService, kService, 5, 110100);
        d = knockDial(r, 115100, f, DialRules::kTrial);
        r.reply(kOne, kService, kService, 5, 115200);
        CHECK(!r.next(120000 - 1, f, &d) && !r.trialDialsOver());
        CHECK(!r.next(120000, f, &d) && r.trialDialsOver() && r.takeTrialOver());
        // The service going and coming back does not start it again.
        r.serviceGone();
        present(r, 130000);
        CHECK(!r.next(130000, f, &d) && r.trialDialsOver() && !r.takeTrialOver());
    }
    {   // a confirmed trial ends the count with nothing to say; a new trial has its own time, and its first round at
        // the first chance
        DialRules r(c);
        present(r, 1000);
        DialRules::Facts f = facts();
        f.onTrial = true;
        DialRules::Dial d = knockDial(r, 2000, f, DialRules::kTrial);
        r.reply(kOne, kService, kService, 5, 2100);
        CHECK(!r.next(22000, f, &d) && r.takeTrialOver());
        f.onTrial = false;   // confirmed after the time
        CHECK(!r.next(23000, f, &d) && !r.trialDialsOver() && r.trialDialsFrom() == 0 && !r.takeTrialOver());
        f.onTrial = true;   // a new trial
        d = knockDial(r, 24000, f, DialRules::kTrial);
        CHECK(r.trialDialsFrom() == 24000 && !r.trialDialsOver());
        r.reply(kOne, kService, kService, 5, 24100);
        knockDial(r, 29000, f, DialRules::kTrial);
        r.reply(kOne, kService, kService, 5, 29100);
        f.onTrial = false;   // confirmed in its time: nothing for the log
        CHECK(!r.next(30000, f, &d) && !r.takeTrialOver() && !r.next(60000, f, &d));
        f.onTrial = true;   // and another
        for (int64_t round = 61000; round < 81000; round += 5000) {
            knockDial(r, round, f, DialRules::kTrial);
            r.reply(kOne, kService, kService, 5, round + 100);
        }
        CHECK(!r.next(81000 - 1, f, &d) && !r.trialDialsOver() && r.trialDialsFrom() == 61000);
        CHECK(!r.next(81000, f, &d) && r.trialDialsOver() && r.takeTrialOver());
    }
    {   // --dial-knock off: the trial changes nothing, and has no time of its own
        DialConfig off = plain();
        off.trialDialForMs = 20000;
        DialRules onTrial(off), not_(off);
        present(onTrial, 1000, true);
        present(not_, 1000, true);
        DialRules::Facts f = facts(kOne, kTwo), g = facts(kOne, kTwo);
        f.onTrial = true;
        for (int64_t t = 1000; t < 400000; t += 500) {
            DialRules::Dial a, b;
            bool da = onTrial.next(t, f, &a), db = not_.next(t, g, &b);
            std::string at = "at " + num((size_t)t);
            CHECK_IN(at, da == db && onTrial.state() == not_.state());
            if (!da) continue;
            CHECK_IN(at, a.address == b.address && a.why == b.why && a.why != DialRules::kTrial);
            CHECK_IN(at, onTrial.listing(a.address, 0, false, f, t) == not_.listing(b.address, 0, false, g, t));
        }
        CHECK(!onTrial.takeTrialOver() && !onTrial.trialDialsOver() && onTrial.trialDialsFrom() == 0);
    }
}

int main() {
    testKnock();
    testKnockByDialup();
    testDialupRequests();
    testTrialTime();
    testRepoint();
    testAnswers();
    testAsk();
    testSettle();
    testSignalAsks();
    testEndedSignalAsks();
    testDoubt();
    testFailedDials();
    testDialFailures();
    testTriggers();
    testBackOff();
    testWhoAndWhen();
    testPairingTrouble();
    testOvertaken();
    printf("test_dial_rules: ok\n");
    return 0;
}
