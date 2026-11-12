// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The CRC32 that agent/frame.cpp calls, for libNbtApp.so: see crc32.hpp. The agent's own is in agent/inflate.cpp,
// on zlib, and the two are never linked into one program.

#include "crc32.hpp"

namespace nbt {

uint32_t crc32(uint32_t crc, const void* data, size_t len) { return plugin::crc32(crc, data, len); }

}  // namespace nbt
