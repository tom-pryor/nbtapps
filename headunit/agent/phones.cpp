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

}  // namespace

std::string PhoneStore::encode(const std::vector<PairedPhone>& phones) {
    json::Writer w;
    w.beginObject().key("version").integer(kPhonesVersion).key("phones").beginArray();
    for (size_t i = 0; i < phones.size(); ++i) {
        const PairedPhone& p = phones[i];
        w.beginObject().key("phoneId").string(p.id).key("name").string(p.name);
        w.key("key").string(toHex(p.key, sizeof p.key)).key("pairedAt").integer(p.pairedAt).endObject();
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
        PairedPhone phone;
        uint8_t idBytes[kPhoneIdChars / 2];
        bool good = id.isString() && fromHex(id.string(), idBytes, sizeof idBytes) && name.isString() &&
                    utf8Length(name.string()) >= 1 && utf8Length(name.string()) <= kMaxPhoneNameChars && key.isString() &&
                    fromHex(key.string(), phone.key, sizeof phone.key) && pairedAt.isInteger() && pairedAt.integer() >= 0;
        for (size_t k = 0; good && k < phones->size(); ++k) good = (*phones)[k].id != id.string();
        if (!good) {
            phones->clear();
            if (problem != 0) *problem = "one of its phones has no usable id, name, key or date, or is there twice";
            return false;
        }
        phone.id = id.string();
        phone.name = name.string();
        phone.pairedAt = pairedAt.integer();
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

}  // namespace nbt
