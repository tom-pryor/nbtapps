#!/bin/sh
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
# Signs a release of the app with the release key, on this machine: the key is never on GitHub.
#
#   tools/sign.sh VERSION --notes FILE [--compare FILE]    the draft release android-vVERSION on GitHub
#   tools/sign.sh VERSION --from FILE [--notes FILE]       an unsigned APK here; GitHub is left out
#                 [--compare FILE]
#
# --notes FILE: the release's notes, what is new, one on each line. They are checked first, before the key or
# GitHub is looked at, by the catalogue's rule for notes: no blank line, none over 200 characters, no control
# character. They are what the release on GitHub says is new, and what the catalogue repo records with it.
#
# From GitHub: downloads the unsigned APK that the Android release workflow left in the draft release, and checks
# it against its SHA-256. Then, either way:
#   - checks the APK: this app, the version VERSION, not debuggable, aligned, not signed yet
#   - with --compare, requires FILE, a build of the same commit made here (tools/build_release.sh), to be the
#     same bytes
#   - signs it with apksigner, which asks for the key's password. Signature schemes v2 and v3; the entries are
#     left as they are, so the signed APK is the unsigned one byte for byte with the signature added
#   - checks the signature as a phone would: it verifies, there is one signer, and it is the release certificate
#   - leaves release/nbtapps-VERSION.apk and release/release.json, which the catalogue repo records a release from,
#     and with --notes release/notes.txt, a copy of FILE, beside them
# From GitHub, last: shows the text the release will have (the notes, a link to the changelog, the build and what
# was signed), asks, then adds the signed APK to the draft release and publishes it.
#
# The key: ~/.config/nbtapps/keys/android-release.properties, or the file NBTAPPS_SIGNING names, says where the
# store is and which key in it (storeFile, keyAlias). It must not hold a password. android/RELEASING.md says more.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
out="$here/release"
me=sign.sh
. "$here/tools/apk_checks.sh"
signing=${NBTAPPS_SIGNING:-"$HOME/.config/nbtapps/keys/android-release.properties"}
# The certificate every release is signed with. Android takes an update only from the key it was signed with.
release_cert=843dd6519def36860dfff183634424753b00a059846d6e2662ba608bff04d587

usage() {
    echo "usage: tools/sign.sh VERSION [--notes FILE] [--from FILE] [--compare FILE]" >&2
    exit 2
}

