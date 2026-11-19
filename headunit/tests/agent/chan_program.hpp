// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_TESTS_AGENT_CHAN_PROGRAM_HPP
#define NBT_TESTS_AGENT_CHAN_PROGRAM_HPP

// What the two test programs of a channel share (chan_echo.cpp, chan_feed.cpp): the program's side of a channel as
// agent.hpp ("Channels") sets it. The program listens on a local socket, ${tmp}/channels/<name>, which it finds in
// NBT_TMP_DIR and NBT_CHANNEL as the agent starts it, or at --socket PATH. The agent connects once for each channel
// and writes one line of JSON; after that each message, either way, is two bytes of length, high byte first, and the
// bytes. One thread, one select loop, any number of connections; each says what it does on standard error, which is
// the channel's log. Built for the test bench and for the head unit, with the agent's tests.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace chan {

const size_t kMaxMessage = 4096;

struct Connection {
    int fd;
    std::string in, out;
    bool greeted;         // the agent's line has come
    std::string line;     // and this is it
    unsigned long count;  // messages so far: taken by chan_echo, sent by chan_feed
    bool closing;         // close once `out` is written
    Connection() : fd(-1), greeted(false), count(0), closing(false) {}
};

inline void say(const char* program, const std::string& text) {
    fprintf(stderr, "%s: %s\n", program, text.c_str());
    fflush(stderr);
}

inline std::string socketPath(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0) return argv[i + 1];
    }
    const char* tmp = getenv("NBT_TMP_DIR");
    const char* name = getenv("NBT_CHANNEL");
    if (tmp == 0 || name == 0) return std::string();
    return std::string(tmp) + "/channels/" + name;
}

// The socket, listening: a file left there by an earlier run is removed first. -1 if it cannot be made.
inline int listenAt(const std::string& path) {
    struct sockaddr_un address;
    if (path.empty() || path.size() >= sizeof address.sun_path) return -1;
    unlink(path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (bind(fd, (struct sockaddr*)&address, sizeof address) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

inline void frame(const std::string& message, std::string* out) {
    out->push_back((char)((message.size() >> 8) & 0xFF));
    out->push_back((char)(message.size() & 0xFF));
    out->append(message);
}

// The next whole message in `in`, taken out of it. False when there is none yet, or *bad when the length is 0 or
// over kMaxMessage.
inline bool next(std::string* in, std::string* message, bool* bad) {
    *bad = false;
    if (in->size() < 2) return false;
    size_t length = ((size_t)(unsigned char)(*in)[0] << 8) | (unsigned char)(*in)[1];
    if (length == 0 || length > kMaxMessage) {
        *bad = true;
        return false;
    }
    if (in->size() < 2 + length) return false;
    message->assign(*in, 2, length);
    in->erase(0, 2 + length);
    return true;
}

inline long long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace chan

#endif
