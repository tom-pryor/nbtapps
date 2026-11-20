// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_CHANNELS_HPP
#define NBT_AGENT_CHANNELS_HPP

// The channels that are open (agent.hpp, "Channels" has the messages): one entry for each, with its link and stream
// number, the package and channel it is, how far it is, the program's end of it, and what waits to go either way.
// The agent owns one table, decides whether a channel may be opened, and answers channel.open; the table does the
// rest: it connects to the program, carries the messages, and closes the channel, telling the phone why.
//
// The program's end is an interface (ProgramEnd), made by a ProgramEnds the agent is given: so a test can stand in
// for the program.
//
// Credits. The phone may send what the agent granted: kChannelCredit at first, and as many again as the program has
// taken off the socket, half a credit at a time, or what there is once the program has taken all that waited; so at
// most kChannelCredit messages wait here for a program. The agent sends the phone what the phone granted, and reads
// a program's socket only while it may, while no whole message of it waits here, and while less than two frames
// wait on the link: a program that sends faster than the phone takes waits in its own socket.
//
// An entry that is closed stays in the table, marked, until reap() takes it away: a caller that holds a pointer to
// it within a turn of the agent's loop holds a valid one. Nothing here blocks.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>
#include <sys/select.h>

#include <deque>
#include <string>
#include <vector>

#include "json.hpp"
#include "peer.hpp"

namespace nbt {

// The program's end of one channel.
class ProgramEnd {
public:
    virtual ~ProgramEnd() {}
    enum Connect {
        kConnected,   // the program has taken the connection
        kWaiting,     // not yet: ask again once fd() is writable, or a little later
        kRefused      // nothing takes it; *why says why
    };
    virtual Connect connect(std::string* why) = 0;
    // What to wait on in the agent's loop; -1 for none.
    virtual int fd() const = 0;
    // Reads what the program has sent, at most `most` bytes, onto *in. False when the program has closed its end.
    virtual bool read(std::string* in, size_t most) = 0;
    // Writes what the program takes of `len` bytes now: how many, or -1 when it has closed its end.
    virtual long write(const char* bytes, size_t len) = 0;
};

struct Channel;

// Makes the program's end of a channel, given the path of the program's socket. Not owned by the table.
class ProgramEnds {
public:
    virtual ~ProgramEnds() {}
    virtual ProgramEnd* make(const std::string& socketPath) = 0;
};

// The program's end as a program has it (agent.hpp, "Channels"): a local stream socket the program listens on, which
// the agent connects to without waiting, once for each channel.
class SocketEnds : public ProgramEnds {
public:
    virtual ProgramEnd* make(const std::string& socketPath);
};

// Whether a path fits a local socket's address on this machine.
bool fitsSocketAddress(const std::string& path);

// The folder of a package's channels, ${tmp}/channels, where each program listens: `tmpDir` is the package's ${tmp}.
std::string channelsDir(const std::string& tmpDir);
// Makes it, for the agent's user alone (mode 700). It is on the RAM disk, so the agent makes it at each start, and at
// an install of a package with channels.
bool makeChannelsDir(const std::string& tmpDir, std::string* error);

// Told what becomes of a channel: the agent answers channel.open, and says it in the log.
class ChannelHost {
public:
    virtual ~ChannelHost() {}
    // The program has taken the connection: channel.open is answered now.
    virtual void channelConnected(Channel* channel) = 0;
    // It did not: channel.open is answered with `unavailable`, and `why` says why.
    virtual void channelUnavailable(Channel* channel, const std::string& why) = 0;
    // An open channel has closed, for `reason` (agent.hpp), or because the phone closed it or its link went (reason
    // 0); `why` for the log.
    virtual void channelClosed(Channel* channel, const char* reason, const std::string& why) = 0;
    // Milliseconds on a clock that only goes forward.
    virtual int64_t channelNowMs() = 0;
};

struct Channel {
    Peer* peer;
    unsigned link;              // the link's number, for the log
    uint16_t stream;
    std::string package, name, access, version;
    int64_t requestId;          // the channel.open to answer
    bool connecting;            // not answered yet
    bool closed;                // waits for reap()
    int64_t openedMs;
    int64_t retryAtMs;          // connecting: when to try again
    std::string socketPath;
    ProgramEnd* end;            // owned; 0 once closed
    unsigned toPhone;           // messages the agent may still send the phone
    unsigned fromPhone;         // messages the phone may still send
    std::deque<std::string> waiting;   // messages from the phone that the program has not taken yet
    std::string out;            // bytes on their way to the program: its first line, or the message at the front
    bool outIsMessage;          // `out` is the front of `waiting`, framed
    unsigned taken;             // messages the program has taken since the phone was last granted more
    std::string in;             // bytes from the program that are not in a DATA frame yet
    bool programClosed;         // the program has closed its end: the channel closes once `in` holds no message

