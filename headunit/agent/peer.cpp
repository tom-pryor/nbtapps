// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// One end of a link at the message layer: see peer.hpp.

#include "peer.hpp"

#include <string.h>

#include "json_writer.hpp"
#include "utf8.hpp"

namespace nbt {

namespace {

// More than this queued for a side that is not reading means it never will: the link is closed.
const size_t kMaxQueued = 8u << 20;
// Read per call of readable(), so one busy link does not starve the others.
const size_t kReadBudget = 1u << 20;

}  // namespace

std::string requestText(int64_t id, const std::string& method, const std::string& params) {
    json::Writer w;
    w.beginObject().key("id").integer(id).key("method").string(method).key("params").raw(params).endObject();
    return w.text();
}

std::string resultText(int64_t id, const std::string& result) {
    json::Writer w;
    w.beginObject().key("id").integer(id).key("result").raw(result).endObject();
    return w.text();
}

std::string errorText(int64_t id, const std::string& code, const std::string& message) {
    json::Writer w;
    w.beginObject().key("id").integer(id).key("error").beginObject();
    w.key("code").string(code).key("message").string(sanitizeUtf8(message, 0)).endObject().endObject();
    return w.text();
}

std::string eventText(const std::string& name, int64_t seq, const std::string& params) {
    json::Writer w;
    w.beginObject().key("event").string(name);
    if (seq >= 0) w.key("seq").integer(seq);
    w.key("params").raw(params).endObject();
    return w.text();
}

Peer::Peer(Link* link, bool dialled, PeerHandler* handler)
    : user(0), link_(link), handler_(handler), open_(true), expectMagic_(!dialled), maxFrame_(kMaxFramePayload),
      nextId_(1), closing_(false), incoming_(0) {
    if (dialled) out_.assign((const char*)kLinkMagic, kLinkMagicBytes);
}

Peer::~Peer() { delete link_; }

void Peer::close(const std::string& reason) {
    if (!open_) return;
    open_ = false;
    reason_ = reason;
    link_->close();
    out_.clear();
}

void Peer::closeWhenFlushed(const std::string& reason) {
    if (!open_) return;
    closing_ = true;
    closingReason_ = reason;
    flush();
}

void Peer::setMaxFrame(uint32_t bytes) {
    maxFrame_ = bytes;
    decoder_.setMaxPayload(bytes);
}

void Peer::flush() {
    while (open_ && !out_.empty()) {
        long n = link_->write(out_.data(), out_.size());
        if (n < 0) {
            close("the link ended");
            return;
        }
        if (n == 0) break;
        out_.erase(0, (size_t)n);
    }
    if (open_ && closing_ && out_.empty()) close(closingReason_);
}

bool Peer::readable() {
    char buffer[16384];
    size_t budget = kReadBudget;
    bool heard = false;
    while (open_ && !closing_ && budget > 0) {
        long n = link_->read(buffer, sizeof buffer);
        if (n < 0) {
            close("the other side closed it");
            return heard;
        }
        if (n == 0) return heard;
        heard = true;
        budget -= (size_t)n < budget ? (size_t)n : budget;
        const char* p = buffer;
        size_t len = (size_t)n;
        if (expectMagic_) {
            size_t take = kLinkMagicBytes - magic_.size();
            if (take > len) take = len;
            magic_.append(p, take);
            p += take;
            len -= take;
            if (memcmp(magic_.data(), kLinkMagic, magic_.size()) != 0) {
                close("the link did not open with the magic bytes");
                return heard;
            }
            if (magic_.size() < kLinkMagicBytes) continue;
            expectMagic_ = false;
        }
        decoder_.feed(p, len);
        Frame frame;
        std::string why;
        while (open_ && !closing_) {
            FrameDecoder::Result result = decoder_.next(&frame, &why);
            if (result == FrameDecoder::kNeedMore) break;
            if (result == FrameDecoder::kError) {
                close("framing error: " + why);
                return heard;
            }
            handleFrame(frame);
        }
    }
    return heard;
}

void Peer::handleFrame(const Frame& frame) {
    if (frame.type == kFrameData) {
        handler_->onData(this, frame.stream, frame.payload, frame.last());
        return;
    }
    incoming_ = &frame.payload;
    handleMessage(frame.payload);
    incoming_ = 0;
}

void Peer::handleMessage(const std::string& text) {
    json::Document doc;
    std::string why;
    if (!doc.parse(text, &why)) {
        close("a message outside the JSON profile: " + why);
        return;
    }
    json::Value root = doc.root();
    json::Value id = root.find("id"), method = root.find("method"), event = root.find("event");
    json::Value params = root.find("params");

    if (method.present()) {
        // Without a usable id there is nothing to answer to.
        if (!method.isString() || !id.isInteger() || id.integer() < 1) {
            close("a request with no usable id or method");
            return;
        }
        if (params.present() && !params.isObject()) {
            respondError(id.integer(), "bad-request", "params is not an object");
            return;
        }
        handler_->onRequest(this, id.integer(), method.string(), params);
        return;
    }
    if (event.present()) {
        if (!event.isString() || (params.present() && !params.isObject())) {
            close("an event with no usable name or params");
            return;
        }
        json::Value seq = root.find("seq");
        handler_->onEvent(this, event.string(), seq.isInteger() && seq.integer() >= 0 ? seq.integer() : -1, params);
        return;
    }
    json::Value result = root.find("result"), error = root.find("error");
    if (id.isInteger() && (result.isObject() || error.isObject()) && !(result.present() && error.present())) {
        std::set<int64_t>::iterator waiting = waiting_.find(id.integer());
        if (waiting == waiting_.end()) return;   // an answer to nothing we asked: ignored
        waiting_.erase(waiting);
        if (result.present()) {
            handler_->onResponse(this, id.integer(), true, result, "", "");
        } else {
            json::Value code = error.find("code"), message = error.find("message");
            if (!code.isString()) {
                close("an error with no code");
                return;
            }
            handler_->onResponse(this, id.integer(), false, json::Value(), code.string(),
                                 message.isString() ? message.string() : std::string());
        }
        return;
    }
    close("a message that is not a request, a response or an event");
}

bool Peer::sendMessage(const std::string& text) {
    if (!open_ || closing_ || text.size() > maxFrame_) return false;
    encodeFrame(kFrameMsg, 0, 0, text.data(), text.size(), &out_);
    flush();
    if (open_ && out_.size() > kMaxQueued) close("the other side stopped reading");
    return true;
}

int64_t Peer::request(const std::string& method, const std::string& params) {
    int64_t id = nextId_;
    if (!sendMessage(requestText(id, method, params))) return 0;
    ++nextId_;
    waiting_.insert(id);
    return id;
}

void Peer::respond(int64_t id, const std::string& result) {
    if (!open_ || closing_) return;
    std::string text = resultText(id, result);
    if (text.size() > maxFrame_) {
        respondError(id, "internal", "the reply does not fit in a frame on this link");
        return;
    }
    sendMessage(text);
}

void Peer::respondError(int64_t id, const char* code, const std::string& message) {
    std::string text = errorText(id, code, message);
    // An error is short unless its message is long, and the code is the part that matters.
    if (text.size() > maxFrame_) text = errorText(id, code, "");
    sendMessage(text);
}

bool Peer::event(const std::string& name, int64_t seq, const std::string& params) { return sendMessage(eventText(name, seq, params)); }

void Peer::data(uint16_t stream, const void* bytes, size_t len, bool last) {
    if (!open_ || closing_ || len > maxFrame_) return;
    encodeFrame(kFrameData, last ? kFlagLast : 0, stream, bytes, len, &out_);
    flush();
}

}  // namespace nbt
