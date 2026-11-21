// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_NODE_READER_HPP
#define NBT_AGENT_NODE_READER_HPP

// The one reader of an open serial node, /dev/btsppN: a thread of its own that asks the node for bytes from the
// moment it is opened until it is closed (serial_link.hpp says what the node is for).
//
// What is understood about the node, and what the code is built on:
//
//   * What the phone sends waits in the car's Bluetooth stack, ahead of the node. It moves into the node only when
//     new bytes come from the phone while nothing waits there already, or when a read() reaches the node. select()
//     does not ask the stack: it says, truthfully, that the node is empty while bytes wait ahead of it. So nothing
//     here waits on the node with select(). A read is what brings the bytes forward, and one is always out.
//   * Bytes outlive the end of their link and come on a later link, ahead of what the phone sends on that one: as
//     a run of bytes, in pieces of up to about 1,700 bytes, not message by message.
//   * A read that waits on the node can be ended. A kernel time limit armed just before it (TimerTimeout) makes it
//     fail with EINTR, and nothing is consumed; a close() of the descriptor from another thread, and the end of the
//     link, make it fail with EBADF. The car's own reader of the node reads it that way: a blocking read under a
//     time limit, armed again for each read.
//
// So a NodeReader issues its first read as soon as it is started, which is just after the open, and another one
// each time one comes back, every tick at the latest, for as long as it runs. What it reads goes into a queue that
// its owner takes from, waiting on a condition variable: no time limit anywhere else depends on the node. In drain
// mode what it reads is counted and dropped instead, until the link ends.
//
// The one part that differs between the head unit and the build host is the timed read (NodeReadFn): on QNX a
// blocking read() under TimerTimeout, and on the host, where the tests' nodes are pseudo-terminals, select() and
// then read(). A test gives its own in its place.
//
// The node's input queue, for a link that another program is to have (NodePrimer, below):
//
//   * A read that does not wait, made while nothing is in the node's queue, takes nothing, and has the stack hand
//     what waits ahead of the node into the queue: select() then says so, to every reader of the node. Once one read
//     has been made on a link, what the phone sends later comes into the queue as it comes, for a reader that waits
//     on select(), also after the node has been closed and opened again. A program that waits on select() before
//     it reads, on a link no read was ever made on, never sees anything.
//   * Bytes that were read can be put back into the queue (tcinject() with a negative count adds them as they
//     are, behind what is there), and a later reader gets them first and then what follows, in order, provided a
//     descriptor stays open on the node: the last close empties the queue. The node takes bytes as they are (raw)
//     when it is opened; its queue holds 6144 bytes, and a put-back larger than the room left is cut short with no
//     error.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "serial_log.hpp"

namespace nbt {

// One read of `fd` into `buf` that waits `ms` at most, as read() returns: the bytes read, 0 at the end, or -1 with
// errno. EINTR, ETIMEDOUT and EAGAIN mean that the time was up and nothing was read.
typedef long (*NodeReadFn)(int fd, void* buf, size_t len, int ms);

// The platform's own. On QNX nothing that can block comes between arming the time limit and the read(): the limit
// holds for the next blocking kernel call only.
// When the time limit cannot be armed, the read goes ahead without it, and the error is kept: the first reader
// whose read comes back after that says it in the log, once in the process.
long timedNodeRead(int fd, void* buf, size_t len, int ms);
// What timedNodeRead keeps when arming failed: the first error, an errno value. A test may note one of its own.
void noteNodeReadArmError(int error);
// True, with that error, the first time it is asked after one was noted, and never again.
bool takeNodeReadArmError(int* error);

// How long one read waits at most: a tick.
const int kNodeTickMs = 100;
// What one read asks for.
const size_t kNodeReadBytes = 32768;
// While this much waits in the queue for its owner, no more is read: the bytes wait ahead of the node, as they
// do for a reader that is slow.
const size_t kNodeQueueMost = 262144;

class NodeReader {
public:
    // Starts the thread on the node `fd`, open for reading and writing, which it takes over. `log` must outlive
    // stop(). 0 if the thread cannot be started; the node is closed then.
    static NodeReader* start(int fd, const std::string& path, SerialLog* log, NodeReadFn read = 0);

    // The node, for writes into it: a write does not wait on the reader.
    int fd() const { return fd_; }
    const std::string& path() const { return path_; }

    enum Got {
        kData,      // bytes were added to *out: all that had been read
        kNothing,   // nothing came in `ms`
        kEnded      // the link has ended, and every byte read before that has been taken
    };
    // Waits `ms` at most for what has been read.
    Got next(int ms, std::vector<uint8_t>* out);
    // How the link ended, once next() has said kEnded or ended() is true: 0 if the last read gave 0 (the node was
    // closed on the far side), otherwise that read's errno.
    int endError();
    bool ended();

