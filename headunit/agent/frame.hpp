// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_FRAME_HPP
#define NBT_AGENT_FRAME_HPP

// The framing layer of the phone and agent protocol (plan document, "Framing"). The side that dialled opens the
// link with four magic bytes; after that everything is frames:
//
//   u8  type      1 = MSG (UTF-8 JSON), 2 = DATA (binary)
//   u8  flags     bit 0: reserved for a compressed payload    bit 1: last DATA frame of its stream
//   u16 stream    0 for MSG; for DATA, the stream its blob.fetch reply named (1 to 65,535)
//   u32 length    payload bytes, at most 65,536 or the lower maxFrame that `hello` settled on
//       payload
//   u32 check     CRC32 over the header and the payload
//
// Integers are little-endian. A frame that breaks any of this ends the link: there is no resync.
// The check is CRC32 on every link in this version. The MAC that TCP gets after `auth` comes with the transport
// work, and nothing here should face a network until it has.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "protocol_constants.hpp"   // kMaxFramePayload, kMinFramePayload

namespace nbt {

const size_t kFrameHeaderBytes = 8;
const size_t kFrameCheckBytes = 4;

const uint8_t kFrameMsg = 1;
const uint8_t kFrameData = 2;
const uint8_t kFlagCompressed = 1;   // reserved; a frame that has it is refused
const uint8_t kFlagLast = 2;         // DATA only

const size_t kLinkMagicBytes = 4;
extern const uint8_t kLinkMagic[kLinkMagicBytes];   // "NBTL"

struct Frame {
    uint8_t type, flags;
    uint16_t stream;
    std::string payload;

    Frame() : type(0), flags(0), stream(0) {}
    bool last() const { return (flags & kFlagLast) != 0; }
};

// Appends one frame to *out. The caller keeps to the rules above; a frame that breaks them is never built here.
void encodeFrame(uint8_t type, uint8_t flags, uint16_t stream, const void* payload, size_t len, std::string* out);

// Bytes in, frames out. A link hands over whatever it read, in pieces of any size.
class FrameDecoder {
public:
    FrameDecoder() : maxPayload_(kMaxFramePayload), start_(0), failed_(false) {}

    // The largest payload accepted from now on: the link's maxFrame once `hello` has set it.
    void setMaxPayload(uint32_t bytes) { maxPayload_ = bytes; }

    void feed(const void* data, size_t len);

    enum Result {
        kFrame,      // *out holds the next frame
        kNeedMore,   // no whole frame is buffered
        kError       // the bytes are not a frame; *error says why. The decoder is finished
    };
    Result next(Frame* out, std::string* error);

    // Bytes fed and not yet handed out as frames.
    size_t buffered() const { return buffer_.size() - start_; }

private:
    uint32_t maxPayload_;
    std::string buffer_;
    size_t start_;
    bool failed_;
};

}  // namespace nbt

#endif
