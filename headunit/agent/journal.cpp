// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The journal file: see journal.hpp.

#include "journal.hpp"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "fsutil.hpp"

namespace nbt {

bool Journal::create(const std::string& path, std::string* error) {
    close();
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0600);
    if (fd_ < 0) {
        if (error != 0) *error = "cannot start the journal " + path + ": " + strerror(errno);
        return false;
    }
    path_ = path;
    fs::syncDir(fs::parentOf(path));
    return true;
}

bool Journal::reopen(const std::string& path, std::string* error) {
    close();
    fd_ = ::open(path.c_str(), O_RDWR | O_APPEND);
    if (fd_ < 0) {
        if (error != 0) *error = "cannot open the journal " + path + ": " + strerror(errno);
        return false;
    }
    char last = 0;
    off_t size = lseek(fd_, 0, SEEK_END);
    if (size < 1 || pread(fd_, &last, 1, size - 1) != 1 || last != '\n') {
        close();
        if (error != 0) *error = "the last line of the journal " + path + " is not whole";
        return false;
    }
    path_ = path;
    return true;
}

bool Journal::append(const std::string& line, std::string* error) {
    std::string text = line + "\n";
    const char* p = text.data();
    size_t left = text.size();
    while (fd_ >= 0 && left > 0) {
        ssize_t n = ::write(fd_, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        p += n;
        left -= (size_t)n;
    }
    if (fd_ < 0 || left > 0 || !fs::syncFd(fd_)) {
        if (error != 0) *error = "cannot write the journal " + path_ + ": " + strerror(errno);
        return false;
    }
    return true;
}

void Journal::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool Journal::read(const std::string& path, std::vector<std::string>* lines, std::string* error) {
    std::string text;
    lines->clear();
    if (!fs::readFile(path, 64u << 20, &text, error)) return false;
    size_t start = 0;
    for (;;) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) break;   // what follows the last newline was cut off
        lines->push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return true;
}

}  // namespace nbt
