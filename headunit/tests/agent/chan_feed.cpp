// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// A test program of a channel (chan_program.hpp): on each connection it writes numbered messages, "message 1 of N",
// as fast as the connection takes them, and closes the connection after the last. The channel `feed` of the test
// package chan, which is read only. What comes the other way is read and dropped.
//
//   chan_feed [--socket PATH] [--size BYTES] COUNT
//
//   COUNT          the messages on each connection; 0 for no end
//   --size BYTES   each message filled up to BYTES with dots (1 to 4096), after its words
//
// It runs until it is ended. It exits with 2 when it cannot listen.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include "chan_program.hpp"

using namespace chan;

int main(int argc, char** argv) {
    long count = -1, size = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--size" && i + 1 < argc) size = atol(argv[++i]);
        else if (arg == "--socket" && i + 1 < argc) ++i;
        else if (count < 0 && !arg.empty() && arg[0] >= '0' && arg[0] <= '9') count = atol(arg.c_str());
        else {
            say("chan_feed", "unknown argument " + arg);
            return 2;
        }
    }
    if (count < 0 || size < 0 || size > (long)kMaxMessage) {
        say("chan_feed", "usage: chan_feed [--socket PATH] [--size BYTES] COUNT");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    std::string path = socketPath(argc, argv);
    int listener = listenAt(path);
    if (listener < 0) {
        say("chan_feed", "cannot listen at '" + path + "': " + strerror(errno));
        return 2;
    }
    say("chan_feed", "listening at " + path);
    std::vector<Connection> connections;
    for (;;) {
        fd_set readable, writable;
        FD_ZERO(&readable);
        FD_ZERO(&writable);
        FD_SET(listener, &readable);
        int top = listener;
        for (size_t i = 0; i < connections.size(); ++i) {
            FD_SET(connections[i].fd, &readable);
            if (connections[i].greeted) FD_SET(connections[i].fd, &writable);
            if (connections[i].fd > top) top = connections[i].fd;
        }
        if (select(top + 1, &readable, &writable, 0, 0) < 0 && errno != EINTR) return 1;
        if (FD_ISSET(listener, &readable)) {
            int fd = accept(listener, 0, 0);
            if (fd >= 0) {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                Connection c;
                c.fd = fd;
                connections.push_back(c);
            }
        }
        for (size_t i = 0; i < connections.size(); ++i) {
            Connection& c = connections[i];
            bool gone = false;
            if (FD_ISSET(c.fd, &readable)) {
                char buffer[8192];
                ssize_t n = read(c.fd, buffer, sizeof buffer);
                if (n > 0 && !c.greeted) c.in.append(buffer, (size_t)n);
                else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) gone = true;
                size_t end = c.in.find('\n');
                if (!c.greeted && end != std::string::npos) {
                    c.line = c.in.substr(0, end);
                    c.in.clear();
                    c.greeted = true;
                    say("chan_feed", "connection: " + c.line);
                }
            }
            // A few messages at a time, so that the queue here stays short.
            while (!gone && c.greeted && c.out.size() < 4 * (2 + kMaxMessage) && (count == 0 || (long)c.count < count)) {
                ++c.count;
                char words[64];
                snprintf(words, sizeof words, "message %lu of %ld", c.count, count);
                std::string message = words;
                if ((long)message.size() < size) message.append((size_t)(size - (long)message.size()), '.');
                frame(message, &c.out);
            }
            if (!gone && !c.out.empty() && FD_ISSET(c.fd, &writable)) {
                ssize_t n = write(c.fd, c.out.data(), c.out.size());
                if (n > 0) c.out.erase(0, (size_t)n);
                else if (n < 0 && errno != EAGAIN && errno != EINTR) gone = true;
            }
            if (!gone && c.greeted && c.out.empty() && count != 0 && (long)c.count >= count) gone = true;
            if (gone) {
                char sent[32];
                snprintf(sent, sizeof sent, "%lu", c.count);
                say("chan_feed", std::string("connection closed after ") + sent + " messages");
                close(c.fd);
                connections.erase(connections.begin() + (long)i);
                --i;
            }
        }
    }
}