    // From here on what is read, and what waits in the queue now, is counted and dropped.
    void drain();
    uint64_t dropped();

    // Ends the thread and frees this object. Returns the node's descriptor, still open, if the thread had stopped
    // within three ticks; what it had read and its owner had not taken yet is added to *rest then, if `rest` is
    // given, and dropped otherwise. Otherwise its read did not come back when its time was up: the node is closed
    // under it, the log says so, the thread frees itself when that read returns, and -1 is returned.
    int stop(std::vector<uint8_t>* rest = 0);
    // stop(), and the node closed.
    void finish();

private:
    NodeReader(int fd, const std::string& path, SerialLog* log, NodeReadFn read);
    ~NodeReader();
    NodeReader(const NodeReader&);
    NodeReader& operator=(const NodeReader&);

    static void* threadMain(void* self);
    void run();

    int fd_;
    std::string path_;
    SerialLog* log_;
    NodeReadFn read_;
    pthread_t thread_;

    pthread_mutex_t mu_;   // everything below
    pthread_cond_t cv_;    // bytes, the end, a stop, a taken queue, the thread's end
    std::vector<uint8_t> queue_;
    bool drain_, stop_, ended_, done_, released_;
    int error_;
    uint64_t dropped_;
};

// ---- the node's input queue: a link primed for another program, and bytes put back ---------------------------------

// The calls on a node's input queue that NodePrimer makes. A member left 0 is the platform's (platformNodeCalls()); a
// test gives its own in its place.
struct NodeCalls {
    // How many bytes wait in the node's input queue (tcischars()). -1 with errno.
    long (*queued)(int fd);
    // Whether the node gives bytes as they came, with nothing done to them on the way in: no lines, echo, signals,
    // stripped bits or changed line ends (tcgetattr()). 1 or 0; -1 with errno.
    int (*raw)(int fd);
    // How many bytes the node's input queue holds (devctl() DCMD_CHR_ISSIZE). -1 with errno.
    long (*queueSize)(int fd);
    // One read that does not wait, as read() returns: -1 with EAGAIN when there is nothing to read.
    long (*readNow)(int fd, void* buf, size_t len);
    // Puts `len` bytes into the node's input queue, behind what waits there, as they are, as if the phone had sent
    // them (tcinject() with a negative count). 0, or -1 with errno.
    int (*inject)(int fd, const void* buf, size_t len);

    NodeCalls() : queued(0), raw(0), queueSize(0), readNow(0), inject(0) {}
};

// The platform's own. On QNX all five; on the build host the queue's fill (FIONREAD), the terminal settings and the
// read that does not wait, but neither the queue's size nor a put-back, which fail with ENOSYS.
NodeCalls platformNodeCalls();

// What a link that another program is to have needs from the agent, which opened its node (the top of this file):
// prime() makes sure that what the phone sent comes into the node's queue, and putBack() gives back what the agent
// read from it. The caller then keeps its descriptor open, unread, until the link ends. Each says why it failed, and
// what was read and is not back in the queue then is lost.
class NodePrimer {
public:
    explicit NodePrimer(const NodeCalls& calls = NodeCalls());

    struct Result {
        long queued;       // prime(): what the queue held when it was looked at
        size_t taken;      // prime(): the bytes its read took, which came in the moment before
        size_t putBack;    // the bytes put back into the queue, in all
        size_t fromQueue;  // of those, the ones that had come into the queue and were read out of it just before,
                           // so that they go back behind the others
        size_t lost;       // on a failure: the bytes read that are not back in the queue
        bool ended;        // a read gave 0: the link has ended
        Result() : queued(0), taken(0), putBack(0), fromQueue(0), lost(0), ended(false) {}
    };

    // If nothing is in the node's queue, one read that does not wait, which takes nothing as a rule; bytes it does
    // take are put back (putBack()). With bytes in the queue already, nothing is done. False with *why if the queue
    // cannot be looked at, the read fails or ends, or what it took cannot be put back.
    bool prime(int fd, Result* result, std::string* why);
    // Puts `read` back into the node's queue, in the order it came, with what has come into the queue since the
    // last read behind it: that is read out first, so that the phone's order is kept. Only into a raw node, and only
    // what fits in its queue. Nothing to put back is no failure, and makes no call. False with *why otherwise.
    bool putBack(int fd, const std::vector<uint8_t>& read, Result* result, std::string* why);

private:
    NodeCalls calls_;
};

}  // namespace nbt

#endif
