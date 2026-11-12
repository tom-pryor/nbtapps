// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Ed25519 verification on TweetNaCl: see ed25519.hpp and vendor/tweetnacl/README.

#include "ed25519.hpp"

#include <stdlib.h>
#include <string.h>

#include <vector>

extern "C" {
#include "vendor/tweetnacl/tweetnacl.h"

// TweetNaCl calls this to make keys and nonces for its other functions. The agent uses none of them, so reaching it
// is a bug, and stopping is safer than handing out bytes that are not random.
void randombytes(unsigned char*, unsigned long long) { abort(); }
}

namespace nbt {

namespace {

// The order of the base point, little-endian: 2^252 + 27742317777372353535851937790883648493.
const uint8_t kOrder[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};

// s < order, both little-endian. Neither value is secret, so this need not run in constant time.
bool belowOrder(const uint8_t s[32]) {
    for (int i = 31; i >= 0; --i) {
        if (s[i] < kOrder[i]) return true;
        if (s[i] > kOrder[i]) return false;
    }
    return false;
}

}  // namespace

bool ed25519Verify(const uint8_t publicKey[kEd25519KeyBytes], const void* message, size_t len,
                   const uint8_t signature[kEd25519SignatureBytes]) {
    // TweetNaCl reduces the second half of the signature instead of checking it, which lets a third party turn one
    // valid signature into another for the same message. RFC 8032 (5.1.7) says to refuse those.
    if (!belowOrder(signature + 32)) return false;

    // crypto_sign_open takes the signature and the message as one buffer, and wants a scratch buffer of that size.
    std::vector<unsigned char> sealed(len + kEd25519SignatureBytes);
    std::vector<unsigned char> opened(len + kEd25519SignatureBytes);
    memcpy(&sealed[0], signature, kEd25519SignatureBytes);
    if (len > 0) memcpy(&sealed[kEd25519SignatureBytes], message, len);
    unsigned long long openedLen = 0;
    return crypto_sign_ed25519_tweet_open(&opened[0], &openedLen, &sealed[0], sealed.size(), publicKey) == 0 &&
           openedLen == len;
}

}  // namespace nbt
