// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The car dials the phone: see dial_rules.hpp.

#include "dial_rules.hpp"

#include "phones.hpp"

using namespace nbt::btspp;

namespace nbt {

DialRules::DialRules(const DialConfig& config)
    : config_(config), nextPollAt_(0), fastUntil_(0), signalAt_(0), pairingWasOpen_(false), nextPairingAt_(0),
      state_(kIdle), stale_(false), okAt_(0), roundWhy_(kForced), forcedAsk_(false), endedAskUntil_(0),
      doubtful_(false), repointOut_(false), repointAt_(0), knockDial_(false), nextTrialAt_(0), trialClock_(false),
      trialFrom_(0), trialOver_(false), trialOverNews_(false) {}

void DialRules::fastPolls(int64_t now) {
    fastUntil_ = now + config_.pollFirstForMs;
    nextPollAt_ = now + config_.pollFirstMs;
}

void DialRules::phoneAsked(const std::string& address, int64_t now) {
    if (config_.knock) return;
    // The request that is pending now, whoever's: one that names no phone is nobody's own.
    askedBy_ = addressKey(address);
    if (askedBy_.empty()) return;
    signal_ = address;
    signalAt_ = now;
}

void DialRules::linkCame() { signal_.clear(); }

void DialRules::linkEnded(int64_t now) { fastPolls(now); }

DialRules::PhoneState& DialRules::phone(const std::string& address) { return phones_[addressKey(address)]; }

const DialRules::PhoneState* DialRules::find(const std::string& address) const {
    std::map<std::string, PhoneState>::const_iterator it = phones_.find(addressKey(address));
    return it == phones_.end() ? 0 : &it->second;
}

void DialRules::devices(const std::vector<BtDevice>& devices, int64_t now, std::vector<Presence::Change>* changes,
                        std::vector<unsigned>* troubled) {
    std::vector<Presence::Change> all;
    presence_.update(devices, now, &all);
    for (size_t i = 0; i < all.size(); ++i) {
        const Presence::Change& c = all[i];
        PhoneState& s = phone(c.address);
        // A phone that comes or goes, or comes back in reach, starts afresh.
        if (c.connectionChanged && !c.added) {
            s.backOffMs = 0;
            s.backOffUntil = 0;
            s.misses = 0;
        }
        // A phone that has just connected to the car, or is connected in the list it first comes in: its settle time
        // starts.
        bool rose = c.connectionChanged && c.connectionBefore < kConnConnected;
        if (!c.removed && c.connection >= kConnConnected && (c.added || rose)) s.settleUntil = now + config_.settleMs;
        // A phone that has just connected to the car: polls come at the first rate. One that was there already when
        // the first list came is polled at the first chance anyway.
        if (c.connectionChanged && !c.added && c.connection >= kConnConnected && c.connectionBefore < kConnConnected)
            fastPolls(now);
        if (c.removed) continue;
        if (c.security == kSecurityKeyMissing && !s.trouble) {
            s.trouble = true;
            if (troubled != 0) troubled->push_back(c.number);
        } else if (s.trouble && pairedSecurity(c.security)) {
            s.trouble = false;
        }
    }
    if (changes != 0) changes->insert(changes->end(), all.begin(), all.end());
}

void DialRules::serviceGone() {
    presence_.forget();
    round_.clear();
    if (state_ == kAsking || state_ == kDialling) state_ = kIdle;
    // A knock that waits is for a car that can no longer be asked to dial; the next comes afresh.
    knocks_.clear();
    knockDial_ = false;
    // What it had out went with it, and a service that comes back starts with a record of its own.
    endedAsk_.clear();
    doubtful_ = false;
}

unsigned DialRules::pairingTrouble(const std::string& address) {
    std::string who;
    if (!colonAddress(address, &who)) {
        if (state_ != kAsking && state_ != kDialling) return 0;
        who = current_.address;
    }
    PhoneState& s = phone(who);
    if (s.trouble) return 0;
    s.trouble = true;
    return presence_.number(who);
}

bool DialRules::eligible(const Presence::Phone& p, const Facts& facts) const {
    if (!Presence::dialable(p) || troubled(p.address)) return false;
    if (facts.pairingOpen) return true;
    for (size_t i = 0; i < facts.ours.size(); ++i) {
        if (addressKey(facts.ours[i]) == addressKey(p.address)) return true;
    }
    return false;
}

unsigned DialRules::eligibleCount(const Facts& facts) const {
    unsigned n = 0;
    for (size_t i = 0; i < presence_.phones().size(); ++i) n += eligible(presence_.phones()[i], facts) ? 1 : 0;
    return n;
}

unsigned DialRules::force() {
    round_.clear();
    roundWhy_ = kForced;
    for (size_t i = 0; i < presence_.phones().size(); ++i) {
        const Presence::Phone& p = presence_.phones()[i];
        if (Presence::dialable(p) && !troubled(p.address)) round_.push_back(p.address);
    }
    return (unsigned)round_.size();
}

void DialRules::afresh() {
    if (config_.knock) return;
    round_.clear();
    roundWhy_ = kAfresh;
    for (size_t i = 0; i < presence_.phones().size(); ++i) round_.push_back(presence_.phones()[i].address);
}

// The ask of a phone, for whatever reason: kAsking. With the knock, the dial itself: kDialling.
void DialRules::start(const Presence::Phone& p, Why why, int64_t now, Dial* out) {
    std::string colon;
    current_ = Dial();
    current_.address = colonAddress(p.address, &colon) ? colon : p.address;
    current_.number = p.number;
    current_.why = why;
    current_.askedAt = now;
    current_.at = now;
    forcedAsk_ = why == kForced;
    state_ = config_.knock ? kDialling : kAsking;
    // A dial afresh after a link left behind goes on for what brought the dial before it.
    if (why != kAfresh) knockDial_ = why == kKnock;
    if (why == kKnock) ++knockCounts_.dials;
    // An ask of the phone, for whatever reason, is the retry that may have been due.
    phone(p.address).retryAt = 0;
    *out = current_;
}

// The next phone of the round that may still be asked: a forced round goes to any phone that is dialable, the
// others to the ones that are eligible; an afresh round leaves out a phone that is settling, as a poll does.
bool DialRules::takeFromRound(int64_t now, const Facts& facts, Dial* out) {
    while (!round_.empty()) {
        std::string address = round_.front();
        round_.erase(round_.begin());
        const Presence::Phone* p = presence_.find(address);
        if (p == 0 || !Presence::dialable(*p) || troubled(address)) continue;
        if (roundWhy_ != kForced && !eligible(*p, facts)) continue;
        if ((roundWhy_ == kPoll || roundWhy_ == kAfresh) && settling(address, now)) continue;
        start(*p, roundWhy_, now, out);
        return true;
    }
    return false;
}

// A phone that asked for a link, and has none of the agent's after its time: once, after its settle time and its
// back-off, and not again within signalAskMs of the last ask a request of its brought that went to the car, but for
// one more if an ask in that time was ended before its answer came.
bool DialRules::signalDue(int64_t now, const Facts& facts, Dial* out) {
    if (signal_.empty() || now - signalAt_ < (int64_t)config_.signalWaitMs) return false;
    const Presence::Phone* p = presence_.find(signal_);
    if (p == 0 || !eligible(*p, facts)) {
        signal_.clear();
        return false;
    }
    PhoneState& s = phone(p->address);
    bool counted = s.signals.asked && now - s.signals.at < (int64_t)config_.signalAskMs;
    if (counted && !s.signals.spare) {
        signal_.clear();
        return false;
    }
    // It waits: for its settle time, and for a back-off after a failed dial, which its requests do not cut short.
    if (settling(p->address, now) || now < s.backOffUntil) return false;
    signal_.clear();
    signalsBefore_ = s.signals;
    if (counted) {
        s.signals.spare = false;
        s.signals.spareUsed = true;
    } else {
        s.signals = SignalAsks();
        s.signals.asked = true;
        s.signals.at = now;
    }
    start(*p, kSignal, now, out);
    return true;
}

// The ask that is out is ended: its answer may still come, and no other ask goes until it has or its time is over.
// One that a phone's request brought gives that phone's requests one more ask in their signalAskMs, once: it went to
// the car, so it counts, but its answer was not waited for.
void DialRules::endAsk(int64_t) {
    endedAsk_ = current_.address;
    endedAskUntil_ = current_.askedAt + config_.askLimitMs;
    state_ = kIdle;
    if (current_.why != kSignal) return;
    SignalAsks& c = phone(current_.address).signals;
    if (c.asked && !c.spareUsed) c.spare = true;
}

DialRules::Listing DialRules::notSent(int64_t now) {
    if (state_ != kAsking) return kNotThisAsk;
    if (current_.why == kSignal) phone(current_.address).signals = signalsBefore_;
    return missed(now);
}

bool DialRules::keepAsking(const Facts& facts, int64_t now) {
    if (state_ != kAsking) return false;
    // The phone's own request that brought this ask may still be pending, or come again: that holds back the rest,
    // not this one. Any other request, one that names no phone among them, ends it.
    bool own = current_.why == kSignal && !askedBy_.empty() && askedBy_ == addressKey(current_.address);
    if (facts.registered && !facts.linkOfOurs && (!facts.requestPending || own) && !repointWaits(now)) return true;
    endAsk(now);
    return false;
}

// ---- the knock ---------------------------------------------------------------------------------------------------

void DialRules::count(KnockEnd why) {
    switch (why) {
    case kKnockQuiet: ++knockCounts_.ignoredQuiet; break;
    case kKnockBusy: ++knockCounts_.ignoredBusy; break;
    case kKnockNotDialable: ++knockCounts_.ignoredNotDialable; break;
    default: break;
    }
}

DialRules::KnockEnd DialRules::knocked(const std::string& address, int64_t now, bool dialup) {
    if (!config_.knock) return kNoKnock;
    if (dialup) ++knockCounts_.byDialup;
    else ++knockCounts_.byMarker;
    std::string key = addressKey(address);
    if (key.empty()) {
        count(kKnockNotDialable);
        return kKnockNotDialable;
    }
    // A knock in the quiet time brings no dial.
    const PhoneState* s = find(address);
    if (s != 0 && now < s->quietUntil) {
        count(kKnockQuiet);
        return kKnockQuiet;
    }
    // The phone's dial is out or up, or its knock waits already: this one adds nothing. That is how the connects of a
    // burst that come up one after another bring one dial.
    bool same = state_ != kIdle && addressKey(current_.address) == key;
    for (size_t i = 0; i < knocks_.size() && !same; ++i) same = addressKey(knocks_[i].address) == key;
    if (same) {
        count(kKnockBusy);
        return kKnockBusy;
    }
    Knock k;
    k.address = address;
    k.at = now;
    k.dialup = dialup;
    knocks_.push_back(k);
    return kKnockWaits;
}

std::vector<DialRules::KnockNews> DialRules::takeKnockNews() {
    std::vector<KnockNews> news;
    news.swap(knockNews_);
    return news;
}

int64_t DialRules::quietUntil(const std::string& address) const {
    const PhoneState* s = find(address);
    return s == 0 ? 0 : s->quietUntil;
}

void DialRules::ignore(const Knock& k, KnockEnd why, int64_t now) {
    count(why);
    KnockNews n;
    n.number = presence_.number(k.address);
    n.end = why;
    n.waitedMs = now - k.at;
    n.dialup = k.dialup;
    knockNews_.push_back(n);
}

// The knocks that wait and can no longer bring a dial: a link of the agent's is up, the phone is not there to dial,
// or it has waited knockWaitMs. Whether the phone is one of the agent's does not count (see the top of
// dial_rules.hpp). A device list that has not come yet tells nothing of the phone: the knock waits for it.
void DialRules::judgeKnocks(int64_t now, const Facts& facts) {
    for (size_t i = 0; i < knocks_.size();) {
        Knock k = knocks_[i];
        const Presence::Phone* p = presence_.known() ? presence_.find(k.address) : 0;
        KnockEnd why = kKnockWaits;
        if (facts.linkUp) why = kKnockBusy;
        else if (presence_.known() && (p == 0 || !Presence::dialable(*p) || troubled(k.address))) why = kKnockNotDialable;
        else if (now - k.at >= (int64_t)config_.knockWaitMs) why = kKnockBusy;
        if (why == kKnockWaits) {
            ++i;
            continue;
        }
        knocks_.erase(knocks_.begin() + (long)i);
        ignore(k, why, now);
    }
}

// A dial a knock brought gave the agent no link: the phone's knocks are ignored for a while. No back-off: the
// quiet time takes its place.
void DialRules::knockFailed(int64_t now) {
    if (!knockDial_) return;
    knockDial_ = false;
    ++knockCounts_.dialsFailed;
    phone(current_.address).quietUntil = now + config_.knockQuietMs;
}

bool DialRules::takeTrialOver() {
    bool news = trialOverNews_;
    trialOverNews_ = false;
    return news;
}

// The trial has ended: a trial after it has its own time, and its first round at the first chance.
void DialRules::trialEnded() {
    trialClock_ = false;
    trialOver_ = false;
    trialOverNews_ = false;
    nextTrialAt_ = 0;
}

// Whether the trial brings rounds now: the agent is on trial, and trialDialForMs have not passed since the first
// moment a round could go, the service registered and the car's device list come (see the top of dial_rules.hpp).
// Facts that say it is not on trial end the count.
bool DialRules::trialDials(int64_t now, const Facts& facts) {
    if (!facts.onTrial) {
        trialEnded();
        return false;
    }
    if (trialOver_) return false;
    if (!trialClock_) {
        if (!facts.registered || !presence_.known()) return false;
        trialClock_ = true;
        trialFrom_ = now;
    }
    if (now - trialFrom_ < (int64_t)config_.trialDialForMs) return true;
    trialOver_ = true;
    trialOverNews_ = true;
    return false;
}

// With the knock: a dial, with no ask, for a knock, a round of the pairing window's, serial.dial's or the trial's, or
// a phone whose link left behind has just been ended; nothing else.
bool DialRules::nextKnockMode(int64_t now, const Facts& facts, Dial* out) {
    if (facts.linkOfOurs || facts.requestPending) round_.clear();
    // A round the trial brought ends with the trial, or with the trial's time for rounds.
    bool trial = trialDials(now, facts);
    if (!trial && roundWhy_ == kTrial) round_.clear();
    judgeKnocks(now, facts);
    if (state_ != kIdle || !facts.registered || facts.linkOfOurs || !presence_.known()) return false;
    // The car takes one ServiceConnect of the agent's at a time.
    if (repointWaits(now)) return false;
    // A phone's request that is pending may bring a link of the agent's, or a knock: nothing goes until it has.
    if (facts.requestPending) return false;
    if (stale_) {
        // A link that was left behind has just been ended: its phone at once, once, for what brought the dial.
        stale_ = false;
        const Presence::Phone* p = presence_.find(current_.address);
        if (p != 0 && Presence::dialable(*p) && !troubled(p->address)) {
            bool forced = forcedAsk_;
            start(*p, kAfresh, now, out);
            forcedAsk_ = forced;
            return true;
        }
        knockFailed(now);
    }
    // The first knock that waits: judgeKnocks() has just found its phone there and dialable, the agent's or not.
    while (!knocks_.empty()) {
        Knock k = knocks_.front();
        knocks_.erase(knocks_.begin());
        const Presence::Phone* p = presence_.find(k.address);
        if (p == 0) continue;
        KnockNews n;
        n.number = p->number;
        n.end = kKnockDialled;
        n.waitedMs = now - k.at;
        n.dialup = k.dialup;
        knockNews_.push_back(n);
        start(*p, kKnock, now, out);
        return true;
    }
    if (takeFromRound(now, facts, out)) return true;
    // The pairing window: every phone there, in turn.
    if (facts.pairingOpen && (!pairingWasOpen_ || now >= nextPairingAt_)) {
        nextPairingAt_ = now + config_.pairingRoundMs;
        round_.clear();
        roundWhy_ = kPairing;
        for (size_t i = 0; i < presence_.phones().size(); ++i) round_.push_back(presence_.phones()[i].address);
        pairingWasOpen_ = true;
        if (takeFromRound(now, facts, out)) return true;
    }
    if (!facts.pairingOpen) pairingWasOpen_ = false;
    // On trial: the agent's phones that are there, in turn, so that the app that waits for the new version is
    // reached whether or not it knocks; for trialDialForMs.
    if (trial && now >= nextTrialAt_) {
        nextTrialAt_ = now + config_.trialDialMs;
        round_.clear();
        roundWhy_ = kTrial;
        for (size_t i = 0; i < presence_.phones().size(); ++i) {
            if (eligible(presence_.phones()[i], facts)) round_.push_back(presence_.phones()[i].address);
        }
        if (takeFromRound(now, facts, out)) return true;
    }
    return false;
}

bool DialRules::next(int64_t now, const Facts& facts, Dial* out) {
    if (config_.knock) return nextKnockMode(now, facts, out);
    // A link of the agent's has come, or may be coming: what is left of a round is not wanted any more, nor the ask.
    if (facts.linkOfOurs || facts.requestPending) round_.clear();
    if (state_ == kAsking) keepAsking(facts, now);
    if (state_ != kIdle || !facts.registered || facts.linkOfOurs || !presence_.known()) return false;
    // The car takes one ServiceConnect of the agent's at a time: none may follow an ask while the re-point waits.
    if (repointWaits(now)) return false;
    // The car answers one query at a time: one that was ended may be out still.
    if (!endedAsk_.empty()) {
        if (now < endedAskUntil_) return false;
        endedAsk_.clear();
    }
    // While a phone's request is pending, only the ask that request brings: the rest wait for what comes of it.
    if (facts.requestPending) return signalDue(now, facts, out);
    if (stale_) {
        // A link that was left behind has just been ended: its phone at once, once, unless it is settling.
        stale_ = false;
        // It holds what brought the dial before it: one a forced round brought is still one for any dialable phone.
        const Presence::Phone* p = presence_.find(current_.address);
        if (p != 0 && Presence::dialable(*p) && !troubled(p->address) && !settling(p->address, now)) {
            bool forced = forcedAsk_;
            start(*p, kAfresh, now, out);
            forcedAsk_ = forced;
            return true;
        }
    }
    if (takeFromRound(now, facts, out)) return true;

    if (signalDue(now, facts, out)) return true;
    // The pairing window: every phone there, in turn.
    if (facts.pairingOpen && (!pairingWasOpen_ || now >= nextPairingAt_)) {
        nextPairingAt_ = now + config_.pairingRoundMs;
        round_.clear();
        roundWhy_ = kPairing;
        for (size_t i = 0; i < presence_.phones().size(); ++i) round_.push_back(presence_.phones()[i].address);
        pairingWasOpen_ = true;
        if (takeFromRound(now, facts, out)) return true;
    }
    if (!facts.pairingOpen) pairingWasOpen_ = false;
    // A retry that is due; one for a phone that is settling waits.
    for (size_t i = 0; i < presence_.phones().size(); ++i) {
        const Presence::Phone& p = presence_.phones()[i];
        std::map<std::string, PhoneState>::iterator it = phones_.find(addressKey(p.address));
        if (it == phones_.end() || it->second.retryAt == 0 || now < it->second.retryAt) continue;
        if (now < it->second.settleUntil) continue;
        it->second.retryAt = 0;
        if (!eligible(p, facts) || now < it->second.backOffUntil) continue;
        start(p, kRetry, now, out);
        return true;
    }
    // A poll: each phone of the agent's that is eligible, not held back and not settling. One that is settling has
    // the next poll come as soon as it has settled.
    if (now >= nextPollAt_) {
        nextPollAt_ = now + (now < fastUntil_ ? config_.pollFirstMs : config_.pollLaterMs);
        round_.clear();
        roundWhy_ = kPoll;
        int64_t settled = 0;
        for (size_t i = 0; i < presence_.phones().size(); ++i) {
            const Presence::Phone& p = presence_.phones()[i];
            if (!eligible(p, facts) || heldBack(p.address, now)) continue;
            if (settling(p.address, now)) {
                int64_t until = find(p.address)->settleUntil;
                if (settled == 0 || until < settled) settled = until;
                continue;
            }
            round_.push_back(p.address);
        }
        if (settled != 0 && settled < nextPollAt_) nextPollAt_ = settled;
        if (takeFromRound(now, facts, out)) return true;
    }
    return false;
}

// An ask with an error or no answer: as one that did not list the service, only counted. An ask does no harm, and the
// next poll asks again.
DialRules::Listing DialRules::missed(int64_t) {
    state_ = kIdle;
    ++phone(current_.address).misses;
    return kNoListing;
}

DialRules::Listing DialRules::listing(const std::string& address, uint32_t error, bool listed, const Facts& facts,
                                      int64_t now) {
    if (state_ != kAsking || addressKey(address).empty() || addressKey(address) != addressKey(current_.address)) {
        // The answer to an ask that was ended: the car is free for the next.
        if (!endedAsk_.empty() && !addressKey(address).empty() && addressKey(address) == addressKey(endedAsk_))
            endedAsk_.clear();
        return kNotThisAsk;
    }
    if (error != 0) return missed(now);
    phone(current_.address).misses = 0;
    state_ = kIdle;
    if (!listed) return kNotListed;
    // What brought the ask must still hold: for a forced round, that the phone is dialable; for anything else, that
    // it is eligible as things are now (still one of the agent's phones, or the pairing window still open).
    const Presence::Phone* p = presence_.find(current_.address);
    bool due = p != 0 && (forcedAsk_ ? Presence::dialable(*p) && !troubled(current_.address) : eligible(*p, facts));
    if (!due) return kNoLongerDue;
    state_ = kDialling;
    current_.at = now;
    return kDialNow;
}

bool DialRules::askOverdue(int64_t now) const {
    return state_ == kAsking && now - current_.askedAt >= (int64_t)config_.askLimitMs;
}

DialRules::Listing DialRules::listingLost(int64_t now) {
    if (state_ != kAsking) return kNotThisAsk;
    return missed(now);
}

void DialRules::askDropped(int64_t now) {
    if (state_ == kAsking) endAsk(now);
}

bool DialRules::settling(const std::string& address, int64_t now) const {
    const PhoneState* s = find(address);
    return s != 0 && now < s->settleUntil;
}

unsigned DialRules::askMisses(const std::string& address) const {
    const PhoneState* s = find(address);
    return s == 0 ? 0 : s->misses;
}

DialRules::Reply DialRules::reply(const std::string& address, const std::string& service, const std::string& ourService,
                                  uint32_t error, int64_t now) {
    if (state_ != kDialling || service != ourService || addressKey(address) != addressKey(current_.address)) return kNotThis;
    if (error == 0) {
        state_ = kUp;
        okAt_ = now;
        doubtful_ = false;
        return kOpenNode;
    }
    state_ = kIdle;
    if (error == 2) return kLookForNode;
    // A rejection of the request was never the car's answer, and 10 does no harm: tried again a while later. With
    // the knock there are no retries: one a knock brought gives the phone its quiet time, as any failure does.
    if (!doubtingAnswer(error)) {
        if (knockDial_) knockFailed(now);
        else phone(current_.address).retryAt = now + config_.retryMs;
        return kRetryLater;
    }
    // A failure the car answered, after the phone listed the service: its record may be wrong from here on, and the
    // phone is not dialled again soon.
    doubtful_ = true;
    dialFailed(now);
    if (error == 5) return kNotListening;
    if (error == 14) return kUnreachable;
    return kFailed;
}

bool DialRules::doubtingAnswer(uint32_t error) {
    return error != 0 && error != 2 && error != 10 && error != kNoError;
}

bool DialRules::overdue(int64_t now) const { return state_ == kDialling && now - current_.at >= (int64_t)config_.replyLimitMs; }

void DialRules::gaveUp(int64_t now) {
    dialFailed(now);
    state_ = kIdle;
    doubtful_ = true;
}

// A dial of the current phone that gave the agent no link: the phone is backed off, or for a dial a knock brought
// given its quiet time, and the run is counted.
void DialRules::dialFailed(int64_t now) {
    if (knockDial_) knockFailed(now);
    else backOff(current_.address, now);
    ++phone(current_.address).dialFails;
}

void DialRules::notThere(int64_t now) {
    if (knockDial_) knockFailed(now);
    else phone(current_.address).retryAt = now + config_.retryMs;
    state_ = kIdle;
}

void DialRules::stale() { state_ = kEnding; stale_ = true; }

void DialRules::notEnded(int64_t now) {
    // Dialled again at once, it would find the same link, and the car would be asked in vain once more.
    stale_ = false;
    if (knockDial_) knockFailed(now);
    else backOff(current_.address, now);
    state_ = kIdle;
}

bool DialRules::overtakes(int64_t now) const {
    return state_ == kDialling || (okAt_ != 0 && now >= okAt_ && now - okAt_ < (int64_t)config_.overtakeMs);
}

void DialRules::linkUp(int64_t) {
    round_.clear();
    signal_.clear();
    PhoneState& s = phone(current_.address);
    s.backOffMs = 0;
    s.backOffUntil = 0;
    s.dialFails = 0;
    state_ = kUp;
    knockDial_ = false;
}

void DialRules::notTheApp(int64_t now) {
    dialFailed(now);
    state_ = kEnding;
}

unsigned DialRules::dialFailures(const std::string& address) const {
    const PhoneState* s = find(address);
    return s == 0 ? 0 : s->dialFails;
}

void DialRules::ending() {
    if (state_ == kUp) state_ = kEnding;
}

void DialRules::ended(int64_t) {
    state_ = kIdle;
    // A stale link's phone is dialled again by next(), once; otherwise nothing is owed.
}

void DialRules::phoneLinkEnded() { doubtful_ = false; }

void DialRules::repointSent(int64_t now) {
    repointOut_ = true;
    repointAt_ = now;
}

void DialRules::repointDone() { repointOut_ = false; }

bool DialRules::repointWaits(int64_t now) const {
    return repointOut_ && now - repointAt_ < (int64_t)config_.repointLimitMs;
}

bool DialRules::repointOverdue(int64_t now) const {
    return repointOut_ && now - repointAt_ >= (int64_t)config_.repointLimitMs;
}

void DialRules::backOff(const std::string& address, int64_t now) {
    PhoneState& s = phone(address);
    if (s.backOffMs == 0) s.backOffMs = config_.backOffMs;
    s.backOffUntil = now + s.backOffMs;
    s.backOffMs = s.backOffMs * 2 > (int64_t)config_.backOffMostMs ? (int64_t)config_.backOffMostMs : s.backOffMs * 2;
}

int64_t DialRules::backedOffUntil(const std::string& address) const {
    const PhoneState* s = find(address);
    return s == 0 ? 0 : s->backOffUntil;
}

int64_t DialRules::nextBackOffMs(const std::string& address) const {
    const PhoneState* s = find(address);
    return s == 0 || s->backOffMs == 0 ? (int64_t)config_.backOffMs : s->backOffMs;
}

bool DialRules::heldBack(const std::string& address, int64_t now) const {
    const PhoneState* s = find(address);
    return s != 0 && (now < s->backOffUntil || (s->retryAt != 0 && now < s->retryAt));
}

bool DialRules::troubled(const std::string& address) const {
    const PhoneState* s = find(address);
    return s != 0 && s->trouble;
}

}  // namespace nbt
