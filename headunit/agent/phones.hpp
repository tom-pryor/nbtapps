// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_PHONES_HPP
#define NBT_AGENT_PHONES_HPP

// The phones that are paired with this head unit (plan document, "Authentication" and "Enrolment on the car's
// screen"). A phone is paired when the agent has given it a pairing key, which it gets for the code on the car's
// screen (`pairing.issue`); with that key it proves on every later link that it is that phone (`auth`).
//
//   <state>/phones.json     { "version": 1, "phones": [ { "phoneId", "name", "key", "pairedAt", "address"?,
//                                                          "dialled"? } ] }
//
// `address` is the phone's Bluetooth address, 00:11:22:33:44:55 in upper case, when the agent has learned it: the
// address of the serial link on which the phone last paired or did `auth`. The car dials a phone of the agent's
// at that address (serial_link.hpp). It is kept here and in memory, and goes nowhere else: no answer and no line
// of the log holds it. A file of an agent from before it has none, and is read as it is; an address that is not
// in that form makes the file one that cannot be read, as any other member that is not what encode() writes.
//
// `dialled` is true once a link the car dialled to that address has been the agent's (the phone's app opened it
// with the magic bytes): the car's dial works for that phone, and the listener leaves a link the phone opens itself
// to another program while one may read it (serial_link.hpp). It is written only when true; a file without it, an
// older agent's among them, reads as "not yet". It goes with the address: a phone whose address changes, or that is
// forgotten, has it no more. One that is not true or false makes the file one that cannot be read.
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

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace nbt {

const size_t kPairingKeyBytes = 32;

// A Bluetooth address in the form the file keeps, 00:11:22:33:44:55 with upper-case digits, from that form in either
// case or from its twelve digits alone. False for anything else.
bool colonAddress(const std::string& in, std::string* out);
const size_t kMaxPairedPhones = 16;

struct PairedPhone {
    std::string id;                     // 32 lowercase hex characters, as `hello` gives it
    std::string name;                   // 1 to 64 characters, as `hello` gave it when the phone was paired
    uint8_t key[kPairingKeyBytes];
    int64_t pairedAt;                   // seconds since 1970, by the head unit's clock, which may be wrong
    std::string address;                // 00:11:22:33:44:55, upper case; empty: not known
    bool dialled;                       // a link the car dialled to `address` has been the agent's

    PairedPhone() : pairedAt(0), dialled(false) {
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
    // Gives the phone with this id the Bluetooth address `address` (colonAddress()), and takes it from any other
    // phone that had it: an address is one phone's. Nothing is written if that is so already. *changed: the file
    // was written. False with *error set if the file could not be written, or for an address that is not one;
    // nothing has changed then. A phone that is not paired is no error, and nothing changes. A phone whose address
    // changes loses its `dialled`: that was for the address it had.
    bool setAddress(const std::string& id, const std::string& address, bool* changed, std::string* error);
    // The addresses that are known, oldest pairing first.
    std::vector<std::string> addresses() const;
    // A link the car dialled to `address` (colonAddress()) has been the agent's: the phone with that address is
    // `dialled` from now on. Nothing is written if it is so already, or if no phone has that address. *changed: the
    // file was written. False with *error set if the file could not be written; nothing has changed then.
    bool setDialled(const std::string& address, bool* changed, std::string* error);
    // The addresses of the phones that are `dialled`, oldest pairing first.
    std::vector<std::string> dialledAddresses() const;

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

// The addresses of the paired phones, and which of them are `dialled`, for a thread that is not the agent's: the
// agent keeps the list up to date from its PhoneStore, and the Bluetooth serial listener reads it. Under a lock of
// its own.
class PhoneAddressList {
public:
    PhoneAddressList() { pthread_mutex_init(&mu_, NULL); }
    ~PhoneAddressList() { pthread_mutex_destroy(&mu_); }
    // The addresses, none of them `dialled`.
    void set(const std::vector<std::string>& addresses);
    // The addresses, and those of them that are `dialled`.
    void set(const std::vector<std::string>& addresses, const std::vector<std::string>& dialled);
    std::vector<std::string> get();
    // Whether `address`, in any form colonAddress() takes, is one of them.
    bool has(const std::string& address);
    // Whether it is one of them and `dialled`: a link the car dialled to it has been the agent's.
    bool dialledBefore(const std::string& address);

private:
    PhoneAddressList(const PhoneAddressList&);
    PhoneAddressList& operator=(const PhoneAddressList&);
    pthread_mutex_t mu_;
    std::vector<std::string> list_, dialled_;
};

}  // namespace nbt

#endif
