// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBTAPP_CRC32_HPP
#define NBTAPP_CRC32_HPP

// CRC32 (the one of zip and of the protocol's frames) without zlib. The agent takes its CRC32 from zlib, which it
// links in whole; libNbtApp.so is loaded into the HMI's process and takes nothing along that it can do without.
// A bit at a time: the plugin's messages are a few hundred bytes.
//
// src/crc32.cpp gives it the name the agent's frame code calls (nbt::crc32). tests/agent/test_plugin.cpp holds it
// against zlib's.

#include <stddef.h>
#include <stdint.h>

namespace nbt {
namespace plugin {

// Start with 0 and pass the result back in to continue over more data.
inline uint32_t crc32(uint32_t crc, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

}  // namespace plugin
}  // namespace nbt

#endif
