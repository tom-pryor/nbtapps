#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""`make agent-launcher`: the starter (agent/nbtstart.sh), a version's launcher (agent/nbtlaunch.sh) and the first
install (agent/hu_install.sh), with no car.

  python3 tests/e2e/run_launcher.py

Each case is a folder under build/bench/work/launcher that holds an agent's folder as the starter expects it
(`home`), and what the case's agents are to do: the starter is there as `launcher`, and each version's folder
holds its program and its launcher. Most agents here are a few lines of shell that stand in for nbtagent: one
writes down that it was started, as which version and with which arguments, and ends with the status the case's
`plan` names for that start. Four cases start the real nbtagent on a folder and end it with a signal. Two more
have nbtagent update itself under the starter, to a second build of it that says it is a later version: once with
a controller that confirms the new version, once with a new version that falls over first. The package of that
version is made here as `make agent-pkg` makes the one for a head unit (tools/agent_pkg.py), of the two programs
in the bench image, and signed with the test key. In those two the starter in place is another one than the
package's: the confirmed version puts its own there, and a version that is not confirmed leaves it alone. The
install script is run on folders that stand in for the USB stick, one of which is that package unpacked: its
payload folder is a first install.

Three cases are about there being one starter: a second one beside a first, the file a killed starter left,
and the wait before the first start that the iDrive plugin asks for. Two are about a version's launcher being
the version's own: a new version whose launcher does not run fails its try, and each version is started by the
launcher in its folder, with what the starter hands it.

`home` stands in for flash and `<case>/ram` for the RAM disk, where the launcher keeps the log of every start;
a try that fails has its log copied to `home` once. Two cases hold what `home` and the agent's own folders are
before a start against what they are after
it: names, sizes, modes, inodes and the times of the last change, which is every write but one to a file's time
of last access.

The scripts start programs, so all of it runs in the bench container (tools/bench.py sh, which needs
`make bench-image` first): this program writes the folders and one script that runs every case, hands that script
to one container, and then looks at what each case left behind. The container's sh is Debian's dash, which the
starter is told to run a version's launcher with (NBT_TEST_LAUNCH_SH). The head unit's is ksh, which nothing here
runs.
"""
import os, re, shlex, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import agent_pkg  # noqa: E402
import nbtpkg  # noqa: E402

WORK = os.path.join(ROOT, "build", "bench", "work", "launcher")
STARTER = os.path.join(ROOT, "agent", "nbtstart.sh")
LAUNCHER = os.path.join(ROOT, "agent", "nbtlaunch.sh")
INSTALL = os.path.join(ROOT, "agent", "hu_install.sh")
BENCH = [sys.executable, os.path.join(ROOT, "tools", "bench.py"), "sh"]
PAUSE = "0.05"                                   # between a failure and the next start; the launcher's own is 2 s
REAL_SOCKET = "/tmp/nbt-launch.sock"             # in the container: a local socket's path has to be short

# Stands in for nbtagent. It is <case>/home/<version>/nbtagent, and <case>/plan has a line for each start: an exit
# status, `kill` (ends by SIGKILL, as a program that fell over), `confirm` (does what an agent on trial does
# when it confirms itself, and asks to be started again), or `stay` (runs for a second, then ends as stopped).
FAKE_AGENT = r'''#!/bin/sh
here=${0%/*}
version=${here##*/}
top=$(cd "$here/../.." && pwd)
if [ "$1" = --version ]; then
    echo "nbtagent $version (build fake)"
    exit 0
fi
{ printf '%s' "$version"; for a in "$@"; do printf '|%s' "$a"; done; echo; } >> "$top/starts"
echo "$NBTAGENT_LOG" >> "$top/lognames"
n=$(($(wc -l < "$top/starts")))
step=$(sed -n "${n}p" "$top/plan")
echo "fake agent $version, start $n"
echo "fake agent $version, start $n, on standard error" >&2
case $step in
    kill) kill -KILL $$ ;;
    stay) sleep 1; exit 0 ;;
    confirm)
        echo "$version" > "$top/home/current.new" && mv "$top/home/current.new" "$top/home/current" &&
            mv "$top/home/trial.started" "$top/confirmed"
        exit 4
        ;;
    *) exit "${step:-0}" ;;
esac
'''

# The real nbtagent, which gets the signal the plan names for this start once it says that it is listening. The
# shell becomes the agent, so the signal goes to the agent. Before the signal's name the plan's line may have
# `ask`: nbtctl says hello and asks for the head unit's profile first; and `fill`: the log is made longer than the
# agent lets it be before the agent has said a word.
REAL_AGENT = r'''#!/bin/sh
here=${0%/*}
top=$(cd "$here/../.." && pwd)
echo real >> "$top/starts"
n=$(($(wc -l < "$top/starts")))
step=$(sed -n "${n}p" "$top/plan")
socket=
before=
for a in "$@"; do
    if [ "$before" = --socket ]; then socket=$a; fi
    before=$a
done
case " $step " in
    *" fill "*) dd if=/dev/zero bs=1024 count=600 2>/dev/null | tr '\0' 'x' >> "$NBTAGENT_LOG"; echo >> "$NBTAGENT_LOG" ;;
esac
if [ -n "$step" ]; then
    (
        while ! grep -q "listening at" "$NBTAGENT_LOG" 2>/dev/null; do sleep 0.05; done
        case " $step " in
            *" ask "*) /opt/nbtagent/nbtctl --socket "$socket" info > "$top/asked$n.out" 2>&1; echo $? > "$top/asked$n.status" ;;
        esac
        kill -"${step##* }" $$
    ) &
fi
exec /opt/nbtagent/nbtagent "$@"
'''

# What a write would change, of everything in the folders named: see the top of this file.
SNAPSHOT = "stat -c '%n|%F|%a|%s|%i|%y|%z'"


class Failed(Exception):
    pass


def check(condition, what, detail=""):
    if not condition:
        raise Failed(what + ("\n" + detail if detail else ""))


def write(path, text, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, mode)


def read(path):
    with open(path) as f:
        return f.read()


# Every run starts from nothing: the cases below make their folders as they are defined.
if os.path.isdir(WORK):
    shutil.rmtree(WORK)
os.makedirs(WORK)

driver = []     # the lines of the script that runs every case
cases = []      # (name, the function that looks at what the case left behind)


class Case:
    def __init__(self, name):
        self.name = name
        self.dir = os.path.join(WORK, name)
        self.home = os.path.join(self.dir, "home")
        self.ram = os.path.join(self.dir, "ram")
        self.ram_log = os.path.join(self.ram, "log")
        self.trial_log = os.path.join(self.home, "log", "trial.log")
        self.commands = 0
        os.makedirs(self.dir)

    def path(self, *parts):
        return os.path.join(self.dir, *parts)

    def make_home(self, versions, current=None, trial=None, started=None, agent=FAKE_AGENT, starter=None):
        """An agent's folder as hu_install.sh leaves one: the starter in it as `launcher`, and in each version's
        folder the program, the version's launcher and the starter. `starter` is the starter's text when it is to
        be another one than agent/nbtstart.sh."""
        os.makedirs(self.home)
        text = read(STARTER) if starter is None else starter
        write(os.path.join(self.home, "launcher"), text, 0o755)
        for version in versions:
            write(os.path.join(self.home, version, "nbtagent"), agent, 0o755)
            write(os.path.join(self.home, version, "nbtlaunch.sh"), read(LAUNCHER), 0o755)
            write(os.path.join(self.home, version, "nbtstart.sh"), text, 0o755)
        for name, text in (("current", current), ("trial", trial), ("trial.started", started)):
            if text is not None:
                write(os.path.join(self.home, name), text)

    def plan(self, steps):
        write(self.path("plan"), "".join(step + "\n" for step in steps))

    def run(self, argv, env=None, cwd=None, timed=False):
        """Adds a command to the driver; returns the name its output and status are kept under."""
        self.commands += 1
        label = "run%d" % self.commands
        variables = dict(env or {})
        line = "env " + " ".join(shlex.quote("%s=%s" % item) for item in sorted(variables.items())) + " " if variables else ""
        line += " ".join(shlex.quote(word) for word in argv)
        if cwd is not None:
            line = "cd " + shlex.quote(cwd) + " && " + line
        out = shlex.quote(self.path(label))
        if timed:
            driver.append("date +%%s%%N > %s.t0" % out)
        driver.append("( %s ) > %s.out 2> %s.err; echo $? > %s.status" % (line, out, out, out))
        if timed:
            driver.append("date +%%s%%N > %s.t1" % out)
        return label

    def launch_env(self, pause=PAUSE):
        return {"NBT_TEST_LAUNCH_PAUSE": pause, "NBT_TEST_LAUNCH_RUN": self.ram_log, "NBT_TEST_LAUNCH_SH": "sh"}

    def launch(self, args=(), pause=PAUSE, **more):
        return self.run(["sh", os.path.join(self.home, "launcher")] + list(args), env=self.launch_env(pause), **more)

    def snapshot(self, label, whole=(), below=()):
        """Adds to the driver: what `whole` folders and everything in them are now, and what is in the `below`
        ones, which are (folder, a folder in it to leave out)."""
        out = shlex.quote(self.path(label + ".snap"))
        driver.append(": > " + out)
        for folder in whole:
            driver.append("find %s -exec %s {} + | sort >> %s" % (shlex.quote(folder), SNAPSHOT, out))
        for folder, leave_out in below:
            driver.append("find %s -mindepth 1 -path %s -prune -o -exec %s {} + | sort >> %s"
                          % (shlex.quote(folder), shlex.quote(os.path.join(folder, leave_out)), SNAPSHOT, out))

    def snap(self, label):
        return read(self.path(label + ".snap"))

    def move(self, source, target):
        driver.append("mv %s %s" % (shlex.quote(source), shlex.quote(target)))

    def status(self, label="run1"):
        return int(read(self.path(label + ".status")))

    def out(self, label="run1"):
        return read(self.path(label + ".out"))

    def err(self, label="run1"):
        return read(self.path(label + ".err"))

    def seconds(self, label="run1"):
        return (int(read(self.path(label + ".t1"))) - int(read(self.path(label + ".t0")))) / 1e9

    def starts(self):
        if not os.path.exists(self.path("starts")):
            return []
        return read(self.path("starts")).split("\n")[:-1]

    def log(self, back=0):
        """The log of the last start, or of the start `back` before it; None if there is no such file."""
        path = os.path.join(self.ram_log, "agent.log" + (".%d" % back if back else ""))
        return read(path) if os.path.exists(path) else None

    def kept(self):
        """The copy in the agent's folder of what a try said that failed; None if there is none."""
        return read(self.trial_log) if os.path.isfile(self.trial_log) else None

    def lognames(self):
        return read(self.path("lognames")).split("\n")[:-1]

    def home_file(self, name):
        path = os.path.join(self.home, name)
        return read(path) if os.path.isfile(path) else None

    def describe(self):
        """What the case left behind, for a failure's message."""
        text = "starts %r\n" % self.starts()
        for back in (2, 1, 0):
            if self.log(back) is not None:
                text += "--- agent.log%s\n%s" % (".%d" % back if back else "", self.log(back)[:4000])
        if self.kept() is not None:
            text += "--- home/log/trial.log\n%s" % self.kept()[:4000]
        for name in sorted(os.listdir(self.dir)):
            if name.startswith("run") and os.path.isfile(self.path(name)) and os.path.getsize(self.path(name)) > 0:
                text += "--- %s\n%s" % (name, read(self.path(name)))
        return text


