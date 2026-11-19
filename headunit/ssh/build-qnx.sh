#!/bin/sh
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
# Builds Dropbear's dropbearmulti (server, key tool, scp) and OpenSSH's sftp-server for the head unit (QNX 6.5.0,
# armle-v7, GCC 4.4.2), in the QNX SDP image, from the sources `make ssh-src` exported and patched. The two
# programs, stripped, go to OUT; the builds themselves to OUT/obj. Nothing is written into the sources. The
# bare ntoarmv7-gcc driver needs no QNX licence file. `make ssh` runs this; README says why each option is there.
#
# usage: build-qnx.sh DROPBEAR_SRC OPENSSH_SRC OUT
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

export CC=ntoarmv7-gcc
export CFLAGS="-O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=softfp -D_QNX_SOURCE -std=gnu99"
export LIBS=-lsocket

# Runs a step with its output in a log, and shows the end of the log when it fails.
step() {
    log=$1
    shift
    if ! "$@" > "$log" 2>&1; then
        tail -40 "$log" >&2
        echo "build-qnx.sh: failed, see $(pwd)/$log" >&2
        exit 1
    fi
}

echo "[SSH] Dropbear for QNX"
cd "$out/obj/dropbear"
cp "$dropbear/localoptions.h" .
step configure.log "$dropbear/configure" --host=arm-unknown-nto-qnx6.5.0eabi --build=x86_64-linux-gnu \
    --disable-zlib --disable-harden --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp \
    --disable-wtmpx --disable-loginfunc --disable-pututline --disable-pututxline --disable-shadow \
    --enable-bundled-libtom ac_cv_have_decl_htole64=yes
step make.log make PROGRAMS="dropbear dropbearkey scp" MULTI=1 -j"$jobs"
ntoarmv7-strip -o "$out/dropbearmulti" dropbearmulti

echo "[SSH] OpenSSH's sftp-server for QNX"
cd "$out/obj/openssh"
step configure.log "$openssh/configure" --host=arm-unknown-nto-qnx6.5.0eabi --build=x86_64-linux-gnu \
    --without-openssl --without-zlib --without-hardening --without-stackprotect --without-pie \
    --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx --without-shadow \
    --disable-strip
step make.log make -j"$jobs" sftp-server
ntoarmv7-strip -o "$out/sftp-server" sftp-server

ls -l "$out/dropbearmulti" "$out/sftp-server"
