// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Raw inflate and deflate, and CRC32, on zlib: see inflate.hpp.

#include "inflate.hpp"

#include <string.h>
#include <zlib.h>

namespace nbt {

struct Inflater::State {
    z_stream z;
    bool ready;
    bool finished;
};

Inflater::Inflater() : state_(new State) {
    memset(&state_->z, 0, sizeof state_->z);
    // Negative window bits: a raw stream with no zlib header or checksum.
    state_->ready = inflateInit2(&state_->z, -MAX_WBITS) == Z_OK;
    state_->finished = false;
}

Inflater::~Inflater() {
    if (state_->ready) inflateEnd(&state_->z);
    delete state_;
}

Inflater::Result Inflater::inflate(const uint8_t** in, size_t* inLen, uint8_t* out, size_t outCap, size_t* produced) {
    *produced = 0;
    if (!state_->ready || state_->finished) return kError;
    z_stream& z = state_->z;
    // zlib counts in unsigned int; the caller's buffers are far smaller, but cap them all the same.
    const size_t kChunk = 1u << 30;
    size_t feed = *inLen < kChunk ? *inLen : kChunk;
    size_t room = outCap < kChunk ? outCap : kChunk;
    z.next_in = (Bytef*)*in;
    z.avail_in = (uInt)feed;
    z.next_out = out;
    z.avail_out = (uInt)room;
    int rc = ::inflate(&z, Z_NO_FLUSH);
    size_t used = feed - z.avail_in;
    *in += used;
    *inLen -= used;
    *produced = room - z.avail_out;
    if (rc == Z_STREAM_END) {
        state_->finished = true;
        return kDone;
    }
    // Z_BUF_ERROR only says that this call could make no progress: out of input or out of room.
    if (rc != Z_OK && rc != Z_BUF_ERROR) {
        state_->finished = true;
        return kError;
    }
    return z.avail_out == 0 ? kOutputFull : kNeedInput;
}

struct Deflater::State {
    z_stream z;
    bool ready;
    bool finished;
};

Deflater::Deflater() : state_(new State) {
    memset(&state_->z, 0, sizeof state_->z);
    // Negative window bits: a raw stream, as for Inflater. Memory level 8 is zlib's default.
    state_->ready = deflateInit2(&state_->z, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK;
    state_->finished = false;
}

Deflater::~Deflater() {
    if (state_->ready) deflateEnd(&state_->z);
    delete state_;
}

bool Deflater::deflate(const void* in, size_t len, bool finish, std::string* out) {
    if (!state_->ready || state_->finished) return false;
    z_stream& z = state_->z;
    const uint8_t* p = (const uint8_t*)in;
    uint8_t buffer[16384];
    // zlib counts in unsigned int: the input goes in pieces it can count.
    const size_t kChunk = 1u << 30;
    for (;;) {
        size_t feed = len < kChunk ? len : kChunk;
        bool lastPiece = feed == len;
        z.next_in = (Bytef*)p;
        z.avail_in = (uInt)feed;
        int flush = finish && lastPiece ? Z_FINISH : Z_NO_FLUSH;
        int rc = Z_OK;
        do {
            z.next_out = buffer;
            z.avail_out = sizeof buffer;
            rc = ::deflate(&z, flush);
            if (rc == Z_STREAM_ERROR) {
                state_->finished = true;
                return false;
            }
            out->append((const char*)buffer, sizeof buffer - z.avail_out);
        } while (z.avail_out == 0 || (flush == Z_FINISH && rc != Z_STREAM_END));
        p += feed;
        len -= feed;
        if (lastPiece) break;
    }
    if (finish) state_->finished = true;
    return true;
}

uint32_t crc32(uint32_t crc, const void* data, size_t len) {
    const Bytef* p = (const Bytef*)data;
    uLong c = crc;
    while (len > 0) {
        uInt n = len < (1u << 30) ? (uInt)len : (1u << 30);
        c = ::crc32(c, p, n);
        p += n;
        len -= n;
    }
    return (uint32_t)c;
}

}  // namespace nbt
