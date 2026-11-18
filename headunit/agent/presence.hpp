// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_PRESENCE_HPP
#define NBT_AGENT_PRESENCE_HPP

// Which of the car's phones are there, as the car's device list says: what the car dialling a phone of the agent's
// goes by (dial_rules.hpp). No devices and no clock of its own: the caller feeds it each device list that comes
// (DeviceListGet's reply and the inform DeviceListChanged, spp_service.hpp) with the time. Plain C++98,
// host-tested in tests/agent/test_presence.cpp.
//
// For each phone in the list it keeps the connection value (0 not in range, 1 in range, 2 connected, 3 service
// connected), the security value (7 and 8: paired, with a link key; 9: the key is missing, as on a phone that has
// lost its pairing with the car) and when each last changed. That is in memory only: a device list is asked for
// when the car's Bluetooth service attaches and again each time it comes back, and forget() drops what is known
// while it is gone.
//
// A phone gets a small number, phone 1, phone 2, ..., the first time it is seen, for the life of the process:
// that is how the log names it. Its address is never in the log; its name the device list carries is read past
// and not kept at all (spp_service.hpp).
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "spp_service.hpp"

namespace nbt {
namespace btspp {

// The security values of a phone that is paired with the car and has its key.
inline bool pairedSecurity(uint32_t security) { return security == 7 || security == 8; }
// The security value of a phone whose key the car has and the phone no longer: a dial makes the car start a new
// pairing, which puts a prompt on both screens.
const uint32_t kSecurityKeyMissing = 9;

class Presence {
public:
    struct Phone {
        std::string address;     // as the car's list gives it
        unsigned number;         // phone 1, phone 2, ...
        uint32_t connection, security;
        int64_t connectionAt, securityAt;   // when each last changed; the time of the first list for a phone in it
        Phone() : number(0), connection(0), security(0), connectionAt(0), securityAt(0) {}
    };
    // What one list changed for one phone.
    struct Change {
        std::string address;
        unsigned number;
        bool added, removed;             // came into the list, or went out of it
        bool connectionChanged, securityChanged;
        uint32_t connectionBefore, connection, securityBefore, security;
        Change()
            : number(0), added(false), removed(false), connectionChanged(false), securityChanged(false),
              connectionBefore(0), connection(0), securityBefore(0), security(0) {}
    };

    Presence() : next_(1), known_(false) {}

    // A whole device list, which comes at `nowMs`. *changes, if given, gets what differs from the list before,
    // phone by phone: every phone of a first list counts as added.
    void update(const std::vector<BtDevice>& devices, int64_t nowMs, std::vector<Change>* changes);
    // The car's Bluetooth service has gone: nothing is known of the phones until the next list. Numbers stay.
    void forget() {
        phones_.clear();
        known_ = false;
    }
    // A list has come since the start, or since forget().
    bool known() const { return known_; }

    // The phones of the last list.
    const std::vector<Phone>& phones() const { return phones_; }
    // The one with this address, in whatever form; 0 if it is not in the last list.
    const Phone* find(const std::string& address) const;
    // The number of the phone with this address, given now if it has none yet.
    unsigned number(const std::string& address);

    // Connected (2 or more), whatever its security.
    static bool present(const Phone& p) { return p.connection >= kConnConnected; }
    // Present and paired with the car, with its key: what a dial may go to.
    static bool dialable(const Phone& p) { return present(p) && pairedSecurity(p.security); }
    unsigned presentCount() const;
    unsigned dialableCount() const;

private:
    std::vector<Phone> phones_;
    std::map<std::string, unsigned> numbers_;   // by addressKey()
    unsigned next_;
    bool known_;
};

}  // namespace btspp
}  // namespace nbt

#endif
