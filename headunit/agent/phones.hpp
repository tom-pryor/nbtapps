// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_PHONES_HPP
#define NBT_AGENT_PHONES_HPP

// The phones that are paired with this head unit (plan document, "Authentication" and "Enrolment on the car's
// screen"). A phone is paired when the agent has given it a pairing key, which it gets for the code on the car's
// screen (`pairing.issue`); with that key it proves on every later link that it is that phone (`auth`).
//
//   <state>/phones.json     { "version": 1, "phones": [ { "phoneId", "name", "key", "pairedAt" } ] }
//
// The file is in the state folder, which outlasts a new version of the agent, and is readable by the agent's user
// only: a key in it is all it takes to be that phone. It is written whole, and appears under its name only when
// it is complete (fsutil.hpp).
//
// A file that cannot be read as that is taken for no phones at all: every phone then pairs again, and no phone
// gets in on a damaged record. The agent starts either way; what it installed does not depend on a phone.
//
// The oldest pairing goes when a new phone would be one more than kMaxPairedPhones: a car has a handful of
// phones, and nothing else bounds the file.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace nbt {

const size_t kPairingKeyBytes = 32;
const size_t kMaxPairedPhones = 16;

struct PairedPhone {
    std::string id;                     // 32 lowercase hex characters, as `hello` gives it
    std::string name;                   // 1 to 64 characters, as `hello` gave it when the phone was paired
    uint8_t key[kPairingKeyBytes];
    int64_t pairedAt;                   // seconds since 1970, by the head unit's clock, which may be wrong

    PairedPhone() : pairedAt(0) {
        for (size_t i = 0; i < kPairingKeyBytes; ++i) key[i] = 0;
    }
};

class PhoneStore {
public:
    PhoneStore() {}

    // Reads the file at `path`, and keeps the path for what is written later. With no file there are no phones.
    // False, with *problem saying why, if a file is there and is not what this class writes: there are no phones
    // then either, and the file is replaced by the next change.
    bool load(const std::string& path, std::string* problem);

    // The phone with this id, or 0. The pointer holds until the next change.
    const PairedPhone* find(const std::string& id) const;
    // Oldest pairing first.
    const std::vector<PairedPhone>& all() const { return phones_; }

    // Pairs `phone`, in place of a pairing with the same id. False with *error set if the file could not be
    // written; nothing has changed then.
    bool put(const PairedPhone& phone, std::string* error);
    // Unpairs one phone, or every one. *count is how many went. False with *error set if the file could not be
    // written; nothing has changed then.
    bool forget(const std::string& id, size_t* count, std::string* error);
    bool forgetAll(size_t* count, std::string* error);

    // The file's text for these phones.
    static std::string encode(const std::vector<PairedPhone>& phones);
    // The phones of a file's text. False with *problem set for a text that encode() does not write.
    static bool decode(const std::string& text, std::vector<PairedPhone>* phones, std::string* problem);

private:
    PhoneStore(const PhoneStore&);
    PhoneStore& operator=(const PhoneStore&);
    bool replace(const std::vector<PairedPhone>& phones, std::string* error);

    std::string path_;
    std::vector<PairedPhone> phones_;
};

}  // namespace nbt

#endif
