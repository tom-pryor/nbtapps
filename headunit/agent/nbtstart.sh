#!/bin/ksh
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#
# nbtstart.sh - the starter: decides which version of the agent is started on a head unit, and has that version's
# own launcher (<version>/nbtlaunch.sh) start it. hu_install.sh puts it into the agent's folder as `launcher`, and
# that folder is the one this file is in. It is the one script there that belongs to no version.
#
#   <folder>/launcher &                     the agent on this head unit
#   <folder>/launcher --root DIR ... &      its arguments go to the version's launcher, and so to nbtagent
#
# The folder (the plan document, "Agent updates"):
#   <version>/nbtagent        one version of the agent. A version's folder is never written over
#   <version>/nbtlaunch.sh    that version's launcher: how its agent is started, and started again when it fails
#   <version>/nbtstart.sh     the starter that version came with: see "A newer starter" below
#   current                   one line: the version to start
#   trial                     one line: a version that was placed and has not been tried
#   trial.started             the same file once its one try has begun
#   log/trial.log             what the last try said that failed while the head unit was on
#
# Before each start it looks at the files again:
#   trial is there            it is renamed to trial.started, and the version it names is started
#   trial.started is there    a try began and was not confirmed: the version in current is started
#   neither                   the version in current is started
# So a version gets one try, with no counter, and its launcher is tried with it: a new version whose launcher does
# not run on this head unit fails its try like one whose agent does not. The agent that confirms itself writes
# current and removes trial.started; this script renames that one file and removes nothing. It cannot sync the
# rename, and a power cut that loses it gives the version a second try.
#
# What it does when a version's launcher ends, by the exit status (nbtlaunch.sh):
#   0            the agent was stopped: the starter ends as well
#   4            the agent asked to be started again: at once, after a new look at the files
#   any other    of a version on trial: the try has failed. This start's log is copied into the agent's folder,
#                and the files are looked at again, which starts the version in current
#                of the version in current: the starter ends with that status
# With no version to start it ends with status 1, and with another starter running, with status 3.
#
# What it hands a version's launcher: its own arguments as they are, and in the environment
#   NBT_LAUNCH_STARTER=1      the level of this hand-over, for a launcher that comes to need more of a starter
#   NBT_LAUNCH_TRIAL          1 for a version on trial, empty otherwise
#   NBT_LAUNCH_NOTE           what the start's log is to hold of this script: what the files said, where they
#                             said something, and this starter's revision
#   NBT_LAUNCH_DELAY          as this script was started with it, for the first launcher only
#
# The log, of every start, is /tmp/nbtapp/log/agent.log, on the RAM disk, which a power down empties. A version's
# launcher begins the file anew for each start; this script adds a line to the file as it is.
#
# The folder is on flash, and this script writes to it for a version on trial only: the rename, and, when that
# version's launcher ends otherwise than stopped or asking to be started again, one copy of that start's log as
# log/trial.log, over the one of the try before. A try that works leaves no log there, and neither does one
# that a power down ends. Every other start reads the folder and writes nothing to it.
#
# To stop the agent, stop the agent (SIGTERM): its launcher follows, and the starter after it. A starter that is
# killed leaves its agent running with nothing to start it again. It ignores SIGHUP, so it goes on when the SSH
# session ends that it was started from, and what it starts inherits that.
#
# Only one starter to a head unit. The first one writes its process id to launcher.pid beside the log, on the RAM
# disk, and removes the file when it ends. A second one finds the file, and the process it names running, and ends
# with status 3 before it has looked at trial. A file whose process is gone was left by a starter that was
# killed: the next one takes it over. A power down empties the RAM disk, and the file with it. The process id is
# all that is compared, so a starter that was killed and whose id another program has since been given keeps the
# next starter off until the file is removed or the head unit powers down. The iDrive plugin reads the same file
# before it starts this script with the head unit (src/agent_start.hpp).
#
# A newer starter. This script is not on trial with a version: it is what gives a version its try. Each version
# comes with the starter of its time, and an agent that is the version in current puts its own in place of this
# file when the two differ (agent_home.hpp): once it is confirmed, and at each start after that. A starter that
# is running goes on as it was read; the new one is what the next start of the head unit runs.
#
# Its revision. STARTER_REVISION below is this file's own number, raised when the file changes and by nothing
# else: a version of the agent that comes with the same starter comes with the same number. The log of each
# start it makes names it ("starter revision 2"), so the log says which starter is in place on a head unit. The
# starter before the first number, which came with agent 1.1.0, says nothing of it.
#
# A version's name starts with a digit and holds letters, digits, '.', '+' and '-' only, 64 at most, so no file's
# content can name a folder outside this one or one of the files beside the versions.
#
# Written for the head unit's ksh and for sh, with the shell's own commands and mv, mkdir, cp, rm and sleep,
# which is what a head unit can be counted on to have. It takes the PATH it is started with.

# As agent/exit_status.hpp has them: tests/agent/test_exit_status.cpp reads these three lines.
EXIT_STOPPED=0
EXIT_NO_RESTART=3
EXIT_RESTART=4

STARTER_REVISION=2

