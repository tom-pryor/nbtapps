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
// The ask. Before every dial, whatever brings it, the car is asked for the phone's services (ServiceListQuery,
// spp_service.hpp), and the state is kAsking. The phone's app publishes the agent's service only while it listens,
// so the answer says whether a dial would be taken. What it asks of the listener:
//
//   the agent's service is listed, in any state   the dial goes at once: kDialling
//   it is not listed                              no dial, and nothing is held against the phone: the next poll
//                                                 asks again
//   an error, or no answer in askLimitMs          as not listed; the third in a row for the phone backs it off, as a
//                                                 dial with no answer does
//
// The answer counts only for the phone asked: a reply for another address is passed over, and one for the phone
// asked is the answer whoever's query brought it (the car may give its one answer to two clients whose queries met).
// A phone that was listed and has since gone below connection 2, out of the device list or into pairing trouble is
// not dialled. A phone whose connection is below 2 is never asked: a query to a phone with no link to the car
// pages it. An ask is ended, and its answer passed over when it comes, by a link of the agent's that comes, by a
// phone's request that becomes pending (all but the request of the phone asked, for the ask that request brought)
// and by the registration's going, as a round is. The car answers one query at a time, so no other ask goes until
// that answer has come or askLimitMs have passed since the ask went.
//
// The settle time. The car's own media component asks a phone for its services as the phone's audio connects, and
// of two queries that meet, one fails. So for settleMs after the device list first shows a phone connected
// (connection rising to 2 or more, or at 2 or more in the first list that comes), no poll, retry or afresh dial asks
// it, and the ask its own request brings waits until then. A forced round and the pairing window ask at once. A
// phone's requests bring one ask of it every signalAskMs at most: a request's time that runs out sooner brings none.
//
// What the car answers to the dial (GapErrorCode, spp_service.hpp), and what comes of it:
//
//   0   PAGE answered, link up: the node is there; the listener opens it and waits for the magic bytes
//   2   BUSY: a link on the instance is up already. If its node is there, a link was left behind (an agent that was
//       killed): the listener ends it and dials afresh. If the car does not end it, the phone is backed off instead,
//       and is not dialled again at once. If no node is there, as for any other answer
//   5   DEFAULT: the phone is there and its app is not listening, or anything else went wrong. The next poll asks
//       again
//   14  PAGE_TIMEOUT: the phone is not in reach. Not asked by a poll again until its connection changes in the car's
//       device list
//   any other answer, or a rejection: asked and dialled again kDial retryMs later
//   no answer in replyLimitMs: the listener looks for the node and ends what may be there, and the phone is backed off
//
// A dial that still fails. A dial that the car answers with anything but 0, 2 or 10, or does not answer at all, may
// leave the car's record of phone-opened serial links wrong: the car then refuses phone-opened links (another
// program's among them) after one of them ends. A rejection of the request is not an answer of the car's. From such
// a dial recordDoubtful() is true, until a dial is answered 0, the car says that a phone-opened link on its instance
// kDisconnectInstance has ended (phoneLinkEnded()), or the car's Bluetooth service goes. Nothing is sent to mend it:
// a ServiceDisconnect for that instance with no live link holds the instance up for well over a minute.
//
// Only a phone that is connected to the car (connection 2 or more) and paired with it, with its key (security 7 or
// 8), is asked and dialled: a dial to a phone that has lost its pairing makes the car start a new one, with a prompt
// on both screens, and fails only some 80 seconds later. A phone that the car says is being paired anew or failed to
// pair (the informs SimplePairingConfirmationRequired and PairingError), or whose security becomes 9, is not asked
// again until the device list shows it at 7 or 8 after a change. PairingComplete, a pairing that worked, stops
// nothing and ends nothing.
//
// One ask or dial at a time, and none while a link of the agent's is up or being decided on, of either kind: a dial
// is only ever for a phone that has no link to the agent. While a phone's request for a link that may be the
// agent's is pending (its node is still looked for), only the ask that request brings may go: the rest wait for what
// comes of it.
//
// What brings an ask, and so a dial, in this order:
//   a round that is under way      one phone after the other, the rest of a round that one of the below began
//   the phone's own request        a phone that asked the car for a serial link, and got no link of the agent's
//                                  within signalWaitMs: asked then, or once its settle time is over, whatever else
//                                  holds it back from polls, and whether or not its request is still pending
//   the pairing window             while it is open, every phone that is connected and paired with the car, whether or
//                                  not it is the agent's, every pairingRoundMs: that is how a new phone's app is reached
//   a retry                        retryMs after a dial's answer that was not understood
//   a poll                         every phone of the agent's that is eligible, not held back and not settling: every
//                                  pollFirstMs for pollFirstForMs after a phone connects to the car or a link of the
//                                  agent's ends, and every pollLaterMs after that. The first comes as soon as the device
//                                  list has come, and one comes as soon as a phone that a poll left out has settled
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
// Asks of a phone in a row with an error or no answer that back it off.
const unsigned kAskMissesToBackOff = 3;

