// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_DIAL_RULES_HPP
#define NBT_AGENT_DIAL_RULES_HPP

// The car dials the phone: when the Bluetooth serial listener (serial_link.hpp) has the car open a serial link to
// a phone of the agent's, and what it makes of the answer. No devices and no clock of its own: the listener feeds it
// what the car says, with the time, and does what it decides. Plain C++98, host-tested in
// tests/agent/test_dial_rules.cpp.
//
// A phone app opens its link to the car through the car's one serial node for phone-opened links, /dev/btspp0,
// which another program may hold. The car can also open a link itself: ServiceConnect asks the car's Bluetooth
// service to connect to a service of a phone's, by the service's registered name, on an instance of the car's own,
// whose node is /dev/<instance>. The agent always uses the one instance kDialInstance, so its dialled link has the
// node /dev/nbtapp, which nothing else uses. The phone's app listens on the agent's UUID, and accepts.
//
// What the car answers (GapErrorCode, spp_service.hpp), and what comes of it:
//
//   0   PAGE answered, link up: the node is there; the listener opens it and waits for the magic bytes
//   2   BUSY: a link on the instance is up already. If its node is there, a link was left behind (an agent that was
//       killed): the listener ends it and dials afresh. If not, as for any other answer
//   5   DEFAULT: the phone is there and its app is not listening, or anything else went wrong. Harmless: the next
//       poll dials again
//   14  PAGE_TIMEOUT: the phone is not in reach. Not dialled by a poll again until its connection changes in the car's
//       device list
//   any other answer, or a rejection: dialled again kDial retryMs later
//   no answer in replyLimitMs: the listener looks for the node and ends what may be there, and the phone is backed off
//
// Only a phone that is connected to the car (connection 2 or more) and paired with it, with its key (security 7 or
// 8), is dialled: a dial to a phone that has lost its pairing makes the car start a new one, with a prompt on both
// screens, and fails only some 80 seconds later. A phone that the car's pairing informs name, or whose security
// becomes 9, is not dialled again until the device list shows it at 7 or 8 after a change.
//
// One dial at a time, and none while a link of the agent's is up or being decided on, of either kind: a dial is
// only ever for a phone that has no link to the agent.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "dial_config.hpp"
#include "presence.hpp"

namespace nbt {

// The device and the instance of the car's that every dial goes to, and so the dialled link's node, /dev/nbtapp.
const char* const kDialDevice = "BtCtrl";
const char* const kDialInstance = "nbtapp";

class DialRules {
public:
    enum State {
        kIdle,        // no dial, no dialled link
        kDialling,    // ServiceConnect is out, and has no answer yet
        kUp,          // the car has the link: its node is being read, or is the agent's link
        kEnding       // it is being ended: ServiceDisconnect, and what comes of it
    };
    // Why a phone is dialled now.
    enum Why { kForced, kPoll, kSignal, kPairing, kRetry, kAfresh };
    // What an answer to ServiceConnect asks of the listener.
    enum Reply {
        kNotThis,        // not the answer to this dial (another service, another phone, or no dial out): nothing
        kOpenNode,       // 0
        kLookForNode,    // 2: stale() if the node is there, notThere() if not
        kNotListening,   // 5
        kUnreachable,    // 14
        kRetryLater      // anything else
    };
    struct Dial {
        std::string address;   // 00:11:22:33:44:55: the car takes no other form
        unsigned number;       // the phone's, for the log
        Why why;
        int64_t at;            // when ServiceConnect went out
        Dial() : number(0), why(kForced), at(0) {}
    };
    // What the agent and the listener know at a decision.
    struct Facts {
        bool registered;                 // the agent's service is registered, and the car's Bluetooth service is there
        bool linkOfOurs;                 // a link of the agent's, of either kind, is up or being decided on
        bool pairingOpen;                // the agent's pairing window is open
        std::vector<std::string> ours;   // the paired phones' addresses (phones.hpp)
        Facts() : registered(false), linkOfOurs(false), pairingOpen(false) {}
    };

    explicit DialRules(const DialConfig& config);

    // A device list from the car. What changed for a phone counts: a connection that changed takes its back-off and
    // its wait for a change away, and security 9 stops its dials; 7 or 8 after a change lets them go on.
    // *changes: as Presence::update(). *troubled: the numbers of the phones whose dials stop now.
    void devices(const std::vector<btspp::BtDevice>& devices, int64_t now, std::vector<btspp::Presence::Change>* changes,
                 std::vector<unsigned>* troubled);
    // The car's Bluetooth service has gone: nothing is known of the phones, and a dial that was out is lost.
    void serviceGone();
    const btspp::Presence& presence() const { return presence_; }
    // The number of the phone with this address, for the log.
    unsigned number(const std::string& address) { return presence_.number(address); }

