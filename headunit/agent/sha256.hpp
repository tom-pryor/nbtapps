// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SHA256_HPP
#define NBT_AGENT_SHA256_HPP

// SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104). The agent hashes every archive, manifest and installed file with
// the first, and the protocol's `auth` and per-frame MAC on TCP use the second.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

namespace nbt {

const size_t kSha256Bytes = 32;

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    // Writes the digest. The object is spent afterwards: make a new one for the next message.
    void finish(uint8_t out[kSha256Bytes]);

private:
    void block(const uint8_t* p);

    uint32_t state_[8];
    uint64_t bytes_;
    uint8_t buffer_[64];
    size_t used_;
};

void sha256(const void* data, size_t len, uint8_t out[kSha256Bytes]);

class HmacSha256 {
public:
    HmacSha256(const void* key, size_t keyLen);
    void update(const void* data, size_t len);
    void finish(uint8_t out[kSha256Bytes]);

private:
    Sha256 inner_;
    uint8_t outerPad_[64];
};

void hmacSha256(const void* key, size_t keyLen, const void* data, size_t len, uint8_t out[kSha256Bytes]);

// Compares without stopping at the first difference, so the time taken says nothing about where a MAC or a proof
// went wrong.
bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len);

}  // namespace nbt

#endif
