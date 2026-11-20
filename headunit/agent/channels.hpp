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

// The switch of a switched channel (manifest `switch`): this file in the agent's run folder, `runDir`, is there while
// it is on. So it is off until it is switched on, and off again after a power down, and nothing is written to flash
// for it.
std::string switchFile(const std::string& runDir, const std::string& package, const std::string& name);

// The folder of a package's channels, ${tmp}/channels, where each program listens: `tmpDir` is the package's ${tmp}.
std::string channelsDir(const std::string& tmpDir);
// Makes it, for the agent's user alone (mode 700). It is on the RAM disk, so the agent makes it at each start, and at
// an install of a package with channels.
bool makeChannelsDir(const std::string& tmpDir, std::string* error);

// Told what becomes of a channel: the agent answers channel.open, and says it in the log. Each channel.open is
// answered once, by channelConnected or channelRefused, unless its link goes first.
class ChannelHost {
public:
    virtual ~ChannelHost() {}
    // The program has taken the connection: channel.open is answered now.
    virtual void channelConnected(Channel* channel) = 0;
    // It did not, or the channel was closed while it connected: channel.open is answered with the error `code`
    // (unavailable, off or busy), and `why` says why.
    virtual void channelRefused(Channel* channel, const char* code, const std::string& why) = 0;
    // A channel has closed: an open one for `reason` (agent.hpp), or because the phone closed it or its link went
    // (reason 0); one that was still connecting only because its link went, its channel.open unanswered. `why` for
    // the log.
    virtual void channelClosed(Channel* channel, const char* reason, const std::string& why) = 0;
    // Milliseconds on a clock that only goes forward.
    virtual int64_t channelNowMs() = 0;
    // A line for the log: a program started or ended.
    virtual void channelNote(const std::string& line) = 0;
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
    // What starts the program when nothing listens (the manifest's `start`), if the agent may start it.
    bool canStart;
    std::string program;        // its absolute path
    std::vector<std::string> args, env;
    std::string cwd, log;       // the folder it runs in, and the file its output is appended to
    bool startedHere;           // this channel's open started it

    Channel()
        : peer(0), link(0), stream(0), requestId(0), connecting(true), closed(false), openedMs(0), retryAtMs(0), end(0),
          toPhone(0), fromPhone(0), outIsMessage(false), taken(0), programClosed(false), canStart(false), startedHere(false) {}
};

class Channels {
public:
    Channels(ChannelHost* host, ProgramEnds* ends);
    ~Channels();

    void setEnds(ProgramEnds* ends) { ends_ = ends; }
    // While a job runs: no program's socket is read or written, and nothing goes to a phone. What a phone sends waits,
    // within its credit.
    void setPaused(bool paused) { paused_ = paused; }
    bool paused() const { return paused_; }

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
    // the link has gone). One that is still connecting has its channel.open answered instead, with the error that
    // fits `reason`: off for off, busy for package-changed, unavailable for any other; with reason 0 nothing is
    // answered. The program's end is closed at once. Asking twice does nothing.
    void close(Channel* channel, const char* reason, const std::string& why);
    // Every channel of a link, which has ended: nobody is told.
    void closeLink(const Peer* peer, const std::string& why);
    // Every channel of a package, connecting ones too, or of one of its channels when `name` is not empty.
    void closePackage(const std::string& package, const std::string& name, const char* reason, const std::string& why);
    // Every channel that is still connecting: its channel.open is answered with the error `code`. A program that one
    // of them started is ended, as one that does not take the connection in time is, unless another channel of it
    // is open.
    void closeConnecting(const char* code, const std::string& why);

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

    // ---- the programs the agent starts for channels
    // Where their marks go (the run folder), and the shell that starts them. Call before anything is started.
    void setPrograms(const std::string& runDir, const std::string& shell);
    // The mark that a program runs for a channel, which outlasts the agent and not a power down: the pid.
    std::string pidFile(const std::string& package, const std::string& name) const;
    // A program that an agent before this one started and that still runs, as its mark says, which the caller has
    // checked is that program: this agent ends it as one of its own, and caps its log.
    void adoptProgram(const std::string& package, const std::string& name, long pid, const std::string& log);
    bool programRunning(const std::string& package, const std::string& name) const;
    // Whether one runs for any of these packages.
    bool programsRunning(const std::vector<std::string>& packages) const;
    // Ends what runs for a package, or for its channel of that name when `name` is not empty: SIGTERM to its group,
    // and SIGKILL kProgramKillAfterMs later if it is still there. Nothing waits for it.
    void endPrograms(const std::string& package, const std::string& name, const std::string& why);
    // Each turn of the loop, and while an install step runs: collects the programs that have ended, sends SIGKILL to
    // those that were asked to end and have not, and keeps each one's log under kChannelLogCapBytes: past it, the log
    // is copied to <log>.1 and emptied, and emptied even when the copy fails (a full RAM disk), what it held lost. A
    // log that cannot be set aside or emptied is said in the agent's log once, and again only after it has been.
    void tick();
    // True while a program is being ended: the loop then waits no longer than kChannelStartRetryMs.
    bool ending() const;

private:
    Channels(const Channels&);
    Channels& operator=(const Channels&);

    Channel* find(const Peer* peer, uint16_t stream);
    void connect(Channel* channel);
    void writeTo(Channel* channel);
    void readFrom(Channel* channel);
    void grant(Channel* channel);
    bool mayRead(const Channel* channel) const;
    // Starts the channel's program: false with *why set if it could not be.
    bool start(Channel* channel, std::string* why);
    // A channel that is connecting closes, its channel.open answered with the error `code`.
    void refuse(Channel* channel, const char* code, const std::string& why);

    // A program the agent started, or an earlier agent did.
    struct Started {
        std::string package, name, log;
        long pid;
        bool child;               // this agent's own child: collected with waitpid
        int64_t endingSinceMs;    // 0, or when it was sent SIGTERM
        bool killed;              // sent SIGKILL
        bool logTrouble;          // its log could not be set aside or emptied, and the log said so
        Started() : pid(0), child(false), endingSinceMs(0), killed(false), logTrouble(false) {}
    };

    // Keeps the log of a program under kChannelLogCapBytes: see tick().
    void capLog(Started* program);

    ChannelHost* host_;
    ProgramEnds* ends_;
    bool paused_;
    std::vector<Channel*> all_;
    std::string runDir_, shell_;
    std::vector<Started> started_;
    std::vector<std::pair<std::string, std::string> > lastEnd_;   // "package name", how its program last ended
};

// The most bytes a channel's program's log holds before the agent sets it aside as <log>.1 and empties it.
const uint64_t kChannelLogCapBytes = 256 * 1024;
// How long a program has, from SIGTERM, before SIGKILL.
const int64_t kProgramKillAfterMs = 2000;

// A message as it goes to and comes from the program: two bytes of length, high byte first, and the bytes.
std::string channelFrame(const std::string& message);

}  // namespace nbt

#endif
