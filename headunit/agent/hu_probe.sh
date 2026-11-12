#!/bin/ksh
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#
# hu_probe.sh - reads off a head unit what the agent's platform code (agent/platform.hpp) has to know and the
# repo cannot tell: which node this is, what is mounted where and what is read-only, how much room /var/opt and
# /tmp have, where HmiMain3D's plugins are, which tools a hook can count on, whether /dev/urandom is there, and
# what restarts the HMI, and where the shared libraries are that the agent's programs ask for.
#
# Usage (ignition on, SSH open; run it on the node the HMI runs on, e.g. after `on -f /net/hu-omap/ /bin/sh`):
#   ksh /mnt/umassXXXXX/hu_probe.sh > /mnt/umassXXXXX/hu_probe.log 2>&1
#   ksh /mnt/umassXXXXX/hu_probe.sh --exec-test >> /mnt/umassXXXXX/hu_probe.log 2>&1
# A USB stick is noexec, so the script is handed to ksh and not started itself. If that is refused too, copy it
# to /tmp first.
#
# With no argument it changes nothing: it lists folders (ls), file systems (mount with no argument, df),
# libraries and processes (pidin), prints the plugins' manifests, reads 16 bytes of /dev/urandom, and prints the usage text of
# OnOffDSICommander and shutdown with `use`, which reads it out of the file. It runs neither of those two, and it
# does not read the system log or any configuration of the car.
#
# --exec-test is the one thing here that writes, and does nothing else: it copies a small stock program (uname)
# into /tmp and into /var/opt as nbt_probe.<pid>, runs the copy, and removes it. That answers whether ${app} can
# live under /var/opt, which no listing shows (a USB stick is noexec and nothing says so).
#
# The log names the car's processes with their arguments and the files of its plugin folders, and nothing of a
# phone or a person that I know of; it goes only where the command line above sends it.
#
# Written for the head unit's ksh and few tools: no head, tail, sed, awk or printf is assumed. Never run on a car.

EXEC_DIRS=${HU_PROBE_EXEC_DIRS:-"/tmp /var/opt"}     # override only for host tests
PROBE_FILE=""

sec() { echo; echo "=== $* ==="; }

# Where a tool is, or nothing.
where() { whence -p "$1" 2>/dev/null || command -v "$1" 2>/dev/null; }
have() { [ -n "$(where "$1")" ]; }

# run CMD ARGS...: the command line, what it prints, and its exit status when that is not 0.
run() {
    echo "\$ $*"
    if have "$1"; then
        "$@" 2>&1
        rc=$?
        if [ $rc -ne 0 ]; then echo "[exit status $rc]"; fi
    else
        echo "[no $1 here]"
    fi
}

# place PATH: what is there, whether this user may write to it now, and the file system that holds it.
place() {
    echo "-- $1"
    if [ ! -e "$1" ] && [ ! -L "$1" ]; then
        echo "   not there"
        return
    fi
    ls -ld "$1" 2>&1
    if [ -d "$1" ]; then
        if [ -w "$1" ]; then echo "   writable now: yes"; else echo "   writable now: no (read-only file system, or not permitted)"; fi
        df -kP "$1" 2>&1
    fi
}

# lsglob PATTERN...: ls -ld of every match; a pattern nothing matches says nothing.
lsglob() {
    for p in "$@"; do
        if [ -e "$p" ] || [ -L "$p" ]; then ls -ld "$p" 2>&1; fi
    done
}

# ---- the probe ------------------------------------------------------------------------------------------------------

