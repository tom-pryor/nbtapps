// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Which of the car's phones are there: see presence.hpp.

#include "presence.hpp"

namespace nbt {
namespace btspp {

unsigned Presence::number(const std::string& address) {
    std::string key = addressKey(address);
    std::map<std::string, unsigned>::const_iterator it = numbers_.find(key);
    if (it != numbers_.end()) return it->second;
    unsigned n = next_++;
    numbers_[key] = n;
    return n;
}

const Presence::Phone* Presence::find(const std::string& address) const {
    std::string key = addressKey(address);
    if (key.empty()) return 0;
    for (size_t i = 0; i < phones_.size(); ++i) {
        if (addressKey(phones_[i].address) == key) return &phones_[i];
    }
    return 0;
}

void Presence::update(const std::vector<BtDevice>& devices, int64_t nowMs, std::vector<Change>* changes) {
    std::vector<Phone> next;
    for (size_t i = 0; i < devices.size(); ++i) {
        const BtDevice& d = devices[i];
        if (addressKey(d.address).empty()) continue;
        bool twice = false;
        for (size_t k = 0; k < next.size() && !twice; ++k) twice = addressKey(next[k].address) == addressKey(d.address);
        if (twice) continue;
        Phone p;
        p.address = d.address;
        p.number = number(d.address);
        p.connection = d.connection;
        p.security = d.security;
        p.connectionAt = p.securityAt = nowMs;
        Change c;
        c.address = d.address;
        c.number = p.number;
        c.connection = d.connection;
        c.security = d.security;
        const Phone* before = find(d.address);
        if (before == 0) {
            c.added = true;
            c.connectionChanged = c.securityChanged = true;
        } else {
            c.connectionBefore = before->connection;
            c.securityBefore = before->security;
            c.connectionChanged = before->connection != d.connection;
            c.securityChanged = before->security != d.security;
            if (!c.connectionChanged) p.connectionAt = before->connectionAt;
            if (!c.securityChanged) p.securityAt = before->securityAt;
        }
        next.push_back(p);
        if (changes != 0 && (c.added || c.connectionChanged || c.securityChanged)) changes->push_back(c);
    }
    for (size_t i = 0; i < phones_.size() && changes != 0; ++i) {
        bool kept = false;
        for (size_t k = 0; k < next.size() && !kept; ++k) kept = addressKey(next[k].address) == addressKey(phones_[i].address);
        if (kept) continue;
        Change c;
        c.address = phones_[i].address;
        c.number = phones_[i].number;
        c.removed = true;
        c.connectionChanged = phones_[i].connection != kConnNotInRange;
        c.connectionBefore = phones_[i].connection;
        c.securityBefore = c.security = phones_[i].security;
        changes->push_back(c);
    }
    phones_ = next;
    known_ = true;
}

unsigned Presence::presentCount() const {
    unsigned n = 0;
    for (size_t i = 0; i < phones_.size(); ++i) n += present(phones_[i]) ? 1 : 0;
    return n;
}

unsigned Presence::dialableCount() const {
    unsigned n = 0;
    for (size_t i = 0; i < phones_.size(); ++i) n += dialable(phones_[i]) ? 1 : 0;
    return n;
}

}  // namespace btspp
}  // namespace nbt