case $0 in
    */*) AGENT_HOME=${0%/*} ;;
    *) AGENT_HOME=. ;;
esac
if [ -z "$AGENT_HOME" ]; then AGENT_HOME=/; fi
LOG_DIR=${NBT_TEST_LAUNCH_RUN:-/tmp/nbtapp/log}     # on the RAM disk; override only for host tests
LOG=$LOG_DIR/agent.log
TRIAL_LOG_DIR=$AGENT_HOME/log                       # on flash
TRIAL_LOG=$TRIAL_LOG_DIR/trial.log
LOCK=$LOG_DIR/launcher.pid                          # on the RAM disk: the one starter's process id
SH=${NBT_TEST_LAUNCH_SH:-/bin/ksh}                  # what runs a version's launcher; override only for host tests

say() { echo "launcher: $*" >> "$LOG"; }

# name_of FILE: sets NAME to the version that FILE names, or to nothing when it names none.
name_of() {
    NAME=
    if [ -f "$1" ]; then read -r NAME < "$1"; fi
    case $NAME in
        '' | [!0-9]* | *[!0-9A-Za-z.+-]*) NAME= ;;
    esac
    if [ ${#NAME} -gt 64 ]; then NAME=; fi
}

# there VERSION: whether that version is here to be started: its program, and its launcher.
there() {
    [ -n "$1" ] && [ -f "$AGENT_HOME/$1/nbtagent" ] && [ -x "$AGENT_HOME/$1/nbtagent" ] && [ -f "$AGENT_HOME/$1/nbtlaunch.sh" ]
}

# choose: sets VERSION to the version to start now, or to nothing when there is none; TRYING to 1 when that
# version is on trial; and NOTE to what the log is to say about the files.
choose() {
    VERSION=
    TRYING=
    NOTE=
    if [ -f "$AGENT_HOME/trial" ]; then
        name_of "$AGENT_HOME/trial"
        if mv -f "$AGENT_HOME/trial" "$AGENT_HOME/trial.started" 2> /dev/null; then
            if there "$NAME"; then
                VERSION=$NAME
                TRYING=1
                NOTE="trying version $VERSION, once"
                return
            fi
            NOTE="the file trial names no version that is here"
        else
            NOTE="the file trial cannot be renamed, so what it names is not tried"
        fi
    elif [ -f "$AGENT_HOME/trial.started" ]; then
        name_of "$AGENT_HOME/trial.started"
        NOTE="version ${NAME:-?} was tried and has not confirmed itself"
        if [ -f "$TRIAL_LOG" ]; then
            NOTE="$NOTE; $TRIAL_LOG is what the last try said that failed while the head unit was on"
        else
            NOTE="$NOTE; no try has left a log"
        fi
    fi
    name_of "$AGENT_HOME/current"
    if there "$NAME"; then VERSION=$NAME; fi
}

# keep_try: copies this start's log into the agent's folder, over the copy of the try before. For a version on
# trial whose launcher has ended and was not stopped: the one write to flash that a try's log costs.
keep_try() {
    if [ -d "$TRIAL_LOG_DIR" ] || mkdir "$TRIAL_LOG_DIR" 2> /dev/null; then
        if cp "$LOG" "$TRIAL_LOG" 2> /dev/null; then
            say "what this try said is kept as $TRIAL_LOG"
            return
        fi
    fi
    say "what this try said could not be kept as $TRIAL_LOG"
}

# holder: sets OTHER to the process id that launcher.pid holds, or to nothing when it holds none.
holder() {
    OTHER=
    if [ -f "$LOCK" ]; then read -r OTHER < "$LOCK"; fi
    case $OTHER in
        '' | *[!0-9]*) OTHER= ;;
    esac
}

# take_lock: makes this starter the one starter here. Returns 1, with OTHER the process id of the one that is,
# when there is one. The file is made with the shell's noclobber, which makes it only if it is not there.
take_lock() {
    if (set -C; echo $$ > "$LOCK") 2> /dev/null; then return 0; fi
    holder
    # A file with nothing in it yet: another starter may be between making it and writing to it.
    if [ -z "$OTHER" ]; then
        sleep 1
        holder
    fi
    if [ -n "$OTHER" ] && [ "$OTHER" != $$ ] && kill -0 "$OTHER" 2> /dev/null; then return 1; fi
    # Its starter is gone and did not remove it. Two starters that find that at once both write; the one whose
    # id the file holds a moment later has it.
    echo $$ > "$LOCK.$$" || return 1
    mv -f "$LOCK.$$" "$LOCK" || return 1
    sleep 1
    holder
    [ "$OTHER" = $$ ]
}

# drop_lock: when this starter ends. Only its own file, and that one file by its name.
drop_lock() {
    holder
    if [ "$OTHER" = $$ ]; then rm -f "$LOG_DIR/launcher.pid"; fi
}

trap '' HUP

if [ -d "$LOG_DIR" ] || mkdir -p "$LOG_DIR" 2> /dev/null; then
    :
else
    echo "launcher: cannot make $LOG_DIR" >&2
    exit 1
fi

if take_lock; then
    trap drop_lock 0
else
    echo "launcher: another launcher is running here (process ${OTHER:-?}, says $LOCK): this one starts nothing" >&2
    exit $EXIT_NO_RESTART
fi

while :; do
    choose
    if [ -z "$VERSION" ]; then
        if [ -n "$NOTE" ]; then say "$NOTE"; fi
        say "the file current names no version that is here: nothing to start"
        echo "launcher: $AGENT_HOME/current names no version that is here: nothing to start" >&2
        exit 1
    fi
    NBT_LAUNCH_STARTER=1 NBT_LAUNCH_TRIAL=$TRYING NBT_LAUNCH_NOTE="${NOTE:+$NOTE; }starter revision $STARTER_REVISION" \
        "$SH" "$AGENT_HOME/$VERSION/nbtlaunch.sh" "$@" < /dev/null
    status=$?
    unset NBT_LAUNCH_DELAY     # the wait is before the first start only
    if [ $status -eq $EXIT_STOPPED ]; then exit 0; fi
    if [ $status -eq $EXIT_RESTART ]; then continue; fi
    if [ -z "$TRYING" ]; then exit $status; fi
    # The try is over, and trial.started says so to the next look at the files.
    say "the try of version $VERSION ended with status $status"
    keep_try
done