probe() {
    echo "hu_probe 2, pid $$"

    sec "who and where"
    run date
    run uname -a
    run hostname
    run id
    echo "PATH=$PATH"
    echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
    echo "SHELL=$SHELL HOME=$HOME PWD=$PWD"
    echo "nodes under /net:"
    if [ -d /net ]; then ls /net 2>&1; else echo "   no /net"; fi
    run pidin info

    sec "tools (where each one is; - for none)"
    for t in ksh sh mount umount df du cp mv rm ln mkdir rmdir chmod chown cat ls grep sed awk head tail wc sort cut \
             tr find sync sleep kill slay pidin sloginfo use on env dd od cksum touch date uname hostname id tar gzip \
             gunzip ifconfig netstat printf expr basename dirname shutdown OnOffDSICommander; do
        w=$(where "$t")
        echo "$t: ${w:--}"
    done

    sec "libraries (nbtagent and nbtctl ask for libsocket.so.3, libcpp.so.4 and libc.so.3; the plugin for the rest)"
    run getconf _CS_LIBPATH
    oldifs=$IFS
    IFS=:
    libdirs=""
    for d in $LD_LIBRARY_PATH; do libdirs="$libdirs $d"; done
    IFS=$oldifs
    for l in ldqnx.so.2 libc.so.3 libsocket.so.3 libcpp.so.4 libm.so.2 libstdc++.so.6; do
        found=no
        for d in /proc/boot /lib /usr/lib /lib/dll /usr/lib/dll /opt/hmi/ID5/lib $libdirs; do
            if [ -e "$d/$l" ] || [ -L "$d/$l" ]; then
                ls -l "$d/$l" 2>&1
                found=yes
            fi
        done
        if [ $found = no ]; then echo "$l: in none of the folders looked in"; fi
    done

    sec "file systems"
    run mount
    run df -kP
    run df -n
    for p in / /fs /fs/sda0 /net/hu-omap/fs/sda0 /opt /opt/hmi /var /var/opt /var/opt/nbtapp /tmp /dev/shmem /mnt; do
        place "$p"
    done
    echo "-- the system partition as other nodes show it:"
    lsglob /net/*/fs/sda0
    echo "-- /fs and /mnt:"
    ls -l /fs 2>&1
    ls -l /mnt 2>&1

    sec "plugin folders"
    for base in /opt/hmi/ID5/data/ro /opt/hmi/ID5/data/rw; do
        place "$base"
        place "$base/Plugins"
        [ -d "$base/Plugins" ] || continue
        for dir in "$base"/Plugins/*; do
            [ -d "$dir" ] || continue
            echo "-- $dir"
            ls -l "$dir" 2>&1
            for f in "$dir"/*.manifest; do
                [ -f "$f" ] || continue
                echo "-- $f:"
                cat "$f" 2>&1
                echo
            done
            for f in "$dir"/*.xml; do
                [ -f "$f" ] || continue
                echo "-- lines with entryButtonFocusID in $f:"
                grep entryButtonFocusID "$f" 2>&1
            done
        done
    done
    echo "-- /opt/hmi/ID5/data:"
    ls -l /opt/hmi/ID5/data 2>&1
    echo "-- any other Plugins folder:"
    lsglob /opt/hmi/*/data/*/Plugins /fs/*/opt/hmi/*/data/*/Plugins /var/opt/*/Plugins /var/opt/*/*/Plugins \
           /var/opt/*/*/*/Plugins /var/opt/*/*/*/*/Plugins
    echo "-- /var/opt:"
    ls -l /var/opt 2>&1

    sec "processes"
    run pidin arg
    run pidin users
    # Who the HMI runs as: the plugin has to be allowed to open the agent's socket.
    pidin arg 2>/dev/null | while read pid prog rest; do
        case "$prog" in
        *HmiMain3D*)
            echo "-- HmiMain3D is pid $pid:"
            ls -ld "/proc/$pid" "/proc/$pid/as" 2>&1
            ;;
        esac
    done

    sec "devices"
    echo "-- /dev:"
    ls /dev 2>&1
    lsglob /dev/random /dev/urandom /dev/socket /dev/slog /dev/btspp*
    echo "-- /dev/socket (1 is the local socket family the agent listens on):"
    ls -l /dev/socket 2>&1
    if [ ! -e /dev/urandom ]; then
        echo "/dev/urandom: not there"
    elif have dd; then
        # In the background, so a device that never answers costs three seconds and not the probe.
        dd if=/dev/urandom of=/dev/null bs=16 count=1 >/dev/null 2>&1 &
        ddpid=$!
        n=0
        while kill -0 $ddpid 2>/dev/null && [ $n -lt 3 ]; do sleep 1; n=$((n + 1)); done
        if kill -0 $ddpid 2>/dev/null; then
            kill $ddpid 2>/dev/null
            echo "/dev/urandom: a read of 16 bytes did not come back in 3 s"
        else
            wait $ddpid
            echo "/dev/urandom: a read of 16 bytes ended with exit status $?"
        fi
    else
        echo "/dev/urandom: there; no dd to read it with"
    fi

    sec "restarting (nothing here is run; use prints a program's usage text from its file)"
    for t in OnOffDSICommander shutdown; do
        w=$(where "$t")
        if [ -z "$w" ]; then
            echo "$t: not in PATH"
            lsglob /opt/*/bin/"$t" /opt/*/*/bin/"$t" /proc/boot/"$t" /fs/*/opt/*/bin/"$t"
            continue
        fi
        ls -l "$w" 2>&1
        run use "$w"
    done

    sec "what starts things"
    echo "-- /proc/boot:"
    ls /proc/boot 2>&1
    for d in /etc /etc/rc.d /opt/sys/etc /opt/sys/bin; do
        [ -d "$d" ] || continue
        echo "-- $d:"
        ls -l "$d" 2>&1
    done

    sec "done"
    echo "Nothing was changed. For whether /var/opt runs programs: hu_probe.sh --exec-test"
}

