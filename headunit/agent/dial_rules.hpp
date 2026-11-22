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
// The rules go one of two ways, by DialConfig::knock: the knock (`nbtagent --dial-knock on`, the default), or the ask
// and the polls (`--dial-knock off`), as before the knock, which the rest of this comment after "The knock" is about.
// The answers to a dial, the re-point and the clean-up of a link left behind are the same either way.
//
// The knock. The car fetches only the first records of a phone's service list, and on current phones an app's own
// record comes after them, so a list may never name the agent's service while the app listens; and a dial that the
// app does not take may cost the phone its media audio at the car (the re-point, below). So the car dials a phone only
// when its app has just said that it listens: it knocks (constants.txt, serial.knockMagic). The app opens a link of
// its own and writes the knock's magic bytes into it and nothing else: the listener takes those bytes as the knock of
// the phone whose request brought the link, and lets the link go (serial_link.hpp), knocked(). A request that the car
// refuses brings no link, and so no knock: while the car refuses the phone's own links (it does so while it has no
// media audio connection to the phone), its app cannot knock. What becomes of a knock:
//
//   from a phone that is not in the car's device list connected and paired with it, or has pairing trouble: ignored
//   from a phone that is not one of the agent's, while the pairing window is closed: ignored
//   in the phone's quiet time: ignored
//   while a link of the agent's is up, or a dial of that phone is out or up: ignored, as one that adds nothing
//   while a phone's request is pending, a link is being decided on, a dial of another phone is out, the re-point
//   waits, the service is not registered or the device list has not come: it waits, knockWaitMs at most, and is
//   ignored after that
//   otherwise: the car dials the phone at once (kKnock), with ServiceConnect and no ask
//
// The knocks of the connects of one burst of the app's that come up one after another are one knock: the second
// finds the first waiting, or its dial out or up, and adds nothing; after a dial that failed it finds the quiet time.
// A phone's request for a serial link is no knock, whatever becomes of it: in this way it brings no ask and no dial
// (phoneAsked() takes nothing). A dial a knock brought that gives the agent no link (any answer but 0, BUSY with a
// link left behind, no answer, something that is not the app) brings the re-point as any failed dial does, and the
// phone's quiet time: its knocks are ignored for knockQuietMs. It is not backed off: the quiet time takes the place
// of the back-off, which only polls and requests went by.
//
// Besides the knock, in this way the car dials only: the pairing window's rounds, every phone connected to the car
// and paired with it in turn every pairingRoundMs; serial.dial's round (force()); a phone whose link left behind a
// dial has just ended (stale()); and while this version of the agent is on trial (Facts::onTrial) and no link of the
// agent's is up, a round of the agent's phones that are there every trialDialMs, so that an update is never left
// waiting for a phone whose app does not know to knock. Each with ServiceConnect at once, and no ask: a list cannot be
// trusted to name the app. No poll, no ask, no retry, and no round of the phones afresh after a link left behind at
// the start (afresh()): the phones knock.
//
// The trial's rounds go on for trialDialForMs only, so that an update nobody confirms (the app closed, the phone gone)
// does not bring a failed dial every trialDialMs for as long as the agent runs. The time is counted from the first
// next() that finds the agent on trial, the service registered and the car's device list come, whatever else holds a
// round back then, so that a slow start of the car's Bluetooth takes none of it; the service going and coming back
// later does not start it again. When it is up, the rest of a round under way is dropped (a dial that is out runs to
// its answer), takeTrialOver() is true once, for the log, and from then on the rules go as if the agent were not on
// trial: a phone is dialled when its app knocks. A trial that is confirmed ends the rounds as before. Facts that say
// the agent is not on trial end the count, and so does trialEnded(), so a new trial (the flag true again, or a new
// process) has its own trialDialForMs, and its first round at the first chance.
//
// The ask. Before every dial, whatever brings it, the car is asked for the phone's services (ServiceListQuery,
// spp_service.hpp), and the state is kAsking. The phone's app publishes the agent's service only while it listens,
// so the answer says whether a dial would be taken. An ask does no harm, and may be repeated as often as polls
// come; a dial the car answers with a failure does harm (below), and is not repeated soon. What the ask's answer asks
// of the listener:
//
//   the agent's service is listed, in any state   the dial goes at once: kDialling
//   it is not listed                              no dial, and nothing is held against the phone: the next poll
//                                                 asks again
//   an error, or no answer in askLimitMs          the same. askMisses() counts them in a row, for the log to say
//                                                 once when kAskMissesToSay have come; a phone whose app only
//                                                 listens cannot ask for anything itself, so it is never held back
//                                                 from polls for them
//
// The answer counts only for the phone asked: a reply for another address is passed over, and one for the phone
// asked is the answer whoever's query brought it (the car may give its one answer to two clients whose queries met).
// A listed phone is dialled only if what brought the ask still holds when the answer comes: for a forced round (and
// the afresh ask after a dial one brought) that it is still connected, paired and out of pairing trouble; for
// anything else that it is still eligible(), so a phone the agent has forgotten, or one that is not the agent's once
// the pairing window has closed, is not dialled. A phone whose connection is below 2 is never asked: a query to a phone with no link to the car
// pages it. An ask is ended, and its answer passed over when it comes, by a link of the agent's that comes, by a
// phone's request that becomes pending (all but the request of the phone asked, for the ask that request brought)
// and by the registration's going, as a round is. The car answers one query at a time, so no other ask goes until
// that answer has come or askLimitMs have passed since the ask went.
//
// The settle time. The car's own media component asks a phone for its services as the phone's audio connects, and
// of two queries that meet, one fails. So for settleMs after the device list first shows a phone connected
// (connection rising to 2 or more, or at 2 or more in the first list that comes), no poll, retry or afresh dial asks
// it, and the ask its own request brings waits until then. A forced round and the pairing window ask at once. A
// phone's requests bring one ask of it every signalAskMs at most, counted from an ask that went to the car: a
// request's time that runs out sooner brings none. An ask that could not be sent cost the car nothing and does not
// count. One that went and was ended before its answer came (by another phone's request, say) did cost a query, and
// counts, but gives the phone's requests one more ask in that time, once: so a phone whose asks other requests keep
// ending is asked twice in signalAskMs at most, and no more.
//
// What the car answers to the dial (GapErrorCode, spp_service.hpp), and what comes of it:
//
//   0   PAGE answered, link up: the node is there; the listener opens it and waits for the magic bytes
//   2   BUSY: a link on the instance is up already. If its node is there, a link was left behind (an agent that was
//       killed): the listener ends it and dials afresh. If the car does not end it, the phone is backed off instead,
//       and is not dialled again at once. If no node is there, as for 10
//   5   DEFAULT: the phone is there and did not take the link. Its service list named the agent's service just
//       before, so its app stopped listening in between, or something else went wrong. The phone is backed off
//   14  PAGE_TIMEOUT: the phone is not in reach. The phone is backed off
//   10, or a rejection of the request (never sent to the car, or refused by its Bluetooth service): asked and
//       dialled again retryMs later
//   any other answer: the phone is backed off
//   no answer in replyLimitMs: the dial is given up (gaveUp()), the listener looks for the node and ends what may be
//       there, and the phone is backed off. The car may still answer it later: that answer is the answer to nothing
//       that is out, neither an ask nor a later dial (but see serial_link.hpp for one that comes while a new dial of
//       the same phone is out); a failure brings the re-point (below), and a 0 has the listener end the link the car
//       has up after all. A dial that is out when the listener stops is given up the same way, with no clean-up
//
// A dial that still fails. A dial that the car answers with anything but 0, 2 or 10, or does not answer at all, may
// leave the car's record of phone-opened serial links wrong: the car then refuses phone-opened links (another
// program's among them) after one of them ends. A rejection of the request is not an answer of the car's. From such
// a dial recordDoubtful() is true, until a dial is answered 0, the car says that a phone-opened link on its instance
// kDisconnectInstance has ended (phoneLinkEnded()), or the car's Bluetooth service goes. Nothing is sent to make the
// record right: a ServiceDisconnect for that instance with no live link holds the instance up for well over a minute.
// So such a dial is not repeated soon: its phone is backed off, for backOffMs, twice as long each time up to
// backOffMostMs, until a dialled link of the agent's comes up for it or its connection changes in the car's device
// list.
//
// The re-point. Where the car answers a dial with such a failure, its Bluetooth service keeps an entry for the agent's
// instance that still names the phone. The end of a later phone-opened link can then be booked by the car against that
// entry, and the car counts one connected service too few for the phone; when that count reaches zero, the car closes
// the phone's media audio. So the moment such an answer comes (doubtingAnswer(), the same rule as the doubt above),
// the agent sends one more ServiceConnect on its own instance, with kRepointAddress, an address of no device, and
// kRepointService, a service the instance does not serve. The car answers it with 5 within some tens of milliseconds
// and sends nothing over the air, and on the way writes that address into the agent's entry: a link end booked
// against the entry then names an address the car has no record of, and the phone's count is left alone. It does not
// make the record right, and recordDoubtful() stays as it is. It helps only if it reaches the car before the next
// phone-opened link ends, so it goes on the Bluetooth service's thread as soon as the answer is seen
// (serial_link.hpp), and an answer to a dial that was given up (for no answer, or as the listener stopped) brings it
// too, by the same rule, as long as no ServiceConnect of the agent's is out when it comes. The car takes one
// ServiceConnect of a client's at a time, so while it waits for its answer (repointWaits()), for repointLimitMs at
// most, no ask or dial starts, and an ask that is out is ended. An answer other than 5, a 5 that took repointSlowMs or
// longer, or none in repointLimitMs is not what is expected of the car: the listener then sends no more re-points in
// that run of the agent.
//
// A dial that keeps failing. dialFailures() counts, for each phone, the dials in a row that gave the agent no link:
// an answer that sets the doubt above, no answer, or a link that came up and was not the app's (notTheApp()). A
// dialled link that becomes the agent's (linkUp()) starts the count again; it is kept in memory only. At
// kDialFailsToUnmark the listener gives up the phone's mark that a dialled link has reached its app (phones.hpp,
// `dialled`), so that a link the phone opens itself is decided on again (serial_link.hpp).
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
//                                  within signalWaitMs: asked then, or once its settle time and its back-off are
//                                  over, a retry that is due later notwithstanding, and whether or not its request is
//                                  still pending. A back-off is not cut short by requests: a phone whose dial has just
//                                  failed would otherwise be dialled again at every request
//   the pairing window             while it is open, every phone that is connected and paired with the car, whether or
//                                  not it is the agent's, every pairingRoundMs: that is how a new phone's app is reached
//   a retry                        retryMs after a dial answered 10, or 2 with no node there, or whose request was
//                                  rejected
//   a poll                         every phone of the agent's that is eligible, not held back (backed off, or due for
//                                  a retry later) and not settling: every
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
// The re-point (see the top of this file): ServiceConnect on kDialInstance to an address that belongs to no device
// (a locally administered one), for a service that the instance does not serve, sent right after the car answers a
// dial with a failure. The car answers it with 5 and writes the address into the instance's entry, so that the end of
// a phone-opened link that it books against that entry names no phone, and no phone's count of connected services
// drops for it.
const char* const kRepointAddress = "02:00:00:DD:DD:DD";
const char* const kRepointService = "HFPGW";
// Asks of a phone in a row with an error or no answer at which the log says so: once in each such run. When dials
// that give the agent no link give up a phone's `dialled` mark: kDialFailsToUnmark, dial_config.hpp.
const unsigned kAskMissesToSay = 3;

