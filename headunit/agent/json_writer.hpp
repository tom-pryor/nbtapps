// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_JSON_WRITER_HPP
#define NBT_AGENT_JSON_WRITER_HPP

// Writes JSON inside the strict profile that json.hpp reads. Compact, on one line, with members in the order they
// are added.
//
//   json::Writer w;
//   w.beginObject().key("id").string(id).key("files").beginArray();
//   ...
//   w.endArray().endObject();          // w.text() is the result
//
// The agent's records and journal are written with it, and so are the protocol's messages.
//
// The caller keeps to the profile's two rules the writer cannot check cheaply: no key twice in one object, and
// strings that are valid UTF-8. Text from outside (a hook's output) goes through validUtf8 first.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>

namespace nbt {
namespace json {

class Writer {
public:
    Writer() : afterKey_(false) {}

    Writer& beginObject() { return open('{'); }
    Writer& endObject() { return close('}'); }
    Writer& beginArray() { return open('['); }
    Writer& endArray() { return close(']'); }

    Writer& key(const std::string& name) {
        separate();
        quote(name);
        text_ += ':';
        afterKey_ = true;
        return *this;
    }
    Writer& string(const std::string& value) {
        separate();
        quote(value);
        return *this;
    }
    Writer& integer(int64_t value) {
        separate();
        char digits[32];
        snprintf(digits, sizeof digits, "%lld", (long long)value);
        text_ += digits;
        return *this;
    }
    Writer& boolean(bool value) {
        separate();
        text_ += value ? "true" : "false";
        return *this;
    }

    // A value written elsewhere, put in as it is: the text of another Writer, which is inside the profile already.
    Writer& raw(const std::string& json) {
        separate();
        text_ += json;
        return *this;
    }

    const std::string& text() const { return text_; }

private:
    // A comma before every value but the first in its object or array, and none between a key and its value.
    void separate() {
        if (afterKey_) {
            afterKey_ = false;
            return;
        }
        if (!first_.empty()) {
            if (first_.back() == 0) text_ += ',';
            first_.back() = 0;
        }
    }
    Writer& open(char bracket) {
        separate();
        text_ += bracket;
        first_.push_back(1);
        return *this;
    }
    Writer& close(char bracket) {
        text_ += bracket;
        first_.pop_back();
        return *this;
    }
    void quote(const std::string& value) {
        static const char hex[] = "0123456789abcdef";
        text_ += '"';
        for (size_t i = 0; i < value.size(); ++i) {
            unsigned char c = (unsigned char)value[i];
            if (c == '"' || c == '\\') {
                text_ += '\\';
                text_ += (char)c;
            } else if (c < 0x20) {
                text_ += "\\u00";
                text_ += hex[c >> 4];
                text_ += hex[c & 15];
            } else {
                text_ += (char)c;
            }
        }
        text_ += '"';
    }

    std::string text_;
    std::vector<char> first_;   // per open object or array: 1 while nothing is written in it yet
    bool afterKey_;
};

}  // namespace json
}  // namespace nbt

#endif