def case(function):
    c = Case(function.__name__.replace("_", "-"))
    cases.append((c.name, function(c), c))
    return function


def in_order(text, *lines):
    at = 0
    for line in lines:
        found = text.find(line + "\n", at)
        if found < 0:
            return False
        at = found + len(line) + 1
    return True


# ---- the starter and a version's launcher -----------------------------------------------------------------------------

@case
def plain(c):
    c.make_home(["1.0.0"], current="1.0.0\n")
    c.plan(["0"])
    c.launch(["--alpha", "b c", ""])

    def look():
        check(c.status() == 0 and c.out() == "" and c.err() == "", "an agent that is stopped ends the launcher, which prints nothing")
        check(c.starts() == ["1.0.0|--alpha|b c|"], "the version in current, with the launcher's arguments as they were")
        check(in_order(c.log(), "launcher: starting version 1.0.0", "fake agent 1.0.0, start 1",
                       "fake agent 1.0.0, start 1, on standard error", "launcher: the agent was stopped"),
              "the log holds the launcher's lines and both of the agent's outputs")
        check(c.log(1) is None and os.listdir(c.ram_log) == ["agent.log"], "one start, one log file, on the RAM disk")
        check(sorted(os.listdir(c.home)) == ["1.0.0", "current", "launcher"], "and nothing new in the agent's folder")
        check(c.lognames() == [os.path.join(c.ram_log, "agent.log")], "the agent is told which file its log is")
    return look


@case
def started_from_its_folder(c):
    c.make_home(["2.0.0-rc.1+b7"], current="2.0.0-rc.1+b7")      # every kind of character a name may hold, no line end
    c.plan(["0"])
    c.run(["sh", "launcher"], env=c.launch_env(), cwd=c.home)

    def look():
        check(c.status() == 0 and c.starts() == ["2.0.0-rc.1+b7"], "started as `sh launcher` from its own folder")
    return look


@case
def second_launcher(c):
    c.make_home(["1.0.0", "1.1.0"], current="1.0.0\n")
    c.plan(["stay"])
    # The first in the background; the second once the first's agent runs. A trial that turns up between the two
    # is the first one's to try, at its next start, and not the second one's.
    both = ('sh "$0" & first=$!; sleep 0.4; echo 1.1.0 > "$1/trial"; sh "$0"; echo "second $?"; '
            'wait $first; echo "first $?"')
    c.run(["sh", "-c", both, os.path.join(c.home, "launcher"), c.home], env=c.launch_env())

    def look():
        check(c.out() == "second 3\nfirst 0\n", "the second launcher ends with status 3, the first as it would have", c.out())
        check("launcher: another launcher is running here (process " in c.err() and c.err().count("\n") == 1,
              "and says why, on standard error", c.err())
        check(c.starts() == ["1.0.0"], "one agent was started")
        check(c.home_file("trial") == "1.1.0\n" and c.home_file("trial.started") is None, "the trial has not had its try")
        check(os.listdir(c.ram_log) == ["agent.log"], "no log was renamed, and the first launcher's file is gone with it",
              repr(os.listdir(c.ram_log)))
    return look


@case
def file_of_a_killed_launcher(c):
    c.make_home(["1.0.0"], current="1.0.0\n")
    c.plan(["0"])
    write(os.path.join(c.ram_log, "launcher.pid"), "3999999\n")      # no process has this id
    c.launch()

    def look():
        check(c.status() == 0 and c.err() == "" and c.starts() == ["1.0.0"], "a file whose launcher is gone is taken over", c.err())
        check(os.listdir(c.ram_log) == ["agent.log"], "and removed when this launcher ends", repr(os.listdir(c.ram_log)))
    return look