class DialRules {
public:
    enum State {
        kIdle,        // no ask, no dial, no dialled link
        kAsking,      // ServiceListQuery is out, and has no answer yet
        kDialling,    // ServiceConnect is out, and has no answer yet
        kUp,          // the car has the link: its node is being read, or is the agent's link
        kEnding       // it is being ended: ServiceDisconnect, and what comes of it
    };
    // Why a phone is asked, and dialled, now. kKnock and kTrial only with the knock (DialConfig::knock).
    enum Why { kForced, kPoll, kSignal, kPairing, kRetry, kAfresh, kKnock, kTrial };
    // What became of a knock: the knock's magic bytes on a link a phone opened.
    enum KnockEnd {
        kNoKnock,          // the knock is not taken: DialConfig::knock is off
        kKnockWaits,       // taken: the dial goes as soon as it may (next())
        kKnockDialled,     // its dial has gone (a KnockNews of next()'s)
        kKnockNotOurs,     // ignored: not a phone of the agent's, and the pairing window is closed
        kKnockQuiet,       // ignored: in the phone's quiet time after a knock's dial that failed
        kKnockBusy,        // ignored: a link of the agent's is up, a dial of that phone is out or up or its knock
                           // waits already, or it waited knockWaitMs in vain
        kKnockNotDialable  // ignored: it names no phone, or one that is not connected to the car and paired with it,
                           // or has had pairing trouble
    };
    // A knock that next() dialled or ignored, for the log.
    struct KnockNews {
        unsigned number;
        KnockEnd end;
        int64_t waitedMs;
        KnockNews() : number(0), end(kKnockWaits), waitedMs(0) {}
    };
    // The knocks so far, for serial.status.
    struct KnockCounts {
        unsigned byMarker;                                                // knocks seen
        unsigned ignoredNotOurs, ignoredQuiet, ignoredBusy, ignoredNotDialable;
        unsigned dials, dialsFailed;                                      // dials knocks brought, and of those the
                                                                          // ones that gave the agent no link
        KnockCounts()
            : byMarker(0), ignoredNotOurs(0), ignoredQuiet(0), ignoredBusy(0), ignoredNotDialable(0), dials(0),
              dialsFailed(0) {}
    };
    // What an answer to ServiceListQuery asks of the listener.
    enum Listing {
        kNotThisAsk,     // not the answer to this ask (another phone, an ask that was ended, or none out): nothing
        kDialNow,        // the agent's service is listed: ServiceConnect for current() now
        kNotListed,      // it is not: no dial
        kNoListing,      // an error, a reply not read, a rejection, or no answer in askLimitMs: no dial, and nothing
                         // held against the phone (askMisses() counts it)
        kNoLongerDue     // listed, and what brought the ask no longer holds: the phone has gone below connection 2,
                         // out of the list or into pairing trouble since the ask, or, for an ask that a forced round
                         // did not bring, it is no longer one of the agent's phones and the pairing window is not
                         // open: no dial
    };
    // What an answer to ServiceConnect asks of the listener.
    enum Reply {
        kNotThis,        // not the answer to this dial (another service, another phone, or no dial out): nothing
        kOpenNode,       // 0
        kLookForNode,    // 2: stale() if the node is there, notThere() if not
        kNotListening,   // 5: the phone is backed off
        kUnreachable,    // 14: the same
        kRetryLater,     // 10, or a rejection of the request: asked and dialled again DialConfig::retryMs later
        kFailed          // any other answer: the phone is backed off
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
        // For the knock: of linkOfOurs, whether a link a phone opened is being decided on (deciding), and whether a
        // link of the agent's is up or opened and not yet taken by the agent (linkUp); and whether this version of
        // the agent is on trial, not yet confirmed (agent_home.hpp), whose rounds go for trialDialForMs only (see the
        // top of this file).
        bool deciding, linkUp, onTrial;
        Facts()
            : registered(false), linkOfOurs(false), requestPending(false), pairingOpen(false), deciding(false),
              linkUp(false), onTrial(false) {}
    };

