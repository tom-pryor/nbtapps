// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Links over stream sockets: see link.hpp.

#include "link.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace nbt {

namespace {

void setNonBlocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

bool localAddress(const std::string& path, struct sockaddr_un* out, std::string* error) {
    memset(out, 0, sizeof *out);
    out->sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof out->sun_path) {
        // A limit of local sockets themselves: about 100 bytes, less on some systems.
        char limit[32];
        snprintf(limit, sizeof limit, "%u", (unsigned)(sizeof out->sun_path - 1));
        if (error != 0) *error = "the socket path " + path + " is empty or longer than " + limit + " bytes";
        return false;
    }
    memcpy(out->sun_path, path.c_str(), path.size() + 1);
    return true;
}

}  // namespace

SocketLink::SocketLink(int fd, const char* kind) : fd_(fd), kind_(kind) { setNonBlocking(fd_); }

SocketLink::~SocketLink() { close(); }

long SocketLink::read(void* out, size_t cap) {
    if (fd_ < 0) return -1;
    for (;;) {
        ssize_t n = ::read(fd_, out, cap);
        if (n > 0) return (long)n;
        if (n == 0) return -1;   // the other side closed
        if (errno == EINTR) continue;
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    }
}

long SocketLink::write(const void* data, size_t len) {
    if (fd_ < 0) return -1;
    for (;;) {
        // The process ignores SIGPIPE (the agent and the controller both set that at start), so a link that has
        // gone shows as an error here.
        ssize_t n = ::write(fd_, data, len);
        if (n >= 0) return (long)n;
        if (errno == EINTR) continue;
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    }
}

void SocketLink::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool LocalListener::open(const std::string& path, std::string* error, bool* taken) {
    close();
    if (taken != 0) *taken = false;
    struct sockaddr_un address;
    if (!localAddress(path, &address, error)) return false;

    // Is an agent answering there already? Only a socket nobody listens on is cleared away.
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) {
        if (error != 0) *error = std::string("cannot make a socket: ") + strerror(errno);
        return false;
    }
    bool answered = connect(probe, (struct sockaddr*)&address, sizeof address) == 0;
    ::close(probe);
    if (answered) {
        if (taken != 0) *taken = true;
        if (error != 0) *error = "an agent is already listening at " + path;
        return false;
    }
    struct stat st;
    if (lstat(path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            if (error != 0) *error = path + " exists and is not a socket";
            return false;
        }
        unlink(path.c_str());
    }

    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        if (error != 0) *error = std::string("cannot make a socket: ") + strerror(errno);
        return false;
    }
    // Only the user the agent runs as may connect. The mask covers the moment between bind and chmod.
    mode_t old = umask(0177);
    int bound = bind(fd_, (struct sockaddr*)&address, sizeof address);
    umask(old);
    if (bound != 0 || chmod(path.c_str(), 0600) != 0 || listen(fd_, 8) != 0) {
        if (error != 0) *error = "cannot listen at " + path + ": " + strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    setNonBlocking(fd_);
    path_ = path;
    return true;
}

void LocalListener::close() {
    if (fd_ < 0) return;
    ::close(fd_);
    fd_ = -1;
    unlink(path_.c_str());
}

Link* LocalListener::accept() {
    if (fd_ < 0) return 0;
    for (;;) {
        int fd = ::accept(fd_, 0, 0);
        if (fd >= 0) return new SocketLink(fd, "local");
        if (errno == EINTR) continue;
        return 0;
    }
}

bool TcpListener::open(const char* address, unsigned port, std::string* error) {
    close();
    struct sockaddr_in where;
    memset(&where, 0, sizeof where);
    where.sin_family = AF_INET;
    where.sin_port = htons((unsigned short)port);
    if (port > 65535 || inet_pton(AF_INET, address, &where.sin_addr) != 1) {
        if (error != 0) *error = std::string(address) + " with that port is not an IPv4 address and a port";
        return false;
    }
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        if (error != 0) *error = std::string("cannot make a socket: ") + strerror(errno);
        return false;
    }
    // An agent that restarts must not wait for the last one's connections to time out of the port.
    int on = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    socklen_t length = sizeof where;
    if (bind(fd_, (struct sockaddr*)&where, sizeof where) != 0 || listen(fd_, 8) != 0 ||
        getsockname(fd_, (struct sockaddr*)&where, &length) != 0) {
        char number[16];
        snprintf(number, sizeof number, "%u", port);
        if (error != 0) *error = std::string("cannot listen at ") + address + ":" + number + ": " + strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    setNonBlocking(fd_);
    port_ = ntohs(where.sin_port);
    return true;
}

void TcpListener::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    port_ = 0;
}

Link* TcpListener::accept() {
    if (fd_ < 0) return 0;
    for (;;) {
        int fd = ::accept(fd_, 0, 0);
        if (fd >= 0) {
            // Messages are small and each waits for its answer: they must not sit in the socket to be batched.
            int on = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
            return new SocketLink(fd, "tcp");
        }
        if (errno == EINTR) continue;
        return 0;
    }
}

Link* connectLocal(const std::string& path, std::string* error) {
    struct sockaddr_un address;
    if (!localAddress(path, &address, error)) return 0;
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
    return new SocketLink(fd, "local");
}

}  // namespace nbt