@case
def waits_before_the_first_start(c):
    c.make_home(["1.0.0"], current="1.0.0\n")
    c.plan(["4", "0"])
    env = c.launch_env()
    env["NBT_LAUNCH_DELAY"] = "1"
    c.run(["sh", os.path.join(c.home, "launcher")], env=env, timed=True)
    env = c.launch_env()
    env["NBT_LAUNCH_DELAY"] = "1s"
    c.run(["sh", os.path.join(c.home, "launcher")], env=env, timed=True)

    def look():
        check(c.status() == 0 and c.starts() == ["1.0.0"] * 3, "two starts by the first launcher, one by the second")
        check(1.0 <= c.seconds() < 1.9, "one wait, before the first start and not before the second", "%.2f s" % c.seconds())
        check(c.log(2).startswith("launcher: waited 1 s before this start, as NBT_LAUNCH_DELAY says\n") and
              "waited" not in c.log(1), "the log of the first start says so")
        check(c.status("run2") == 0 and c.seconds("run2") < 0.9 and "waited" not in c.log(),
              "a value that is not a number of seconds is no wait", "%.2f s" % c.seconds("run2"))
    return look


@case
def trial_that_fails(c):
    c.make_home(["1.0.0", "1.1.0"], current="1.0.0\n", trial="1.1.0\n")
    c.plan(["1", "0"])
    c.launch()

    def look():
        check(c.status() == 0 and c.starts() == ["1.1.0", "1.0.0"], "the trial once, then the version in current")
        check(c.home_file("trial") is None and c.home_file("trial.started") == "1.1.0\n", "trial was renamed, not removed")
        check(c.home_file("current") == "1.0.0\n", "the launcher does not write current")
        check(in_order(c.log(1), "launcher: trying version 1.1.0, once; starter revision 2", "launcher: starting version 1.1.0",
                       "fake agent 1.1.0, start 1", "launcher: the agent ended with status 1, on its one try",
                       "launcher: the try of version 1.1.0 ended with status 1",
                       "launcher: what this try said is kept as " + c.trial_log), "the log of the try, on the RAM disk")
        check(c.kept() is not None and c.kept().endswith("launcher: the try of version 1.1.0 ended with status 1\n") and
              c.log(1).startswith(c.kept()) and c.kept().startswith("launcher: trying version 1.1.0, once; starter revision 2\n"),
              "and a copy of it in the agent's folder, up to the try's end")
        check(os.listdir(os.path.join(c.home, "log")) == ["trial.log"], "which is all that folder holds")
        check(in_order(c.log(), "launcher: version 1.1.0 was tried and has not confirmed itself; %s is what the last try said"
                       " that failed while the head unit was on; starter revision 2" % c.trial_log, "launcher: starting version 1.0.0"),
              "the log of the way back")
        check(c.lognames() == [os.path.join(c.ram_log, "agent.log")] * 2, "both agents are told the same file for their log")
    return look


@case
def trial_that_confirms(c):
    c.make_home(["1.0.0", "1.1.0"], current="1.0.0\n", trial="1.1.0\n")
    c.plan(["confirm", "0"])
    c.launch()

    def look():
        check(c.status() == 0 and c.starts() == ["1.1.0", "1.1.0"], "a version that confirmed itself is started from current")
        check(c.home_file("current") == "1.1.0\n" and c.home_file("trial") is None and c.home_file("trial.started") is None,
              "what the agent wrote")
        check(in_order(c.log(1), "launcher: trying version 1.1.0, once; starter revision 2", "launcher: the agent asked to be started again"),
              "status 4, in the log of the try")
        check("tried" not in c.log() and "trying" not in c.log() and "kept" not in c.log(1), "nothing about a trial once it is over")
        check(sorted(os.listdir(c.home)) == ["1.0.0", "1.1.0", "current", "launcher"], "a try that works leaves no log in the agent's folder")
    return look


@case
def trial_already_started(c):
    c.make_home(["1.0.0", "1.1.0"], current="1.0.0\n", started="1.1.0\n")
    c.plan(["0"])
    c.launch()

    def look():
        check(c.status() == 0 and c.starts() == ["1.0.0"], "a try that began before is not begun again")
        check(c.home_file("trial.started") == "1.1.0\n", "trial.started is the agent's to remove")
        check(sorted(os.listdir(c.home)) == ["1.0.0", "1.1.0", "current", "launcher", "trial.started"],
              "and a start that tries nothing leaves no log in the agent's folder")
        check("launcher: version 1.1.0 was tried and has not confirmed itself; no try has left a log; starter revision 2\n" in c.log(),
              "the log says that the try's own is gone")
    return look


@case
def trial_beside_an_old_one(c):
    c.make_home(["1.0.0", "1.1.0", "1.2.0"], current="1.0.0\n", trial="1.2.0\n", started="1.1.0\n")
    c.plan(["0"])
    c.launch()

    def look():
        check(c.status() == 0 and c.starts() == ["1.2.0"], "a new trial is tried whatever was tried before")
        check(c.home_file("trial") is None and c.home_file("trial.started") == "1.2.0\n", "and takes the old one's place")
        check(c.kept() is None and not os.path.exists(os.path.join(c.home, "log")), "a try that was stopped is not one that failed: no copy of its log")
    return look


@case
def trial_with_no_line_end(c):
    c.make_home(["1.0.0", "1.1.0"], current="1.0.0\n", trial="1.1.0")
    c.plan(["0"])
    c.launch()

    def look():
        check(c.status() == 0 and c.starts() == ["1.1.0"], "a name with no line end after it")
    return look


LONG = "1" * 65
BAD_TRIALS = [("empty", ""), ("blank", "\n"), ("up", "../1.0.0\n"), ("up-inside", "1.0.0/../1.1.0\n"), ("letter", "v1.1.0\n"),
              ("two-words", "1.1.0 x\n"), ("not-there", "9.9.9\n"), ("long", LONG + "\n"), ("not-a-program", "1.5.0\n"),
              ("no-launcher", "1.6.0\n"), ("a-file-beside", "log\n")]


def bad_trial(c, content):
    # Every folder a bad name could lead to holds an agent that would say so if it were started.
    c.make_home(["1.0.0", "1.1.0", "v1.1.0", LONG], current="1.0.0\n", trial=content)
    write(os.path.join(c.home, "1.5.0", "nbtagent"), FAKE_AGENT, 0o644)
    write(os.path.join(c.home, "1.5.0", "nbtlaunch.sh"), read(LAUNCHER), 0o755)
    write(os.path.join(c.home, "1.6.0", "nbtagent"), FAKE_AGENT, 0o755)     # a program, and no launcher to start it
    c.plan(["0"])
    c.launch()

    def look():
        check(c.status() == 0 and c.starts() == ["1.0.0"], "a trial that names nothing usable: the version in current")
        check(c.home_file("trial") is None and c.home_file("trial.started") == content, "it is renamed all the same")
        check("launcher: the file trial names no version that is here; starter revision 2\n" in c.log(), "and the log says so")
        check(not os.path.exists(os.path.join(c.home, "log")), "on the RAM disk: nothing was tried")
    return look


for label, content in BAD_TRIALS:
    c = Case("bad-trial-" + label)
    cases.append((c.name, bad_trial(c, content), c))


