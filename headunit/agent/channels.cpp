// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The channels that are open: see channels.hpp.

#include "channels.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>

#include "fsutil.hpp"
#include "json_writer.hpp"
#include "protocol_constants.hpp"

namespace nbt {

namespace {

const size_t kReadBytes = 2 * (2 + kChannelMaxMessage);   // read from a program at a time
const unsigned kMostCredit = 1u << 20;                     // what the agent counts of a phone's credit

std::string number(uint64_t value) {
    char text[32];
    snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    return text;
}

// The length of the message at the front of a program's bytes, or 0 while two bytes have not come.
size_t frontLength(const std::string& in) {
    if (in.size() < 2) return 0;
    return ((size_t)(unsigned char)in[0] << 8) | (unsigned char)in[1];
}

// A whole message is at the front of a program's bytes.
bool wholeMessage(const std::string& in) {
    size_t length = frontLength(in);
    return length != 0 && in.size() >= 2 + length;
}

}  // namespace

bool fitsSocketAddress(const std::string& path) {
    struct sockaddr_un address;
    return !path.empty() && path.size() < sizeof address.sun_path;
}

std::string channelsDir(const std::string& tmpDir) { return tmpDir + "/channels"; }

bool makeChannelsDir(const std::string& tmpDir, std::string* error) {
    std::string dir = channelsDir(tmpDir);
    if (!fs::makeDirs(dir, error)) return false;
    if (chmod(dir.c_str(), 0700) != 0) {
        if (error != 0) *error = dir + ": " + strerror(errno);
        return false;
    }
    return true;
}

namespace {

class SocketEnd : public ProgramEnd {
public:
    explicit SocketEnd(const std::string& path) : path_(path), fd_(-1), pending_(false) {}
    virtual ~SocketEnd() {
        if (fd_ >= 0) ::close(fd_);
    }

    virtual Connect connect(std::string* why) {
        if (pending_) return finish(why);
        if (!fitsSocketAddress(path_)) {
            *why = "the socket's path is too long for a local socket: " + path_;
            return kRefused;
        }
        fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) {
            *why = std::string("no socket: ") + strerror(errno);
            return kRefused;
        }
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
        // Not into a program the agent or a hook starts.
        fcntl(fd_, F_SETFD, fcntl(fd_, F_GETFD, 0) | FD_CLOEXEC);
        struct sockaddr_un address;
        memset(&address, 0, sizeof address);
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
        if (::connect(fd_, (struct sockaddr*)&address, sizeof address) == 0) return kConnected;
        int e = errno;
        if (e == EINPROGRESS || e == EINTR) {
            pending_ = true;
            return kWaiting;
        }
        ::close(fd_);
        fd_ = -1;
        // The program's queue of connections is full: a moment later.
        if (e == EAGAIN) return kWaiting;
        *why = e == ENOENT ? "there is no socket at " + path_
                           : e == ECONNREFUSED ? "nothing listens at " + path_ : path_ + ": " + strerror(e);
        return kRefused;
    }

    virtual int fd() const { return fd_; }

    virtual bool read(std::string* in, size_t most) {
        char buffer[16384];
        size_t want = most < sizeof buffer ? most : sizeof buffer;
        if (want == 0) return true;
        ssize_t n = ::read(fd_, buffer, want);
        if (n > 0) {
            in->append(buffer, (size_t)n);
            return true;
        }
        return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    }

    virtual long write(const char* bytes, size_t len) {
        ssize_t n = ::write(fd_, bytes, len);
        if (n >= 0) return (long)n;
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    }

private:
    // A connect that is under way: done once the socket is writable, with its error.
    Connect finish(std::string* why) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd_, &writable);
        struct timeval none;
        none.tv_sec = none.tv_usec = 0;
        if (select(fd_ + 1, 0, &writable, 0, &none) <= 0) return kWaiting;
        int error = 0;
        socklen_t length = sizeof error;
        if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) != 0) error = errno;
        pending_ = false;
        if (error == 0) return kConnected;
        ::close(fd_);
        fd_ = -1;
        *why = path_ + ": " + strerror(error);
        return kRefused;
    }

    std::string path_;
    int fd_;
    bool pending_;
};

}  // namespace

ProgramEnd* SocketEnds::make(const std::string& socketPath) { return new SocketEnd(socketPath); }

std::string channelFrame(const std::string& message) {
    std::string out;
    out.reserve(2 + message.size());
    out.push_back((char)((message.size() >> 8) & 0xFF));
    out.push_back((char)(message.size() & 0xFF));
    out.append(message);
    return out;
}

Channels::Channels(ChannelHost* host, ProgramEnds* ends) : host_(host), ends_(ends) {}

Channels::~Channels() {
    for (size_t i = 0; i < all_.size(); ++i) {
        delete all_[i]->end;
        delete all_[i];
    }
}

Channel* Channels::add(Channel* channel, const std::string& firstLine) {
    channel->connecting = true;
    channel->closed = false;
    channel->openedMs = channel->retryAtMs = host_->channelNowMs();
    channel->out = firstLine + "\n";
    channel->outIsMessage = false;
    all_.push_back(channel);
    connect(channel);
    return channel;
}