class DialRules {
public:
    enum State {
        kIdle,        // no ask, no dial, no dialled link
        kAsking,      // ServiceListQuery is out, and has no answer yet
        kDialling,    // ServiceConnect is out, and has no answer yet
        kUp,          // the car has the link: its node is being read, or is the agent's link
        kEnding       // it is being ended: ServiceDisconnect, and what comes of it
    };
    // Why a phone is asked, and dialled, now.
    enum Why { kForced, kPoll, kSignal, kPairing, kRetry, kAfresh };
    // What an answer to ServiceListQuery asks of the listener.
    enum Listing {
        kNotThisAsk,     // not the answer to this ask (another phone, an ask that was ended, or none out): nothing
        kDialNow,        // the agent's service is listed: ServiceConnect for current() now
        kNotListed,      // it is not: no dial
        kNoListing,      // an error, a reply not read, or no answer in askLimitMs: no dial
        kAskBackedOff,   // the same, the third in a row for the phone: no dial, and the phone is backed off
        kNoLongerDue     // listed, and the phone has gone below connection 2, out of the list, or into pairing
                         // trouble since the ask: no dial
    };
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
        int64_t askedAt;       // when ServiceListQuery went out
        int64_t at;            // when the step that is out went out: the ask, and from kDialNow, ServiceConnect
        Dial() : number(0), why(kForced), askedAt(0), at(0) {}
    };
    // What the agent and the listener know at a decision.
    struct Facts {
        bool registered;                 // the agent's service is registered, and the car's Bluetooth service is there
        bool linkOfOurs;                 // a link of the agent's, of either kind, is up or being decided on; an ask or a
                                         // dial of these rules' own is not one
        bool requestPending;             // a phone's request for a link that may be the agent's has come, and its node
                                         // is still looked for
        bool pairingOpen;                // the agent's pairing window is open
        std::vector<std::string> ours;   // the paired phones' addresses (phones.hpp)
        Facts() : registered(false), linkOfOurs(false), requestPending(false), pairingOpen(false) {}
    };

    explicit DialRules(const DialConfig& config);

    // A device list from the car. What changed for a phone counts: a connection that changed takes its back-off, its
    // run of asks with no answer and its wait for a change away, a connection that rises to 2 or more starts its
    // settle time, and security 9 stops its asks; 7 or 8 after a change lets them go on.
    // *changes: as Presence::update(). *troubled: the numbers of the phones whose asks stop now.
    void devices(const std::vector<btspp::BtDevice>& devices, int64_t now, std::vector<btspp::Presence::Change>* changes,
                 std::vector<unsigned>* troubled);
    // The car's Bluetooth service has gone: nothing is known of the phones, an ask or a dial that was out is lost,
    // and the doubt about the car's record goes (recordDoubtful()): the service starts afresh.
    void serviceGone();
    const btspp::Presence& presence() const { return presence_; }
    // The number of the phone with this address, for the log.
    unsigned number(const std::string& address) { return presence_.number(address); }

    // One of the car's pairing informs that stop asks and dials (PairingError, SimplePairingConfirmationRequired;
    // never PairingComplete), whose layout is not known: `address` is the phone it names if it begins with one;
    // empty, or anything that is not a Bluetooth address, stands for the phone being asked or dialled, if one is.
    // That phone is not asked again until the device list shows it at 7 or 8 after a change. Its number if this is
    // news, for the log to say once; 0 otherwise.
    unsigned pairingTrouble(const std::string& address);

    // A phone may be asked and dialled: it is connected, paired with the car with its key, has had no pairing
    // trouble since, and is a phone of the agent's or the pairing window is open.
    bool eligible(const btspp::Presence::Phone& phone, const Facts& facts) const;
    unsigned eligibleCount(const Facts& facts) const;

    // A round of asks now, one to each phone that is connected and paired with the car with its key, whether or
    // not it is a phone of the agent's, and whatever holds it back otherwise, its settle time too. How many phones
    // that is.
    unsigned force();

    // The triggers. A phone asked the car for a serial link, for any service (`address`, as the request gave it):
    // if no link of the agent's comes within DialConfig::signalWaitMs, and the phone is eligible, it is asked then
    // (or once it has settled), whatever holds it back from polls.
    void phoneAsked(const std::string& address, int64_t now);
    // A link of the agent's, of either kind, came: what a phone asked for is answered.
    void linkCame();
    // A link of the agent's ended: polls come at the first rate again.
    void linkEnded(int64_t now);
    // When the next poll is due.
    int64_t nextPollAt() const { return nextPollAt_; }
    // A round of asks now to each phone that is eligible: after a link left behind has been ended.
    void afresh();

    // The ask to make now, if one is due: ServiceListQuery for out->address, and the state is kAsking from here. One
    // ask or dial at a time, and none while a link of the agent's is up or being decided on, while the service is
    // not registered, or while an ask that was ended may still be out in the car; while a phone's request is pending,
    // only the ask of the phone that asked, once its time has run. A link of the agent's, a pending request or the
    // registration's going ends the round, and the ask that is out (keepAsking()).
    bool next(int64_t now, const Facts& facts, Dial* out);
    State state() const { return state_; }
    // The ask or dial that is out or up, while the state is not kIdle.
    const Dial& current() const { return current_; }

    // While asking: whether the ask goes on as things stand. A link of the agent's, a phone's request that is
    // pending (unless it is the asked phone's, and the ask the one its request brought) and the registration's going
    // end it: kIdle, and its answer is passed over when it comes. next() does this too.
    bool keepAsking(const Facts& facts, int64_t now);
    // The answer to ServiceListQuery: `error` the car's (kNoError for a rejection or a reply not read), `listed`
    // whether the services named the agent's own. It counts only while asking, for the phone asked.
    Listing listing(const std::string& address, uint32_t error, bool listed, int64_t now);
    // No answer in askLimitMs: listingLost() then, which is kNoListing or kAskBackedOff.
    bool askOverdue(int64_t now) const;
    Listing listingLost(int64_t now);
    // The listener drops the ask that is out, as it stops: kIdle, and its answer is passed over.
    void askDropped(int64_t now);
    // Whether polls leave the phone alone for its settle time now.
    bool settling(const std::string& address, int64_t now) const;
    // How many asks of the phone in a row had an error or no answer.
    unsigned askMisses(const std::string& address) const;

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
    // The car was asked to end that link and its node is still there: kIdle, and the phone is not dialled again at
    // once but backed off, as for something that is not the app.
    void notEnded(int64_t now);
    // The car ends a phone's request for a link now: whether the agent's own dial overtook it. The dial is out, or
    // was answered 0 less than DialConfig::overtakeMs ago.
    bool overtakes(int64_t now) const;

    // The magic bytes came on the dialled node: kUp, the phone's back-off is gone, and so is the rest of the round.
    void linkUp(int64_t now);
    // They did not, or something else came: something answered on the agent's UUID that is not the app. The phone
    // is backed off, and the link is ended: kEnding.
    void notTheApp(int64_t now);
    // The agent ends the dialled link: kEnding.
    void ending();
    // The dialled link has ended, or what was ended is gone: kIdle. A stale link's phone is dialled again at once.
    void ended(int64_t now);

    // Whether the car's record of phone-opened links may be wrong since a dial that failed (see the top of this
    // file).
    bool recordDoubtful() const { return doubtful_; }
    // The car says that a link on its instance for phone-opened links (kDisconnectInstance) has ended, as such ends
    // are said when its record is right: the doubt goes.
    void phoneLinkEnded();

    // The back-off of a phone: 0 if it has none, or the time until which polls leave it alone.
    int64_t backedOffUntil(const std::string& address) const;
    // How long the next back-off of the phone will be.
    int64_t nextBackOffMs(const std::string& address) const;
    // Whether polls leave the phone alone now: backed off, unreachable until a change, or due for a retry later. Its
    // settle time is settling().
    bool heldBack(const std::string& address, int64_t now) const;
    // Whether the phone's asks have stopped for pairing trouble.
    bool troubled(const std::string& address) const;

