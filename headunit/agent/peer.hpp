// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_PEER_HPP
#define NBT_AGENT_PEER_HPP

// One end of a link, at the message layer (plan document, "Messages" and the "Message reference" tab): the magic
// bytes, frames in and out, and the envelope of every message.
//
//   { "id": 7, "method": "job.start", "params": { } }               a request; each side counts its own ids from 1
//   { "id": 7, "result": { } }                                      its one response,
//   { "id": 7, "error": { "code": "busy", "message": "..." } }      or this
//   { "event": "job.progress", "seq": 31, "params": { } }           an event; nothing answers it
//
// Both sides make requests, so the agent and a controller use the same class; what the methods mean is the
// handler's business. What ends the link here, with no answer: wrong magic bytes, a broken frame, a message outside
// the JSON profile, and a message that is none of the four forms.
//
// Nothing here blocks. The owner waits on fd(), calls readable() when there is something to read and flush() when
// wantsWrite() and the link can take more. A handler may close the peer from inside a callback, but must not
// delete it there.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

#include <set>
#include <string>

#include "frame.hpp"
#include "json.hpp"
#include "link.hpp"

namespace nbt {

class Peer;

class PeerHandler {
public:
    virtual ~PeerHandler() {}
    // A request from the other side. `params` is an object, or absent if the request had none. Answer it with
    // respond() or respondError(), now or later.
    virtual void onRequest(Peer* peer, int64_t id, const std::string& method, const json::Value& params) = 0;
    // The answer to a request made with request(). With `ok`, `result` is the result object; otherwise `code` and
    // `message` are the error's.
    virtual void onResponse(Peer* peer, int64_t id, bool ok, const json::Value& result, const std::string& code,
                            const std::string& message) = 0;
    // `seq` is below 0 if the event carried none.
    virtual void onEvent(Peer* peer, const std::string& event, int64_t seq, const json::Value& params) = 0;
    virtual void onData(Peer* peer, uint16_t stream, const std::string& bytes, bool last) = 0;
};

class Peer {
public:
    // Takes over `link`. `dialled` says that this side made the connection, and so sends the magic bytes; the
    // other side expects them.
    Peer(Link* link, bool dialled, PeerHandler* handler);
    ~Peer();

    int fd() const { return link_->fd(); }
    const char* linkKind() const { return link_->kind(); }
    bool open() const { return open_; }
    // Why the link ended; empty while it is open.
    const std::string& closeReason() const { return reason_; }
    bool wantsWrite() const { return open_ && !out_.empty(); }

    // Reads what the link has and hands each whole frame to the handler. True if anything came, a single byte
    // included: the other side is there.
    bool readable();
    // Writes what is queued, as far as the link takes it.
    void flush();

    // The largest payload in either direction, once `hello` has set it.
    void setMaxFrame(uint32_t bytes);
    uint32_t maxFrame() const { return maxFrame_; }

    // Each `params` or `result` is the text of a JSON object. A message that does not fit the link's frame is not
    // sent: request() then returns 0, event() false, and respond() sends the error `internal` in its place.
    int64_t request(const std::string& method, const std::string& params);
    void respond(int64_t id, const std::string& result);
    void respondError(int64_t id, const char* code, const std::string& message);
    bool event(const std::string& name, int64_t seq, const std::string& params);   // seq below 0: none
    void data(uint16_t stream, const void* bytes, size_t len, bool last);

    void close(const std::string& reason);
    // Closes once everything queued has been written: for a last answer before the link goes. From here on
    // nothing more is queued, and nothing that comes is read.
    void closeWhenFlushed(const std::string& reason);
    // True from closeWhenFlushed() until the link has closed.
    bool closing() const { return open_ && closing_; }
    // Bytes queued and not yet taken by the link.
    size_t queued() const { return out_.size(); }
    // The payload of the message that is being handed to the handler: the bytes of its frame, as they came. Empty
    // outside a callback. `auth` covers the bytes of `hello` with it.
    const std::string& incoming() const { return incoming_ != 0 ? *incoming_ : none_; }

    // The owner's own notes about this end of the link.
    void* user;

private:
    Peer(const Peer&);
    Peer& operator=(const Peer&);
    void handleFrame(const Frame& frame);
    void handleMessage(const std::string& text);
    bool sendMessage(const std::string& text);

    Link* link_;
    PeerHandler* handler_;
    FrameDecoder decoder_;
    bool open_;
    bool expectMagic_;
    std::string magic_;        // the magic bytes read so far
    std::string out_;
    uint32_t maxFrame_;
    int64_t nextId_;
    std::set<int64_t> waiting_;   // requests of ours that have no answer yet
    bool closing_;
    std::string closingReason_;
    const std::string* incoming_;
    std::string none_;
    std::string reason_;
};

// The texts of the four message forms, for anything that writes them without a Peer (stored events, tests).
std::string requestText(int64_t id, const std::string& method, const std::string& params);
std::string resultText(int64_t id, const std::string& result);
std::string errorText(int64_t id, const std::string& code, const std::string& message);
std::string eventText(const std::string& name, int64_t seq, const std::string& params);

}  // namespace nbt

#endif
