// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_UTF8_HPP
#define NBT_AGENT_UTF8_HPP

// UTF-8 checks for names that do not come through the JSON parser (zip entry names), character counts for the
// manifest's length limits, which are in characters where the path limits are in bytes, and the repair of text
// that goes into a message.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>

#include <string>

namespace nbt {

// True if `text` is well-formed UTF-8: shortest forms only, no surrogates, nothing above U+10FFFF (Unicode,
// table 3-7). The same rule as the JSON parser applies to strings.
inline bool validUtf8(const std::string& text) {
    const unsigned char* p = (const unsigned char*)text.data();
    const unsigned char* end = p + text.size();
    while (p != end) {
        unsigned char lead = *p;
        if (lead < 0x80) {
            ++p;
            continue;
        }
        int more;
        unsigned char low = 0x80, high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            more = 1;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            more = 2;
            if (lead == 0xE0) low = 0xA0;
            if (lead == 0xED) high = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            more = 3;
            if (lead == 0xF0) low = 0x90;
            if (lead == 0xF4) high = 0x8F;
        } else {
            return false;
        }
        if (end - p < more + 1) return false;
        if (p[1] < low || p[1] > high) return false;
        for (int i = 2; i <= more; ++i) {
            if (p[i] < 0x80 || p[i] > 0xBF) return false;
        }
        p += more + 1;
    }
    return true;
}

// Text from outside (a hook's output) made safe to send: every byte that is not part of well-formed UTF-8 becomes
// U+FFFD. Output arrives in pieces that can end in the middle of a character; with `tail`, such an unfinished
// character at the very end is not replaced but put into *tail, for the caller to put in front of the next piece.
inline std::string sanitizeUtf8(const std::string& text, std::string* tail) {
    std::string out;
    out.reserve(text.size());
    if (tail != 0) tail->clear();
    const unsigned char* p = (const unsigned char*)text.data();
    const unsigned char* end = p + text.size();
    while (p != end) {
        unsigned char lead = *p;
        if (lead < 0x80) {
            out += (char)lead;
            ++p;
            continue;
        }
        int more = 0;
        unsigned char low = 0x80, high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            more = 1;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            more = 2;
            if (lead == 0xE0) low = 0xA0;
            if (lead == 0xED) high = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            more = 3;
            if (lead == 0xF0) low = 0x90;
            if (lead == 0xF4) high = 0x8F;
        }
        // How many of the bytes that should follow are there, and right?
        int good = 0;
        while (more != 0 && good < more && p + 1 + good != end) {
            unsigned char c = p[1 + good];
            if (c < (good == 0 ? low : 0x80) || c > (good == 0 ? high : 0xBF)) break;
            ++good;
        }
        if (more != 0 && good == more) {
            out.append((const char*)p, (size_t)more + 1);
            p += more + 1;
        } else if (more != 0 && p + 1 + good == end && tail != 0) {
            tail->assign((const char*)p, (size_t)(end - p));   // cut off, not wrong
            break;
        } else {
            out += "\xEF\xBF\xBD";
            ++p;
        }
    }
    return out;
}

// The number of characters (code points) in well-formed UTF-8: every byte that is not a continuation byte.
inline size_t utf8Length(const std::string& text) {
    size_t n = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (((unsigned char)text[i] & 0xC0) != 0x80) ++n;
    }
    return n;
}

}  // namespace nbt

#endif
