// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The text of a package's settings file: see settings_file.hpp.

#include "settings_file.hpp"

namespace nbt {

void SettingsFile::parse(const std::string& text) {
    lines_.clear();
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        size_t next = end == std::string::npos ? text.size() : end + 1;
        lines_.push_back(text.substr(at, next - at));
        at = next;
    }
}

bool SettingsFile::isLineOf(const std::string& line, const std::string& key) {
    return !key.empty() && line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '=';
}

bool SettingsFile::value(const std::string& key, std::string* out) const {
    for (size_t i = lines_.size(); i-- > 0;) {
        const std::string& line = lines_[i];
        if (!isLineOf(line, key)) continue;
        size_t end = line.size();
        if (end > 0 && line[end - 1] == '\n') --end;
        if (end > 0 && line[end - 1] == '\r') --end;
        size_t from = key.size() + 1;
        *out = end > from ? line.substr(from, end - from) : std::string();
        return true;
    }
    return false;
}

size_t SettingsFile::count(const std::string& key) const {
    size_t n = 0;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (isLineOf(lines_[i], key)) ++n;
    }
    return n;
}

void SettingsFile::set(const std::string& key, const std::string& value) {
    std::string line = key + "=" + value + "\n";
    bool placed = false;
    for (size_t i = 0; i < lines_.size();) {
        if (!isLineOf(lines_[i], key)) {
            ++i;
        } else if (!placed) {
            lines_[i++] = line;
            placed = true;
        } else {
            lines_.erase(lines_.begin() + (long)i);
        }
    }
    if (placed) return;
    if (!lines_.empty() && lines_.back()[lines_.back().size() - 1] != '\n') lines_.back() += '\n';
    lines_.push_back(line);
}

void SettingsFile::remove(const std::string& key) {
    for (size_t i = 0; i < lines_.size();) {
        if (isLineOf(lines_[i], key)) lines_.erase(lines_.begin() + (long)i);
        else ++i;
    }
}

std::string SettingsFile::text() const {
    std::string out;
    for (size_t i = 0; i < lines_.size(); ++i) out += lines_[i];
    return out;
}

}  // namespace nbt
