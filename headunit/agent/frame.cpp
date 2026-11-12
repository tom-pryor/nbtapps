// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The frame codec: see frame.hpp.

#include "frame.hpp"

#include <stdio.h>

#include "inflate.hpp"

namespace nbt {

const uint8_t kLinkMagic[kLinkMagicBytes] = {'N', 'B', 'T', 'L'};

namespace {

void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

std::string number(unsigned long value) {
    char text[32];
    snprintf(text, sizeof text, "%lu", value);
    return text;
}

}  // namespace

void encodeFrame(uint8_t type, uint8_t flags, uint16_t stream, const void* payload, size_t len, std::string* out) {
    uint8_t header[kFrameHeaderBytes];
    header[0] = type;
    header[1] = flags;
    header[2] = (uint8_t)stream;
    header[3] = (uint8_t)(stream >> 8);
    put32(header + 4, (uint32_t)len);
    uint32_t crc = crc32(0, header, sizeof header);
    crc = crc32(crc, payload, len);
    uint8_t check[kFrameCheckBytes];
    put32(check, crc);
    out->append((const char*)header, sizeof header);
    out->append((const char*)payload, len);
    out->append((const char*)check, sizeof check);
}

void FrameDecoder::feed(const void* data, size_t len) {
    if (failed_) return;
    // What was handed out already is dropped once it is most of the buffer, so the buffer stays about one frame.
    if (start_ > 0 && start_ >= buffer_.size() / 2) {
        buffer_.erase(0, start_);
        start_ = 0;
    }
    buffer_.append((const char*)data, len);
}

FrameDecoder::Result FrameDecoder::next(Frame* out, std::string* error) {
    if (failed_) {
        if (error != 0) *error = "the link has already failed";
        return kError;
    }
    if (buffered() < kFrameHeaderBytes) return kNeedMore;
    const uint8_t* p = (const uint8_t*)buffer_.data() + start_;
    uint8_t type = p[0], flags = p[1];
    uint16_t stream = (uint16_t)(p[2] | (p[3] << 8));
    uint32_t length = get32(p + 4);

    // The header is judged before its payload is waited for, so a wrong length is refused at once and never
    // buffered.
    std::string why;
    if (type != kFrameMsg && type != kFrameData) {
        why = "unknown frame type " + number(type);
    } else if ((flags & kFlagCompressed) != 0) {
        why = "a compressed payload, which this version does not have";
    } else if ((flags & ~(kFlagCompressed | kFlagLast)) != 0) {
        why = "unknown frame flags " + number(flags);
    } else if (type == kFrameMsg && flags != 0) {
        why = "a MSG frame with flags";
    } else if (type == kFrameMsg && stream != 0) {
        why = "a MSG frame with a stream number";
    } else if (type == kFrameData && stream == 0) {
        why = "a DATA frame with no stream number";
    } else if (length > maxPayload_) {
        why = "a frame of " + number(length) + " bytes; the limit is " + number(maxPayload_);
    }
    if (!why.empty()) {
        failed_ = true;
        if (error != 0) *error = why;
        return kError;
    }
    size_t whole = kFrameHeaderBytes + (size_t)length + kFrameCheckBytes;
    if (buffered() < whole) return kNeedMore;
    if (crc32(0, p, kFrameHeaderBytes + length) != get32(p + kFrameHeaderBytes + length)) {
        failed_ = true;
        if (error != 0) *error = "a frame whose check value is wrong";
        return kError;
    }
    out->type = type;
    out->flags = flags;
    out->stream = stream;
    out->payload.assign((const char*)p + kFrameHeaderBytes, length);
    start_ += whole;
    return kFrame;
}

}  // namespace nbt
