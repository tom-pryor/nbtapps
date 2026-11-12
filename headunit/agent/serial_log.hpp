// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SERIAL_LOG_HPP
#define NBT_AGENT_SERIAL_LOG_HPP

#include <string>

namespace nbt {

// Where the Bluetooth serial code's account of what it does goes, a line at a time, from any of its threads.
class SerialLog {
public:
    virtual ~SerialLog() {}
    virtual void line(const std::string& line) = 0;
};

}  // namespace nbt

#endif
