#!/bin/sh
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
# Builds the same two programs as build-qnx.sh, from the same sources and with the same options where they apply,
# for this machine, with its own compiler (CC, gcc when unset): dropbearmulti (server, key tool, scp) and
# sftp-server, stripped, to OUT; the builds themselves to OUT/obj. Nothing is written into the sources. For tests
# on this machine; `make ssh-host` runs this.
#
# usage: build-host.sh DROPBEAR_SRC OPENSSH_SRC OUT
set -eu
if [ $# -ne 3 ]; then
    echo "usage: $0 DROPBEAR_SRC OPENSSH_SRC OUT" >&2
    exit 2
fi
dropbear=$(cd "$1" && pwd)
openssh=$(cd "$2" && pwd)
mkdir -p "$3/obj/dropbear" "$3/obj/openssh"
out=$(cd "$3" && pwd)
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

export CC="${CC:-gcc}"
export CFLAGS="-O2"

step() {
    log=$1
    shift
    if ! "$@" > "$log" 2>&1; then
        tail -40 "$log" >&2
        echo "build-host.sh: failed, see $(pwd)/$log" >&2
        exit 1
    fi
}

echo "[SSH] Dropbear for this machine"
cd "$out/obj/dropbear"
cp "$dropbear/localoptions.h" .
step configure.log "$dropbear/configure" \
    --disable-zlib --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp \
    --disable-wtmpx --disable-loginfunc --disable-pututline --disable-pututxline --disable-shadow \
    --enable-bundled-libtom
step make.log make PROGRAMS="dropbear dropbearkey scp" MULTI=1 -j"$jobs"
strip -o "$out/dropbearmulti" dropbearmulti

echo "[SSH] OpenSSH's sftp-server for this machine"
cd "$out/obj/openssh"
step configure.log "$openssh/configure" \
    --without-openssl --without-zlib --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp \
    --disable-wtmpx --without-shadow --disable-strip
step make.log make -j"$jobs" sftp-server
strip -o "$out/sftp-server" sftp-server

ls -l "$out/dropbearmulti" "$out/sftp-server"