    // One of the car's pairing informs (PairingComplete, PairingError, SimplePairingConfirmationRequired), whose
    // layout is not known: `address` is the phone it names if it begins with one; empty, or anything that is not a
    // Bluetooth address, stands for the phone being dialled, if one is. That phone is not dialled again until the device list shows it at 7 or 8
    // after a change. Its number if this is news, for the log to say once; 0 otherwise.
    unsigned pairingTrouble(const std::string& address);

    // A phone may be dialled: it is connected, paired with the car with its key, has had no pairing trouble since,
    // and is a phone of the agent's or the pairing window is open.
    bool eligible(const btspp::Presence::Phone& phone, const Facts& facts) const;
    unsigned eligibleCount(const Facts& facts) const;

    // A round of dials now, one to each phone that is connected and paired with the car with its key, whether or
    // not it is a phone of the agent's, and whatever holds it back otherwise. How many phones that is.
    unsigned force();

    // The dial to make now, if one is due: and the state is kDialling from here, for out->address. One at a time,
    // and none while a link of the agent's is up or being decided on, or while the service is not registered.
    bool next(int64_t now, const Facts& facts, Dial* out);
    State state() const { return state_; }
    // The dial that is out or up, while the state is not kIdle.
    const Dial& current() const { return current_; }

    // The answer to ServiceConnect: it counts only for the dial that is out, its service ours and its address the
    // phone's. kNoError (spp_service.hpp) for a rejection of the request.
    Reply reply(const std::string& address, const std::string& service, const std::string& ourService, uint32_t error,
                int64_t now);
    // No answer in replyLimitMs. The listener ends what may be up, and then gaveUp().
    bool overdue(int64_t now) const;
    // The dial is given up: the phone is backed off. kIdle.
    void gaveUp(int64_t now);
    // 2, and no node: as any other answer that is not understood.
    void notThere(int64_t now);
    // 2 with the node there: a link was left behind. The listener ends it (kEnding), and then dials the phone afresh.
    void stale();

    // The magic bytes came on the dialled node: kUp, and the phone's back-off is gone.
    void linkUp(int64_t now);
    // They did not, or something else came: something answered on the agent's UUID that is not the app. The phone
    // is backed off, and the link is ended: kEnding.
    void notTheApp(int64_t now);
    // The agent ends the dialled link: kEnding.
    void ending();
    // The dialled link has ended, or what was ended is gone: kIdle. A stale link's phone is dialled again at once.
    void ended(int64_t now);

    // The back-off of a phone: 0 if it has none, or the time until which polls leave it alone.
    int64_t backedOffUntil(const std::string& address) const;
    // How long the next back-off of the phone will be.
    int64_t nextBackOffMs(const std::string& address) const;
    // Whether polls leave the phone alone now: backed off, unreachable until a change, or due for a retry later.
    bool heldBack(const std::string& address, int64_t now) const;
    // Whether the phone's dials have stopped for pairing trouble.
    bool troubled(const std::string& address) const;

private:
    struct PhoneState {
        int64_t backOffMs;        // the next back-off; 0: none has been
        int64_t backOffUntil;
        bool waitChange;          // 14: until its connection changes
        int64_t retryAt;          // an answer not understood: dialled again then; 0: none
        bool trouble;             // pairing trouble: no dial until it is at 7 or 8 after a change
        PhoneState() : backOffMs(0), backOffUntil(0), waitChange(false), retryAt(0), trouble(false) {}
    };
    PhoneState& phone(const std::string& address);
    const PhoneState* find(const std::string& address) const;
    void backOff(const std::string& address, int64_t now);
    bool takeFromRound(int64_t now, const Facts& facts, Dial* out);
    void start(const btspp::Presence::Phone& p, Why why, int64_t now, Dial* out);

    DialConfig config_;
    btspp::Presence presence_;
    std::map<std::string, PhoneState> phones_;   // by addressKey()
    State state_;
    Dial current_;
    bool stale_;                                 // the current dial's phone had a link left behind: again at once
    std::vector<std::string> round_;             // the phones of a round still to be dialled, by address
    Why roundWhy_;
};

}  // namespace nbt

#endif