def nothing_to_start(c, current):
    c.make_home(["1.0.0"], current=current, trial="1.0.0/..\n")
    c.plan(["0"])
    c.launch()

    def look():
        check(c.status() == 1 and c.starts() == [], "no version to start: status 1 and nothing started")
        check("nothing to start" in c.err() and "launcher: the file current names no version that is here: nothing to start\n" in c.log(),
              "said on standard error and in the log")
    return look


for label, content in (("none", None), ("up", "../1.0.0\n"), ("not-there", "1.0.1\n"), ("empty", "")):
    c = Case("no-current-" + label)
    cases.append((c.name, nothing_to_start(c, content), c))


def ends_with(c, status):
    c.make_home(["1.0.0"], current="1.0.0\n")
    c.plan([str(status), "0"])
    c.launch()

    def look():
        check(c.status() == status and c.starts() == ["1.0.0"], "status %d ends the launcher with the same" % status)
        check("launcher: the agent ended with status %d, which a new start would not change\n" % status in c.log(), "the log")
    return look


for status in (2, 3):
    c = Case("status-%d" % status)
    cases.append((c.name, ends_with(c, status), c))


@case
def restarts(c):
    c.make_home(["1.0.0"], current="1.0.0\n")
    c.plan(["4"] * 6 + ["0"])
    c.launch(pause="30")     # not taken: a restart that was asked for comes at once

    def look():
        check(c.status() == 0 and len(c.starts()) == 7, "an agent that asks is started again, however often")
        check("fake agent 1.0.0, start 7\n" in c.log() and "fake agent 1.0.0, start 6\n" in c.log(1) and
              "fake agent 1.0.0, start 5\n" in c.log(2), "each start has its log, and the two before it are kept")
        check(sorted(os.listdir(c.ram_log)) == ["agent.log", "agent.log.1", "agent.log.2"], "and no more than those")
    return look


@case
def failures(c):
    c.make_home(["1.0.0"], current="1.0.0\n")
    c.plan(["1", "7", "kill", "126", "1", "0"])
    c.launch()

    def look():
        check(c.status() == 1 and len(c.starts()) == 5, "five failures in a row: the launcher gives up with status 1")
        check(in_order(c.log(), "launcher: the agent ended with status 1: failure 5 of 5", "launcher: giving up"), "the last log")
        check("launcher: the agent ended with status 126: failure 4 of 5\n" in c.log(1), "the log before it")
        check("launcher: the agent ended with status 137: failure 3 of 5\n" in c.log(2), "an agent that a signal ended is a failure")
    return look


@case
def failures_with_a_restart_between(c):
    c.make_home(["1.0.0"], current="1.0.0\n")
    c.plan(["1", "1", "1", "1", "4", "1", "1", "1", "1", "0"])
    c.launch()

    def look():
        check(c.status() == 0 and len(c.starts()) == 10, "a restart that was asked for starts the count again")
    return look


@case
def pause(c):
    c.make_home(["1.0.0"], current="1.0.0\n")
    c.plan(["1", "0"])
    c.launch(pause="1", timed=True)

    def look():
        check(c.status() == 0 and len(c.starts()) == 2, "one failure, then a start that is stopped")
        check(c.seconds() >= 1.0, "the launcher waits before it starts an agent that failed", "%.2f s" % c.seconds())
    return look


@case
def real_agent(c):
    c.make_home(["1.0.0"], current="1.0.0\n", agent=REAL_AGENT)
    c.plan(["KILL", "TERM"])
    c.launch(["--root", c.path("root"), "--socket", REAL_SOCKET])

    def look():
        check(c.status() == 0 and c.starts() == ["real", "real"], "nbtagent killed, started again, then stopped")
        check("listening at " + REAL_SOCKET in c.log(1) and "launcher: the agent ended with status 137: failure 1 of 5\n" in c.log(1),
              "the agent that was killed")
        check("listening at " + REAL_SOCKET in c.log() and in_order(c.log(), "launcher: starting version 1.0.0") and
              c.log().endswith("stopped\nlauncher: the agent was stopped\n"), "the agent that was stopped", c.log())
    return look


@case
def a_failed_try_takes_the_place_of_the_last(c):
    c.make_home(["1.0.0", "1.1.0"], current="1.0.0\n", trial="1.1.0\n")
    write(c.trial_log, "launcher: trying version 1.0.5, once\nthe try before\n")
    c.plan(["kill", "0"])
    c.launch()

    def look():
        check(c.status() == 0 and c.starts() == ["1.1.0", "1.0.0"], "the trial once, then the version in current")
        check(c.kept().startswith("launcher: trying version 1.1.0, once; starter revision 2\n") and
              "launcher: the agent ended with status 137, on its one try\n" in c.kept() and
              c.kept().endswith("launcher: the try of version 1.1.0 ended with status 137\n") and "the try before" not in c.kept(),
              "one copy: of the last try that failed", c.kept())
        check(os.listdir(os.path.join(c.home, "log")) == ["trial.log"], "and nothing beside it")
    return look


@case
def trial_that_a_new_start_would_not_help(c):
    # Status 3 ends the starter when it is the version in current that ends with it. Of a version on trial it is
    # a try that failed like any other: the version before may well be one that runs here.
    c.make_home(["1.0.0", "1.1.0"], current="1.0.0\n", trial="1.1.0\n")
    c.plan(["3", "0"])
    c.launch()

    def look():
        check(c.status() == 0 and c.starts() == ["1.1.0", "1.0.0"], "a version on trial that ends with 3: the version in current is started")
        check(c.kept() is not None and "launcher: the agent ended with status 3, which a new start would not change\n" in c.kept() and
              c.kept().endswith("launcher: the try of version 1.1.0 ended with status 3\n"), "and what it said is kept", c.kept())
        check(c.log(1).endswith("launcher: what this try said is kept as %s\n" % c.trial_log), "which the log says")
    return look


# What a new version's launcher is when it does not run on this head unit: the shell cannot read it to its end.
NO_LAUNCHER = "#!/bin/sh\nif [ -n \"$1\" ]; then\n    echo 'an if with no fi'\n"


@case
def trial_whose_launcher_does_not_run(c):
    c.make_home(["1.0.0", "1.1.0"], current="1.0.0\n", trial="1.1.0\n")
    write(os.path.join(c.home, "1.1.0", "nbtlaunch.sh"), NO_LAUNCHER, 0o755)
    c.plan(["0"])
    c.launch()

    def look():
        check(c.status() == 0 and c.starts() == ["1.0.0"], "the new version's agent is never started, and the version in current is")
        check(c.home_file("trial") is None and c.home_file("trial.started") == "1.1.0\n" and c.home_file("current") == "1.0.0\n",
              "the try is over: the agent of the version before finds trial.started and undoes the update")
        check(c.kept() is not None and "launcher: the try of version 1.1.0 ended with status " in c.kept(),
              "what is known of the try is kept in the agent's folder", str(c.kept()))
        check(in_order(c.log(), "launcher: version 1.1.0 was tried and has not confirmed itself; %s is what the last try said"
                       " that failed while the head unit was on; starter revision 2" % c.trial_log, "launcher: starting version 1.0.0",
                       "fake agent 1.0.0, start 1", "launcher: the agent was stopped"), "the log of the way back")
        check("nbtlaunch.sh" in c.err(), "what the shell said of the launcher goes where the starter's own output goes", c.err())
    return look


# A launcher of a version's own: it writes down what the starter handed it, and starts its agent once.
OWN_LAUNCHER = r'''#!/bin/sh
here=${0%/*}
echo "${here##*/}|$NBT_LAUNCH_STARTER|$NBT_LAUNCH_TRIAL|$NBT_LAUNCH_NOTE|$#" >> "$here/../../handed"
"$here/nbtagent" "$@"
'''