std::vector<Channel*> Channels::on(const Peer* peer) const {
    std::vector<Channel*> out;
    for (size_t i = 0; i < all_.size(); ++i) {
        if (!all_[i]->closed && all_[i]->peer == peer) out.push_back(all_[i]);
    }
    // In the order of their numbers, which is the order they were given out in until the numbers start again.
    for (size_t i = 1; i < out.size(); ++i) {
        for (size_t k = i; k > 0 && out[k - 1]->stream > out[k]->stream; --k) std::swap(out[k - 1], out[k]);
    }
    return out;
}

unsigned Channels::count(const std::string& package, const std::string& name) const {
    unsigned n = 0;
    for (size_t i = 0; i < all_.size(); ++i) {
        const Channel* c = all_[i];
        if (!c->closed && c->package == package && c->name == name) ++n;
    }
    return n;
}

bool Channels::inUse(const Peer* peer, uint16_t stream) const {
    for (size_t i = 0; i < all_.size(); ++i) {
        if (!all_[i]->closed && all_[i]->peer == peer && all_[i]->stream == stream) return true;
    }
    return false;
}

Channel* Channels::find(const Peer* peer, uint16_t stream) {
    for (size_t i = 0; i < all_.size(); ++i) {
        Channel* c = all_[i];
        // One that is connecting has a number the phone has not been told yet.
        if (!c->closed && !c->connecting && c->peer == peer && c->stream == stream) return c;
    }
    return 0;
}

void Channels::connect(Channel* c) {
    std::string why;
    if (ends_ == 0) {
        why = "this agent has no way to reach a program";
    } else {
        if (c->end == 0) c->end = ends_->make(c->socketPath);
        ProgramEnd::Connect state = c->end->connect(&why);
        if (state == ProgramEnd::kConnected) {
            c->connecting = false;
            host_->channelConnected(c);
            return;
        }
        if (state == ProgramEnd::kWaiting) {
            c->retryAtMs = host_->channelNowMs() + kChannelStartRetryMs;
            if (host_->channelNowMs() - c->openedMs < kChannelStartWaitMs) return;
            why = "the program did not take the connection in " + number((uint64_t)(kChannelStartWaitMs / 1000)) + " s";
        }
    }
    // Nothing was said of this number to the phone, so nothing is said of its end either.
    c->closed = true;
    delete c->end;
    c->end = 0;
    host_->channelUnavailable(c, why);
}

void Channels::close(Channel* c, const char* reason, const std::string& why) {
    if (c->closed) return;
    c->closed = true;
    if (!c->connecting && reason != 0 && c->peer->open() && !c->peer->closing()) {
        json::Writer w;
        w.beginObject().key("stream").integer(c->stream).key("reason").string(reason).endObject();
        c->peer->event("channel.close", -1, w.text());
    }
    delete c->end;
    c->end = 0;
    c->waiting.clear();
    c->in.clear();
    c->out.clear();
    host_->channelClosed(c, reason, why);
}

void Channels::closeLink(const Peer* peer, const std::string& why) {
    for (size_t i = 0; i < all_.size(); ++i) {
        if (!all_[i]->closed && all_[i]->peer == peer) close(all_[i], 0, why);
    }
}

void Channels::closePackage(const std::string& package, const std::string& name, const char* reason, const std::string& why) {
    for (size_t i = 0; i < all_.size(); ++i) {
        Channel* c = all_[i];
        if (!c->closed && c->package == package && (name.empty() || c->name == name)) close(c, reason, why);
    }
}

void Channels::data(Peer* peer, uint16_t stream, const std::string& bytes, bool last) {
    Channel* c = find(peer, stream);
    // A close and a message can cross: what comes for a number that is not open is dropped.
    if (c == 0) return;
    if (!last || bytes.empty() || bytes.size() > kChannelMaxMessage) {
        close(c, "protocol", !last ? "a DATA frame with no last flag" : "a message of " + number(bytes.size()) + " bytes");
        return;
    }
    if (c->access == "read") {
        close(c, "protocol", "a message from the phone on a channel it may only read");
        return;
    }
    if (c->fromPhone == 0) {
        close(c, "protocol", "a message over the credit the agent gave");
        return;
    }
    --c->fromPhone;
    c->waiting.push_back(bytes);
}

void Channels::credit(Peer* peer, const json::Value& params) {
    json::Value stream = params.find("stream"), messages = params.find("messages");
    if (!stream.isInteger() || stream.integer() < kChannelFirstStream || stream.integer() > 65535) return;
    Channel* c = find(peer, (uint16_t)stream.integer());
    if (c == 0) return;
    if (!messages.isInteger() || messages.integer() < 1 || messages.integer() > 65535) {
        close(c, "protocol", "a channel.credit with no messages from 1 to 65535");
        return;
    }
    // More than any phone can use is as good as no limit.
    c->toPhone = c->toPhone + (unsigned)messages.integer() > kMostCredit ? kMostCredit : c->toPhone + (unsigned)messages.integer();
}

