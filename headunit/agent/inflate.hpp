// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_INFLATE_HPP
#define NBT_AGENT_INFLATE_HPP

// Raw deflate decompression and CRC32 for the package reader, and raw deflate compression for the files a
// controller pulls (files.fetch), on top of zlib. The QNX SDP 6.5.0 ships zlib for the
// target (libz.a is linked in, so nothing depends on a libz on the head unit); host tests use the host's.
// zlib.h stays out of this header.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace nbt {

// One raw deflate stream, as a zip entry holds it (no zlib or gzip header), fed and drained in pieces.
class Inflater {
public:
    enum Result {
        kNeedInput,    // every input byte was used and the stream has not ended: feed more
        kOutputFull,   // the output buffer is full: call again with room
        kDone,         // the stream ended. Input bytes left over are not part of it
        kError         // the data is not a deflate stream, or it wants a preset dictionary
    };

    Inflater();
    ~Inflater();

    // Reads from *in and writes to out. Advances *in and lowers *inLen by what was read, and sets *produced to the
    // bytes written. After kDone or kError the object is finished.
    Result inflate(const uint8_t** in, size_t* inLen, uint8_t* out, size_t outCap, size_t* produced);

private:
    Inflater(const Inflater&);
    Inflater& operator=(const Inflater&);

    struct State;
    State* state_;
};

// One raw deflate stream made of bytes given in pieces: no zlib or gzip header, as RFC 1951 has it. Level 6, the
// window zlib's largest (32 KiB).
class Deflater {
public:
    Deflater();
    ~Deflater();

    // Compresses `len` bytes and appends what comes out of it so far to *out. With `finish` these are the last
    // bytes: the stream is ended after them, and the object is finished. False if zlib fails; the object is then
    // finished too.
    bool deflate(const void* in, size_t len, bool finish, std::string* out);

private:
    Deflater(const Deflater&);
    Deflater& operator=(const Deflater&);

    struct State;
    State* state_;
};

// The CRC32 of zip and of the protocol's frames on a serial link. Start with 0 and pass the result back in to
// continue over more data.
uint32_t crc32(uint32_t crc, const void* data, size_t len);

}  // namespace nbt

#endif
