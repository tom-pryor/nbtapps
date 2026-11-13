#!/bin/sh
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
# Lists the source files in git that do not start with the licence header, and fails if there are any.
#
# `reuse lint` cannot see a missing header: REUSE.toml's first annotation covers every file, so a new source file
# without its two lines still passes. This looks at the kinds of file that take a comment, and leaves out those
# that must not have a header, as REUSE.toml lists them: the test packages (hashed and signed), the copies of the
# head unit's test data in the app, the debug build's signed sample catalogue, and third-party files under their
# own licence (TweetNaCl, the Gradle wrapper).
#
#   .github/scripts/check_headers.sh        (from anywhere in the repo)
set -eu
cd "$(git rev-parse --show-toplevel)"

missing=$(git ls-files -- \
    '*.kt' '*.kts' '*.cpp' '*.hpp' '*.c' '*.h' '*.sh' '*.py' '*.xml' '*.pro' '*.yml' '*.yaml' \
    '*Makefile' '*Dockerfile' \
    ':!:headunit/tests/packages/**' \
    ':!:headunit/agent/vendor/tweetnacl/**' \
    ':!:android/app/src/test/resources/agent/**' \
    ':!:android/app/src/debug/assets/catalogue/**' \
    ':!:android/gradlew' ':!:android/gradlew.bat' ':!:android/gradle/wrapper/**' \
    | xargs -r grep -L 'SPDX-License-Identifier' || true)

if [ -n "$missing" ]; then
    echo "check_headers.sh: these files have no licence header (see CONTRIBUTING.md):" >&2
    printf '  %s\n' $missing >&2
    exit 1
fi
echo "check_headers.sh: every source file has its licence header"
