#!/bin/sh
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
# Builds release/NbtApp/: the library in the QNX SDP 6.5.0 Docker image (qcc, GCC 4.4.2, armv7le), then the
# manifest, XML and text / image DBs on the host (python3), then the plugin's own package of that folder ->
# release/pkg/ (not signed), then `make check`.
#
# usage: ./build.sh          # build + check
#        ./build.sh agent    # the agent: its tests (in the bench container), then built for the head unit -> release/agent/,
#                            # its own package -> release/pkg/ (not signed), and `make agent-check`
#        ./build.sh clean    # remove build/ and release/
#
# Environment:
#   QNX_LICENSE_DIR  (required) directory holding your QNX `licenses` file; mounted at /etc/qnx/license.
#                    It must be writable: qcc creates licenses.lck next to the file.
#   QNX_IMAGE        Docker image with the SDP (default: qnx650-sdk)
set -eu

image=${QNX_IMAGE:-qnx650-sdk}

cd "$(dirname "$0")"

if [ "${1:-}" = clean ]; then
    exec make clean
fi
target=lib
if [ "${1:-}" = agent ]; then
    target=$1
elif [ $# -gt 0 ]; then
    echo "usage: $0 [agent|clean]" >&2
    exit 1
fi

if [ -z "${QNX_LICENSE_DIR:-}" ]; then
    echo "build.sh: set QNX_LICENSE_DIR to the directory holding your QNX 'licenses' file" >&2
    exit 1
fi
if [ ! -f "$QNX_LICENSE_DIR/licenses" ]; then
    echo "build.sh: no licenses file in '$QNX_LICENSE_DIR'" >&2
    exit 1
fi
if [ ! -w "$QNX_LICENSE_DIR" ]; then
    echo "build.sh: '$QNX_LICENSE_DIR' is not writable (qcc needs it for licenses.lck)" >&2
    exit 1
fi
if ! docker image inspect "$image" >/dev/null 2>&1; then
    echo "build.sh: Docker image '$image' not found (set QNX_IMAGE)" >&2
    exit 1
fi

# The image has no git, so the version is worked out here.
version=$(git describe --always --dirty 2>/dev/null || echo unknown)

if [ "$target" = agent ]; then
    make agent-test
fi
docker run --rm -u "$(id -u):$(id -g)" \
    -v "$(pwd)":/src \
    -v "$(cd "$QNX_LICENSE_DIR" && pwd)":/etc/qnx/license \
    "$image" make "$target" VERSION="$version"
if [ "$target" = lib ]; then
    make pkg
    make plugin-pkg
    make check
elif [ "$target" = agent ]; then
    make agent-pkg
    make agent-check
fi
