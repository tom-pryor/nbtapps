#!/bin/sh
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
# Builds the app as it goes to users, and says what was built: runs the unit tests, builds the minified release
# signed with the release key, checks the signature, and leaves two files in release/:
#
#   nbtapps-<version>.apk   the app
#   release.json            its package name, version code and name, lowest Android API level, size, SHA-256,
#                           and the SHA-256 of the certificate it is signed with
#
# release.json is read off the finished APK, not off the build files, so it describes the file beside it. The
# catalogue repo records a release from it. The version is `appVersion` in app/build.gradle.kts: raise it before
# every release, or no phone takes the new build as an update.
#
#   tools/release.sh
#
# The release key is not in this repo. It is made once, and every release is signed with it:
#
#   keytool -genkeypair -keystore ~/.config/nbtapps/keys/android-release.jks -alias nbtapps \
#       -keyalg RSA -keysize 4096 -validity 36500 -dname "CN=NBTApps"
#
# and ~/.config/nbtapps/keys/android-release.properties (or the file NBTAPPS_SIGNING names) says how it is opened:
#
#   storeFile=android-release.jks
#   storePassword=...
#   keyAlias=nbtapps
#
# (keytool asks for one password, which opens the store and the key. A key with a password of its own has it in a
# fourth line, keyPassword=...)
#
# Keep a copy of the key somewhere else. Android takes an update only from the key the installed app was signed
# with, so if it is lost no installed app can be updated again.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
out="$here/release"
signing=${NBTAPPS_SIGNING:-"$HOME/.config/nbtapps/keys/android-release.properties"}
package=com.nbtapps.store

fail() {
    echo "release.sh: $*" >&2
    exit 1
}

[ -f "$signing" ] || fail "there is no release key: $signing is missing. The head of this script says how to make one."

sdk=${ANDROID_HOME:-}
if [ -z "$sdk" ] && [ -f "$here/local.properties" ]; then
    sdk=$(sed -n 's/^sdk\.dir=//p' "$here/local.properties")
fi
[ -n "$sdk" ] && [ -d "$sdk/build-tools" ] || fail "no Android SDK: set ANDROID_HOME, or sdk.dir in local.properties"
tools=$(ls -d "$sdk"/build-tools/*/ | sort -V | tail -n 1)
[ -x "$tools/apksigner" ] && [ -x "$tools/aapt2" ] || fail "no apksigner and aapt2 in $tools"

# There is no unit-test task for the release build: the tests run against the debug build of the same sources.
(cd "$here" && NBTAPPS_SIGNING="$signing" ./gradlew :app:testDebugUnitTest :app:assembleRelease)

apk="$here/app/build/outputs/apk/release/app-release.apk"
[ -f "$apk" ] || fail "the build left no signed APK at $apk"

# The signature is checked as a phone would check it, and the certificate is read from the APK itself.
certs=$("$tools/apksigner" verify --print-certs "$apk" 2>/dev/null) || {
    "$tools/apksigner" verify --print-certs "$apk" >&2 || true
    fail "the APK's signature does not verify"
}
case $certs in
    *"Signer #2"*) fail "the APK has more than one signer" ;;
    *"CN=Android Debug"*) fail "the APK is signed with a debug key, not the release key" ;;
esac
cert=$(printf '%s\n' "$certs" | sed -n 's/^Signer #1 certificate SHA-256 digest: \([0-9a-f]\{64\}\)$/\1/p')
[ -n "$cert" ] || fail "could not read the signing certificate's SHA-256"

badging=$("$tools/aapt2" dump badging "$apk")
field() {
    printf '%s\n' "$badging" | sed -n "s/^package:.* $1='\([^']*\)'.*/\1/p"
}
name=$(field name)
code=$(field versionCode)
version=$(field versionName)
min_sdk=$(printf '%s\n' "$badging" | sed -n "s/^minSdkVersion:'\([0-9]*\)'.*/\1/p")

[ "$name" = "$package" ] || fail "the APK is the package '$name', not $package"
printf '%s\n' "$code" | grep -Eqx '[1-9][0-9]*' || fail "the APK's version code is not a number: '$code'"
printf '%s\n' "$version" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+' || fail "the APK's version is not a release's: '$version'"
[ -n "$min_sdk" ] || fail "could not read the APK's lowest Android API level"
case $badging in
    *application-debuggable*) fail "the APK is debuggable" ;;
esac

mkdir -p "$out"
file="nbtapps-$version.apk"
cp "$apk" "$out/$file"
size=$(wc -c < "$out/$file" | tr -d ' ')
sha=$(sha256sum "$out/$file" | cut -d ' ' -f 1)

cat > "$out/release.json" <<JSON
{
  "package": "$name",
  "versionCode": $code,
  "versionName": "$version",
  "minSdk": $min_sdk,
  "size": $size,
  "sha256": "$sha",
  "certSha256": "$cert",
  "file": "$file"
}
JSON

echo
echo "release $version (version code $code), $size bytes"
echo "  $out/$file"
echo "  $out/release.json"
