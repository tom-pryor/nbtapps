// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The problem file of Bluetooth serial: see problem_log.hpp.

#include "problem_log.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fsutil.hpp"

namespace nbt {

ProblemLog::ProblemLog(const std::string& path, const std::string& who)
    : path_(path), who_(who), recorded_(0), counted_(0) {
    pthread_mutex_init(&mu_, NULL);
}

ProblemLog::~ProblemLog() { pthread_mutex_destroy(&mu_); }

void ProblemLog::note(const std::string& kind, const std::string& text) {
    time_t now = time(0);
    struct tm when;
    localtime_r(&now, &when);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &when);
    std::string line = std::string(stamp) + " agent " + who_ + ": " + text;
    // One line, printable, and short.
    for (size_t i = 0; i < line.size(); ++i) {
        unsigned char c = (unsigned char)line[i];
        if (c < 0x20 || c >= 0x7f) line[i] = '?';
    }
    if (line.size() > (size_t)kMostLineBytes - 1) line = line.substr(0, (size_t)kMostLineBytes - 4) + "...";
    pthread_mutex_lock(&mu_);
    bool write = kinds_.count(kind) == 0 && recorded_ < (unsigned)kMostLines;
    if (write) {
        kinds_.insert(kind);
        queued_.push_back(line + "\n");
        ++recorded_;
    } else {
        ++counted_;
    }
    pthread_mutex_unlock(&mu_);
}

std::string ProblemLog::withLine(const std::string& text, const std::string& line) {
    if (text.size() + line.size() <= (size_t)kMostBytes) return text + line;
    // The older half of the lines goes: from the first line that starts after the middle.
    size_t cut = text.find('\n', text.size() / 2);
    std::string kept = cut == std::string::npos ? std::string() : text.substr(cut + 1);
    if (kept.size() + line.size() > (size_t)kMostBytes) kept.clear();
    return kept + line;
}

bool ProblemLog::flush(std::string* why) {
    std::vector<std::string> lines;
    pthread_mutex_lock(&mu_);
    lines.swap(queued_);
    pthread_mutex_unlock(&mu_);
    if (lines.empty()) return true;
    std::string text, more;
    if (fs::kindOf(path_) == fs::kFile && !fs::readFile(path_, (size_t)kMostBytes * 2, &text, why)) text.clear();
    for (size_t i = 0; i < lines.size(); ++i) more += lines[i];
    std::string whole = text;
    for (size_t i = 0; i < lines.size(); ++i) whole = withLine(whole, lines[i]);
    bool ok;
    if (whole == text + more) {
        // Room for them: added at the end.
        int fd = open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
        ok = fd >= 0 && write(fd, more.data(), more.size()) == (ssize_t)more.size();
        if (!ok && why != 0) *why = path_ + ": " + strerror(errno);
        if (fd >= 0) close(fd);
    } else {
        ok = fs::writeFile(path_, whole, 0644, why);
    }
    if (ok) return true;
    pthread_mutex_lock(&mu_);
    recorded_ -= (unsigned)lines.size();
    counted_ += (unsigned)lines.size();
    pthread_mutex_unlock(&mu_);
    return false;
}

unsigned ProblemLog::recorded() {
    pthread_mutex_lock(&mu_);
    unsigned n = recorded_;
    pthread_mutex_unlock(&mu_);
    return n;
}

unsigned ProblemLog::counted() {
    pthread_mutex_lock(&mu_);
    unsigned n = counted_;
    pthread_mutex_unlock(&mu_);
    return n;
}

}  // namespace nbt