@case
def each_version_has_its_own_launcher(c):
    c.make_home(["1.0.0", "1.1.0"], current="1.0.0\n", trial="1.1.0\n")
    write(os.path.join(c.home, "1.1.0", "nbtlaunch.sh"), OWN_LAUNCHER, 0o755)
    c.plan(["confirm", "0"])
    c.launch(["--root", "/somewhere"])

    def look():
        check(c.status() == 0 and c.starts() == ["1.1.0|--root|/somewhere"] * 2, "the new version, on trial and then from current")
        check(read(c.path("handed")) == "1.1.0|1|1|trying version 1.1.0, once; starter revision 2|2\n1.1.0|1||starter revision 2|2\n",
              "both times by the launcher in its own folder, which is told when it is a try, and why", read(c.path("handed")))
        check(c.log() is None, "and the launcher of the version before, which keeps the log, was never run")
    return look


@case
def try_that_a_power_down_ended(c):
    # What a power down in the middle of a try leaves: trial.started, an empty RAM disk, and in the agent's folder
    # the log of an earlier try, if one failed.
    c.make_home(["1.0.0", "1.1.0"], current="1.0.0\n", started="1.1.0\n")
    write(c.trial_log, "launcher: trying version 1.0.5, once\nthe try before\n")
    c.plan(["0"])
    c.snapshot("before", whole=[c.home])
    c.launch()
    c.snapshot("after", whole=[c.home])

    def look():
        check(c.status() == 0 and c.starts() == ["1.0.0"], "the version in current")
        check(c.snap("before") == c.snap("after") and c.trial_log in c.snap("before"), "nothing in the agent's folder is written",
              c.snap("before") + "---\n" + c.snap("after"))
        check("launcher: version 1.1.0 was tried and has not confirmed itself; %s is what the last try said that failed while"
              " the head unit was on; starter revision 2\n" % c.trial_log in c.log(), "and the log names the copy that is there")
    return look


@case
def failed_try_with_no_folder_for_its_log(c):
    c.make_home(["1.0.0", "1.1.0"], current="1.0.0\n", trial="1.1.0\n")
    write(os.path.join(c.home, "log"), "a file where the folder would be\n")
    c.plan(["1", "0"])
    c.launch()

    def look():
        check(c.status() == 0 and c.starts() == ["1.1.0", "1.0.0"], "the launcher goes on to the version in current")
        check("launcher: what this try said could not be kept as %s\n" % c.trial_log in c.log(1), "and says that there is no copy")
        check(c.home_file("log") == "a file where the folder would be\n", "the file is left alone")
        check("has not confirmed itself; no try has left a log; starter revision 2\n" in c.log(), "and the way back finds none")
    return look


@case
def no_folder_for_the_log(c):
    c.make_home(["1.0.0"], current="1.0.0\n")
    write(c.ram, "a file where the folder would be\n")
    c.plan(["0"])
    c.launch()

    def look():
        check(c.status() == 1 and c.starts() == [], "nowhere to keep the log: status 1 and nothing started")
        check("launcher: cannot make " + c.ram_log in c.err(), "said on standard error")
    return look


@case
def writes_nothing(c):
    # Every way an agent that is not on trial can end, with a trial that was begun before left where it is.
    c.make_home(["1.0.0", "1.1.0"], current="1.0.0\n", started="1.1.0\n")
    c.plan(["1", "4", "kill", "7", "0"])
    c.snapshot("before", whole=[c.home])
    c.launch()
    c.snapshot("after", whole=[c.home])

    def look():
        check(c.status() == 0 and c.starts() == ["1.0.0"] * 5, "five starts of the version in current")
        check(c.snap("before") == c.snap("after") and c.home in c.snap("before"), "the agent's folder is as it was, to the last time stamp",
              c.snap("before") + "---\n" + c.snap("after"))
        check("fake agent 1.0.0, start 5\n" in c.log() and c.kept() is None, "the log is on the RAM disk, and only there")
    return look


@case
def real_agent_writes_nothing(c):
    c.make_home(["1.0.0"], current="1.0.0\n", agent=REAL_AGENT)
    root = c.path("root")
    args = ["--root", root, "--socket", REAL_SOCKET]
    flash = dict(whole=[c.home], below=[(root, "run")])      # the folder's `run` stands in for the RAM disk
    c.plan(["TERM", "ask TERM", "ask TERM"])
    first = c.launch(args)                                   # a new head unit: this start makes the folders
    c.snapshot("first", **flash)
    again = c.launch(args)
    c.snapshot("again", **flash)
    c.move(os.path.join(root, "run"), c.path("was-run"))     # a power down: the RAM disk is empty
    c.move(c.ram, c.path("was-ram"))
    after = c.launch(args)
    c.snapshot("after", **flash)

    def look():
        check(c.status(first) == 0 and c.status(again) == 0 and c.status(after) == 0 and c.starts() == ["real"] * 3, "three starts, each stopped")
        for n in (2, 3):
            check(read(c.path("asked%d.status" % n)) == "0\n" and "headUnitId" in read(c.path("asked%d.out" % n)),
                  "start %d answered a controller" % n, read(c.path("asked%d.out" % n)))
        check(os.path.join(root, "state") in c.snap("first") and os.path.join(root, "run") not in c.snap("first"),
              "the agent's own folders are in what is compared", c.snap("first"))
        check(c.snap("first") == c.snap("again"), "an agent that is started again writes nothing to flash",
              c.snap("first") + "---\n" + c.snap("again"))
        check(c.snap("first") == c.snap("after"), "nor does the first one after a power down",
              c.snap("first") + "---\n" + c.snap("after"))
        check("listening at " + REAL_SOCKET in c.log() and c.log(1) is None, "its log is on the RAM disk, which was empty")
    return look


@case
def real_agent_with_a_long_log(c):
    c.make_home(["1.0.0"], current="1.0.0\n", agent=REAL_AGENT)
    c.plan(["fill TERM"])
    c.launch(["--root", c.path("root"), "--socket", REAL_SOCKET])

    def look():
        check(c.status() == 0 and c.starts() == ["real"], "nbtagent started and stopped")
        older = os.path.join(c.ram_log, "agent.log.1")
        check(c.log(1).startswith("launcher: starter revision 2\nlauncher: starting version 1.0.0\nxxxx") and len(c.log(1)) > 600 * 1024,
              "what the log held is set aside")
        check(c.log().split("\n")[0].endswith("nbtagent: the log had passed 512 KB and was begun again; what it held is in " + older),
              "the new file says so in its first line", c.log()[:400])
        check(len(c.log()) < 8192 and c.log().endswith("stopped\nlauncher: the agent was stopped\n"),
              "and the log goes on in a new file, the launcher's lines with it", c.log()[:4000])
        check(sorted(os.listdir(c.ram_log)) == ["agent.log", "agent.log.1"], "two files")
    return look


@case
def real_agent_started_wrongly(c):
    c.make_home(["1.0.0"], current="1.0.0\n", agent=REAL_AGENT)
    c.plan([])
    c.launch(["--no-such-option"])

    def look():
        check(c.status() == 2 and c.starts() == ["real"], "nbtagent with an argument it does not take is not started again")
        check("usage: nbtagent" in c.log(), "its usage is in the log")
    return look


