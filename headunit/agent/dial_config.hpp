// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_DIAL_CONFIG_HPP
#define NBT_AGENT_DIAL_CONFIG_HPP

// The car dials the phone: what the Bluetooth serial listener (serial_link.hpp) goes by when it has the car open a
// serial link to a phone of the agent's (dial_rules.hpp), in one place. `nbtagent --dial off` turns all of it off,
// and the agent then takes the links phones open, and no other, as it did before.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdio.h>

#include <string>

namespace nbt {

struct DialConfig {
    bool on;                     // the car dials the agent's phones at all
    unsigned pollFirstMs;        // after a phone has connected to the car, or a link of the agent's has ended: a poll of
    unsigned pollFirstForMs;     // the agent's phones this often, for this long,
    unsigned pollLaterMs;        // and this often after that
    unsigned signalWaitMs;       // a phone asked for a serial link and no link of the agent's came in this time: a dial
                                 // of that phone at once
    unsigned pairingRoundMs;     // while the pairing window is open: every phone there is dialled in turn, a round this
                                 // often
    unsigned replyLimitMs;       // a dial with no answer for this long is given up, and its phone backed off
    unsigned retryMs;            // an answer that is not understood: the phone is dialled again this much later
    unsigned backOffMs;          // something that is not the app answered on the agent's UUID: the phone is left
    unsigned backOffMostMs;      // alone by polls this long, twice as long each time it happens again, up to this
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
    std::string node;            // the dialled link's node, /dev/<instance>

    DialConfig()
        : on(true), pollFirstMs(10000), pollFirstForMs(120000), pollLaterMs(30000), signalWaitMs(1000),
          pairingRoundMs(5000), replyLimitMs(95000), retryMs(15000), backOffMs(30000), backOffMostMs(300000), magicWaitMs(3000),
          disconnectWaitMs(3000), closeWaitMs(5000), overtakeMs(1000), listWaitMs(3000), registerCleanupMs(15000),
          node("/dev/nbtapp") {}
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

// What the car dialling the phone goes by, for the agent's first lines in the log.
inline std::string describeDial(const DialConfig& c) {
    if (!c.on)
        return "the car does not dial phones (--dial off): only the links phones open are taken, on the car's one node "
               "for them";
    char text[1600];
    snprintf(text, sizeof text,
             "the car dials a phone of the agent's that is connected to it and paired with it, on its instance "
             "'nbtapp' (%s): polls every %u s for %u s after such a phone connects or a link of the agent's ends, then "
             "every %u s; at once when the phone asks for a link and no link of the agent's comes in %u ms; every phone "
             "there every %u s while the pairing window is open. The app's magic bytes are waited for %u ms; a dial with "
             "no answer is given up after %u s, and one answered with an error not understood is tried again after %u "
             "s; a phone on whose UUID something else answers, or whose link left behind the car does not end, is left "
             "alone by polls for %u s, twice as long each time up to %u s. A dialled link that the agent ends has %u ms "
             "to be ended by the phone before the car is asked to, and the car's answer is waited for %u ms. A phone's "
             "request that the car ends while the agent's dial is out, or up to %u ms after it was answered, is not "
             "counted as a refusal. A registration refused while %s is there: the car's device list is waited for %u ms "
             "and the link ended for each phone there, %u s at most in all, and the service registered again at once if "
             "it has gone",
             c.node.c_str(), c.pollFirstMs / 1000, c.pollFirstForMs / 1000, c.pollLaterMs / 1000, c.signalWaitMs,
             c.pairingRoundMs / 1000, c.magicWaitMs, c.replyLimitMs / 1000, c.retryMs / 1000, c.backOffMs / 1000,
             c.backOffMostMs / 1000, c.closeWaitMs, c.disconnectWaitMs, c.overtakeMs, c.node.c_str(), c.listWaitMs,
             c.registerCleanupMs / 1000);
    return text;
}

}  // namespace nbt

#endif
