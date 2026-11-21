#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""A stand-in for Dropbear in the relay's tests (run_relay.py), started by the relay in the server's place through
its test hook, NBT_SSHRELAY_TEST_SERVER, with the server's arguments; its standard input and output are the
server's end of the socket pair.

  standin_server.py MODE STATE ARGS...

It first writes STATE/server-<pid>.json: its arguments, environment, open files, process group and session, and
what the -D folder holds with the modes of the folder and of authorized_keys. Then, by MODE:

  echo            sends back what it reads, until its input ends
  slow            the same, reading 16 KiB at a time, 10 ms apart
  source N        writes N bytes of pattern() at once, then reads until its input ends
  burst-exit N    writes N bytes of pattern() and exits at once
  leftovers       starts what a login leaves behind (below), then echoes until its input ends
  leftovers-exit  the same, waits 1.5 s and exits by itself; one of them keeps the socket open

What it leaves behind, with the pids in STATE/pids-<pid>.json:
  inSession    in the server's session, in a group of its own, ignoring SIGHUP: a background job
  plain        in the server's own group and session, with SIGHUP as it comes
  leader       a session of its own, as a login shell on a pty leads one
  leaderChild  in the leader's session, in a group of its own, ignoring SIGHUP
  outsider     a session of its own that the server did not start (its parent is one of the server's children,
               which has ended), ignoring SIGHUP: not one of the server's, so the relay must leave it alone

When its input ends it writes STATE/eof-<pid>; in echo and leftovers, if STATE/stay-<pid> is there by then, it does
not exit but stays, as a server whose relay died might.
"""
import json, os, signal, stat, sys, time


def pattern(n):
    """The bytes the stand-in writes, which the test compares: i % 251 for i from 0."""
    block = bytes(i % 251 for i in range(251 * 64))
    out = bytearray()
    while len(out) < n:
        out += block
    return bytes(out[:n])


def put(path, value):
    with open(path + ".tmp", "w") as f:
        json.dump(value, f)
    os.rename(path + ".tmp", path)


def record(state, args):
    fds = {}
    for name in os.listdir("/proc/self/fd"):
        try:
            fds[name] = os.readlink("/proc/self/fd/" + name)
        except OSError:
            pass
    info = {
        "pid": os.getpid(), "args": args, "env": dict(os.environ), "fds": fds,
        "group": os.getpgid(0), "session": os.getsid(0),
        "stdinSocket": stat.S_ISSOCK(os.fstat(0).st_mode), "sameInOut": os.path.samestat(os.fstat(0), os.fstat(1)),
    }
    if "-D" in args:
        keys = args[args.index("-D") + 1]
        info["keysDir"] = keys
        info["keysDirMode"] = stat.S_IMODE(os.stat(keys).st_mode)
        info["keysMode"] = stat.S_IMODE(os.stat(keys + "/authorized_keys").st_mode)
        info["keysFiles"] = sorted(os.listdir(keys))
        with open(keys + "/authorized_keys") as f:
            info["keys"] = f.read()
    put(os.path.join(state, "server-%d.json" % os.getpid()), info)


def to_null(keep_socket):
    if not keep_socket:
        null = os.open("/dev/null", os.O_RDWR)
        os.dup2(null, 0)
        os.dup2(null, 1)
        os.close(null)


def stay():
    while True:
        time.sleep(3600)


def leave_behind(state, keep_socket):
    pids = {}
    ready_r, ready_w = os.pipe()

    def child(name, body):
        pid = os.fork()
        if pid == 0:
            os.close(ready_r)
            try:
                body()
            finally:
                os._exit(0)
        pids[name] = pid

    def in_session():
        signal.signal(signal.SIGHUP, signal.SIG_IGN)
        os.setpgid(0, 0)
        to_null(keep_socket)
        os.write(ready_w, b"x")
        stay()

    def plain():
        to_null(False)
        os.write(ready_w, b"x")
        stay()

    def leader():
        os.setsid()
        to_null(False)
        inner = os.fork()
        if inner == 0:
            signal.signal(signal.SIGHUP, signal.SIG_IGN)
            os.setpgid(0, 0)
            os.write(ready_w, b"%d\n" % os.getpid())
            stay()
        stay()

    def outsider_parent():
        to_null(False)
        inner = os.fork()
        if inner == 0:
            os.setsid()
            signal.signal(signal.SIGHUP, signal.SIG_IGN)
            os.write(ready_w, b"o%d\n" % os.getpid())
            stay()
        os._exit(0)

    child("inSession", in_session)
    child("plain", plain)
    child("leader", leader)
    child("outsiderParent", outsider_parent)
    os.close(ready_w)
    got = b""
    while got.count(b"x") < 2 or got.count(b"\n") < 2:
        part = os.read(ready_r, 64)
        if not part:
            break
        got += part
    os.close(ready_r)
    for line in got.replace(b"x", b"").split(b"\n"):
        if line.startswith(b"o"):
            pids["outsider"] = int(line[1:])
        elif line:
            pids["leaderChild"] = int(line)
    os.waitpid(pids.pop("outsiderParent"), 0)
    put(os.path.join(state, "pids-%d.json" % os.getpid()), pids)


def echo(state, slow=False):
    while True:
        data = os.read(0, 16384 if slow else 65536)
        if not data:
            break
        view = memoryview(data)
        while view:
            n = os.write(1, view)
            view = view[n:]
        if slow:
            time.sleep(0.01)
    open(os.path.join(state, "eof-%d" % os.getpid()), "w").close()
    if os.path.exists(os.path.join(state, "stay-%d" % os.getpid())):
        stay()


def main():
    mode, state, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    count = 0
    if mode in ("source", "burst-exit"):
        count, args = int(args[0]), args[1:]
    record(state, args)
    if mode == "echo":
        echo(state)
    elif mode == "slow":
        echo(state, slow=True)
    elif mode == "source":
        data = pattern(count)
        sent = 0
        while sent < count:
            sent += os.write(1, data[sent:sent + 65536])
        while os.read(0, 65536):
            pass
        open(os.path.join(state, "eof-%d" % os.getpid()), "w").close()
    elif mode == "burst-exit":
        data = pattern(count)
        sent = 0
        while sent < count:
            sent += os.write(1, data[sent:])
        os._exit(0)
    elif mode == "leftovers":
        leave_behind(state, keep_socket=False)
        echo(state)
    elif mode == "leftovers-exit":
        leave_behind(state, keep_socket=True)
        time.sleep(1.5)
        os._exit(0)
    else:
        sys.exit("standin_server.py: unknown mode " + mode)


if __name__ == "__main__":
    main()
