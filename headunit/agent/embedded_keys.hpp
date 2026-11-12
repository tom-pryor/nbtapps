// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_EMBEDDED_KEYS_HPP
#define NBT_AGENT_EMBEDDED_KEYS_HPP

// The package key built into the agent: the official source's (plan document, "Integrity and trust"). Only
// packages signed by a key listed here install, and only such a key may sign the reserved org.nbtapp. ids, the
// agent's own package among them. There is one key and no spare.
//
// The host tests add the test key at run time (nbtagent --test-key, compiled in only with NBT_TEST_KEYS); that
// key's private half is public, so it must never be listed here. tests/agent/test_embedded_keys.cpp holds that,
// and tools/check_agent.py looks for the key below in the nbtagent that goes to a head unit.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdint.h>
#include <string.h>

#include <vector>

#include "package.hpp"

namespace nbt {

// nbtapps-official-package, fingerprint d041 6126 bb84 2c92: the 32 bytes of keys/package.pub in the catalogue
// repo, whose private half signs every package of the official catalogue. tools/agent_pkg.py (for
// tools/check_agent.py and for a release) and the catalogue repo's tests read this table as text: one
// `const uint8_t k...[32] = { 0x.., ... };` to a key.
const uint8_t kOfficialPackageKey[32] = {
    0x9e, 0xe0, 0xf6, 0x38, 0x4e, 0xe2, 0x9c, 0xdb, 0x6d, 0xb2, 0x6b, 0xd1, 0x35, 0xd0, 0x39, 0x17,
    0x22, 0x13, 0x4f, 0x84, 0x9e, 0x9e, 0x42, 0xab, 0x98, 0x2c, 0xf9, 0x3a, 0xfb, 0x13, 0x05, 0x1a,
};

// Adds the built-in keys to *out, each with `embedded` set.
inline void addEmbeddedKeys(std::vector<TrustedKey>* out) {
    TrustedKey key;
    memcpy(key.key, kOfficialPackageKey, sizeof key.key);
    key.name = "NBTApps";
    key.embedded = true;
    out->push_back(key);
}

}  // namespace nbt

#endif
