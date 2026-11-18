// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The paired phones: see phones.hpp.

#include "phones.hpp"

#include "fsutil.hpp"
#include "hex.hpp"
#include "json.hpp"
#include "json_writer.hpp"
#include "utf8.hpp"

namespace nbt {

namespace {

const int kPhonesVersion = 1;
const size_t kMaxPhonesBytes = 64u << 10;
const size_t kPhoneIdChars = 32;
const size_t kMaxPhoneNameChars = 64;

bool isHexDigit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

}  // namespace

bool colonAddress(const std::string& in, std::string* out) {
    std::string digits;
    if (in.size() == 17) {
        for (size_t i = 0; i < in.size(); ++i) {
            if (i % 3 == 2) {
                if (in[i] != ':') return false;
            } else {
                digits += in[i];
            }
        }
    } else if (in.size() == 12) {
        digits = in;
    } else {
        return false;
    }
    std::string shown;
    for (size_t i = 0; i < digits.size(); ++i) {
        char c = digits[i];
        if (!isHexDigit(c)) return false;
        if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
        if (i > 0 && i % 2 == 0) shown += ':';
        shown += c;
    }
    if (out != 0) *out = shown;
    return true;
}

std::string PhoneStore::encode(const std::vector<PairedPhone>& phones) {
    json::Writer w;
    w.beginObject().key("version").integer(kPhonesVersion).key("phones").beginArray();
    for (size_t i = 0; i < phones.size(); ++i) {
        const PairedPhone& p = phones[i];
        w.beginObject().key("phoneId").string(p.id).key("name").string(p.name);
        w.key("key").string(toHex(p.key, sizeof p.key)).key("pairedAt").integer(p.pairedAt);
        if (!p.address.empty()) w.key("address").string(p.address);
        w.endObject();
    }
    w.endArray().endObject();
    return w.text() + "\n";
}

bool PhoneStore::decode(const std::string& text, std::vector<PairedPhone>* phones, std::string* problem) {
    phones->clear();
    json::Document doc;
    std::string why;
    if (!doc.parse(text, &why)) {
        if (problem != 0) *problem = "it is not JSON of the profile: " + why;
        return false;
    }
    json::Value version = doc.root().find("version"), list = doc.root().find("phones");
    if (!version.isInteger() || version.integer() != kPhonesVersion || !list.isArray() || list.size() > kMaxPairedPhones) {
        if (problem != 0) *problem = "it is not version 1 with a list of at most 16 phones";
        return false;
    }
    for (size_t i = 0; i < list.size(); ++i) {
        json::Value one = list.at(i);
        json::Value id = one.find("phoneId"), name = one.find("name"), key = one.find("key"), pairedAt = one.find("pairedAt");
        json::Value address = one.find("address");
        PairedPhone phone;
        uint8_t idBytes[kPhoneIdChars / 2];
        bool good = id.isString() && fromHex(id.string(), idBytes, sizeof idBytes) && name.isString() &&
                    utf8Length(name.string()) >= 1 && utf8Length(name.string()) <= kMaxPhoneNameChars && key.isString() &&
                    fromHex(key.string(), phone.key, sizeof phone.key) && pairedAt.isInteger() && pairedAt.integer() >= 0;
        // Only the form this class writes.
        std::string shown;
        good = good && (!address.present() ||
                        (address.isString() && colonAddress(address.string(), &shown) && shown == address.string()));
        for (size_t k = 0; good && k < phones->size(); ++k) good = (*phones)[k].id != id.string();
        if (!good) {
            phones->clear();
            if (problem != 0) *problem = "one of its phones has no usable id, name, key, date or address, or is there twice";
            return false;
        }
        phone.id = id.string();
        phone.name = name.string();
        phone.pairedAt = pairedAt.integer();
        if (address.present()) phone.address = address.string();
        phones->push_back(phone);
    }
    return true;
}

bool PhoneStore::load(const std::string& path, std::string* problem) {
    path_ = path;
    phones_.clear();
    if (fs::kindOf(path) == fs::kNone) return true;
    std::string text, why;
    if (!fs::readFile(path, kMaxPhonesBytes, &text, &why)) {
        if (problem != 0) *problem = why;
        return false;
    }
    return decode(text, &phones_, problem);
}

const PairedPhone* PhoneStore::find(const std::string& id) const {
    for (size_t i = 0; i < phones_.size(); ++i) {
        if (phones_[i].id == id) return &phones_[i];
    }
    return 0;
}

bool PhoneStore::replace(const std::vector<PairedPhone>& phones, std::string* error) {
    if (!fs::writeFile(path_, encode(phones), 0600, error)) return false;
    phones_ = phones;
    return true;
}

bool PhoneStore::put(const PairedPhone& phone, std::string* error) {
    std::vector<PairedPhone> next;
    for (size_t i = 0; i < phones_.size(); ++i) {
        if (phones_[i].id != phone.id) next.push_back(phones_[i]);
    }
    // The list is in the order the phones were paired, so the oldest pairing is the first.
    while (next.size() >= kMaxPairedPhones) next.erase(next.begin());
    next.push_back(phone);
    return replace(next, error);
}

bool PhoneStore::forget(const std::string& id, size_t* count, std::string* error) {
    std::vector<PairedPhone> next;
    for (size_t i = 0; i < phones_.size(); ++i) {
        if (phones_[i].id != id) next.push_back(phones_[i]);
    }
    if (count != 0) *count = phones_.size() - next.size();
    if (next.size() == phones_.size()) return true;
    return replace(next, error);
}

bool PhoneStore::forgetAll(size_t* count, std::string* error) {
    if (count != 0) *count = phones_.size();
    // Written even when there is nothing to forget: a file that could not be read goes with it.
    return replace(std::vector<PairedPhone>(), error);
}

bool PhoneStore::setAddress(const std::string& id, const std::string& address, bool* changed, std::string* error) {
    if (changed != 0) *changed = false;
    std::string shown;
    if (!colonAddress(address, &shown)) {
        if (error != 0) *error = "that is not a Bluetooth address";
        return false;
    }
    if (find(id) == 0) return true;
    std::vector<PairedPhone> next = phones_;
    bool differs = false;
    for (size_t i = 0; i < next.size(); ++i) {
        std::string want = next[i].id == id ? shown : (next[i].address == shown ? std::string() : next[i].address);
        if (want == next[i].address) continue;
        next[i].address = want;
        differs = true;
    }
    if (!differs) return true;
    if (!replace(next, error)) return false;
    if (changed != 0) *changed = true;
    return true;
}

std::vector<std::string> PhoneStore::addresses() const {
    std::vector<std::string> all;
    for (size_t i = 0; i < phones_.size(); ++i) {
        if (!phones_[i].address.empty()) all.push_back(phones_[i].address);
    }
    return all;
}

void PhoneAddressList::set(const std::vector<std::string>& addresses) {
    pthread_mutex_lock(&mu_);
    list_ = addresses;
    pthread_mutex_unlock(&mu_);
}

std::vector<std::string> PhoneAddressList::get() {
    pthread_mutex_lock(&mu_);
    std::vector<std::string> all = list_;
    pthread_mutex_unlock(&mu_);
    return all;
}

bool PhoneAddressList::has(const std::string& address) {
    std::string shown;
    if (!colonAddress(address, &shown)) return false;
    pthread_mutex_lock(&mu_);
    bool found = false;
    for (size_t i = 0; i < list_.size() && !found; ++i) found = list_[i] == shown;
    pthread_mutex_unlock(&mu_);
    return found;
}

}  // namespace nbt
