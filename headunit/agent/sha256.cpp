// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// SHA-256 and HMAC-SHA256, written from FIPS 180-4 and RFC 2104. tests/agent/test_sha256.cpp holds the published
// test vectors and a comparison with Python's hashlib over every length from 0 to 300 bytes.

#include "sha256.hpp"

#include <string.h>

namespace nbt {

namespace {

const uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

Sha256::Sha256() : bytes_(0), used_(0) {
    static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(state_, initial, sizeof state_);
}

void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) |
               (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    bytes_ += len;
    if (used_ > 0) {
        size_t take = 64 - used_;
        if (take > len) take = len;
        memcpy(buffer_ + used_, p, take);
        used_ += take;
        p += take;
        len -= take;
        if (used_ < 64) return;
        block(buffer_);
        used_ = 0;
    }
    for (; len >= 64; p += 64, len -= 64) block(p);
    if (len > 0) {
        memcpy(buffer_, p, len);
        used_ = len;
    }
}

void Sha256::finish(uint8_t out[kSha256Bytes]) {
    uint64_t bits = bytes_ * 8;
    // 0x80, zeros up to 56 mod 64, then the message length in bits as a big-endian 64-bit integer.
    uint8_t pad[72];
    size_t padLen = (used_ < 56 ? 56 : 120) - used_;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (int i = 0; i < 8; ++i) pad[padLen + i] = (uint8_t)(bits >> (56 - 8 * i));
    update(pad, padLen + 8);
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = (uint8_t)(state_[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(state_[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(state_[i] >> 8);
        out[i * 4 + 3] = (uint8_t)state_[i];
    }
}

void sha256(const void* data, size_t len, uint8_t out[kSha256Bytes]) {
    Sha256 h;
    h.update(data, len);
    h.finish(out);
}

HmacSha256::HmacSha256(const void* key, size_t keyLen) {
    // A key longer than one block is replaced by its hash; a shorter one is padded with zeros.
    uint8_t block[64];
    memset(block, 0, sizeof block);
    if (keyLen > sizeof block) {
        sha256(key, keyLen, block);
    } else {
        memcpy(block, key, keyLen);
    }
    uint8_t innerPad[64];
    for (size_t i = 0; i < sizeof block; ++i) {
        innerPad[i] = block[i] ^ 0x36;
        outerPad_[i] = block[i] ^ 0x5c;
    }
    inner_.update(innerPad, sizeof innerPad);
}

void HmacSha256::update(const void* data, size_t len) { inner_.update(data, len); }

void HmacSha256::finish(uint8_t out[kSha256Bytes]) {
    uint8_t innerDigest[kSha256Bytes];
    inner_.finish(innerDigest);
    Sha256 outer;
    outer.update(outerPad_, sizeof outerPad_);
    outer.update(innerDigest, sizeof innerDigest);
    outer.finish(out);
}

void hmacSha256(const void* key, size_t keyLen, const void* data, size_t len, uint8_t out[kSha256Bytes]) {
    HmacSha256 mac(key, keyLen);
    mac.update(data, len);
    mac.finish(out);
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

}  // namespace nbt