# ---- the agent updates itself -----------------------------------------------------------------------------------------
# nbtagent itself, under the starter, on a folder that stands in for the head unit: the agent's own folder is
# <root>/agent there. The image's nbtagent is the agent's version as agent/version.hpp has it (VERSION), and its
# folder there has to have that name: an agent takes its own package only where the folder it runs from is named
# after the version it is. The next version (NEXT_VERSION) comes in a package of the image's second build, which
# says it is that version, of the image's nbtctl, and of agent/'s launcher and starter. nbtctl plays the phone:
# it offers the package, and its `status` is what a phone's job.status is.
#
# The starter in place, and in the folder of the version that runs, is another one than the package's: the one
# of a version before, which differs by a line. A new version puts its own in place once it is confirmed.

with open(os.path.join(ROOT, "agent", "version.hpp")) as _f:
    VERSION = re.search(r'^const char kAgentVersion\[\] = "(.*)";$', _f.read(), re.M).group(1)
NEXT_VERSION = "1.2.0"      # what the image's nbtagent-next says it is: AGENT_NEXT_VERSION in the Makefile
OLDER_STARTER = read(STARTER) + "# This line is the test's: the starter as a version before had it.\n"
next_package = []           # the path of that version's package, once it is made


def _numbers(version):
    return [int(n) for n in version.split("-")[0].split("+")[0].split(".")]


# An agent takes only a version above its own, so the second build has to say a higher one.
if _numbers(NEXT_VERSION) <= _numbers(VERSION):
    sys.exit("run_launcher: the agent is version %s, so its second build cannot be %s: raise AGENT_NEXT_VERSION in the "
             "Makefile and NEXT_VERSION here" % (VERSION, NEXT_VERSION))


