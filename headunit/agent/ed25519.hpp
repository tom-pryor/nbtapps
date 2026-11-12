// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_ED25519_HPP
#define NBT_AGENT_ED25519_HPP

// Ed25519 signature verification (RFC 8032), on the vendored TweetNaCl. The agent only ever verifies: a package's
// manifest.sig against the package keys it trusts. It holds no private key and makes none.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

namespace nbt {

const size_t kEd25519KeyBytes = 32;
const size_t kEd25519SignatureBytes = 64;

// True if `signature` is the signature of `message` under `publicKey`. A signature whose second half is not below
// the group order is refused, as RFC 8032 requires and as stricter verifiers do, so that every reader of a package
// gives the same answer for it.
bool ed25519Verify(const uint8_t publicKey[kEd25519KeyBytes], const void* message, size_t len,
                   const uint8_t signature[kEd25519SignatureBytes]);

}  // namespace nbt

#endif
