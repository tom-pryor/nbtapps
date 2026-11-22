// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SERIAL_DIAL_HPP
#define NBT_AGENT_SERIAL_DIAL_HPP

// What the agent tells the Bluetooth serial listener (serial_link.hpp) for the car dialling a phone of the agent's
// (dial_rules.hpp), and asks of it: the pairing window, a dial now, how Bluetooth serial stands, for the method
// serial.status and info.get (agent.hpp), and which phones' `dialled` marks to give up. The agent's thread calls it;
// the listener's thread acts on it.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdint.h>

#include <string>
#include <vector>

namespace nbt {

// How Bluetooth serial stands. Counts, and names of states: never a phone's address or name.
struct SerialStatus {
    bool registered;                   // the agent's serial service is registered with the car's Bluetooth service
    int64_t checkedMsAgo;              // it was last registered or checked this long ago; below 0: never
    std::vector<std::string> stack;    // the stack's version strings (VersionGet); empty until they have come
    unsigned phonesPresent;            // phones connected to the car, in its device list
    unsigned phonesEligible;           // of those, the ones the car may dial now: paired with it, and the agent's
    bool dialOn;                       // the car dials phones in this run (nbtagent --dial)
    bool knock;                        // and only when they knock (nbtagent --dial-knock, dial_rules.hpp)
    std::string dialState;           // idle, asking, dialling, up, ending
    std::string lastResult;            // what the last dial came to: the car's answer by its name (OK, DEFAULT, BUSY,
                                       // PAGE_TIMEOUT, ...), "rejected", "no answer", "not the app"; "none" before one
    std::string link;                  // the agent's serial links up: none, phone-opened, dialled, both
    bool recordDoubtful;               // a dial failed after the phone's service list named the agent's service, and
                                       // the car's record of phone-opened links may be wrong since (dial_rules.hpp)
    bool repointStopped;               // a re-point after a failed dial came to something the car is not expected to
                                       // do, and no more are sent in this run (dial_rules.hpp)
    // Counts since the agent started. notListening: the dials answered 5, after an ask that listed the agent's
    // service (agent.hpp says what that means now).
    unsigned dials, answeredOk, notListening, notInReach, busy, otherAnswers, noAnswer, notTheApp, leftBehind;
    // The asks for a phone's service list before a dial: sent, and what they came to.
    unsigned asks, asksListed, asksNotListed, asksNoAnswer, askErrors;
    // The re-points after failed dials: sent, answered as expected (5, in time), with anything else (which stops
    // them), and the ones that could not be sent at all.
    unsigned repoints, repointsExpected, repointsUnexpected, repointsNotSent;
    unsigned checks, checksRestored, refusals;
    // leftUnread: the links phones opened that the rule left to another program, never read: primed for it, or left
    // as they were when that could not be done (serial_link.hpp). putBack: the links on which bytes the agent had
    // read were put back into the node's queue for another program, by the rule's priming or when a link was let go.
    unsigned leftUnread, putBack;
    // The knocks (dial_rules.hpp): seen by their magic bytes; ignored, as not the agent's phone while the pairing
    // window is closed, in the phone's quiet time, while something of the agent's was under way (a link up, the
    // phone's dial out or its knock waiting already, or a wait for its dial that ran out), or for a phone that cannot
    // be dialled (not connected to the car and paired with it, pairing trouble, no phone named); the dials knocks
    // brought, and of those the ones that gave the agent no link.
    unsigned knocksByMarker, knocksIgnoredNotOurs, knocksIgnoredQuiet, knocksIgnoredBusy, knocksIgnoredNotDialable,
        knockDials, knockDialsFailed;
    // The refusals whose look at the car's device list (the first, and every 20th) found the phone connected with no
    // service connected: no media audio, while which the car refuses a phone's own links.
    unsigned refusalsNoAudio;
    unsigned problemsRecorded;        // lines written to the problem file in this run (serial_link.hpp)
    unsigned problemsCounted;          // problems that were not written, only counted
    SerialStatus()
        : registered(false), checkedMsAgo(-1), phonesPresent(0), phonesEligible(0), dialOn(false), knock(false),
          dialState("idle"), lastResult("none"), link("none"), recordDoubtful(false), repointStopped(false), dials(0),
          answeredOk(0), notListening(0), notInReach(0), busy(0), otherAnswers(0), noAnswer(0), notTheApp(0),
          leftBehind(0), asks(0), asksListed(0), asksNotListed(0), asksNoAnswer(0), askErrors(0), repoints(0),
          repointsExpected(0), repointsUnexpected(0), repointsNotSent(0), checks(0), checksRestored(0), refusals(0),
          leftUnread(0), putBack(0), knocksByMarker(0), knocksIgnoredNotOurs(0), knocksIgnoredQuiet(0),
          knocksIgnoredBusy(0), knocksIgnoredNotDialable(0), knockDials(0), knockDialsFailed(0), refusalsNoAudio(0),
          problemsRecorded(0), problemsCounted(0) {}
};

class SerialDial {
public:
    virtual ~SerialDial() {}
    // The agent's pairing window has opened, or has closed: while it is open every phone that is connected to the
    // car and paired with it is dialled in turn, for a new phone's app to be reached and paired.
    virtual void pairingWindow(bool open) = 0;
    // Whether the car dials phones in this run at all, and whether only when they knock (dial_rules.hpp).
    virtual bool dialOn() = 0;
    virtual bool knockOn() = 0;
    // Whether this version of the agent is on trial, not yet confirmed (agent.hpp): with the knock, the car dials the
    // agent's phones while it is, whether they knock or not, for a limited time (DialConfig::trialDialForMs) and
    // then only when they knock. The agent says so when it is given the listener, and
    // again when the version is confirmed.
    virtual void agentOnTrial(bool onTrial) = 0;
    // A round of dials now, to each phone connected to the car and paired with it: how many phones that is. 0 with
    // *why, and *code as an error of the protocol's ("busy" while a link of the agent's is up or a dial is out,
    // "not-found" with no such phone, "io-error" when the car cannot be asked).
    virtual unsigned dialNow(std::string* why, const char** code) = 0;
    virtual SerialStatus serialStatus() = 0;
    // Whether a link the phone at `address` opens itself is left to another program now, unread (the rule of
    // serial_link.hpp): for that phone's app, which then only waits for the car's link. From the agent's thread.
    virtual bool phoneOpenedLeft(const std::string& address) = 0;
    // The addresses of the phones whose `dialled` mark (phones.hpp) is to be given up, since the car's dials to them
    // keep failing (dial_rules.hpp, kDialFailsToUnmark): each handed over once, and the agent clears the mark in its
    // record of paired phones. The listener's thread puts them there and wakes the agent through the pipe it hands
    // links over by (Listener::fd()); the agent's thread takes them, as it takes links.
    virtual std::vector<std::string> takeUndialled() = 0;
};

}  // namespace nbt

#endif
