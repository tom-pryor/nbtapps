# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
# What build_release.sh and sign.sh both do to an APK, sourced by them (`. tools/apk_checks.sh`). Needs `here`
# (the android folder) and `me` (the script's name, for its messages) to be set first.

package=com.nbtapps.store

fail() {
    echo "$me: $*" >&2
    exit 1
}

# Sets `tools` to the newest build-tools of the Android SDK: ANDROID_HOME, or sdk.dir in local.properties.
find_tools() {
    sdk=${ANDROID_HOME:-}
    if [ -z "$sdk" ] && [ -f "$here/local.properties" ]; then
        sdk=$(sed -n 's/^sdk\.dir=//p' "$here/local.properties")
    fi
    [ -n "$sdk" ] && [ -d "$sdk/build-tools" ] ||
        fail "no Android SDK: set ANDROID_HOME, or sdk.dir in local.properties"
    tools=$(ls -d "$sdk"/build-tools/*/ | sort -V | tail -n 1)
    for tool in aapt2 apksigner zipalign; do
        [ -x "$tools/$tool" ] || fail "no $tool in $tools"
    done
}

# apksigner, without the JDK's warning that its crypto library loads native code.
apksigner() {
    "$tools/apksigner" -J-enable-native-access=ALL-UNNAMED "$@"
}

# Reads APK as a phone would and sets name, code, version and min_sdk from it. Fails unless it is this app, with
# a release's version, not debuggable, and aligned as Android needs.
check_apk() {
    badging=$("$tools/aapt2" dump badging "$1") || fail "aapt2 can't read $1"
    name=$(printf '%s\n' "$badging" | sed -n "s/^package:.* name='\([^']*\)'.*/\1/p")
    code=$(printf '%s\n' "$badging" | sed -n "s/^package:.* versionCode='\([^']*\)'.*/\1/p")
    version=$(printf '%s\n' "$badging" | sed -n "s/^package:.* versionName='\([^']*\)'.*/\1/p")
    min_sdk=$(printf '%s\n' "$badging" | sed -n "s/^minSdkVersion:'\([0-9]*\)'.*/\1/p")
    [ "$name" = "$package" ] || fail "$1 is the package '$name', not $package"
    printf '%s\n' "$code" | grep -Eqx '[1-9][0-9]*' || fail "the version code of $1 is not a number: '$code'"
    printf '%s\n' "$version" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+' ||
        fail "the version of $1 is not a release's: '$version'"
    [ -n "$min_sdk" ] || fail "could not read the lowest Android API level of $1"
    case $badging in
        *application-debuggable*) fail "$1 is debuggable" ;;
    esac
    "$tools/zipalign" -c -P 16 4 "$1" > /dev/null || fail "$1 is not aligned"
}

# Fails unless APK has no signature at all, as what CI and build_release.sh make. apksigner then looks for the
# oldest kind, v1, and finds no MANIFEST.MF; anything else it says (a signature, a broken one, or an error of its
# own) is not an unsigned APK.
check_unsigned() {
    said=$(apksigner verify "$1" 2>&1) && fail "$1 is signed already"
    case $said in
        *"ERROR: Missing META-INF/MANIFEST.MF"*) ;;
        *) fail "$1 is not an unsigned APK, apksigner says: $said" ;;
    esac
}
