// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_DIAL_CONFIG_HPP
#define NBT_AGENT_DIAL_CONFIG_HPP

// The car dials the phone: what the Bluetooth serial listener (serial_link.hpp) goes by when it has the car open a
// serial link to a phone of the agent's (dial_rules.hpp), in one place. `nbtagent --dial off` turns all of it off,
// and the agent then takes the links phones open, and no other, as it did before. `nbtagent --dial-knock off` keeps
// the dial and turns the knock off: the car then asks a phone for its services and dials it when they name the
// agent's, at polls and at the phone's requests, as before the knock.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdio.h>
#include <string.h>

#include <string>

#include "protocol_constants.hpp"

namespace nbt {

// Dials of a phone in a row that gave the agent no link at which its `dialled` mark is given up
// (DialRules::dialFailures()).
const unsigned kDialFailsToUnmark = 3;

struct DialConfig {
    bool on;                     // the car dials the agent's phones at all
    bool knock;                  // and only when a phone's app knocks, and in the pairing window, at serial.dial and
                                 // for a while when the agent is on trial, with no ask before the dial and no poll
                                 // (dial_rules.hpp, "The knock"). Off: the asks and polls below, as before the knock
    unsigned knockQuietMs;       // a dial a knock brought gave the agent no link: further knocks of that phone are
                                 // ignored for this long
    unsigned knockWaitMs;        // a knock that came while a phone's request was pending, a link was being decided on
                                 // or the car's device list had not come waits this long at most for its dial
    unsigned trialDialMs;        // while this version of the agent is on trial and no link of the agent's is up: a
                                 // round of dials of the agent's phones that are there this often,
    unsigned trialDialForMs;     // for this long from the first moment such a round could go (the trial known, the
                                 // service registered, the car's device list come); after it the trial brings no
                                 // dials, and a phone is dialled when its app knocks
    unsigned pollFirstMs;       // after a phone has connected to the car, or a link of the agent's has ended: a poll of
    unsigned pollFirstForMs;     // the agent's phones this often, for this long,
    unsigned pollLaterMs;        // and this often after that
    unsigned signalWaitMs;       // a phone asked for a serial link and no link of the agent's came in this time: a dial
                                 // of that phone at once
    unsigned askLimitMs;         // the ask before a dial (ServiceListQuery): one with no answer for this long counts
                                 // as one that did not list the agent's service. The car's answer BUSY to a query
                                 // never reaches the client, so this is how such an ask ends
    unsigned settleMs;           // no poll's ask and no ask a phone's request brings, for a phone, until this long
                                 // after the device list first showed it connected: the car's own media component asks
                                 // for the phone's services as its audio connects, and one ask at a time is answered
    unsigned signalAskMs;        // a phone's requests for a serial link bring one ask of that phone this often at most
                                 // (and one more, once, after an ask of theirs that was ended before its answer)
    unsigned pairingRoundMs;     // while the pairing window is open: every phone there is dialled in turn, a round this
                                 // often
    unsigned replyLimitMs;       // a dial with no answer for this long is given up, and its phone backed off
    unsigned retryMs;            // a dial answered 10, or 2 with no node, or whose request was rejected: the phone is
                                 // asked and dialled again this much later
    unsigned backOffMs;          // a dial that failed or had no answer, or on which something that is not the app
    unsigned backOffMostMs;      // answered: the phone is left alone by polls and by its own requests this long, twice
                                 // as long each time it happens again, up to this
    unsigned magicWaitMs;        // the dialled link: how long the app's magic bytes may take from the open of its node.
                                 // Longer than a phone-opened link's: the app has to accept the link before it writes
    unsigned disconnectWaitMs;   // how long the answer to a ServiceDisconnect of the dial's is waited for; one of any
                                 // kind with no answer in this time is given up, for the next to go
                                 // (SppService::setDisconnectReplyTime)
    unsigned closeWaitMs;        // a dialled link that the agent has ended: how long the phone has to end it before the
                                 // car is asked to
    unsigned overtakeMs;         // the car ends a phone's request for a link while the agent's own dial is out, or this
                                 // long after it was answered 0: the dial overtook the request, which is no refusal
    unsigned listWaitMs;         // the registration refused with the dialled node there: how long the car's device list
                                 // is waited for, to end the link left behind before the service is registered again
    unsigned registerCleanupMs;  // and how long that clean-up may take in all, the device list included, before the
                                 // listener gives up as it does for any registration that is refused
    unsigned repointLimitMs;     // the re-point after a dial the car answered with a failure (dial_rules.hpp): how long
                                 // its answer is waited for, with no ask or dial meanwhile, before it is given up
    unsigned repointSlowMs;      // and an answer 5 that takes this long or longer is not the one expected of the car
    std::string node;            // the dialled link's node, /dev/<instance>

