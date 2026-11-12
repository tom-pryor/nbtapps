// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_HEX_HPP
#define NBT_AGENT_HEX_HPP

// Hex text for hashes, keys and signatures. The manifest and the protocol write them in lowercase only, so
// capitals are refused: two spellings of one hash would otherwise compare as different strings.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace nbt {

inline std::string toHex(const uint8_t* data, size_t len) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

// Exactly `len` bytes written as 2 * len lowercase hex characters; false for anything else.
inline bool fromHex(const std::string& text, uint8_t* out, size_t len) {
    if (text.size() != len * 2) return false;
    for (size_t i = 0; i < len; ++i) {
        int value = 0;
        for (int j = 0; j < 2; ++j) {
            char c = text[i * 2 + j];
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else return false;
            value = value * 16 + digit;
        }
        out[i] = (uint8_t)value;
    }
    return true;
}

}  // namespace nbt

#endif
