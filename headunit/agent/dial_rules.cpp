// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The car dials the phone: see dial_rules.hpp.

#include "dial_rules.hpp"

#include "phones.hpp"

using namespace nbt::btspp;

namespace nbt {

DialRules::DialRules(const DialConfig& config) : config_(config), state_(kIdle), stale_(false), roundWhy_(kForced) {}

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
        }
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
    if (state_ == kDialling) state_ = kIdle;
}

unsigned DialRules::pairingTrouble(const std::string& address) {
    std::string who;
    if (!colonAddress(address, &who)) {
        if (state_ != kDialling) return 0;
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

void DialRules::start(const Presence::Phone& p, Why why, int64_t now, Dial* out) {
    std::string colon;
    current_ = Dial();
    current_.address = colonAddress(p.address, &colon) ? colon : p.address;
    current_.number = p.number;
    current_.why = why;
    current_.at = now;
    state_ = kDialling;
    *out = current_;
}

// The next phone of the round that may still be dialled: a forced round goes to any phone that is dialable, the
// others to the ones that are eligible.
bool DialRules::takeFromRound(int64_t now, const Facts& facts, Dial* out) {
    while (!round_.empty()) {
        std::string address = round_.front();
        round_.erase(round_.begin());
        const Presence::Phone* p = presence_.find(address);
        if (p == 0 || !Presence::dialable(*p) || troubled(address)) continue;
        if (roundWhy_ != kForced && !eligible(*p, facts)) continue;
        start(*p, roundWhy_, now, out);
        return true;
    }
    return false;
}

bool DialRules::next(int64_t now, const Facts& facts, Dial* out) {
    if (state_ != kIdle || !facts.registered || facts.linkOfOurs || !presence_.known()) return false;
    if (stale_) {
        // A link that was left behind has just been ended: its phone at once, once.
        stale_ = false;
        const Presence::Phone* p = presence_.find(current_.address);
        if (p != 0 && Presence::dialable(*p) && !troubled(p->address)) {
            start(*p, kAfresh, now, out);
            return true;
        }
    }
    return takeFromRound(now, facts, out);
}

DialRules::Reply DialRules::reply(const std::string& address, const std::string& service, const std::string& ourService,
                                  uint32_t error, int64_t now) {
    if (state_ != kDialling || service != ourService || addressKey(address) != addressKey(current_.address)) return kNotThis;
    if (error == 0) {
        state_ = kUp;
        return kOpenNode;
    }
    state_ = kIdle;
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
}

void DialRules::notThere(int64_t now) {
    phone(current_.address).retryAt = now + config_.retryMs;
    state_ = kIdle;
}

void DialRules::stale() { state_ = kEnding; stale_ = true; }

void DialRules::linkUp(int64_t) {
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