    explicit DialRules(const DialConfig& config);

    // A device list from the car. What changed for a phone counts: a connection that changed takes its back-off and
    // its run of asks with no answer away, a connection that rises to 2 or more starts its settle time, and security
    // 9 stops its asks; 7 or 8 after a change lets them go on.
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
    // (or once it has settled and its back-off is over), whether or not a retry is due later. One that names no phone
    // brings no ask, and is no phone's own for keepAsking(). With the knock, nothing: a request is no knock.
    void phoneAsked(const std::string& address, int64_t now);
    // A link of the agent's, of either kind, came: what a phone asked for is answered.
    void linkCame();
    // A link of the agent's ended: polls come at the first rate again.
    void linkEnded(int64_t now);
    // When the next poll is due.
    int64_t nextPollAt() const { return nextPollAt_; }
    // A round of asks now to each phone that is eligible: after a link left behind has been ended. With the knock,
    // nothing: the phones knock.
    void afresh();

    // The knock (see the top of this file); with DialConfig::knock off, these take nothing and give kNoKnock.
    // A phone knocked at `now`: kKnockWaits if its dial is to go, or why the knock is ignored.
    KnockEnd knocked(const std::string& address, int64_t now);
    // The knocks next() has dialled or ignored since the last call.
    std::vector<KnockNews> takeKnockNews();
    const KnockCounts& knockCounts() const { return knockCounts_; }
    // Until when the phone's knocks are ignored; 0: they are not.
    int64_t quietUntil(const std::string& address) const;
    // How many knocks wait for their dial.
    size_t knocksWaiting() const { return knocks_.size(); }
    // The trial's rounds (see the top of this file): whether their trialDialForMs have run out since the last call,
    // once for each trial, for the log; whether they are over for this trial; and when their time began (0 while it
    // has not).
    bool takeTrialOver();
    bool trialDialsOver() const { return trialOver_; }
    // The trial has ended (confirmed): as facts that say so, for a listener that is told of it between two next(), so
    // that a trial after it still has its own time.
    void trialEnded();
    int64_t trialDialsFrom() const { return trialClock_ ? trialFrom_ : 0; }

