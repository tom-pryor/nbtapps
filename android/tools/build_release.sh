#!/bin/sh
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
# Builds the app as it goes to users, unsigned: runs the unit tests, builds the minified release, checks it, and
# leaves two files in release/:
#
#   nbtapps-<version>-unsigned.apk          the app, which no phone installs until tools/sign.sh has signed it
#   nbtapps-<version>-unsigned.apk.sha256   its SHA-256, as sha256sum writes it
#
#   tools/build_release.sh
#
# The Android release workflow on GitHub runs this script for a release, so a build here of the same commit is
# the same file, byte for byte: tools/compare_apks.py says so, and tools/sign.sh --compare requires it. The
# version is `appVersion` in app/build.gradle.kts. No key is used: android/RELEASING.md says how a release is signed.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
out="$here/release"
me=build_release.sh
. "$here/tools/apk_checks.sh"

find_tools

# There is no unit-test task for the release build: the tests run against the debug build of the same sources.
(cd "$here" && ./gradlew :app:testDebugUnitTest :app:assembleRelease)

apk="$here/app/build/outputs/apk/release/app-release-unsigned.apk"
[ -f "$apk" ] || fail "the build left no unsigned APK at $apk"
check_unsigned "$apk"
check_apk "$apk"

mkdir -p "$out"
file="nbtapps-$version-unsigned.apk"
cp "$apk" "$out/$file"
(cd "$out" && sha256sum "$file" > "$file.sha256")
size=$(wc -c < "$out/$file" | tr -d ' ')

echo
echo "release $version (version code $code), unsigned, $size bytes"
echo "  $out/$file"
echo "  SHA-256 $(cut -d ' ' -f 1 "$out/$file.sha256")"
