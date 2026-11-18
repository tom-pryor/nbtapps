// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_DIAL_CONFIG_HPP
#define NBT_AGENT_DIAL_CONFIG_HPP

// The car dials the phone: what the Bluetooth serial listener (serial_link.hpp) goes by when it has the car open a
// serial link to a phone of the agent's (dial_rules.hpp), in one place. `nbtagent --dial off` turns all of it off,
// and the agent then takes the links phones open, and no other, as it did before.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <string>

namespace nbt {

struct DialConfig {
    bool on;                     // the car dials the agent's phones at all
    unsigned replyLimitMs;       // a dial with no answer for this long is given up, and its phone backed off
    unsigned retryMs;            // an answer that is not understood: the phone is dialled again this much later
    unsigned backOffMs;          // something that is not the app answered on the agent's UUID: the phone is left
    unsigned backOffMostMs;      // alone by polls this long, twice as long each time it happens again, up to this

    DialConfig() : on(true), replyLimitMs(95000), retryMs(15000), backOffMs(30000), backOffMostMs(300000) {}
};

}  // namespace nbt

#endif