[ $# -ge 1 ] || usage
want=$1
shift
from=
compare=
notes=
while [ $# -gt 0 ]; do
    case $1 in
        --from) [ $# -ge 2 ] || usage; from=$2; shift 2 ;;
        --compare) [ $# -ge 2 ] || usage; compare=$2; shift 2 ;;
        --notes) [ $# -ge 2 ] || usage; notes=$2; shift 2 ;;
        *) usage ;;
    esac
done
printf '%s\n' "$want" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+' || fail "not a version: '$want'"
tag="android-v$want"
[ -z "$compare" ] || [ -f "$compare" ] || fail "no file $compare to compare with"
# The notes, before the key or GitHub: a release on GitHub never says what the catalogue would then refuse.
if [ -n "$notes" ]; then
    check_notes "$notes"
elif [ -z "$from" ]; then
    fail "a release on GitHub needs its notes: --notes FILE, what is new, one note on each line"
fi

# The key. The properties file's values are read, never shown; a password in it is refused.
[ -f "$signing" ] || fail "no $signing: android/RELEASING.md says what goes in it"
prop() {
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$signing" | tail -n 1 | sed 's/[[:space:]]*$//'
}
for key in storePassword keyPassword; do
    [ -z "$(prop "$key")" ] || fail "$signing holds a password ($key): take it out, apksigner asks for it"
done
store=$(prop storeFile)
alias=$(prop keyAlias)
[ -n "$store" ] && [ -n "$alias" ] || fail "$signing needs storeFile and keyAlias"
case $store in
    /*) ;;
    *) store="$(dirname "$signing")/$store" ;;
esac
[ -f "$store" ] || fail "no key store at $store"

find_tools
mkdir -p "$out"

if [ -n "$from" ]; then
    [ -f "$from" ] || fail "no file $from"
    unsigned=$from
else
    command -v gh > /dev/null || fail "no gh: signing a release from GitHub needs the GitHub CLI"
    draft=$(cd "$here" && gh release view "$tag" --json isDraft --jq .isDraft) ||
        fail "no release $tag on GitHub: push the tag, and let the Android release workflow make the draft"
    [ "$draft" = true ] || fail "the release $tag is published already"
    mkdir -p "$out/github"
    file="nbtapps-$want-unsigned.apk"
    (cd "$here" && gh release download "$tag" --dir "$out/github" --clobber --pattern "$file" --pattern "$file.sha256")
    (cd "$out/github" && sha256sum --check --quiet "$file.sha256") ||
        fail "the draft's APK is not the file its SHA-256 names"
    unsigned="$out/github/$file"
fi

check_unsigned "$unsigned"
check_apk "$unsigned"
[ "$version" = "$want" ] || fail "$unsigned is the version $version, not $want"

if [ -n "$compare" ]; then
    python3 "$here/tools/compare_apks.py" "$compare" "$unsigned" ||
        fail "$compare and $unsigned are not the same bytes: is it a build of the same commit?"
fi

# --alignment-preserved: apksigner otherwise lays the entries out anew; with it, the signed APK is the unsigned
# one with the signature added, which compare_apks.py can show. v1 (JAR) signatures are for Android 6 and older,
# v4 is a file beside the APK for incremental installs: neither is made.
signed="$out/nbtapps-$want.apk"
rm -f "$signed"
echo "Signing $unsigned with the key $alias in $store"
apksigner sign --ks "$store" --ks-key-alias "$alias" --alignment-preserved \
    --v1-signing-enabled false --v2-signing-enabled true --v3-signing-enabled true --v4-signing-enabled false \
    --out "$signed" "$unsigned" || fail "apksigner could not sign"

certs=$(apksigner verify --print-certs "$signed" 2>/dev/null) || {
    apksigner verify --print-certs "$signed" >&2 || true
    fail "the signed APK's signature does not verify"
}
case $certs in
    *"Signer #2"*) fail "the signed APK has more than one signer" ;;
esac
cert=$(printf '%s\n' "$certs" | sed -n 's/^Signer #1 certificate SHA-256 digest: \([0-9a-f]\{64\}\)$/\1/p')
[ "$cert" = "$release_cert" ] ||
    fail "the APK is signed with the certificate '$cert', not the release certificate $release_cert"
check_apk "$signed"
[ "$version" = "$want" ] || fail "the signed APK is the version $version, not $want"
python3 "$here/tools/compare_apks.py" "$unsigned" "$signed" > /dev/null ||
    fail "the signed APK is not the unsigned one with a signature added"

file="nbtapps-$want.apk"
size=$(wc -c < "$signed" | tr -d ' ')
sha=$(sha256sum "$signed" | cut -d ' ' -f 1)
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
# Beside release.json, where the catalogue repo's set-app reads it. Through a file of its own, in case FILE is
# that copy already; with no notes, the copy of an earlier release goes.
if [ -n "$notes" ]; then
    cp "$notes" "$out/notes.txt.part"
    mv "$out/notes.txt.part" "$out/notes.txt"
else
    rm -f "$out/notes.txt"
fi

echo
echo "release $version (version code $code), signed, $size bytes"
echo "  $signed"
echo "  $out/release.json"
[ -z "$notes" ] || echo "  $out/notes.txt"
echo "  SHA-256 $sha"
[ -z "$from" ] || exit 0

# The release's text: what is new, then the draft's text, whose line that says it is not published yet goes (cat
# -s closes the gap it leaves), and what was signed. It is shown before the question, as it will be public.
body="$out/github/notes.md"
(cd "$here" && gh release view "$tag" --json body --jq .body) > "$out/github/draft.md"
release_body "$notes" "$out/github/draft.md" "$file" "$sha" "$cert" > "$body"
echo
echo "The release's text on GitHub:"
echo
cat "$body"
echo
printf 'Add %s to the draft release %s and publish it? [y/N] ' "$file" "$tag"
read -r answer
case $answer in
    y | Y | yes) ;;
    *)
        echo "Not published. tools/sign.sh $want --notes FILE signs it again; the draft is as it was."
        exit 0
        ;;
esac

(cd "$here" && gh release upload "$tag" "$signed" --clobber)
(cd "$here" && gh release edit "$tag" --draft=false --notes-file "$body")
echo "Published $tag. The catalogue repo records it from $out/release.json and the notes beside it, $out/notes.txt."
