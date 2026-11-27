// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBTAPP_CALL_LIMIT_HPP
#define NBTAPP_CALL_LIMIT_HPP

// A time limit on a call of the plugin's that can wait in the kernel, for the calls it makes on the HMI's thread.
//
// All of the plugin's work runs on one thread of the HMI's, the one that also drives the iDrive screen: its handlers,
// its timer and create(). A call of ours that waits there stops all of iDrive until it returns. On a head unit,
// limitNextCall() arms a kernel time limit (TimerTimeout) of kCallLimitMs on the thread's next kernel call, as the
// agent bounds its reads of the serial node (agent/node_reader.cpp). A call still waiting to be taken by the server
// it was sent to, or for that server's answer, when the time is up comes back with an error (ETIMEDOUT), and the
// caller takes that as a failure of the call, as it takes any other. The limit is for the next kernel call only, so
// it is armed just before each call it is for, with nothing in between that goes to the kernel.
//
// What it does not cover:
//   - A call of the C library that is several kernel calls has the limit on its first only. open() and stat() of
//     a path first ask the process manager which server has the path, and then ask that server: the limit is on
//     the first question. read() of an open file, fcntl() of its flags, and connect() of a socket are one message
//     to one server, and the limit is on all of it.
//   - A server that has taken the message decides when to let the caller go once the time is up: the kernel tells
//     it, and one that is busy with the message answers when it is done. A server that has not taken it, because
//     all of its threads are busy or stuck, lets the caller go at once.
//   - spawn() has none: agent_start.hpp says why. Nor has close(), nor the reads and writes of a link to the agent
//     once it is made, which are of a socket that does not wait for its peer.
//
// On a build host it does nothing, and the calls are as they were.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#ifdef __QNXNTO__
#include <sys/neutrino.h>
#include <time.h>
#endif

namespace nbt {

const unsigned kCallLimitMs = 50;

inline void limitNextCall() {
#ifdef __QNXNTO__
    // TimerTimeout_r returns its error and leaves errno alone. One that cannot be armed leaves the call with no
    // limit, as it had before there was one.
    _Uint64t ns = (_Uint64t)kCallLimitMs * 1000000u;
    (void)TimerTimeout_r(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY | _NTO_TIMEOUT_RECEIVE, NULL, &ns, NULL);
#endif
}

}  // namespace nbt

#endif
