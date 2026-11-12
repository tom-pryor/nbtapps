// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_LINK_HPP
#define NBT_AGENT_LINK_HPP

// The lowest layer of the protocol: reliable, ordered bytes between the agent and one controller. Framing,
// messages and jobs sit above this interface and do not know which link they run on.
//
// The agent has one thread and waits on every link at once, so a link never blocks: it reads what is there, takes
// what it can, and offers a file descriptor to wait on.
//
// A link is a stream socket (SocketLink). Controllers on the same machine come in on a local socket: the
// command-line controller and the iDrive plugin. There is also a TCP listener, which is for the test bench alone
// (see TcpListener). A phone app's Bluetooth serial link is a socket too, as far as the agent sees it:
// serial_link.hpp puts one in front of the car's serial node.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>

#include <string>

namespace nbt {

class Link {
public:
    virtual ~Link() {}

    // To wait on with select(): readable when read() has something or the link has ended, writable when write()
    // can take more.
    virtual int fd() const = 0;
    // Reads what is there now. Above 0: that many bytes. 0: nothing for the moment. Below 0: the link has ended.
    virtual long read(void* out, size_t cap) = 0;
    // Takes what it can now. The bytes taken, 0 if none for the moment, below 0 if the link has ended.
    virtual long write(const void* data, size_t len) = 0;
    virtual void close() = 0;
    // "local", "serial" or "tcp". The local socket needs no `auth` and is the one kind the pairing window and the
    // list of paired phones are worked from; the other two need `auth` before anything else is answered.
    virtual const char* kind() const = 0;
};

// Where controllers connect to the agent.
class Listener {
public:
    virtual ~Listener() {}
    virtual int fd() const = 0;
    // The next waiting connection, for the caller to delete; 0 if there is none right now.
    virtual Link* accept() = 0;
};

// A connected stream socket of any family.
class SocketLink : public Link {
public:
    // Takes over `fd` and makes it non-blocking.
    SocketLink(int fd, const char* kind);
    virtual ~SocketLink();
    virtual int fd() const { return fd_; }
    virtual long read(void* out, size_t cap);
    virtual long write(const void* data, size_t len);
    virtual void close();
    virtual const char* kind() const { return kind_; }

private:
    SocketLink(const SocketLink&);
    SocketLink& operator=(const SocketLink&);
    int fd_;
    const char* kind_;
};

// Where the agent listens on a head unit, and where nbtctl looks for it there. /tmp does not outlast a power
// down, and neither does the agent.
const char* const kHeadUnitSocket = "/tmp/nbtagent.sock";

// A local (AF_UNIX) socket that controllers on the same machine connect to.
class LocalListener : public Listener {
public:
    LocalListener() : fd_(-1) {}
    virtual ~LocalListener() { close(); }
    // Listens at `path`, mode 0600. A socket file left by an agent that is gone is replaced; one that an agent
    // still answers on is an error, and the only one that sets *taken.
    bool open(const std::string& path, std::string* error, bool* taken = 0);
    void close();
    virtual int fd() const { return fd_; }
    virtual Link* accept();

private:
    LocalListener(const LocalListener&);
    LocalListener& operator=(const LocalListener&);
    int fd_;
    std::string path_;
};

// A TCP socket on one IPv4 address, for a phone app that dials the agent.
//
// The agent asks `auth` of this link as it does of a serial one, and lets a phone be paired over it
// (pairing.issue), which a car's TCP link must not: the key then crosses the link in the clear. The frame MAC
// that the plan document gives TCP is not written either, so whoever can change the bytes on the way can still
// say anything in a paired phone's name. This is for the test bench only: nbtagent offers it only in a host
// build (--debug-tcp), and only on the loopback address, where an Android emulator or a phone on USB reaches it
// through `adb reverse`.
class TcpListener : public Listener {
public:
    TcpListener() : fd_(-1), port_(0) {}
    virtual ~TcpListener() { close(); }
    // Listens on `address` (dotted IPv4, such as 127.0.0.1) and `port`; port 0 takes any free one.
    bool open(const char* address, unsigned port, std::string* error);
    void close();
    // The port it listens on, once open.
    unsigned port() const { return port_; }
    virtual int fd() const { return fd_; }
    virtual Link* accept();

private:
    TcpListener(const TcpListener&);
    TcpListener& operator=(const TcpListener&);
    int fd_;
    unsigned port_;
};

// Connects to an agent's local socket. 0 with *error set if nobody listens there.
Link* connectLocal(const std::string& path, std::string* error);

}  // namespace nbt

#endif