    Channel()
        : peer(0), link(0), stream(0), requestId(0), connecting(true), closed(false), openedMs(0), retryAtMs(0), end(0),
          toPhone(0), fromPhone(0), outIsMessage(false), taken(0), programClosed(false) {}
};

class Channels {
public:
    Channels(ChannelHost* host, ProgramEnds* ends);
    ~Channels();

    void setEnds(ProgramEnds* ends) { ends_ = ends; }

    // A channel that is to connect to `socketPath` and then send `firstLine` to the program; the table owns it. The
    // caller has made every check and filled in who it is for; the table connects, and has the host answer.
    Channel* add(Channel* channel, const std::string& firstLine);

    // The channels of a link that are not closed, in the order of their numbers; connecting ones too.
    std::vector<Channel*> on(const Peer* peer) const;
    // How many of a package's channel of that name are not closed, on every link.
    unsigned count(const std::string& package, const std::string& name) const;
    // Whether a number is in use on a link.
    bool inUse(const Peer* peer, uint16_t stream) const;

    // Closes a channel: the phone is told channel.close with `reason`, unless `reason` is 0 (the phone closed it, or
    // the link has gone). The program's end is closed at once. Asking twice does nothing.
    void close(Channel* channel, const char* reason, const std::string& why);
    // Every channel of a link, which has ended: nobody is told.
    void closeLink(const Peer* peer, const std::string& why);
    // Every open channel of a package, or of one of its channels when `name` is not empty.
    void closePackage(const std::string& package, const std::string& name, const char* reason, const std::string& why);

    // A DATA frame on a channel's number (from kChannelFirstStream up): see agent.hpp.
    void data(Peer* peer, uint16_t stream, const std::string& bytes, bool last);
    // The phone's events channel.credit and channel.close.
    void credit(Peer* peer, const json::Value& params);
    void closedByPhone(Peer* peer, const json::Value& params);

    // The programs' sockets to wait on in the agent's loop: to read while the channel takes more from its program, to
    // write while something waits to go to it or a connect is under way. Returns the highest descriptor, or `top`.
    int addToSets(fd_set* readable, fd_set* writable, int top) const;
    // True while a channel connects: the loop then waits no longer than kChannelStartRetryMs.
    bool connecting() const;
    // One turn: connects what is connecting, writes to the programs and reads from them. Each is tried whatever
    // select said of it: a socket that has nothing to give answers at once.
    void serve();
    // Sends one message of a channel to its phone, if one is whole and the phone has granted credit: true if a frame
    // went out. The agent's turns call it (Agent::pumpLink), so that a link's channels and its files stream take
    // turns, a frame each.
    bool sendOne(Channel* channel);
    // Deletes the entries that are closed.
    void reap();

private:
    Channels(const Channels&);
    Channels& operator=(const Channels&);

    Channel* find(const Peer* peer, uint16_t stream);
    void connect(Channel* channel);
    void writeTo(Channel* channel);
    void readFrom(Channel* channel);
    void grant(Channel* channel);
    bool mayRead(const Channel* channel) const;

    ChannelHost* host_;
    ProgramEnds* ends_;
    std::vector<Channel*> all_;
};

// A message as it goes to and comes from the program: two bytes of length, high byte first, and the bytes.
std::string channelFrame(const std::string& message);

}  // namespace nbt

#endif
