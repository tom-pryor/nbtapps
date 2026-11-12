// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_JSON_TEXT_HPP
#define NBT_AGENT_JSON_TEXT_HPP

// A parsed JSON value written out again, compact and with its members in their original order: for a controller
// that shows a reply as it came.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <string>

#include "json.hpp"
#include "json_writer.hpp"

namespace nbt {

inline void writeJson(const json::Value& value, json::Writer* out) {
    if (value.isObject()) {
        out->beginObject();
        for (size_t i = 0; i < value.size(); ++i) {
            out->key(value.key(i));
            writeJson(value.at(i), out);
        }
        out->endObject();
    } else if (value.isArray()) {
        out->beginArray();
        for (size_t i = 0; i < value.size(); ++i) writeJson(value.at(i), out);
        out->endArray();
    } else if (value.isString()) {
        out->string(value.string());
    } else if (value.isInteger()) {
        out->integer(value.integer());
    } else if (value.isBool()) {
        out->boolean(value.boolean());
    } else {
        out->raw("null");
    }
}

inline std::string jsonText(const json::Value& value) {
    json::Writer w;
    writeJson(value, &w);
    return w.text();
}

}  // namespace nbt

#endif
