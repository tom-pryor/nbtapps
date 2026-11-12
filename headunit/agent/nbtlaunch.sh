#!/bin/ksh
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#
# nbtlaunch.sh - one version's launcher: starts that version's agent, and starts it again when it fails. It is in
# the version's folder beside nbtagent, where hu_install.sh or the agent's own package put it, and the starter
# (nbtstart.sh, in the agent's folder as `launcher`) runs it for the version it has chosen:
#
#   /bin/ksh <folder>/<version>/nbtlaunch.sh [nbtagent's arguments]
#
# It belongs to its version as the agent does. A newer version comes with its own, which is tried with it: if it
# does not run on this head unit, or its agent does not, the try fails and the starter is back at the version
# before, whose launcher is the one that ran until then.
#
# What the starter hands it, in the environment:
#   NBT_LAUNCH_TRIAL=1        this version is on trial: it gets one start, and a failure ends the launcher
#   NBT_LAUNCH_NOTE           what the starter has to say, for the log of the first start: what it read in the
#                             agent's folder, and its revision
#   NBT_LAUNCH_DELAY=<s>      seconds to wait before the first start, 999 at most. The iDrive plugin sets it when
#                             it starts the starter with the head unit (src/agent_start.hpp), so that the agent
#                             comes after the programs it talks to. The starter holds launcher.pid meanwhile
#   NBT_LAUNCH_STARTER        the level of the hand-over: 1 for the three above
#
# The log, of every start, is /tmp/nbtapp/log/agent.log, on the RAM disk, which a power down empties; agent.log.1
# and agent.log.2 are the two starts before. It names the file to the agent (NBTAGENT_LOG), and the agent keeps
# it short: one that has grown past 512 KB becomes agent.log.1, over the one that was there, and the log goes on
# in a new file. This script writes nothing to flash.
#
# What it does when the agent ends, by the exit status (agent/exit_status.hpp), and what it ends with itself:
#   0            the agent was stopped: ends with 0, and the starter ends as well
#   2, 3         a new start would change nothing: ends with that status
#   4            the agent asked to be started again: ends with 4, and the starter looks at the agent's folder
#                again, because the version to start may be another one now
#   any other    a failure, an end by a signal included. On trial: ends with that status. Otherwise: again after
#                2 s, and after five failures in a row it gives up with status 1
# The starter takes 0 and 4 as they are. Any other status ends the try of a version on trial, and ends the
# starter for the version in current.
#
# To stop the agent, stop the agent (SIGTERM): the launcher follows. A launcher that is killed leaves its agent
# running with nothing to start it again.
#
# Written for the head unit's ksh and for sh, with the shell's own commands and mv, mkdir and sleep, which is
# what a head unit can be counted on to have. It takes the PATH it is started with.

# As agent/exit_status.hpp has them: tests/agent/test_exit_status.cpp reads these four lines.
EXIT_STOPPED=0
EXIT_USAGE=2
EXIT_NO_RESTART=3
EXIT_RESTART=4

MAX_FAILURES=5
PAUSE=${NBT_TEST_LAUNCH_PAUSE:-2}     # seconds between a failure and the next start; override only for host tests

case $0 in
    */*) HERE=${0%/*} ;;
    *) HERE=. ;;
esac
if [ -z "$HERE" ]; then HERE=/; fi
VERSION=${HERE##*/}                                 # the folder is named after the version
LOG_DIR=${NBT_TEST_LAUNCH_RUN:-/tmp/nbtapp/log}     # on the RAM disk; override only for host tests
LOG=$LOG_DIR/agent.log

# The starter's words are for this script, and not for the agent it starts.
TRYING=$NBT_LAUNCH_TRIAL
NOTE=$NBT_LAUNCH_NOTE
unset NBT_LAUNCH_TRIAL NBT_LAUNCH_NOTE NBT_LAUNCH_STARTER

# Seconds to wait before the first start: digits only, three at most. Anything else is no wait.
DELAY=${NBT_LAUNCH_DELAY:-0}
case $DELAY in
    '' | *[!0-9]*) DELAY=0 ;;
esac
if [ ${#DELAY} -gt 3 ]; then DELAY=0; fi

say() { echo "launcher: $*" >> "$LOG"; }

if [ -d "$LOG_DIR" ] || mkdir -p "$LOG_DIR" 2> /dev/null; then
    :
else
    echo "launcher: cannot make $LOG_DIR" >&2
    exit 1
fi

WAITED=
if [ "$DELAY" -gt 0 ]; then
    sleep "$DELAY"
    WAITED="waited $DELAY s before this start, as NBT_LAUNCH_DELAY says"
fi

failures=0
while :; do
    # Each start gets a file of its own, and the two before it are kept.
    if [ -f "$LOG.1" ]; then mv -f "$LOG.1" "$LOG.2"; fi
    if [ -f "$LOG" ]; then mv -f "$LOG" "$LOG.1"; fi

    if [ -n "$WAITED" ]; then
        say "$WAITED"
        WAITED=
    fi
    if [ -n "$NOTE" ]; then
        say "$NOTE"
        NOTE=
    fi
    say "starting version $VERSION"
    NBTAGENT_LOG=$LOG "$HERE/nbtagent" "$@" < /dev/null >> "$LOG" 2>&1
    status=$?
    case $status in
        "$EXIT_STOPPED")
            say "the agent was stopped"
            exit 0
            ;;
        "$EXIT_USAGE" | "$EXIT_NO_RESTART")
            say "the agent ended with status $status, which a new start would not change"
            exit $status
            ;;
        "$EXIT_RESTART")
            say "the agent asked to be started again"
            exit $EXIT_RESTART
            ;;
        *)
            if [ -n "$TRYING" ]; then
                say "the agent ended with status $status, on its one try"
                exit $status
            fi
            failures=$((failures + 1))
            say "the agent ended with status $status: failure $failures of $MAX_FAILURES"
            if [ $failures -ge $MAX_FAILURES ]; then
                say "giving up"
                exit 1
            fi
            sleep "$PAUSE"
            ;;
    esac
done
