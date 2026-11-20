// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The car dials the phone: see dial_rules.hpp.

#include "dial_rules.hpp"

#include "phones.hpp"

using namespace nbt::btspp;

namespace nbt {

DialRules::DialRules(const DialConfig& config)
    : config_(config), nextPollAt_(0), fastUntil_(0), signalAt_(0), pairingWasOpen_(false), nextPairingAt_(0),
      state_(kIdle), stale_(false), okAt_(0), roundWhy_(kForced), endedAskUntil_(0), doubtful_(false) {}

void DialRules::fastPolls(int64_t now) {
    fastUntil_ = now + config_.pollFirstForMs;
    nextPollAt_ = now + config_.pollFirstMs;
}

void DialRules::phoneAsked(const std::string& address, int64_t now) {
    if (addressKey(address).empty()) return;
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
            s.waitChange = false;
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
    round_.clear();
    roundWhy_ = kAfresh;
    for (size_t i = 0; i < presence_.phones().size(); ++i) round_.push_back(presence_.phones()[i].address);
}

// The ask of a phone, for whatever reason: kAsking.
void DialRules::start(const Presence::Phone& p, Why why, int64_t now, Dial* out) {
    std::string colon;
    current_ = Dial();
    current_.address = colonAddress(p.address, &colon) ? colon : p.address;
    current_.number = p.number;
    current_.why = why;
    current_.askedAt = now;
    current_.at = now;
    state_ = kAsking;
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

// A phone that asked for a link, and has none of the agent's after its time: once, after its settle time, and not
// again within signalAskMs of the last ask a request of its brought.
bool DialRules::signalDue(int64_t now, const Facts& facts, Dial* out) {
    if (signal_.empty() || now - signalAt_ < (int64_t)config_.signalWaitMs) return false;
    const Presence::Phone* p = presence_.find(signal_);
    if (p == 0 || !eligible(*p, facts)) {
        signal_.clear();
        return false;
    }
    PhoneState& s = phone(p->address);
    if (s.signalAsked && now - s.signalAskAt < (int64_t)config_.signalAskMs) {
        signal_.clear();
        return false;
    }
    if (settling(p->address, now)) return false;   // it waits
    signal_.clear();
    s.signalAsked = true;
    s.signalAskAt = now;
    start(*p, kSignal, now, out);
    return true;
}

// The ask that is out is ended: its answer may still come, and no other ask goes until it has or its time is over.
void DialRules::endAsk(int64_t) {
    endedAsk_ = current_.address;
    endedAskUntil_ = current_.askedAt + config_.askLimitMs;
    state_ = kIdle;
}

bool DialRules::keepAsking(const Facts& facts, int64_t now) {
    if (state_ != kAsking) return false;
    // The phone's own request that brought this ask may still be pending, or come again: that holds back the rest,
    // not this one.
    bool own = current_.why == kSignal && (signal_.empty() || addressKey(signal_) == addressKey(current_.address));
    if (facts.registered && !facts.linkOfOurs && (!facts.requestPending || own)) return true;
    endAsk(now);
    return false;
}

bool DialRules::next(int64_t now, const Facts& facts, Dial* out) {
    // A link of the agent's has come, or may be coming: what is left of a round is not wanted any more, nor the ask.
    if (facts.linkOfOurs || facts.requestPending) round_.clear();
    if (state_ == kAsking) keepAsking(facts, now);
    if (state_ != kIdle || !facts.registered || facts.linkOfOurs || !presence_.known()) return false;
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
        const Presence::Phone* p = presence_.find(current_.address);
        if (p != 0 && Presence::dialable(*p) && !troubled(p->address) && !settling(p->address, now)) {
            start(*p, kAfresh, now, out);
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
        if (!eligible(p, facts) || it->second.waitChange || now < it->second.backOffUntil) continue;
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

// An ask with an error or no answer: as one that did not list the service, and the third in a row backs the phone
// off.
DialRules::Listing DialRules::missed(int64_t now) {
    state_ = kIdle;
    PhoneState& s = phone(current_.address);
    if (++s.misses < kAskMissesToBackOff) return kNoListing;
    s.misses = 0;
    backOff(current_.address, now);
    return kAskBackedOff;
}

DialRules::Listing DialRules::listing(const std::string& address, uint32_t error, bool listed, int64_t now) {
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
    const Presence::Phone* p = presence_.find(current_.address);
    if (p == 0 || !Presence::dialable(*p) || troubled(current_.address)) return kNoLongerDue;
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
    // A failure the car answered, after the phone listed the service: its record may be wrong from here on.
    if (error != 2 && error != 10 && error != kNoError) doubtful_ = true;
    if (error == 2) return kLookForNode;
    if (error == 5) return kNotListening;
    if (error == 14) {
        phone(current_.address).waitChange = true;
        return kUnreachable;
    }
    phone(current_.address).retryAt = now + config_.retryMs;
    return kRetryLater;
}

bool DialRules::overdue(int64_t now) const { return state_ == kDialling && now - current_.at >= (int64_t)config_.replyLimitMs; }

void DialRules::gaveUp(int64_t now) {
    backOff(current_.address, now);
    state_ = kIdle;
    doubtful_ = true;
}

void DialRules::notThere(int64_t now) {
    phone(current_.address).retryAt = now + config_.retryMs;
    state_ = kIdle;
}

void DialRules::stale() { state_ = kEnding; stale_ = true; }

void DialRules::notEnded(int64_t now) {
    // Dialled again at once, it would find the same link, and the car would be asked in vain once more.
    stale_ = false;
    backOff(current_.address, now);
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
    state_ = kUp;
}

void DialRules::notTheApp(int64_t now) {
    backOff(current_.address, now);
    state_ = kEnding;
}

void DialRules::ending() {
    if (state_ == kUp) state_ = kEnding;
}

void DialRules::ended(int64_t) {
    state_ = kIdle;
    // A stale link's phone is dialled again by next(), once; otherwise nothing is owed.
}

void DialRules::phoneLinkEnded() { doubtful_ = false; }

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
    return s != 0 && (now < s->backOffUntil || s->waitChange || (s->retryAt != 0 && now < s->retryAt));
}

bool DialRules::troubled(const std::string& address) const {
    const PhoneState* s = find(address);
    return s != 0 && s->trouble;
}

}  // namespace nbt