def next_agent_package():
    """org.nbtapp.agent of the next version, by the recipe that makes the agent's package for a head unit: the
    programs come out of the image, where they were built, and the test key signs what tools/agent_pkg.py makes
    of them."""
    if not next_package:
        folder = os.path.join(WORK, "next-agent")
        programs = os.path.join(folder, "programs")
        os.makedirs(programs)
        script = os.path.join(folder, "copy.sh")
        write(script, "cp /opt/nbtagent/nbtagent-next %s/nbtagent && cp /opt/nbtagent/nbtctl %s/nbtctl\n"
              % (shlex.quote(programs), shlex.quote(programs)))
        done = subprocess.run(BENCH + [script], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if done.returncode != 0:
            sys.exit("run_launcher: the image's programs could not be copied out\n" + done.stdout.decode("utf-8", "replace"))
        name, data, _ = agent_pkg.build(programs, NEXT_VERSION)
        seed = nbtpkg.read_key(os.path.join(ROOT, "tests", "keys", "test-package.key"))
        with open(os.path.join(folder, name), "wb") as f:
            f.write(nbtpkg.sign_package(data, seed))
        next_package.append(os.path.join(folder, name))
    return next_package[0]


# <root>/agent/<VERSION>/nbtagent: the image's nbtagent, which writes down which process it is so that the case
# can end it. The next version is the program itself, as its package places it, and writes nothing down: the
# case's script finds it by its program (note_pid).
AGENT_NOW = r"""#!/bin/sh
if [ -n "$NBT_TEST_AGENT_PID" ] && [ "$1" != --version ]; then echo $$ > "$NBT_TEST_AGENT_PID"; fi
exec /opt/nbtagent/nbtagent "$@"
"""

# The start of every such case: the starter on the agent's version, which the script has as $now, a controller
# that offers the next version, $next, and the wait for the starter to have started that version on trial.
UPDATE_START = r"""
top=%(top)s
now=%(version)s
next=%(next)s
root=$top/root
home=$root/agent
log=$top/ram/log/agent.log
ctl="/opt/nbtagent/nbtctl --socket %(socket)s"
wait_for() {
    while ! grep -q "$2" "$1" 2> /dev/null; do sleep 0.05; done
}
# Which process the agent of a version is, when that agent is the program its package placed: /proc names every
# process's program.
note_pid() {
    program=$(cd "$home/$1" && pwd -P)/nbtagent
    for p in /proc/[0-9]*; do
        if [ "$(readlink "$p/exe" 2> /dev/null)" = "$program" ]; then echo "${p#/proc/}" > "$top/pid"; fi
    done
}
export NBT_TEST_LAUNCH_PAUSE=%(pause)s NBT_TEST_LAUNCH_RUN=$top/ram/log NBT_TEST_LAUNCH_SH=sh NBT_TEST_AGENT_PID=$top/pid
launch() {
    sh "$home/launcher" --root "$root" --socket %(socket)s --test-key %(key)s > "$top/launcher.out" 2>&1 &
    launcher=$!
}
launch
wait_for "$log" "nbtagent: $now (build .*listening at"
$ctl info > "$top/info-before.out" 2>&1
$ctl install --from "$top/stick" org.nbtapp.agent > "$top/install.out" 2>&1
echo $? > "$top/install.status"
wait_for "$log" "nbtagent: $next (build .*listening at"
note_pid $next
cat "$home/current" > "$top/current-on-trial"
cat "$home/trial.started" > "$top/started-on-trial"
cat "$home/launcher" > "$top/starter-on-trial"
"""


def update_case(c, steps):
    """A case of the kind above, whose script goes on with `steps` once the next version is on trial."""
    root = c.path("root")
    c.home = os.path.join(root, "agent")
    c.trial_log = os.path.join(c.home, "log", "trial.log")
    c.make_home([VERSION], current=VERSION + "\n", agent=AGENT_NOW, starter=OLDER_STARTER)
    os.makedirs(c.path("stick"))
    shutil.copy(next_agent_package(), c.path("stick"))
    values = {"top": shlex.quote(c.dir), "version": VERSION, "next": NEXT_VERSION, "socket": "/tmp/nbt-%s.sock" % c.name, "pause": PAUSE,
              "key": shlex.quote(os.path.join(ROOT, "tests", "keys", "test-package.pub"))}
    write(c.path("update.sh"), UPDATE_START % values + steps)
    return c.run(["sh", c.path("update.sh")])


def said(c, name):
    path = c.path(name)
    return read(path) if os.path.isfile(path) else "(no %s)" % name


@case
def agent_updates_itself(c):
    update_case(c, r"""
$ctl info > "$top/info-on-trial.out" 2>&1                  # a controller that does not ask for the job
cat "$home/current" > "$top/current-after-info"
$ctl status --events > "$top/status.out" 2>&1              # and one that does: the confirmation
echo $? > "$top/status.status"
$ctl list > "$top/list.out" 2>&1
kill -TERM "$(cat "$top/pid")"
wait $launcher
echo $? > "$top/launcher.status"
ls "$home" > "$top/home-after"
# The starter again, as after a power down, which empties the RAM disk: the new version, from current, with
# nothing to finish.
mv "$root/run" "$top/was-run"
mv "$top/ram" "$top/was-ram"
find "$home" "$root/state" -exec stat -c '%%n|%%F|%%a|%%s|%%i|%%y|%%z' {} + | sort > "$top/before.snap"
launch
wait_for "$log" "nbtagent: $next (build .*listening at"
note_pid $next
$ctl info > "$top/info-after.out" 2>&1
kill -TERM "$(cat "$top/pid")"
wait $launcher
echo $? > "$top/launcher2.status"
find "$home" "$root/state" -exec stat -c '%%n|%%F|%%a|%%s|%%i|%%y|%%z' {} + | sort > "$top/after.snap"
""".replace("%%", "%"))

    def look():
        check(c.status() == 0, "the case's script ran to its end", c.err())
        check("agentVersion %s\n" % VERSION in said(c, "info-before.out"), "the launcher started version " + VERSION, said(c, "info-before.out"))
        install = said(c, "install.out")
        check(said(c, "install.status") == "3\n" and "org.nbtapp.agent: restarting" in install and
              "The agent is restarting to finish its own update" in install,
              "the job went as far as restarting, and the link ended with the agent", install)
        check(said(c, "current-on-trial") == VERSION + "\n" and said(c, "started-on-trial") == NEXT_VERSION + "\n",
              "the starter started %s on trial, with current still naming %s" % (NEXT_VERSION, VERSION))
        check(said(c, "starter-on-trial") == OLDER_STARTER, "a version on trial leaves the starter in place as it is")
        check("agentVersion %s" % NEXT_VERSION in said(c, "info-on-trial.out") and said(c, "current-after-info") == VERSION + "\n",
              "the new version answers, and a controller that does not ask for the job confirms nothing", said(c, "info-on-trial.out"))
        status = said(c, "status.out")
        check(said(c, "status.status") == "0\n" and "succeeded" in status and "restarting" not in status.split("\n")[0],
              "the job's status, asked of the new version, is that it succeeded", status)
        check("a controller has reached the new version of the agent" in status, "and the job's log says what confirmed it", status)
        check("org.nbtapp.agent %s " % NEXT_VERSION in said(c, "list.out"), "the agent's own package is installed", said(c, "list.out"))
        check(c.home_file("current") == NEXT_VERSION + "\n" and c.home_file("trial.started") is None and c.home_file("trial") is None,
              "current names the new version, and the trial file is gone")
        check(said(c, "home-after").split() == [VERSION, NEXT_VERSION, "current", "launcher"],
              "both versions stay, and a try that worked leaves no log on flash", said(c, "home-after"))
        placed = os.path.join(c.home, NEXT_VERSION)
        check(sorted(os.listdir(placed)) == ["nbtagent", "nbtctl", "nbtlaunch.sh", "nbtstart.sh"] and
              read(os.path.join(placed, "nbtlaunch.sh")) == read(LAUNCHER) and all(executable(os.path.join(placed, n)) for n in os.listdir(placed)),
              "the new version's folder holds its programs, its launcher and its starter", repr(os.listdir(placed)))
        check(c.home_file("launcher") == read(STARTER) and executable(os.path.join(c.home, "launcher")),
              "and the confirmed version has put its starter in place of the one that was there")
        check(said(c, "launcher.status") == "0\n" and said(c, "launcher2.status") == "0\n", "each starter ended with its agent")
        check("agentVersion %s" % NEXT_VERSION in said(c, "info-after.out"), "the starter starts the new version from current", said(c, "info-after.out"))
        check(read(c.path("before.snap")) == read(c.path("after.snap")) and os.path.join(c.home, "current") in read(c.path("before.snap")),
              "and that start writes nothing to the agent's folders", read(c.path("before.snap")) + "---\n" + read(c.path("after.snap")))
        # What the three agents and the launcher said: the first two before the power down, the last after it.
        old, tried = said(c, "was-ram/log/agent.log.1"), said(c, "was-ram/log/agent.log")
        check(in_order(old, "launcher: starting version " + VERSION) and "ending, for the launcher to start it" in old and
              "launcher: the agent asked to be started again" in old, "the old version ended with status 4", old[-3000:])
        check(in_order(tried, "launcher: trying version %s, once; starter revision 2" % NEXT_VERSION, "launcher: starting version " + NEXT_VERSION) and
              "ON TRIAL" in tried and "It is confirmed" in tried and "launcher: the agent was stopped" in tried,
              "the new version was tried once, and confirmed", tried[-3000:])
        check("launcher is now the starter that version %s came with" % NEXT_VERSION in tried and "is now the starter" not in old,
              "and says that it has put its starter in place", tried[-3000:])
        check("trying" not in c.log() and "ON TRIAL" not in c.log() and "is now the starter" not in c.log() and c.log(1) is None,
              "and the start after that is an ordinary one", c.log())
    return look


@case
def agent_update_nobody_confirms(c):
    update_case(c, r"""
$ctl info > "$top/info-on-trial.out" 2>&1
kill -KILL "$(cat "$top/pid")"                             # the new version falls over before a phone asks
wait_for "$log" "nbtagent: $now (build .*listening at"
$ctl status --events > "$top/status.out" 2>&1
echo $? > "$top/status.status"
$ctl info > "$top/info-after.out" 2>&1
$ctl list > "$top/list.out" 2>&1
ls "$home" > "$top/home-after"
# And the update is there to be run again.
$ctl plan --from "$top/stick" org.nbtapp.agent > "$top/plan-again.out" 2>&1
echo $? > "$top/plan-again.status"
kill -TERM "$(cat "$top/pid")"
wait $launcher
echo $? > "$top/launcher.status"
""")

    def look():
        check(c.status() == 0, "the case's script ran to its end", c.err())
        check(said(c, "install.status") == "3\n" and said(c, "started-on-trial") == NEXT_VERSION + "\n" and
              "agentVersion %s" % NEXT_VERSION in said(c, "info-on-trial.out"), "version %s was placed and ran on trial" % NEXT_VERSION,
              said(c, "install.out"))
        status = said(c, "status.out")
        check("failed" in status and "not-confirmed" in status and "the update of the agent was undone" in status,
              "the old version is back, and says that the job failed for want of a confirmation", status)
        check("agentVersion %s\n" % VERSION in said(c, "info-after.out") and "org.nbtapp.agent" not in said(c, "list.out"),
              "it is version %s that answers, and nothing of the agent's package is installed" % VERSION, said(c, "info-after.out"))
        check(said(c, "home-after").split() == [VERSION, "current", "launcher", "log"] and c.home_file("current") == VERSION + "\n",
              "the new version's folder and the trial file are gone", said(c, "home-after"))
        check(c.kept() is not None and c.kept().startswith("launcher: trying version %s, once; starter revision 2\n" % NEXT_VERSION) and "ON TRIAL" in c.kept() and
              c.kept().endswith("launcher: the try of version %s ended with status 137\n" % NEXT_VERSION),
              "what the try said is kept on flash", str(c.kept())[:2000])
        check(in_order(c.log(), "launcher: version %s was tried and has not confirmed itself; %s is what the last try said"
                       " that failed while the head unit was on; starter revision 2" % (NEXT_VERSION, c.trial_log), "launcher: starting version " + VERSION) and
              "the update of the agent to %s was undone" % NEXT_VERSION in c.log(), "the log of the way back", c.log()[:3000])
        check(c.home_file("launcher") == OLDER_STARTER, "and the starter in place is the one that was there")
        check(said(c, "plan-again.status") == "0\n" and "org.nbtapp.agent" in said(c, "plan-again.out"),
              "and the update can be planned again", said(c, "plan-again.out"))
        check(said(c, "launcher.status") == "0\n", "the starter ended with its agent")
    return look


# ---- the first install ----------------------------------------------------------------------------------------------

def make_stick(c, name, version, agent=FAKE_AGENT, leave_out=None):
    """A folder as release/agent/ is, with a stand-in for each program."""
    stick = c.path(name)
    write(os.path.join(stick, "nbtagent"), agent, 0o644)       # a stick's files are not executable
    write(os.path.join(stick, "nbtctl"), "#!/bin/sh\nexit 0\n", 0o644)
    write(os.path.join(stick, "version"), version)
    shutil.copy(LAUNCHER, os.path.join(stick, "nbtlaunch.sh"))
    shutil.copy(STARTER, os.path.join(stick, "nbtstart.sh"))
    shutil.copy(INSTALL, os.path.join(stick, "hu_install.sh"))
    for script in ("nbtlaunch.sh", "nbtstart.sh", "hu_install.sh"):
        os.chmod(os.path.join(stick, script), 0o644)             # a stick's files are not executable
    if leave_out:
        os.remove(os.path.join(stick, leave_out))
    return stick


def install(c, stick):
    return c.run(["sh", os.path.join(stick, "hu_install.sh")], env={"NBT_TEST_AGENT_HOME": c.home})


def executable(path):
    return os.path.isfile(path) and os.stat(path).st_mode & 0o777 == 0o755


@case
def install_and_start(c):
    first = install(c, make_stick(c, "stick", "1.2.3\n"))
    again = install(c, c.path("stick"))
    newer = install(c, make_stick(c, "stick2", "1.3.0\n"))
    c.plan(["0"])
    started = c.launch(["--root", "/somewhere"])

    def look():
        check(c.status(first) == 0 and c.err(first) == "", "the first install", c.err(first))
        check("hu_install: nbtagent 1.2.3 (build fake)\n" in c.out(first) and
              "hu_install: To start it:  %s/launcher &\n" % c.home in c.out(first) and
              "hu_install: Its log:      /tmp/nbtapp/log/agent.log, which a power down empties\n" in c.out(first) and
              "hu_install:   echo on > %s/autostart\n" % c.home in c.out(first),
              "what it says", c.out(first))
        for version in ("1.2.3", "1.3.0"):
            check(sorted(os.listdir(os.path.join(c.home, version))) == ["nbtagent", "nbtctl", "nbtlaunch.sh", "nbtstart.sh"],
                  "the version's folder holds what the agent's package places", repr(os.listdir(os.path.join(c.home, version))))
            for program in ("nbtagent", "nbtctl", "nbtlaunch.sh", "nbtstart.sh"):
                check(executable(os.path.join(c.home, version, program)), "%s/%s is there and executable" % (version, program))
        check(read(os.path.join(c.home, "1.2.3", "nbtagent")) == FAKE_AGENT, "the program is the stick's")
        check(read(os.path.join(c.home, "1.2.3", "nbtlaunch.sh")) == read(LAUNCHER), "the version's launcher is nbtlaunch.sh")
        check(executable(os.path.join(c.home, "launcher")) and c.home_file("launcher") == read(STARTER), "the starter in place is nbtstart.sh")

        check(c.status(again) == 1 and c.out(again) == "", "a version that is there is not written over")
        check("hu_install:   rm -rf %s/1.2.3\n" % c.home in c.err(again), "and the script says how to remove it", c.err(again))

        check(c.status(newer) == 0 and c.home_file("current") == "1.3.0\n", "a newer version becomes the one to start")
        check(c.status(started) == 0 and c.starts() == ["1.3.0|--root|/somewhere"], "and the launcher starts it")
        check(sorted(os.listdir(c.home)) == ["1.2.3", "1.3.0", "current", "launcher"], "nothing else is left in the folder")
    return look


@case
def install_from_the_package(c):
    """The agent's own package is a zip that holds what a first install needs: unpacked onto a stick, its payload
    folder is one that hu_install.sh runs from. The programs are the image's, so this is nbtagent's own answer
    to --version that the script holds against payload/version."""
    stick = c.path("stick")
    with open(next_agent_package(), "rb") as f:
        files = {e["name"]: e["data"] for e in nbtpkg.read_zip(f.read()) if not e["dir"]}
    for name, data in files.items():                           # as an unzip leaves them: nothing is executable
        path = os.path.join(stick, *name.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    done = c.run(["sh", os.path.join(stick, "payload", "hu_install.sh")], env={"NBT_TEST_AGENT_HOME": c.home})

    def look():
        check(c.status(done) == 0 and c.err(done) == "", "the install from the package's payload folder", c.err(done))
        check("hu_install: nbtagent %s (build " % NEXT_VERSION in c.out(done), "the program says it is the package's version", c.out(done))
        for program in ("nbtagent", "nbtctl", "nbtlaunch.sh", "nbtstart.sh"):
            placed = os.path.join(c.home, NEXT_VERSION, program)
            check(executable(placed), "%s/%s is there and executable" % (NEXT_VERSION, program))
            with open(placed, "rb") as f:
                check(f.read() == files["payload/" + program], "and is the package's")
        check(executable(os.path.join(c.home, "launcher")) and c.home_file("launcher") == read(STARTER), "the starter in place is nbtstart.sh")
        check(c.home_file("current") == NEXT_VERSION + "\n", "the package's version is the one to start")
        check(sorted(os.listdir(c.home)) == [NEXT_VERSION, "current", "launcher"], "nothing else is left in the folder")
    return look


def install_refused(c, why, version="1.0.0\n", agent=FAKE_AGENT, leave_out=None, trial=None, leaves=()):
    stick = make_stick(c, "stick", version, agent, leave_out)
    if trial is not None:
        write(os.path.join(c.home, trial), "1.1.0\n")
    install(c, stick)

    def look():
        check(c.status() == 1 and why in c.err(), "the install is refused, and says why", c.err())
        left = sorted(os.listdir(c.home)) if os.path.isdir(c.home) else []
        check(left == sorted(leaves), "what it leaves behind", repr(left))
    return look


NOT_THIS_VERSION = "#!/bin/sh\necho 'nbtagent 0.9.0 (build fake)'\n"
DOES_NOT_START = "#!/bin/sh\nexit 1\n"
for label, options in (
        ("no-program", dict(why="nbtctl is not there", leave_out="nbtctl")),
        ("no-launcher", dict(why="nbtlaunch.sh is not there", leave_out="nbtlaunch.sh")),
        ("no-starter", dict(why="nbtstart.sh is not there", leave_out="nbtstart.sh")),
        ("bad-version", dict(why="version does not hold a version", version="../1.0.0\n")),
        ("empty-version", dict(why="version does not hold a version", version="")),
        ("update-waiting", dict(why="an update of the agent was begun", trial="trial", leaves=["trial"])),
        ("update-under-way", dict(why="an update of the agent was begun", trial="trial.started", leaves=["trial.started"])),
        ("another-build", dict(why="that is not version 1.0.0", agent=NOT_THIS_VERSION, leaves=["1.0.0"])),
        ("does-not-start", dict(why="nbtagent does not start here", agent=DOES_NOT_START, leaves=["1.0.0"]))):
    c = Case("install-" + label)
    cases.append((c.name, install_refused(c, **options), c))


def main():
    script = os.path.join(WORK, "driver.sh")
    # Ends the container if a case hangs: the container lives as long as this script.
    lines = ["( sleep 150; echo 'run_launcher: a case did not end' >&2; kill -KILL $$ ) &"] + driver
    write(script, "".join(line + "\n" for line in lines))
    done = subprocess.run(BENCH + [script], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    failed = 0
    if done.returncode != 0:
        print("FAIL  the cases did not all run (status %d)\n%s" % (done.returncode, done.stdout.decode("utf-8", "replace")))
        failed += 1
    for name, look, c in cases:
        try:
            look()
        except Failed as failure:
            print("FAIL  %s: %s\n%s" % (name, failure, c.describe()))
            failed += 1
        except Exception as failure:     # a check that fell over: a file that is not there, a log that is None
            print("FAIL  %s: %r" % (name, failure))
            failed += 1
    if failed:
        print("run_launcher: %d of %d failed; the cases' folders are in %s" % (failed, len(cases), os.path.relpath(WORK, ROOT)))
        return 1
    print("run_launcher: ok (%d cases)" % len(cases))
    return 0


if __name__ == "__main__":
    sys.exit(main())
