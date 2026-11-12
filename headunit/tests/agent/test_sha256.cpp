// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/sha256.cpp and agent/hex.hpp: the FIPS 180-4 example digests, every message length from 0 to
// 300 bytes against Python's hashlib, input fed in uneven pieces, and the RFC 4231 HMAC-SHA256 cases.

#include "../../agent/hex.hpp"
#include "../../agent/sha256.hpp"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

using namespace nbt;

static std::string digest(const std::string& message) {
    uint8_t out[kSha256Bytes];
    sha256(message.data(), message.size(), out);
    return toHex(out, sizeof out);
}

static std::string mac(const std::string& key, const std::string& message) {
    uint8_t out[kSha256Bytes];
    hmacSha256(key.data(), key.size(), message.data(), message.size(), out);
    return toHex(out, sizeof out);
}

int main() {
    // 1. FIPS 180-4 examples: empty, one block, two blocks, and a million times "a".
    assert(digest("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(digest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    assert(digest("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnop"
                  "qrstnopqrstu") == "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    assert(digest(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    // 2. Every length from 0 to 300, which crosses each padding case several times. The expected value is a hash
    //    over all 301 digests, computed with Python's hashlib:
    //      data = bytes((i * 7 + 3) & 0xff for i in range(1000)); acc = bytes(32)
    //      for n in range(301): acc = sha256(acc + sha256(data[:n]).digest()).digest()
    std::vector<uint8_t> data(1000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = (uint8_t)(i * 7 + 3);
    uint8_t acc[kSha256Bytes];
    memset(acc, 0, sizeof acc);
    for (size_t n = 0; n <= 300; ++n) {
        uint8_t one[kSha256Bytes];
        sha256(&data[0], n, one);
        Sha256 chain;
        chain.update(acc, sizeof acc);
        chain.update(one, sizeof one);
        chain.finish(acc);
    }
    assert(toHex(acc, sizeof acc) == "a1ffdcdd67ac09feb460db0e221bebad6abc7cf4e8d23ee7aa25f89363992757");

    // 3. The same bytes in pieces of every size from 1 to 130 give the same digest as in one piece.
    uint8_t whole[kSha256Bytes];
    sha256(&data[0], data.size(), whole);
    for (size_t piece = 1; piece <= 130; ++piece) {
        Sha256 h;
        for (size_t at = 0; at < data.size(); at += piece) {
            h.update(&data[at], data.size() - at < piece ? data.size() - at : piece);
        }
        h.update(&data[0], 0);
        uint8_t out[kSha256Bytes];
        h.finish(out);
        assert(memcmp(out, whole, sizeof out) == 0);
    }

    // 4. HMAC-SHA256, RFC 4231 test cases 1, 2, 3, 6 and 7, and a key of exactly one block.
    assert(mac(std::string(20, '\x0b'), "Hi There") ==
           "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    assert(mac("Jefe", "what do ya want for nothing?") ==
           "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    assert(mac(std::string(20, '\xaa'), std::string(50, '\xdd')) ==
           "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
    assert(mac(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First") ==
           "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    assert(mac(std::string(131, '\xaa'),
               "This is a test using a larger than block-size key and a larger than block-size data. The key needs to "
               "be hashed before being used by the HMAC algorithm.") ==
           "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
    std::string blockKey;
    for (int i = 0; i < 64; ++i) blockKey += (char)i;
    assert(mac(blockKey, "key of exactly one block") ==
           "9b56baaf04d2ab336830b1e8a0a14bade386e94a82ac8fd7837b72054396ba26");

    // The streaming form, fed in two pieces, matches the one-shot form.
    HmacSha256 streamed("Jefe", 4);
    streamed.update("what do ya want ", 16);
    streamed.update("for nothing?", 12);
    uint8_t tag[kSha256Bytes];
    streamed.finish(tag);
    assert(toHex(tag, sizeof tag) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // 5. Constant-time comparison.
    uint8_t other[kSha256Bytes];
    memcpy(other, tag, sizeof other);
    assert(constantTimeEqual(tag, other, sizeof tag));
    other[31] ^= 1;
    assert(!constantTimeEqual(tag, other, sizeof tag));
    other[31] ^= 1;
    other[0] ^= 0x80;
    assert(!constantTimeEqual(tag, other, sizeof tag));
    assert(constantTimeEqual(tag, other, 0));

    // 6. Hex: lowercase only, exact length.
    uint8_t raw[4];
    assert(fromHex("00ff10ab", raw, 4) && raw[0] == 0x00 && raw[1] == 0xff && raw[2] == 0x10 && raw[3] == 0xab);
    assert(toHex(raw, 4) == "00ff10ab" && toHex(raw, 0) == "");
    assert(!fromHex("00FF10AB", raw, 4) && !fromHex("00ff10a", raw, 4) && !fromHex("00ff10abcd", raw, 4));
    assert(!fromHex("00ff10ag", raw, 4) && !fromHex(" 0ff10ab", raw, 4) && !fromHex("0x ff10ab", raw, 4));
    assert(fromHex("", raw, 0));

    printf("test_sha256: ok\n");
    return 0;
}
