// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_TESTS_AGENT_TEST_UTIL_HPP
#define NBT_TESTS_AGENT_TEST_UTIL_HPP

// Small helpers shared by the agent's host tests. CHECK is assert with a message that survives -DNDEBUG and prints
// the values a failure needs.

#include <stdio.h>
#include <stdlib.h>

#include <string>

#include "../../agent/hex.hpp"
#include "../../agent/package.hpp"

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

// As CHECK, with a line of context: which package or which case.
#define CHECK_IN(context, condition)                                                  \
    do {                                                                              \
        if (!(condition)) {                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n  in: %s\n", __FILE__, __LINE__, #condition, std::string(context).c_str()); \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

namespace testutil {

inline std::string readFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == 0) {
        fprintf(stderr, "cannot open %s\n", path.c_str());
        exit(1);
    }
    std::string data;
    char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) data.append(buffer, n);
    fclose(f);
    return data;
}

inline nbt::TrustedKey key(const std::string& hex, const char* name, bool embedded) {
    nbt::TrustedKey k;
    if (!nbt::fromHex(hex, k.key, sizeof k.key)) {
        fprintf(stderr, "not a key: %s\n", hex.c_str());
        exit(1);
    }
    k.name = name;
    k.embedded = embedded;
    return k;
}

class StringSink : public nbt::ByteSink {
public:
    virtual bool write(const void* data, size_t len) {
        text.append((const char*)data, len);
        return true;
    }
    std::string text;
};

}  // namespace testutil

#endif