    // The ask to make now, if one is due: ServiceListQuery for out->address, and the state is kAsking from here. One
    // ask or dial at a time, and none while a link of the agent's is up or being decided on, while the service is
    // not registered, while an ask that was ended may still be out in the car, or while the re-point waits for its
    // answer (repointWaits()); while a phone's request is pending, only the ask of the phone that asked, once its time
    // has run. A link of the agent's, a pending request or the registration's going ends the round, and the ask that
    // is out (keepAsking()); the re-point ends the ask that is out. With the knock it is the dial to make now, with no
    // ask: ServiceConnect for out->address, and the state is kDialling from here; none while a phone's request is
    // pending either, and the knocks it dials or ignores are told by takeKnockNews().
    bool next(int64_t now, const Facts& facts, Dial* out);
    State state() const { return state_; }
    // The ask or dial that is out or up, while the state is not kIdle.
    const Dial& current() const { return current_; }

    // While asking: whether the ask goes on as things stand. A link of the agent's, a phone's request that is
    // pending (unless it is the asked phone's, and the ask the one its request brought), the registration's going and
    // a re-point that waits for its answer end it: kIdle, and its answer is passed over when it comes. The pending request is the last one phoneAsked()
    // was told of: one that named another phone, or none, is not the asked phone's. next() does this too.
    bool keepAsking(const Facts& facts, int64_t now);
    // The answer to ServiceListQuery: `error` the car's (kNoError for a rejection or a reply not read), `listed`
    // whether the services named the agent's own, `facts` as they are now. It counts only while asking, for the
    // phone asked. A listed phone is dialled only if what brought the ask still holds (kNoLongerDue otherwise).
    Listing listing(const std::string& address, uint32_t error, bool listed, const Facts& facts, int64_t now);
    // The ask that next() gave could not be sent to the car: as an ask with an error (kNoListing). It cost the car
    // nothing, so an ask a phone's request brought does not count against that phone's requests (signalAskMs).
    Listing notSent(int64_t now);
    // No answer in askLimitMs: listingLost() then, which is kNoListing.
    bool askOverdue(int64_t now) const;
    Listing listingLost(int64_t now);
    // The listener drops the ask that is out, as it stops: kIdle, and its answer is passed over.
    void askDropped(int64_t now);
    // Whether polls leave the phone alone for its settle time now.
    bool settling(const std::string& address, int64_t now) const;
    // How many asks of the phone in a row had an error or no answer. A connection that changes starts it again.
    unsigned askMisses(const std::string& address) const;

