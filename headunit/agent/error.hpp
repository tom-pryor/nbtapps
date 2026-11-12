// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_ERROR_HPP
#define NBT_AGENT_ERROR_HPP

// A refusal or a failure, as the protocol reports it: a code from the fixed list in the plan document ("Message
// reference", Plan problems and step errors) and a message in English for the log.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <string>

namespace nbt {

struct Error {
    std::string code;      // "bad-archive", "bad-signature", "io-error", ...; empty when nothing went wrong
    std::string message;

    // Records the first failure only, and returns false so a check can end with `return error->fail(...)`.
    bool fail(const char* failCode, const std::string& failMessage) {
        if (code.empty()) {
            code = failCode;
            message = failMessage;
        }
        return false;
    }
    void clear() {
        code.clear();
        message.clear();
    }
};

}  // namespace nbt

#endif
