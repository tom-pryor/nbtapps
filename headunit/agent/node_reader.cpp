// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The reader of a serial node: see node_reader.hpp.

#include "node_reader.hpp"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __QNXNTO__
#include <sys/neutrino.h>
#endif

namespace nbt {

namespace {

double monoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

// `ms` from now, as pthread_cond_timedwait takes it.
struct timespec deadlineIn(int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ++ts.tv_sec;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

bool timeUp(int error) { return error == EINTR || error == ETIMEDOUT || error == EAGAIN; }

// Whether a read whose time was up came back then is said once for the whole process: the first time it happens.
pthread_mutex_t gOnTimeMu = PTHREAD_MUTEX_INITIALIZER;
bool gOnTimeSaid = false;

bool firstTimeUp() {
    pthread_mutex_lock(&gOnTimeMu);
    bool first = !gOnTimeSaid;
    gOnTimeSaid = true;
    pthread_mutex_unlock(&gOnTimeMu);
    return first;
}

}  // namespace

long timedNodeRead(int fd, void* buf, size_t len, int ms) {
#ifdef __QNXNTO__
    // The limit is for the next blocking kernel call, which has to be the read: nothing comes between the two.
    _Uint64t ns = (_Uint64t)ms * 1000000u;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &ns, NULL);
    return (long)read(fd, buf, len);
#else
    // A pseudo-terminal, in the tests, says truthfully when it has bytes.
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    int r = select(fd + 1, &rf, NULL, NULL, &tv);
    if (r < 0) return -1;
    if (r == 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    return (long)read(fd, buf, len);
#endif
}

NodeReader::NodeReader(int fd, const std::string& path, SerialLog* log, NodeReadFn read)
    : fd_(fd), path_(path), log_(log), read_(read != 0 ? read : &timedNodeRead), drain_(false), stop_(false),
      ended_(false), done_(false), released_(false), error_(0), dropped_(0) {
    pthread_mutex_init(&mu_, NULL);
    pthread_cond_init(&cv_, NULL);
}

NodeReader::~NodeReader() {
    pthread_cond_destroy(&cv_);
    pthread_mutex_destroy(&mu_);
}

NodeReader* NodeReader::start(int fd, const std::string& path, SerialLog* log, NodeReadFn read) {
    NodeReader* r = new NodeReader(fd, path, log, read);
    if (pthread_create(&r->thread_, NULL, &NodeReader::threadMain, r) != 0) {
        log->line(path + ": no thread to read it: closed");
        close(fd);
        delete r;
        return 0;
    }
    return r;
}

void* NodeReader::threadMain(void* self) {
    static_cast<NodeReader*>(self)->run();
    return NULL;
}

void NodeReader::run() {
    std::vector<uint8_t> buf(kNodeReadBytes);
    for (;;) {
        pthread_mutex_lock(&mu_);
        if (stop_) {
            pthread_mutex_unlock(&mu_);
            break;
        }
        if (!drain_ && queue_.size() >= kNodeQueueMost) {
            struct timespec until = deadlineIn(kNodeTickMs);
            pthread_cond_timedwait(&cv_, &mu_, &until);
            pthread_mutex_unlock(&mu_);
            continue;
        }
        pthread_mutex_unlock(&mu_);

        double asked = monoNow();
        long k = read_(fd_, &buf[0], buf.size(), kNodeTickMs);
        int error = errno;
        if (k > 0) {
            pthread_mutex_lock(&mu_);
            if (drain_) dropped_ += (uint64_t)k;
            else queue_.insert(queue_.end(), buf.begin(), buf.begin() + k);
            pthread_cond_broadcast(&cv_);
            pthread_mutex_unlock(&mu_);
            continue;
        }
        if (k < 0 && timeUp(error)) {
            if (firstTimeUp()) {
                double took = (monoNow() - asked) * 1000.0;
                pthread_mutex_lock(&mu_);
                bool mine = !released_;
                pthread_mutex_unlock(&mu_);
                char line[200];
                snprintf(line, sizeof line, ": a read with nothing to read came back when its time was up, %s: "
                         "%.0f ms after it was issued with a limit of %d ms", took <= 3.0 * kNodeTickMs ? "on time" : "LATE",
                         took, kNodeTickMs);
                if (mine) log_->line(path_ + line);
            }
            continue;
        }
        // 0, or an error of any other kind: the link has ended, or the node has been closed under this thread.
        pthread_mutex_lock(&mu_);
        ended_ = true;
        error_ = k == 0 ? 0 : error;
        pthread_cond_broadcast(&cv_);
        pthread_mutex_unlock(&mu_);
        break;
    }
    pthread_mutex_lock(&mu_);
    done_ = true;
    bool released = released_;
    pthread_cond_broadcast(&cv_);
    pthread_mutex_unlock(&mu_);
    // Its owner has gone, and left the object to the thread.
    if (released) delete this;
}

NodeReader::Got NodeReader::next(int ms, std::vector<uint8_t>* out) {
    pthread_mutex_lock(&mu_);
    if (queue_.empty() && !ended_) {
        struct timespec until = deadlineIn(ms);
        while (queue_.empty() && !ended_) {
            if (pthread_cond_timedwait(&cv_, &mu_, &until) == ETIMEDOUT) break;
        }
    }
    Got got = kNothing;
    if (!queue_.empty()) {
        out->insert(out->end(), queue_.begin(), queue_.end());
        queue_.clear();
        pthread_cond_broadcast(&cv_);   // a reader that waited for room
        got = kData;
    } else if (ended_) {
        got = kEnded;
    }
    pthread_mutex_unlock(&mu_);
    return got;
}

int NodeReader::endError() {
    pthread_mutex_lock(&mu_);
    int e = error_;
    pthread_mutex_unlock(&mu_);
    return e;
}

bool NodeReader::ended() {
    pthread_mutex_lock(&mu_);
    bool e = ended_;
    pthread_mutex_unlock(&mu_);
    return e;
}

void NodeReader::drain() {
    pthread_mutex_lock(&mu_);
    drain_ = true;
    dropped_ += (uint64_t)queue_.size();
    queue_.clear();
    pthread_cond_broadcast(&cv_);
    pthread_mutex_unlock(&mu_);
}

uint64_t NodeReader::dropped() {
    pthread_mutex_lock(&mu_);
    uint64_t d = dropped_;
    pthread_mutex_unlock(&mu_);
    return d;
}

int NodeReader::stop() {
    pthread_mutex_lock(&mu_);
    stop_ = true;
    pthread_cond_broadcast(&cv_);
    // Three ticks: the read that is out has a tick to come back in, and the thread then sees the stop.
    struct timespec until = deadlineIn(3 * kNodeTickMs);
    while (!done_) {
        if (pthread_cond_timedwait(&cv_, &mu_, &until) == ETIMEDOUT) break;
    }
    if (done_) {
        pthread_mutex_unlock(&mu_);
        pthread_join(thread_, NULL);
        int fd = fd_;
        delete this;
        return fd;
    }
    released_ = true;
    int fd = fd_;
    std::string path = path_;
    SerialLog* log = log_;
    pthread_mutex_unlock(&mu_);
    pthread_detach(thread_);
    // From here the thread may free the object at any moment: only the copies are used.
    char line[160];
    snprintf(line, sizeof line, ": its read did not come back within %d ms of being asked to stop, so the node is "
             "closed under it", 3 * kNodeTickMs);
    log->line(path + line);
    close(fd);
    return -1;
}

void NodeReader::finish() {
    int fd = stop();
    if (fd >= 0) close(fd);
}

}  // namespace nbt