void Channels::closedByPhone(Peer* peer, const json::Value& params) {
    json::Value stream = params.find("stream");
    if (!stream.isInteger() || stream.integer() < kChannelFirstStream || stream.integer() > 65535) return;
    Channel* c = find(peer, (uint16_t)stream.integer());
    if (c != 0) close(c, 0, "the phone closed it");
}

int Channels::addToSets(fd_set* readable, fd_set* writable, int top) const {
    for (size_t i = 0; i < all_.size(); ++i) {
        const Channel* c = all_[i];
        if (c->closed || c->end == 0) continue;
        int fd = c->end->fd();
        if (fd < 0 || fd >= FD_SETSIZE) continue;
        if (c->connecting) {
            FD_SET(fd, writable);
        } else {
            if (mayRead(c)) FD_SET(fd, readable);
            if (!c->programClosed && (!c->out.empty() || !c->waiting.empty())) FD_SET(fd, writable);
        }
        if (fd > top) top = fd;
    }
    return top;
}

bool Channels::connecting() const {
    for (size_t i = 0; i < all_.size(); ++i) {
        if (!all_[i]->closed && all_[i]->connecting) return true;
    }
    return false;
}

void Channels::serve() {
    int64_t now = host_->channelNowMs();
    // The entries of this moment: connecting may answer, and the answer adds nothing here.
    size_t n = all_.size();
    for (size_t i = 0; i < n; ++i) {
        Channel* c = all_[i];
        if (c->closed) continue;
        if (c->connecting) {
            if (now >= c->retryAtMs) connect(c);
            continue;
        }
        writeTo(c);
        if (!c->closed) readFrom(c);
        // What it had to say before it went is sent first, by the turns of its link (Agent::pumpLink).
        if (!c->closed && c->programClosed && !wholeMessage(c->in)) close(c, "closed", "the program closed its end");
    }
}

void Channels::writeTo(Channel* c) {
    for (;;) {
        if (c->out.empty() && !c->waiting.empty()) {
            c->out = channelFrame(c->waiting.front());
            c->outIsMessage = true;
        }
        if (c->out.empty() || c->programClosed) return;
        long n = c->end->write(c->out.data(), c->out.size());
        if (n < 0) {
            c->programClosed = true;
            return;
        }
        if (n == 0) return;
        c->out.erase(0, (size_t)n);
        if (c->out.empty() && c->outIsMessage) {
            c->waiting.pop_front();
            c->outIsMessage = false;
            ++c->taken;
            grant(c);
        }
    }
}

// The phone may send as many again as the program has taken: half a credit at a time, or what there is once the
// program has taken everything, so that a phone that waits for the whole of its credit back gets it.
void Channels::grant(Channel* c) {
    if (c->taken == 0 || (c->taken < kChannelCredit / 2 && !(c->waiting.empty() && c->out.empty()))) return;
    if (!c->peer->open() || c->peer->closing()) return;
    json::Writer w;
    w.beginObject().key("stream").integer(c->stream).key("messages").integer(c->taken).endObject();
    if (!c->peer->event("channel.credit", -1, w.text())) return;
    c->fromPhone += c->taken;
    c->taken = 0;
}

bool Channels::mayRead(const Channel* c) const {
    return !c->closed && !c->connecting && !c->programClosed && c->toPhone > 0 && !wholeMessage(c->in) && c->peer->open() &&
           !c->peer->closing() && c->peer->queued() < 2 * (size_t)c->peer->maxFrame();
}

void Channels::readFrom(Channel* c) {
    // Only while the phone has granted credit and its link takes more: a program that sends faster than the phone
    // takes waits in its own socket.
    if (!mayRead(c)) return;
    if (!c->end->read(&c->in, kReadBytes)) c->programClosed = true;
    // Each length as it comes: one the protocol does not allow ends the channel.
    size_t at = 0;
    while (c->in.size() >= at + 2) {
        size_t length = ((size_t)(unsigned char)c->in[at] << 8) | (unsigned char)c->in[at + 1];
        if (length == 0 || length > kChannelMaxMessage) {
            close(c, "protocol", "the program sent a message of " + number(length) + " bytes");
            return;
        }
        at += 2 + length;
    }
}

bool Channels::sendOne(Channel* c) {
    if (c->closed || c->connecting || c->toPhone == 0 || !wholeMessage(c->in)) return false;
    if (!c->peer->open() || c->peer->closing()) return false;
    size_t length = frontLength(c->in);
    c->peer->data(c->stream, c->in.data() + 2, length, true);
    c->in.erase(0, 2 + length);
    --c->toPhone;
    return true;
}

void Channels::reap() {
    for (size_t i = 0; i < all_.size();) {
        if (!all_[i]->closed) {
            ++i;
            continue;
        }
        delete all_[i]->end;
        delete all_[i];
        all_.erase(all_.begin() + (long)i);
    }
}

}  // namespace nbt
