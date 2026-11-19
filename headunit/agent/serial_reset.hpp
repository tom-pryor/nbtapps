// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SERIAL_RESET_HPP
#define NBT_AGENT_SERIAL_RESET_HPP

// What the agent's method serial.reset asks of the Bluetooth serial listener (serial_link.hpp): that the car end
// the phone's serial link. Closing a link's node does not end the link; the car's Bluetooth service has a request
// that does, ServiceDisconnect (spp_service.hpp), and this is how the agent's thread asks the listener to send it.
// A link the car dialled is ended the way the agent ends such a link: the agent tells the phone with link.closing
// and closes its end, and the listener has the car end the link if the phone has not.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdint.h>

#include <string>

namespace nbt {

class SerialReset {
public:
    enum Outcome {
        kResetSent,     // a serial node is open: the request goes to the car shortly
        kResetArmed,    // none is: the next serial link is ended at the open of its node, within *forMs
        kResetFailed,   // the car's Bluetooth service cannot be asked; *why says why
        kResetDialled   // a link the car dialled is up: the agent ends it as it ends a link, with link.closing, and the
                        // car is asked to end it if the phone has not within the time (dial_config.hpp)
    };
    virtual ~SerialReset() {}
    // From the agent's thread. It answers at once and does nothing itself: the listener's thread sends the request,
    // and says in the log what came of it. An empty `device`, `instance` or `service` is the listener's own value.
    virtual Outcome resetSerialLink(const std::string& device, const std::string& instance, const std::string& service,
                                    int64_t* forMs, std::string* why) = 0;
};

}  // namespace nbt

#endif