    // The answer to ServiceConnect: it counts only for the dial that is out, its service ours and its address the
    // phone's. kNoError (spp_service.hpp) for a rejection of the request.
    Reply reply(const std::string& address, const std::string& service, const std::string& ourService, uint32_t error,
                int64_t now);
    // Whether the car's answer to a dial is a failure that leaves its record of phone-opened links doubtful
    // (recordDoubtful()), and so brings the re-point: any answer but 0, 2 and 10, and not kNoError, which is no answer
    // of the car's (a rejection of the request, a reply that could not be read, a send that failed). reply() goes by
    // it too.
    static bool doubtingAnswer(uint32_t error);
    // No answer in replyLimitMs. The listener ends what may be up, and then gaveUp().
    bool overdue(int64_t now) const;
    // The dial is given up, for no answer in its time or because the listener stops while it is out: the phone is
    // backed off, the run of failed dials counts it, and the car's record is doubted. kIdle. The listener never gives
    // a late answer to it to reply().
    void gaveUp(int64_t now);
    // 2, and no node: as 10, asked and dialled again retryMs later.
    void notThere(int64_t now);
    // 2 with the node there: a link was left behind. The listener ends it (kEnding), and then dials the phone afresh.
    void stale();
    // The car was asked to end that link and its node is still there: kIdle, and the phone is not dialled again at
    // once but backed off, as for something that is not the app.
    void notEnded(int64_t now);
    // The car ends a phone's request for a link now: whether the agent's own dial overtook it. The dial is out, or
    // was answered 0 less than DialConfig::overtakeMs ago.
    bool overtakes(int64_t now) const;

