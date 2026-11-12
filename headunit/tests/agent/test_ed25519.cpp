// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/ed25519.cpp: the RFC 8032 test vectors, a signature made by tools/nbtpkg.py with the test
// package key (so the agent and the package tool agree), and what must be refused: any changed bit, another key,
// and a signature whose second half was shifted by the group order.

#include "../../agent/ed25519.hpp"
#include "../../agent/hex.hpp"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <string>

using namespace nbt;

struct Case {
    const char* publicKey;
    const char* messageHex;
    const char* signature;
};

static bool verify(const std::string& publicKey, const std::string& message, const std::string& signature) {
    uint8_t key[kEd25519KeyBytes], sig[kEd25519SignatureBytes];
    assert(fromHex(publicKey, key, sizeof key) && fromHex(signature, sig, sizeof sig));
    return ed25519Verify(key, message.data(), message.size(), sig);
}

static std::string bytesOf(const std::string& hex) {
    if (hex.empty()) return std::string();
    std::string out(hex.size() / 2, '\0');
    assert(fromHex(hex, (uint8_t*)&out[0], out.size()));
    return out;
}

int main() {
    // 1. RFC 8032, section 7.1, tests 1 to 3: messages of 0, 1 and 2 bytes.
    const Case rfc[] = {
        {"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
         "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
         "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
         "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    };
    for (size_t i = 0; i < sizeof rfc / sizeof rfc[0]; ++i) {
        assert(verify(rfc[i].publicKey, bytesOf(rfc[i].messageHex), rfc[i].signature));
        // Each key refuses the other vectors' signatures.
        for (size_t j = 0; j < sizeof rfc / sizeof rfc[0]; ++j) {
            if (i != j) assert(!verify(rfc[i].publicKey, bytesOf(rfc[j].messageHex), rfc[j].signature));
        }
    }

    // 2. A signature from tools/nbtpkg.py (Python's `cryptography`) with tests/keys/test-package.key, over
    //    b"NBTApp test message\n" * 40.
    const std::string testKey = "18680457e3b23112c2f64935bdf852c263df25880d99ece34561c01c76a9758d";
    const std::string testSignature = "2c4f02d34a78e7d64da68141d985e4ae274f02571fc0d616bba3e2a323bbb797"
                                      "c194dcf82327b0ba55b73b7560b402619487734f02889491c0da9e3439e6d30a";
    std::string message;
    for (int i = 0; i < 40; ++i) message += "NBTApp test message\n";
    assert(message.size() == 800 && verify(testKey, message, testSignature));

    // 3. Any single changed bit is refused: in the message, in the signature, in the key.
    for (size_t at = 0; at < message.size(); at += 37) {
        std::string changed = message;
        changed[at] ^= 0x01;
        assert(!verify(testKey, changed, testSignature));
    }
    assert(!verify(testKey, message + "x", testSignature) && !verify(testKey, message.substr(1), testSignature));
    assert(!verify(testKey, "", testSignature));
    uint8_t key[kEd25519KeyBytes], sig[kEd25519SignatureBytes];
    assert(fromHex(testKey, key, sizeof key) && fromHex(testSignature, sig, sizeof sig));
    for (size_t byte = 0; byte < sizeof sig; ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            sig[byte] ^= (uint8_t)(1 << bit);
            assert(!ed25519Verify(key, message.data(), message.size(), sig));
            sig[byte] ^= (uint8_t)(1 << bit);
        }
    }
    for (size_t byte = 0; byte < sizeof key; ++byte) {
        key[byte] ^= 0x04;
        assert(!ed25519Verify(key, message.data(), message.size(), sig));
        key[byte] ^= 0x04;
    }
    assert(ed25519Verify(key, message.data(), message.size(), sig));

    // 4. Malleability: adding the group order to the second half gives a signature the bare curve equation still
    //    accepts. It must be refused, so one package cannot circulate under two signatures.
    static const uint8_t order[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                      0xa2, 0xde, 0xf9, 0xde, 0x14, 0,    0,    0,    0,    0,    0,
                                      0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};
    uint8_t shifted[kEd25519SignatureBytes];
    memcpy(shifted, sig, sizeof shifted);
    unsigned carry = 0;
    for (int i = 0; i < 32; ++i) {
        unsigned sum = shifted[32 + i] + order[i] + carry;
        shifted[32 + i] = (uint8_t)sum;
        carry = sum >> 8;
    }
    assert(carry == 0);   // it still fits in 32 bytes, so only the range check can tell it apart
    assert(!ed25519Verify(key, message.data(), message.size(), shifted));
    // The order itself and all-ones as the second half are out of range too.
    memcpy(shifted + 32, order, 32);
    assert(!ed25519Verify(key, message.data(), message.size(), shifted));
    memset(shifted + 32, 0xff, 32);
    assert(!ed25519Verify(key, message.data(), message.size(), shifted));

    // 5. Degenerate inputs are refused, not crashed on: an all-zero signature and an all-zero key.
    memset(shifted, 0, sizeof shifted);
    assert(!ed25519Verify(key, message.data(), message.size(), shifted));
    uint8_t zeroKey[kEd25519KeyBytes];
    memset(zeroKey, 0, sizeof zeroKey);
    assert(!ed25519Verify(zeroKey, message.data(), message.size(), sig));

    printf("test_ed25519: ok\n");
    return 0;
}
