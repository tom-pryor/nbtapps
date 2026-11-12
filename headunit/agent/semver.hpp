// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SEMVER_HPP
#define NBT_AGENT_SEMVER_HPP

// Package versions and version ranges, in the forms spec/manifest.schema.json allows and nothing wider:
//
//   version   1.2.0, 1.2.0-beta.1, 1.2.0+build.5   three numbers of at most nine digits with no leading zero
//   range     >=2.0.0 <3.0.0                       comparators separated by one space; all of them must hold
//
// A comparator is a version with an optional operator in front (>=, >, <=, <, =); no operator means =.
// Versions order as semver 2.0.0 says: a pre-release sorts before its release, and build metadata is ignored.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace nbt {

struct Version {
    uint32_t part[3];               // major, minor, patch
    std::vector<std::string> pre;   // pre-release identifiers; empty for a release

    Version() { part[0] = part[1] = part[2] = 0; }
};

// False unless `text` is exactly a version. Build metadata is checked and then dropped.
bool parseVersion(const std::string& text, Version* out);

// Negative, zero or positive as a sorts before, equal to or after b.
int compareVersions(const Version& a, const Version& b);

struct Comparator {
    enum Op { kEqual, kLess, kLessEqual, kGreater, kGreaterEqual };
    Op op;
    Version version;
};

struct Range {
    std::vector<Comparator> all;
};

// False unless `text` is exactly a range.
bool parseRange(const std::string& text, Range* out);
bool rangeMatches(const Range& range, const Version& version);

// For strings that were checked when their manifest was read. An empty range matches every version; a string that
// does not parse matches nothing.
bool versionInRange(const std::string& version, const std::string& range);
// Compares two version strings; false if either does not parse.
bool compareVersionStrings(const std::string& a, const std::string& b, int* order);

}  // namespace nbt

#endif
