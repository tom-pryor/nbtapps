// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/semver.cpp: the forms the manifest schema allows and refuses (the same strings
// tests/test_nbtpkg.py puts to the schema), the semver 2.0.0 ordering, and ranges.

#include "../../agent/semver.hpp"

#include <assert.h>
#include <stdio.h>

using namespace nbt;

static int order(const char* a, const char* b) {
    int result = 0;
    assert(compareVersionStrings(a, b, &result));
    int back = 0;
    assert(compareVersionStrings(b, a, &back) && back == -result);
    return result;
}

int main() {
    Version v;
    Range r;

    // 1. Versions: what parses and what does not.
    const char* const good[] = {"1.2.0", "0.0.0", "1.2.0-beta.1", "10.20.30+build.5", "1.0.0-alpha+001", "1.0.0-0.3.7",
                                "1.0.0-x-y-z.--", "999999999.999999999.999999999", "1.0.0-01", 0};
    for (int i = 0; good[i] != 0; ++i) assert(parseVersion(good[i], &v));
    const char* const wrong[] = {"", "1", "1.2", "1.2.0\n", "01.2.0", "1.02.0", "1.2.00", "1.2.0-", "1.2.0+", "v1.2.0",
                                 " 1.2.0", "1.2.0 ", "1.2.0.1", "1.2.0-a..b", "1.2.0-a_b", "1.2.0+a+b",
                                 "1000000000.0.0", "-1.0.0", "1.2.x", "1..0", 0};
    for (int i = 0; wrong[i] != 0; ++i) assert(!parseVersion(wrong[i], &v));

    assert(parseVersion("12.34.56-rc.1+build.9", &v));
    assert(v.part[0] == 12 && v.part[1] == 34 && v.part[2] == 56 && v.pre.size() == 2 && v.pre[0] == "rc" && v.pre[1] == "1");

    // 2. Ordering: the chain from the semver specification, each step strictly rising.
    const char* const chain[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
                                 "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "2.0.0", "2.1.0", "2.1.1",
                                 "10.0.0", 0};
    for (int i = 0; chain[i + 1] != 0; ++i) {
        for (int j = i + 1; chain[j] != 0; ++j) assert(order(chain[i], chain[j]) < 0);
    }
    assert(order("1.2.0", "1.2.0") == 0);
    assert(order("1.2.0+build.1", "1.2.0+build.2") == 0);   // build metadata is ignored
    assert(order("1.0.0-1", "1.0.0-a") < 0);                // a number sorts before a word
    assert(order("1.0.0-2", "1.0.0-10") < 0);               // numbers compare as numbers
    assert(order("1.0.0-007", "1.0.0-7") == 0);
    assert(order("1.0.0-99999999999999999999", "1.0.0-100000000000000000000") < 0);   // of any length
    assert(order("1.0.0-a", "1.0.0-B") > 0);                // words compare as ASCII
    int unused = 0;
    assert(!compareVersionStrings("1.0", "1.0.0", &unused) && !compareVersionStrings("1.0.0", "x", &unused));

    // 3. Ranges: what parses and what does not.
    const char* const ranges[] = {">=1.0.0", "1.0.0", ">=2.0.0 <3.0.0", "=1.2.3 <=1.2.3 >1.0.0-rc.1", "<1.0.0+b", 0};
    for (int i = 0; ranges[i] != 0; ++i) assert(parseRange(ranges[i], &r));
    const char* const noRanges[] = {"", ">= 1.0.0", ">=1.0.0  <2.0.0", "^1.0.0", "~1.0.0", "1.x", ">=1.0.0 ", "*",
                                    ">=1.0.0,<2.0.0", " >=1.0.0", "=>1.0.0", "==1.0.0", ">=1.0", "<>1.0.0", 0};
    for (int i = 0; noRanges[i] != 0; ++i) assert(!parseRange(noRanges[i], &r));

    // 4. Matching: every comparator must hold.
    assert(versionInRange("2.0.0", ">=2.0.0 <3.0.0") && versionInRange("2.9.9", ">=2.0.0 <3.0.0"));
    assert(!versionInRange("3.0.0", ">=2.0.0 <3.0.0") && !versionInRange("1.9.9", ">=2.0.0 <3.0.0"));
    assert(versionInRange("1.2.3", "1.2.3") && versionInRange("1.2.3", "=1.2.3") && !versionInRange("1.2.4", "1.2.3"));
    assert(versionInRange("1.2.3+x", "=1.2.3"));
    assert(versionInRange("1.2.4", ">1.2.3") && !versionInRange("1.2.3", ">1.2.3"));
    assert(versionInRange("1.2.3", "<=1.2.3") && !versionInRange("1.2.4", "<=1.2.3"));
    assert(versionInRange("1.2.2", "<1.2.3") && !versionInRange("1.2.3", "<1.2.3"));
    // A pre-release is below its release, so it is outside a range that starts at the release.
    assert(!versionInRange("2.0.0-rc.1", ">=2.0.0") && versionInRange("2.0.0-rc.1", "<2.0.0"));
    assert(versionInRange("3.0.0-rc.1", ">=2.0.0 <3.0.0"));
    // An empty range means any version; a broken version or range matches nothing.
    assert(versionInRange("0.0.1", "") && !versionInRange("0.1", "") && !versionInRange("1.0.0", ">=1"));
    assert(!versionInRange("1.0.0", ">=2.0.0 <1.0.0"));

    printf("test_semver: ok\n");
    return 0;
}