# ---- --exec-test ----------------------------------------------------------------------------------------------------

cleanup() {
    if [ -n "$PROBE_FILE" ]; then rm -f "$PROBE_FILE"; PROBE_FILE=""; fi
}

exec_test() {
    echo "hu_probe 1 --exec-test, pid $$"
    sec "can a program run from these folders"
    # A stock program that does nothing but print when it has no argument, and that runs where it is.
    SRC=""
    for t in uname hostname date; do
        s=$(whence -p "$t" 2>/dev/null || command -v "$t" 2>/dev/null)
        case "$s" in
        /*)
            if [ -f "$s" ] && "$s" >/dev/null 2>&1; then
                SRC=$s
                break
            fi
            ;;
        esac
    done
    if [ -z "$SRC" ]; then
        echo "no stock program to copy (uname, hostname, date): nothing tested, nothing written"
        return 1
    fi
    echo "the program copied: $SRC"
    ls -l "$SRC" 2>&1

    trap 'cleanup; exit 1' 1 2 15
    trap cleanup 0
    for d in $EXEC_DIRS; do
        echo "-- $d"
        if [ ! -d "$d" ]; then echo "   not there"; continue; fi
        df -kP "$d" 2>&1
        if [ ! -w "$d" ]; then echo "   not writable now: not tested, nothing written"; continue; fi
        f=$d/nbt_probe.$$
        if [ -e "$f" ] || [ -L "$f" ]; then echo "   $f exists already: not tested, nothing written"; continue; fi
        PROBE_FILE=$f
        if ! cat "$SRC" > "$f" 2>/dev/null; then
            echo "   could not write $f: not tested"
            cleanup
            continue
        fi
        chmod 755 "$f" 2>&1
        "$f" >/dev/null 2>&1
        rc=$?
        cleanup
        if [ -e "$f" ]; then echo "   WARNING: $f could not be removed; remove it by hand"; fi
        case $rc in
        0)   echo "   RUNS: a program started from $d" ;;
        126) echo "   DOES NOT RUN: the copy could not be executed (exit status 126; noexec, most likely)" ;;
        *)   echo "   UNCLEAR: the copy ended with exit status $rc, and 0 where it came from" ;;
        esac
    done
    sec "done"
    echo "Every nbt_probe.$$ was removed again, unless a WARNING above says otherwise."
}

# ---------------------------------------------------------------------------------------------------------------------

case "${1:-}" in
"")          probe ;;
--exec-test) exec_test ;;
*)
    echo "usage: ksh hu_probe.sh               read only: lists what the agent needs to know about this head unit"
    echo "       ksh hu_probe.sh --exec-test   writes: copies a small stock program into /tmp and /var/opt, runs the"
    echo "                                     copy and removes it, to see whether those folders run programs"
    exit 2
    ;;
esac