    // The magic bytes came on the dialled node: kUp, the phone's back-off and its run of failed dials are gone, and so
    // is the rest of the round.
    void linkUp(int64_t now);
    // They did not, or something else came: something answered on the agent's UUID that is not the app. The phone
    // is backed off, and the link is ended: kEnding.
    void notTheApp(int64_t now);
    // How many dials of the phone in a row gave the agent no link (see the top of this file).
    unsigned dialFailures(const std::string& address) const;
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

    // The re-point (see the top of this file) went to the car at `now`: from here no ask or dial starts, and an ask
    // that is out is ended, until repointDone() or DialConfig::repointLimitMs have passed. Nothing else here changes:
    // the doubt about the car's record stays.
    void repointSent(int64_t now);
    // Its answer has come, it is given up, or the listener drops it: asks and dials go on.
    void repointDone();
    // Whether it is out: sent, and not yet done.
    bool repointOut() const { return repointOut_; }
    int64_t repointAt() const { return repointAt_; }
    // Whether it is out and its time has not run: nothing new starts.
    bool repointWaits(int64_t now) const;
    // Whether it is out and repointLimitMs have passed since it went: it is to be given up.
    bool repointOverdue(int64_t now) const;

    // The back-off of a phone: 0 if it has none, or the time until which polls and its own requests leave it alone.
    int64_t backedOffUntil(const std::string& address) const;
    // How long the next back-off of the phone will be.
    int64_t nextBackOffMs(const std::string& address) const;
    // Whether polls leave the phone alone now: backed off, or due for a retry later. Its settle time is settling().
    bool heldBack(const std::string& address, int64_t now) const;
    // Whether the phone's asks have stopped for pairing trouble.
    bool troubled(const std::string& address) const;

private:
    // The asks a phone's requests have brought, for the rule that they bring one every signalAskMs at most.
    struct SignalAsks {
        bool asked;               // a request of its brought an ask that went to the car, at `at`
        int64_t at;
        bool spare;               // an ask a request of its brought since `at` was ended before its answer came: one
                                  // more may go before signalAskMs are over
        bool spareUsed;           // that one more has gone
        SignalAsks() : asked(false), at(0), spare(false), spareUsed(false) {}
    };
    struct PhoneState {
        int64_t backOffMs;        // the next back-off; 0: none has been
        int64_t backOffUntil;
        int64_t retryAt;          // 10, 2 with no node, or a rejection: asked and dialled again then; 0: none
        bool trouble;             // pairing trouble: no ask until it is at 7 or 8 after a change
        unsigned misses;          // asks in a row with an error or no answer
        unsigned dialFails;       // dials in a row that gave the agent no link
        int64_t settleUntil;      // its settle time ends then
        int64_t quietUntil;       // the knock: its knocks are ignored until then
        SignalAsks signals;
        PhoneState()
            : backOffMs(0), backOffUntil(0), retryAt(0), trouble(false), misses(0), dialFails(0), settleUntil(0),
              quietUntil(0) {}
    };
    // A knock that waits for its dial.
    struct Knock {
        std::string address;
        int64_t at;
    };
    PhoneState& phone(const std::string& address);
    const PhoneState* find(const std::string& address) const;
    void backOff(const std::string& address, int64_t now);
    void dialFailed(int64_t now);
    bool takeFromRound(int64_t now, const Facts& facts, Dial* out);
    void start(const btspp::Presence::Phone& p, Why why, int64_t now, Dial* out);
    bool signalDue(int64_t now, const Facts& facts, Dial* out);
    Listing missed(int64_t now);
    void endAsk(int64_t now);
    void fastPolls(int64_t now);
    bool nextKnockMode(int64_t now, const Facts& facts, Dial* out);
    bool trialDials(int64_t now, const Facts& facts);
    void judgeKnocks(int64_t now, const Facts& facts);
    void ignore(const Knock& k, KnockEnd why, int64_t now);
    void knockFailed(int64_t now);
    void count(KnockEnd why);

