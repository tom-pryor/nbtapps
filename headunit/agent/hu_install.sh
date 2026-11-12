#!/bin/ksh
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#
# hu_install.sh - puts the agent onto a head unit for the first time, or a newer version by hand: the programs
# and the version's launcher from the folder this script is in, on a USB stick, into a folder of their version's
# own under /var/opt/nbtapp/agent, the starter beside that folder as `launcher`, and that version as the one the
# starter starts.
#
# The folder this script is in is release/agent/, or the payload folder of the agent's own package,
# org.nbtapp.agent-<version>.nbtpkg, which is a zip and is unpacked onto the stick: both hold nbtagent, nbtctl,
# nbtlaunch.sh, nbtstart.sh and version beside this script (tools/agent_pkg.py).
#
# Usage (SSH open; on the node the HMI runs on, e.g. after `on -f /net/hu-omap/ /bin/sh`):
#   ksh /mnt/umassXXXXX/agent/hu_install.sh         release/agent/, copied to the stick as agent/
#   ksh /mnt/umassXXXXX/payload/hu_install.sh       the package, unpacked onto the stick
# A USB stick is noexec, so the script is handed to ksh and not started itself.
#
# What it writes, all of it under /var/opt/nbtapp/agent (the plan document, "Agent updates"):
#   <version>/nbtagent, <version>/nbtctl    copies of the two programs, from `version` beside this script
#   <version>/nbtlaunch.sh                  that version's launcher, which starts its agent
#   <version>/nbtstart.sh                   the starter that version comes with
#   launcher                                a copy of nbtstart.sh: the starter, which runs the launcher of the
#                                           version that current names (nbtstart.sh has the rules)
#   current                                 the version, on one line
# The version's folder then holds what the agent's own package places when an agent updates itself.
# It does not write `autostart`, the file that has the iDrive plugin start the launcher with the head unit; it
# says how to. It writes over no version that is there and removes nothing. A version that is there already has
# to be removed by hand first, with the agent stopped; the script says how. It starts nothing and stops nothing:
# an agent that is running goes on as it is until it is stopped and its launcher started again. It refuses while
# an update of the agent by the agent is under way (a file `trial` or `trial.started`).
#
# It cannot sync what it wrote: a head unit has no sync. Leave the head unit on for a moment after it, or run it
# again if the files are not there after a power down.
#
# Written for the head unit's ksh and for sh, with the shell's own commands and mkdir, cp, chmod and mv.

AGENT_HOME=${NBT_TEST_AGENT_HOME:-/var/opt/nbtapp/agent}     # override only for host tests

case $0 in
    */*) SRC=${0%/*} ;;
    *) SRC=. ;;
esac

fail() {
    echo "hu_install: $*" >&2
    exit 1
}

for name in nbtagent nbtctl nbtlaunch.sh nbtstart.sh version; do
    if [ ! -f "$SRC/$name" ]; then fail "$SRC/$name is not there: this script needs nbtagent, nbtctl, nbtlaunch.sh, nbtstart.sh and version beside it"; fi
done

# A version's name, as nbtstart.sh takes it.
VERSION=
read -r VERSION < "$SRC/version"
case $VERSION in
    '' | [!0-9]* | *[!0-9A-Za-z.+-]*) fail "$SRC/version does not hold a version" ;;
esac
if [ ${#VERSION} -gt 64 ]; then fail "$SRC/version does not hold a version"; fi

mkdir -p "$AGENT_HOME" || fail "cannot make $AGENT_HOME"

if [ -f "$AGENT_HOME/trial" ] || [ -f "$AGENT_HOME/trial.started" ]; then
    fail "an update of the agent was begun and is not finished: $AGENT_HOME holds trial or trial.started. Start the launcher and let the agent finish it"
fi

if [ -e "$AGENT_HOME/$VERSION" ]; then
    echo "hu_install: version $VERSION is there already, and this script writes over nothing." >&2
    echo "hu_install: To put it there anew, stop the agent, then:" >&2
    echo "hu_install:   rm -rf $AGENT_HOME/$VERSION" >&2
    echo "hu_install: and run this script again." >&2
    exit 1
fi

# From here on a failure leaves a folder that is not whole. Nothing starts it, because current does not name it,
# and the next run finds it and says how to remove it.
mkdir "$AGENT_HOME/$VERSION" || fail "cannot make $AGENT_HOME/$VERSION"
cp "$SRC/nbtagent" "$SRC/nbtctl" "$SRC/nbtlaunch.sh" "$SRC/nbtstart.sh" "$AGENT_HOME/$VERSION/" || fail "cannot copy the programs and the scripts to $AGENT_HOME/$VERSION"
chmod 755 "$AGENT_HOME/$VERSION/nbtagent" "$AGENT_HOME/$VERSION/nbtctl" "$AGENT_HOME/$VERSION/nbtlaunch.sh" "$AGENT_HOME/$VERSION/nbtstart.sh" ||
    fail "cannot make the programs and the scripts in $AGENT_HOME/$VERSION executable"

# Does the program start here at all, and is it the version the file beside it says?
said=$("$AGENT_HOME/$VERSION/nbtagent" --version) || fail "$AGENT_HOME/$VERSION/nbtagent does not start here"
echo "hu_install: $said"
case $said in
    "nbtagent $VERSION "*) ;;
    *) fail "that is not version $VERSION, which $SRC/version names: the folder on the stick is not one build's" ;;
esac

# The starter, in place of one that is there: a starter that is running goes on as it was read.
cp "$SRC/nbtstart.sh" "$AGENT_HOME/launcher.new" || fail "cannot copy the starter to $AGENT_HOME"
chmod 755 "$AGENT_HOME/launcher.new" || fail "cannot make the starter executable"
mv -f "$AGENT_HOME/launcher.new" "$AGENT_HOME/launcher" || fail "cannot put the starter in place"

echo "$VERSION" > "$AGENT_HOME/current.new" || fail "cannot write $AGENT_HOME/current.new"
mv -f "$AGENT_HOME/current.new" "$AGENT_HOME/current" || fail "cannot put $AGENT_HOME/current in place"

echo "hu_install: version $VERSION is in $AGENT_HOME/$VERSION, and is the one the launcher starts."
echo "hu_install: To start it:  $AGENT_HOME/launcher &"
echo "hu_install: Its log:      /tmp/nbtapp/log/agent.log, which a power down empties"
echo "hu_install: Nothing starts it with the head unit. To have the iDrive plugin do that, once it has run well:"
echo "hu_install:   echo on > $AGENT_HOME/autostart"
