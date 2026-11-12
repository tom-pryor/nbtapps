// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBTAPP_AGENT_LINK_HPP
#define NBTAPP_AGENT_LINK_HPP

// The plugin's link to the agent: the agent's local socket (/tmp/nbtagent.sock on a head unit), spoken with the
// agent's own frame, JSON and message code (agent/frame.cpp, json.cpp, peer.cpp), as the controller of role
// "plugin". The local socket needs no `auth`: who may open it is the head unit's own programs.
//
// It lives inside the HMI's process, on the app's thread, so it keeps to three rules:
//
//   - Nothing waits. The socket is non-blocking, and poll() reads and writes what there is and returns. The app
//     calls poll() from a timer for as long as isOpen().
//   - Nothing raises a signal. A write to a socket whose agent has gone would be SIGPIPE, and the HMI's process is
//     not ours to change the handling of signals in: bytes go out with send() and MSG_NOSIGNAL.
//   - Nothing is kept open that is not in use. A screen opens the link when it is shown and closes it when it is
//     left; with no agent there, open() says so at once and the screen says it to the user.
//
// No framework header is needed here, so the same code is tested on a build host against a real agent
// (tests/agent/test_plugin.cpp).
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>

#include "../agent/link.hpp"
#include "../agent/peer.hpp"

namespace nbt {

// A connected local socket that never blocks and never raises SIGPIPE.
class QuietLocalLink : public Link {
public:
    // Connects to `path`. 0 with *error set if nobody listens there. A local socket connects, or fails, at once.
    static QuietLocalLink* connectTo(const std::string& path, std::string* error) {
        struct sockaddr_un address;
        memset(&address, 0, sizeof address);
        address.sun_family = AF_UNIX;
        if (path.empty() || path.size() >= sizeof address.sun_path) {
            if (error != 0) *error = "the socket path is empty or too long";
            return 0;
        }
        memcpy(address.sun_path, path.c_str(), path.size() + 1);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            if (error != 0) *error = std::string("cannot make a socket: ") + strerror(errno);
            return 0;
        }
        if (connect(fd, (struct sockaddr*)&address, sizeof address) != 0) {
            if (error != 0) *error = "cannot connect to " + path + ": " + strerror(errno);
            ::close(fd);
            return 0;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        // Not handed on to a program the HMI starts.
        fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
        return new QuietLocalLink(fd);
    }

    virtual ~QuietLocalLink() { close(); }
    virtual int fd() const { return fd_; }
    virtual long read(void* out, size_t cap) {
        if (fd_ < 0) return -1;
        for (;;) {
            ssize_t n = recv(fd_, out, cap, 0);
            if (n > 0) return (long)n;
            if (n == 0) return -1;   // the agent closed it
            if (errno == EINTR) continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
    }
    virtual long write(const void* data, size_t len) {
        if (fd_ < 0) return -1;
        for (;;) {
            ssize_t n = send(fd_, data, len, MSG_NOSIGNAL);
            if (n >= 0) return (long)n;
            if (errno == EINTR) continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
    }
    virtual void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    virtual const char* kind() const { return "local"; }

private:
    explicit QuietLocalLink(int fd) : fd_(fd) {}
    QuietLocalLink(const QuietLocalLink&);
    QuietLocalLink& operator=(const QuietLocalLink&);
    int fd_;
};

// What a screen hears from its link. All of it is called from inside AgentLink::poll().
class AgentLinkListener {
public:
    virtual ~AgentLinkListener() {}
    // The answer to request(). `code` is empty for a result, and the error's code otherwise.
    virtual void agentReply(int64_t id, const json::Value& result, const std::string& code) = 0;
    virtual void agentEvent(const std::string& name, const json::Value& params) = 0;
    // The link ended and was not closed from this side: the agent stopped, or refused `hello`.
    virtual void agentGone(const std::string& why) = 0;
};

class AgentLink : public PeerHandler {
public:
    explicit AgentLink(AgentLinkListener* listener) : listener_(listener), peer_(0), helloId_(0), closedHere_(false) {}
    virtual ~AgentLink() { delete peer_; }

    // Connects and says hello; a link that was open is closed first. False with *error set if no agent listens at
    // `path`. Requests may follow at once: the agent answers them in order, after hello.
    bool open(const std::string& path, std::string* error) {
        delete peer_;
        peer_ = 0;
        closedHere_ = false;
        Link* link = QuietLocalLink::connectTo(path, error);
        if (link == 0) return false;
        peer_ = new Peer(link, true, this);
        helloId_ = peer_->request("hello", "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"plugin\",\"maxFrame\":65536,\"capabilities\":[]}");
        if (helloId_ == 0 || !peer_->open()) {
            if (error != 0) *error = "the agent's socket took no bytes";
            delete peer_;
            peer_ = 0;
            return false;
        }
        return true;
    }

    bool isOpen() const { return peer_ != 0 && peer_->open(); }

    // The request's id, for agentReply(); 0 if the link is not open.
    int64_t request(const std::string& method, const std::string& params) {
        return isOpen() ? peer_->request(method, params) : 0;
    }

    // Ends the link; the listener hears nothing of it. May be called from inside the listener.
    void close() {
        if (peer_ == 0) return;
        closedHere_ = true;
        peer_->close("closed by the plugin");
    }

    // Reads what came and writes what is queued, without waiting. The listener is called from here.
    void poll() {
        if (peer_ == 0) return;
        if (peer_->open()) peer_->flush();
        if (peer_->open()) peer_->readable();
        if (!peer_->open()) {
            // Here, and not in close(): a peer is not deleted from inside its own callback.
            std::string why = peer_->closeReason();
            bool tell = !closedHere_;
            delete peer_;
            peer_ = 0;
            if (tell) listener_->agentGone(why);
        }
    }

    // PeerHandler
    virtual void onRequest(Peer* peer, int64_t id, const std::string& method, const json::Value&) {
        if (method == "ping") peer->respond(id, "{}");
        else peer->respondError(id, "unsupported", "the plugin answers ping only");
    }
    virtual void onResponse(Peer* peer, int64_t id, bool ok, const json::Value& result, const std::string& code, const std::string&) {
        if (id == helloId_) {
            // An agent that will not have this plugin: there is nothing more to say to it.
            if (!ok) peer->close("the agent refused hello: " + code);
            return;
        }
        listener_->agentReply(id, result, ok ? std::string() : (code.empty() ? std::string("internal") : code));
    }
    virtual void onEvent(Peer*, const std::string& event, int64_t, const json::Value& params) { listener_->agentEvent(event, params); }
    virtual void onData(Peer* peer, uint16_t, const std::string&, bool) { peer->close("the agent sent a DATA frame"); }

private:
    AgentLink(const AgentLink&);
    AgentLink& operator=(const AgentLink&);

    AgentLinkListener* listener_;
    Peer* peer_;
    int64_t helloId_;
    bool closedHere_;
};

}  // namespace nbt

#endif