    DialConfig config_;
    btspp::Presence presence_;
    int64_t nextPollAt_;                         // 0: at the first chance
    int64_t fastUntil_;                          // until then polls come every pollFirstMs
    std::string signal_;                         // the phone that asked for a link, while its time runs
    int64_t signalAt_;
    std::string askedBy_;                        // the phone the last request named, by addressKey(); empty for a
                                                 // request that named none (keepAsking())
    bool pairingWasOpen_;
    int64_t nextPairingAt_;
    std::map<std::string, PhoneState> phones_;   // by addressKey()
    State state_;
    Dial current_;
    bool stale_;                                 // the current dial's phone had a link left behind: again at once
    int64_t okAt_;                               // when the last dial was answered 0; 0: none
    std::vector<std::string> round_;             // the phones of a round still to be asked, by address
    Why roundWhy_;
    bool forcedAsk_;                             // the ask that is out holds for any dialable phone: a forced round
                                                 // brought it, or the dial it is the afresh ask after
    std::string endedAsk_;                      // an ask that was ended, which may still be out in the car, until
    int64_t endedAskUntil_;                      // its answer comes or this time
    SignalAsks signalsBefore_;                   // the asking phone's record before the ask a request brought: put
                                                 // back if that ask could not be sent (notSent())
    bool doubtful_;                              // recordDoubtful()
    bool repointOut_;                            // repointOut(), since repointAt_
    int64_t repointAt_;
    // The knock.
    std::vector<Knock> knocks_;                  // waiting for their dial, in the order they came; one to a phone
    std::vector<KnockNews> knockNews_;           // for takeKnockNews()
    KnockCounts knockCounts_;
    bool knockDial_;                             // the dial that is out or up was brought by a knock (afresh after a
                                                 // link left behind included): a failure brings the quiet time
    int64_t nextTrialAt_;                        // the next round while the agent is on trial; 0: at the first chance
    bool trialClock_;                            // the trial's trialDialForMs are counted, from trialFrom_
    int64_t trialFrom_;
    bool trialOver_;                             // they have run out: the trial brings no more rounds
    bool trialOverNews_;                         // for takeTrialOver()
};

}  // namespace nbt

#endif
