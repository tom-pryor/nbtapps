// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/inflate.cpp: raw deflate streams made with zlib itself come back intact however the input and
// the output are cut up; a stream that ends early, is damaged, or has bytes after its end is told apart; CRC32
// matches the standard check value. And the Deflater, which files.fetch sends a file with: what it makes in pieces
// zlib inflates whole, and an Inflater too.

#include "../../agent/inflate.hpp"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include <string>
#include <vector>

using namespace nbt;

// A raw deflate stream (no zlib header), as a zip entry holds.
static std::string deflateRaw(const std::string& plain, int level) {
    z_stream z;
    memset(&z, 0, sizeof z);
    assert(deflateInit2(&z, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::vector<unsigned char> out(deflateBound(&z, plain.size()) + 16);
    z.next_in = (Bytef*)plain.data();
    z.avail_in = (uInt)plain.size();
    z.next_out = &out[0];
    z.avail_out = (uInt)out.size();
    assert(deflate(&z, Z_FINISH) == Z_STREAM_END);
    std::string packed((const char*)&out[0], out.size() - z.avail_out);
    deflateEnd(&z);
    return packed;
}

// Runs a whole stream through an Inflater, feeding `inPiece` bytes at a time into a buffer of `outPiece` bytes.
// Returns the last result; *leftover is the input not consumed when the stream ended.
static Inflater::Result run(const std::string& packed, size_t inPiece, size_t outPiece, std::string* plain,
                            size_t* leftover) {
    Inflater inflater;
    std::vector<uint8_t> out(outPiece);
    const uint8_t* in = (const uint8_t*)packed.data();
    size_t remaining = packed.size();
    plain->clear();
    for (;;) {
        size_t feed = remaining < inPiece ? remaining : inPiece;
        const uint8_t* p = in;
        size_t n = feed;
        for (;;) {
            size_t produced = 0;
            Inflater::Result r = inflater.inflate(&p, &n, &out[0], out.size(), &produced);
            plain->append((const char*)&out[0], produced);
            if (r == Inflater::kDone || r == Inflater::kError) {
                *leftover = remaining - (feed - n);
                return r;
            }
            if (r == Inflater::kNeedInput) break;
        }
        assert(n == 0);
        in += feed;
        remaining -= feed;
        if (feed == 0) {
            *leftover = 0;
            return Inflater::kNeedInput;   // the input ran out before the stream ended
        }
    }
}

int main() {
    // 1. CRC32: the standard check value, the empty input, and continuing over two pieces.
    assert(crc32(0, "123456789", 9) == 0xcbf43926u);
    assert(crc32(0, "", 0) == 0);
    assert(crc32(crc32(0, "1234", 4), "56789", 5) == 0xcbf43926u);

    // 2. Round trips: empty, tiny, repetitive, and bytes that do not compress; stored blocks (level 0) as well.
    std::vector<std::string> samples;
    samples.push_back("");
    samples.push_back("a");
    samples.push_back("hello, hello, hello, hello, hello\n");
    std::string repetitive;
    for (int i = 0; i < 4000; ++i) repetitive += "option = default value\n";
    samples.push_back(repetitive);
    std::string noise;
    unsigned state = 12345;
    for (int i = 0; i < 70000; ++i) {
        state = state * 1103515245u + 12345u;
        noise += (char)(state >> 16);
    }
    samples.push_back(noise);

    const size_t inPieces[] = {1, 7, 4096, 1u << 20};
    const size_t outPieces[] = {1, 13, 4096, 1u << 20};
    for (size_t s = 0; s < samples.size(); ++s) {
        for (int level = 0; level <= 9; level += 3) {
            std::string packed = deflateRaw(samples[s], level);
            for (size_t i = 0; i < 4; ++i) {
                for (size_t o = 0; o < 4; ++o) {
                    if (samples[s].size() > 10000 && inPieces[i] == 1 && outPieces[o] == 1) continue;   // slow, no gain
                    std::string plain;
                    size_t leftover = 99;
                    assert(run(packed, inPieces[i], outPieces[o], &plain, &leftover) == Inflater::kDone);
                    assert(plain == samples[s] && leftover == 0);
                }
            }
        }
    }

    // 3. Bytes after the end of the stream are left over, not swallowed: the package reader refuses such an entry.
    std::string packed = deflateRaw(repetitive, 9);
    std::string plain;
    size_t leftover = 0;
    assert(run(packed + "XYZ", 1u << 20, 1u << 20, &plain, &leftover) == Inflater::kDone);
    assert(plain == repetitive && leftover == 3);
    assert(run(packed + "X", 5, 64, &plain, &leftover) == Inflater::kDone && plain == repetitive && leftover == 1);

    // 4. A stream cut short never reports its end.
    for (size_t cut = 0; cut < packed.size(); cut += packed.size() / 7 + 1) {
        assert(run(packed.substr(0, cut), 64, 64, &plain, &leftover) == Inflater::kNeedInput);
        assert(plain.size() <= repetitive.size() && repetitive.compare(0, plain.size(), plain) == 0);
    }

    // 5. Damage is an error: a reserved block type, a stored block whose length check fails, and a zlib-wrapped
    //    stream where a raw one belongs.
    assert(run(std::string("\x07", 1), 64, 64, &plain, &leftover) == Inflater::kError);
    assert(run(std::string("\x01\x05\x00\x00\x00hello", 10), 64, 64, &plain, &leftover) == Inflater::kError);
    std::vector<unsigned char> wrapped(compressBound(repetitive.size()));
    uLongf wrappedLen = wrapped.size();
    assert(compress(&wrapped[0], &wrappedLen, (const Bytef*)repetitive.data(), repetitive.size()) == Z_OK);
    assert(run(std::string((const char*)&wrapped[0], wrappedLen), 64, 64, &plain, &leftover) != Inflater::kDone ||
           plain != repetitive);

    // 6. A finished Inflater does not start over.
    Inflater inflater;
    std::string tiny = deflateRaw("abc", 9);
    const uint8_t* in = (const uint8_t*)tiny.data();
    size_t n = tiny.size(), produced = 0;
    uint8_t out[16];
    assert(inflater.inflate(&in, &n, out, sizeof out, &produced) == Inflater::kDone);
    assert(produced == 3 && memcmp(out, "abc", 3) == 0 && n == 0);
    in = (const uint8_t*)tiny.data();
    n = tiny.size();
    assert(inflater.inflate(&in, &n, out, sizeof out, &produced) == Inflater::kError && produced == 0);

    // 7. The Deflater: fed in pieces of any size, with the end given with the last piece or after it, its output is
    //    one raw stream that zlib's own inflate reads back whole, with nothing after it.
    const size_t deflatePieces[] = {1, 1000, 16384, 1u << 20};
    for (size_t s = 0; s < samples.size(); ++s) {
        for (size_t i = 0; i < 4; ++i) {
            if (samples[s].size() > 10000 && deflatePieces[i] == 1) continue;
            for (int endApart = 0; endApart < 2; ++endApart) {
                Deflater deflater;
                std::string made;
                size_t at = 0;
                do {
                    size_t take = samples[s].size() - at < deflatePieces[i] ? samples[s].size() - at : deflatePieces[i];
                    bool last = at + take == samples[s].size();
                    assert(deflater.deflate(samples[s].data() + at, take, last && !endApart, &made));
                    at += take;
                } while (at < samples[s].size());
                if (endApart) assert(deflater.deflate("", 0, true, &made));
                // Finished: nothing more goes in.
                assert(!deflater.deflate("x", 1, true, &made));
                z_stream z;
                memset(&z, 0, sizeof z);
                assert(inflateInit2(&z, -MAX_WBITS) == Z_OK);
                std::vector<unsigned char> back(samples[s].size() + 16);
                z.next_in = (Bytef*)made.data();
                z.avail_in = (uInt)made.size();
                z.next_out = &back[0];
                z.avail_out = (uInt)back.size();
                assert(inflate(&z, Z_FINISH) == Z_STREAM_END && z.avail_in == 0);
                assert(std::string((const char*)&back[0], back.size() - z.avail_out) == samples[s]);
                inflateEnd(&z);
                std::string again;
                assert(run(made, 4096, 4096, &again, &leftover) == Inflater::kDone && again == samples[s] && leftover == 0);
            }
        }
    }
    // Text, as logs are, shrinks several times.
    {
        Deflater deflater;
        std::string made;
        assert(deflater.deflate(repetitive.data(), repetitive.size(), true, &made) && made.size() * 10 < repetitive.size());
    }

    printf("test_inflate: ok\n");
    return 0;
}
