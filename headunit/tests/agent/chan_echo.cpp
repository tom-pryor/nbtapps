// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// A test program of a channel (chan_program.hpp): sends back each message it gets, on the connection it came on.
// The channel `echo` of the test package chan, and with --upper its channel `gated`.
//
//   chan_echo [--socket PATH] [--upper] [--slow MS] [--close-after N] [--no-listen] [--ignore-term]
//
//   --upper          sends the message back with a-z as A-Z
//   --slow MS        reads a connection's next message only MS milliseconds after the last: a program that reads slowly
//   --close-after N  closes a connection once it has sent back N messages
//   --no-listen      listens on nothing, and waits to be ended: a program that never takes a connection
//   --ignore-term    ignores SIGTERM, so that only SIGKILL ends it
//
// While a file is at its socket's path with ".hold" added (the socket in $NBT_TMP_DIR/channels/echo: echo.hold), it
// goes on taking messages and sends nothing back: what it would send waits, and goes once the file has been removed.
// So a test makes a program whose answers come long after it has taken the messages, with no option in the manifest.
//
// It runs until it is ended. It exits with 2 when it cannot listen.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include "chan_program.hpp"

using namespace chan;

int main(int argc, char** argv) {
    bool upper = false, noListen = false;
    long slowMs = 0, closeAfter = -1;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--upper") upper = true;
        else if (arg == "--no-listen") noListen = true;
        else if (arg == "--ignore-term") signal(SIGTERM, SIG_IGN);
        else if (arg == "--slow" && i + 1 < argc) slowMs = atol(argv[++i]);
        else if (arg == "--close-after" && i + 1 < argc) closeAfter = atol(argv[++i]);
        else if (arg == "--socket" && i + 1 < argc) ++i;
        else {
            say("chan_echo", "unknown argument " + arg);
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    if (noListen) {
        say("chan_echo", "listening on nothing, as asked");
        for (;;) pause();
    }
    std::string path = socketPath(argc, argv);
    int listener = listenAt(path);
    if (listener < 0) {
        say("chan_echo", "cannot listen at '" + path + "': " + strerror(errno));
        return 2;
    }
    say("chan_echo", "listening at " + path);
    const std::string holdPath = path + ".hold";
    std::vector<Connection> connections;
    std::vector<long long> readAfter;   // per connection: no read before this time
    for (;;) {
        fd_set readable, writable;
        FD_ZERO(&readable);
        FD_ZERO(&writable);
        FD_SET(listener, &readable);
        int top = listener;
        long long now = nowMs(), wait = 1000;
        bool held = access(holdPath.c_str(), F_OK) == 0;
        for (size_t i = 0; i < connections.size(); ++i) {
            Connection& c = connections[i];
            if (!c.closing && now >= readAfter[i]) FD_SET(c.fd, &readable);
            if (!c.closing && now < readAfter[i] && readAfter[i] - now < wait) wait = readAfter[i] - now;
            if (!c.out.empty() && !held) FD_SET(c.fd, &writable);
            // Answers that wait: the file is looked for again soon.
            if (!c.out.empty() && held && wait > 50) wait = 50;
            if (c.fd > top) top = c.fd;
        }
        struct timeval tv;
        tv.tv_sec = (long)(wait / 1000);
        tv.tv_usec = (long)(wait % 1000) * 1000;
        if (select(top + 1, &readable, &writable, 0, &tv) < 0 && errno != EINTR) return 1;
        if (FD_ISSET(listener, &readable)) {
            int fd = accept(listener, 0, 0);
            if (fd >= 0) {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                Connection c;
                c.fd = fd;
                connections.push_back(c);
                readAfter.push_back(0);
            }
        }
        for (size_t i = 0; i < connections.size(); ++i) {
            Connection& c = connections[i];
            bool gone = false;
            if (FD_ISSET(c.fd, &writable)) {
                ssize_t n = write(c.fd, c.out.data(), c.out.size());
                if (n > 0) c.out.erase(0, (size_t)n);
                else if (n < 0 && errno != EAGAIN && errno != EINTR) gone = true;
            }
            if (!gone && FD_ISSET(c.fd, &readable)) {
                char buffer[8192];
                // When reading slowly, no more than the line or the one message: the rest stays with the agent.
                size_t want = sizeof buffer;
                if (slowMs > 0 && !c.greeted) {
                    want = 1;
                } else if (slowMs > 0 && c.in.size() < 2) {
                    want = 2 - c.in.size();
                } else if (slowMs > 0) {
                    size_t length = ((size_t)(unsigned char)c.in[0] << 8) | (unsigned char)c.in[1];
                    want = 2 + length > c.in.size() ? 2 + length - c.in.size() : 1;
                }
                ssize_t n = read(c.fd, buffer, want);
                if (n > 0) c.in.append(buffer, (size_t)n);
                else if (n == 0 || (errno != EAGAIN && errno != EINTR)) gone = true;
            }
            if (!c.greeted) {
                size_t end = c.in.find('\n');
                if (end != std::string::npos) {
                    c.line = c.in.substr(0, end);
                    c.in.erase(0, end + 1);
                    c.greeted = true;
                    say("chan_echo", "connection: " + c.line);
                }
            }
            std::string message;
            bool bad = false;
            while (c.greeted && !c.closing && next(&c.in, &message, &bad)) {
                if (upper) {
                    for (size_t k = 0; k < message.size(); ++k) {
                        if (message[k] >= 'a' && message[k] <= 'z') message[k] = (char)(message[k] - 'a' + 'A');
                    }
                }
                frame(message, &c.out);
                ++c.count;
                if (closeAfter >= 0 && (long)c.count >= closeAfter) c.closing = true;
                if (slowMs > 0) {
                    readAfter[i] = nowMs() + slowMs;
                    break;
                }
            }
            if (bad) {
                say("chan_echo", "a message of a length that is not allowed: the connection is closed");
                gone = true;
            }
            if (c.closing && c.out.empty()) gone = true;
            if (gone) {
                char count[32];
                snprintf(count, sizeof count, "%lu", c.count);
                say("chan_echo", std::string("connection closed after ") + count + " messages");
                close(c.fd);
                connections.erase(connections.begin() + (long)i);
                readAfter.erase(readAfter.begin() + (long)i);
                --i;
            }
        }
    }
}