    DialConfig()
        : on(true), knock(true), knockQuietMs(3000), knockWaitMs(4000), trialDialMs(5000),
          trialDialForMs(120000), pollFirstMs(10000),pollFirstForMs(120000), pollLaterMs(30000), signalWaitMs(1000),
          askLimitMs(6000), settleMs(8000), signalAskMs(10000), pairingRoundMs(5000), replyLimitMs(95000),
          retryMs(15000), backOffMs(30000), backOffMostMs(300000), magicWaitMs(3000),
          disconnectWaitMs(3000), closeWaitMs(5000), overtakeMs(1000), listWaitMs(3000), registerCleanupMs(15000),
          repointLimitMs(3000), repointSlowMs(500), node("/dev/nbtapp") {}
};

// The value of `nbtagent --dial-poll`: three whole numbers of seconds with a comma between them, the poll's first
// period (1 to 3600), how long it holds (0 to 86400) and the period after it (1 to 3600), in digits and nothing else.
inline bool parseDialPoll(const char* text, DialConfig* out) {
    if (text == 0) return false;
    unsigned v[3] = {0, 0, 0};
    const unsigned most[3] = {3600, 86400, 3600}, least[3] = {1, 0, 1};
    int at = 0;
    bool digit = false;
    for (const char* c = text;; ++c) {
        if (*c >= '0' && *c <= '9') {
            v[at] = v[at] * 10 + (unsigned)(*c - '0');
            if (v[at] > most[at]) return false;
            digit = true;
        } else if ((*c == ',' && at < 2) || (*c == 0 && at == 2)) {
            if (!digit || v[at] < least[at]) return false;
            if (*c == 0) break;
            ++at;
            digit = false;
        } else {
            return false;
        }
    }
    out->pollFirstMs = v[0] * 1000;
    out->pollFirstForMs = v[1] * 1000;
    out->pollLaterMs = v[2] * 1000;
    return true;
}

// The value of `nbtagent --dial-knock`: on or off.
inline bool parseDialKnock(const char* text, bool* out) {
    if (text == 0) return false;
    if (strcmp(text, "on") != 0 && strcmp(text, "off") != 0) return false;
    *out = strcmp(text, "on") == 0;
    return true;
}

// What the car dialling the phone goes by, for the agent's first lines in the log.
inline std::string describeDial(const DialConfig& c) {
    if (!c.on)
        return "the car does not dial phones (--dial off): only the links phones open are taken, on the car's one node "
               "for them";
    char text[4000];
    if (c.knock) {
        snprintf(text, sizeof text,
                 "the car dials a phone on its instance 'nbtapp' (%s) when the phone's app knocks, and otherwise only "
                 "while the pairing window is open, at serial.dial and while this version of the agent is on trial "
                 "(--dial-knock on): a link the phone opens that starts with the knock's magic bytes within the decide "
                 "time is that phone's knock, and a phone's request for a serial link is no knock, whatever becomes of "
                 "it. A knock of any phone that is connected to the car and paired with it, one of the agent's or not, "
                 "has the car dial it at once, with no "
                 "ask before it, when no link of the agent's is up and no dial is out; one that comes while a phone's "
                 "request is pending, a link is being decided on or another phone's dial is out waits %u ms at most. "
                 "After a dial a knock brought that gave the agent no link, that phone's knocks are "
                 "ignored for %u ms. No poll, and no ask of a phone's services. While the pairing window is open every "
                 "phone there is dialled every %u s, and while this version of the agent is on trial and no link of the "
                 "agent's is up, every phone of the agent's there every %u s, for %u s from when the trial is known, "
                 "the serial service registered and the car's device list come; after that, until the version is "
                 "confirmed, a phone is dialled when its app knocks, as when it is not on trial. The app's magic bytes "
                 "are waited for %u "
                 "ms; a dial with no answer is given up after %u s; a link left behind on the instance that a dial "
                 "finds is ended, and the phone dialled again at once. A dialled link that the agent ends has %u ms to "
                 "be ended by the phone before the car is asked to, and the car's answer is waited for %u ms. A phone's "
                 "request that the car ends while the agent's dial is out, or up to %u ms after it was answered, is not "
                 "counted as a refusal. A registration refused while %s is there: the car's device list is waited for "
                 "%u ms and the link ended for each phone there, %u s at most in all, and the service registered again "
                 "at once if it has gone. As soon as the car answers a dial with a failure other than 10 or BUSY, one "
                 "re-point request goes on the same instance, so that a link end the car may book against that "
                 "instance names no phone: its answer is waited for %u ms, with no dial meanwhile, and an answer other "
                 "than 5 within %u ms stops these requests for the rest of the run",
                 c.node.c_str(), c.knockWaitMs, c.knockQuietMs,
                 c.pairingRoundMs / 1000, c.trialDialMs / 1000, c.trialDialForMs / 1000, c.magicWaitMs,
                 c.replyLimitMs / 1000, c.closeWaitMs,
                 c.disconnectWaitMs, c.overtakeMs, c.node.c_str(), c.listWaitMs, c.registerCleanupMs / 1000,
                 c.repointLimitMs, c.repointSlowMs);
        return text;
    }
    snprintf(text, sizeof text,
             "the car dials a phone of the agent's that is connected to it and paired with it, on its instance "
             "'nbtapp' (%s), with no knock (--dial-knock off): polls every %u s for %u s after such a phone connects or a "
             "link of the agent's ends, then "
             "every %u s; at once when the phone asks for a link and no link of the agent's comes in %u ms; every phone "
             "there every %u s while the pairing window is open. Before each dial the car asks the phone for its "
             "services, and dials it only if they name the agent's: an ask has %u ms to be answered, one with no "
             "answer or an error is as one that does not name it and holds nothing back, no poll and no request of "
             "the phone's asks it within %u ms of its connecting to the car, and its requests bring one ask of it every "
             "%u ms at most, and one more after an ask of theirs that was ended before its answer. The app's magic "
             "bytes are waited for %u ms; a dial with no answer is given up after %u s, and one answered 10, or BUSY "
             "with no link left behind, or whose request the car's Bluetooth service did not take, is tried again "
             "after %u s; a phone whose dial the car answered with another failure or did not answer, on whose UUID "
             "something else answers, or whose link left behind the car does not end, is left alone by polls and by "
             "its own requests for %u s, twice as long each time up to %u s, until a dialled link of the agent's comes "
             "up for it or its connection changes. After %u dials of a phone in a row that gave the agent no link, a "
             "link the phone opens itself is no longer left to another program, until a dialled link of the agent's "
             "reaches it again. A dialled link that the agent ends has %u ms "
             "to be ended by the phone before the car is asked to, and the car's answer is waited for %u ms. A phone's "
             "request that the car ends while the agent's dial is out, or up to %u ms after it was answered, is not "
             "counted as a refusal. A registration refused while %s is there: the car's device list is waited for %u ms "
             "and the link ended for each phone there, %u s at most in all, and the service registered again at once if "
             "it has gone. As soon as the car answers a dial with a failure other than 10 or BUSY, one re-point request "
             "goes on the same instance, so that a link end the car may book against that instance names no phone: its "
             "answer is waited for %u ms, with no ask or dial meanwhile, and an answer other than 5 within %u ms stops "
             "these requests for the rest of the run",
             c.node.c_str(), c.pollFirstMs / 1000, c.pollFirstForMs / 1000, c.pollLaterMs / 1000, c.signalWaitMs,
             c.pairingRoundMs / 1000, c.askLimitMs, c.settleMs, c.signalAskMs, c.magicWaitMs, c.replyLimitMs / 1000,
             c.retryMs / 1000, c.backOffMs / 1000,
             c.backOffMostMs / 1000, kDialFailsToUnmark, c.closeWaitMs, c.disconnectWaitMs, c.overtakeMs, c.node.c_str(), c.listWaitMs,
             c.registerCleanupMs / 1000, c.repointLimitMs, c.repointSlowMs);
    return text;
}

}  // namespace nbt

#endif
