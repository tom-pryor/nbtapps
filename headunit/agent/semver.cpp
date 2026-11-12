// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Versions and ranges: see semver.hpp.

#include "semver.hpp"

namespace nbt {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdChar(char c) { return isDigit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-'; }

// 0, or one to nine digits with no leading zero.
bool number(const std::string& text, size_t* at, uint32_t* out) {
    size_t start = *at;
    uint32_t value = 0;
    while (*at < text.size() && isDigit(text[*at])) {
        if (*at - start >= 9) return false;
        value = value * 10 + (uint32_t)(text[*at] - '0');
        ++*at;
    }
    if (*at == start) return false;
    if (text[start] == '0' && *at - start > 1) return false;
    *out = value;
    return true;
}

// One or more identifiers of [0-9A-Za-z-], separated by dots.
bool identifiers(const std::string& text, size_t* at, std::vector<std::string>* out) {
    for (;;) {
        size_t start = *at;
        while (*at < text.size() && isIdChar(text[*at])) ++*at;
        if (*at == start) return false;
        if (out != 0) out->push_back(text.substr(start, *at - start));
        if (*at < text.size() && text[*at] == '.') {
            ++*at;
            continue;
        }
        return true;
    }
}

// A version starting at *at; stops at the first character that cannot continue it.
bool versionAt(const std::string& text, size_t* at, Version* out) {
    Version v;
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            if (*at >= text.size() || text[*at] != '.') return false;
            ++*at;
        }
        if (!number(text, at, &v.part[i])) return false;
    }
    if (*at < text.size() && text[*at] == '-') {
        ++*at;
        if (!identifiers(text, at, &v.pre)) return false;
    }
    if (*at < text.size() && text[*at] == '+') {
        ++*at;
        if (!identifiers(text, at, 0)) return false;
    }
    *out = v;
    return true;
}

bool allDigits(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (!isDigit(text[i])) return false;
    }
    return true;
}

// Numeric identifiers compare as numbers, whatever their length: without leading zeros, the longer one is larger.
int compareNumeric(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i + 1 < a.size() && a[i] == '0') ++i;
    while (j + 1 < b.size() && b[j] == '0') ++j;
    size_t lenA = a.size() - i, lenB = b.size() - j;
    if (lenA != lenB) return lenA < lenB ? -1 : 1;
    return a.compare(i, lenA, b, j, lenB);
}

}  // namespace

bool parseVersion(const std::string& text, Version* out) {
    size_t at = 0;
    Version v;
    if (!versionAt(text, &at, &v) || at != text.size()) return false;
    *out = v;
    return true;
}

int compareVersions(const Version& a, const Version& b) {
    for (int i = 0; i < 3; ++i) {
        if (a.part[i] != b.part[i]) return a.part[i] < b.part[i] ? -1 : 1;
    }
    // A release sorts after every pre-release of the same three numbers.
    if (a.pre.empty() || b.pre.empty()) {
        if (a.pre.empty() && b.pre.empty()) return 0;
        return a.pre.empty() ? 1 : -1;
    }
    for (size_t i = 0; i < a.pre.size() && i < b.pre.size(); ++i) {
        bool numA = allDigits(a.pre[i]), numB = allDigits(b.pre[i]);
        int order;
        if (numA && numB) order = compareNumeric(a.pre[i], b.pre[i]);
        else if (numA != numB) order = numA ? -1 : 1;   // a number sorts before a word
        else order = a.pre[i].compare(b.pre[i]);
        if (order != 0) return order < 0 ? -1 : 1;
    }
    if (a.pre.size() != b.pre.size()) return a.pre.size() < b.pre.size() ? -1 : 1;
    return 0;
}

bool parseRange(const std::string& text, Range* out) {
    Range range;
    size_t at = 0;
    for (;;) {
        Comparator c;
        c.op = Comparator::kEqual;
        if (text.compare(at, 2, ">=") == 0) {
            c.op = Comparator::kGreaterEqual;
            at += 2;
        } else if (text.compare(at, 2, "<=") == 0) {
            c.op = Comparator::kLessEqual;
            at += 2;
        } else if (at < text.size() && text[at] == '>') {
            c.op = Comparator::kGreater;
            at += 1;
        } else if (at < text.size() && text[at] == '<') {
            c.op = Comparator::kLess;
            at += 1;
        } else if (at < text.size() && text[at] == '=') {
            at += 1;
        }
        if (!versionAt(text, &at, &c.version)) return false;
        range.all.push_back(c);
        if (at == text.size()) break;
        if (text[at] != ' ') return false;
        ++at;
    }
    *out = range;
    return true;
}

bool rangeMatches(const Range& range, const Version& version) {
    for (size_t i = 0; i < range.all.size(); ++i) {
        int order = compareVersions(version, range.all[i].version);
        bool holds = false;
        switch (range.all[i].op) {
        case Comparator::kEqual: holds = order == 0; break;
        case Comparator::kLess: holds = order < 0; break;
        case Comparator::kLessEqual: holds = order <= 0; break;
        case Comparator::kGreater: holds = order > 0; break;
        case Comparator::kGreaterEqual: holds = order >= 0; break;
        }
        if (!holds) return false;
    }
    return true;
}

bool versionInRange(const std::string& version, const std::string& range) {
    Version v;
    if (!parseVersion(version, &v)) return false;
    if (range.empty()) return true;
    Range r;
    return parseRange(range, &r) && rangeMatches(r, v);
}

bool compareVersionStrings(const std::string& a, const std::string& b, int* order) {
    Version va, vb;
    if (!parseVersion(a, &va) || !parseVersion(b, &vb)) return false;
    *order = compareVersions(va, vb);
    return true;
}

}  // namespace nbt