private:
    struct PhoneState {
        int64_t backOffMs;        // the next back-off; 0: none has been
        int64_t backOffUntil;
        bool waitChange;          // 14: until its connection changes
        int64_t retryAt;          // an answer not understood: asked and dialled again then; 0: none
        bool trouble;             // pairing trouble: no ask until it is at 7 or 8 after a change
        unsigned misses;          // asks in a row with an error or no answer
        int64_t settleUntil;      // its settle time ends then
        bool signalAsked;         // its request has brought an ask, at signalAskAt
        int64_t signalAskAt;
        PhoneState()
            : backOffMs(0), backOffUntil(0), waitChange(false), retryAt(0), trouble(false), misses(0), settleUntil(0),
              signalAsked(false), signalAskAt(0) {}
    };
    PhoneState& phone(const std::string& address);
    const PhoneState* find(const std::string& address) const;
    void backOff(const std::string& address, int64_t now);
    bool takeFromRound(int64_t now, const Facts& facts, Dial* out);
    void start(const btspp::Presence::Phone& p, Why why, int64_t now, Dial* out);
    bool signalDue(int64_t now, const Facts& facts, Dial* out);
    Listing missed(int64_t now);
    void endAsk(int64_t now);
    void fastPolls(int64_t now);

    DialConfig config_;
    btspp::Presence presence_;
    int64_t nextPollAt_;                         // 0: at the first chance
    int64_t fastUntil_;                          // until then polls come every pollFirstMs
    std::string signal_;                         // the phone that asked for a link, while its time runs
    int64_t signalAt_;
    bool pairingWasOpen_;
    int64_t nextPairingAt_;
    std::map<std::string, PhoneState> phones_;   // by addressKey()
    State state_;
    Dial current_;
    bool stale_;                                 // the current dial's phone had a link left behind: again at once
    int64_t okAt_;                               // when the last dial was answered 0; 0: none
    std::vector<std::string> round_;             // the phones of a round still to be asked, by address
    Why roundWhy_;
    std::string endedAsk_;                       // an ask that was ended, which may still be out in the car, until
    int64_t endedAskUntil_;                      // its answer comes or this time
    bool doubtful_;                              // recordDoubtful()
};

}  // namespace nbt

#endif
