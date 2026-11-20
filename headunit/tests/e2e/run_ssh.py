#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""`make ssh-e2e`: the SSH package's bench form (tests/packages/ssh-1.0.0) through the agent, with OpenSSH's client.

  python3 tests/e2e/run_ssh.py [BIN] [TESTDATA]      (default build/agent-host and build/testdata)

The agent runs in its container (tools/bench.py agent, started again whenever it ends) on a head unit of its own in
a short folder under /tmp, trusting the test key. BIN's nbtctl, here, installs the package, switches its channel
and opens it once to read the relay's answer. The client is the image's: ssh, scp and sftp, each in a container of
its own (tools/bench.py client) that holds the client's folder and the agent's socket, with the image's nbtctl as
the proxy command, `nbtctl channel org.nbtapp.ssh ssh --raw --send KEYS --skip-first`, written into a small script.
The relay, Dropbear and the shells run in the agent's container, which Docker lists the processes of.

  - before the switch: off, and an open refused off; on: an open is answered with the host key, and "pty" true
  - that key as the client's only known host, checked strictly: a login with the key the keys message names runs a
    command and gives its exit status, and --skip-first's line is the same answer; a login as root, as the app's,
    runs as the container's user (tools/bench.py); another key is refused; another known host key is refused by the
    client
  - a login on a pty (ssh -tt, its input kept open): a tty, its size, and the exit status of the shell
  - 1 MB with scp -O to the head unit's side and back, and with sftp put and get, each compared
  - the switch off with a session open: the client ends; then nothing runs in the agent's container but the agent,
    and the relay's state file is gone
  - an upgrade to ssh-1.0.1 with a session open: the session ends; the switch stays on; the next login is served by
    a relay the new version started, with the host key of before
  - the agent ended and started again while the relay runs: the relay is known again (an open starts none), a
    login works, and the switch off ends the relay
  - removal: nothing of the package runs, and the host key stays in its data folder; a removal with the data takes
    it
Whatever fails, no container of the test and no process of it is left.
"""
import json, os, re, shutil, signal, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "agent-host"))
DATA = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "testdata"))
BENCH = [sys.executable, os.path.join(ROOT, "tools", "bench.py")]
NBTCTL = os.path.join(BIN, "nbtctl")
PACKAGE, CHANNEL = "org.nbtapp.ssh", "ssh"
USER = "bench"        # tools/bench.py's line in the containers' /etc/passwd
# The agent and its socket, the client's folder and the head unit, in a short folder: the channel's socket is
# ${tmp}/channels/ssh in the head unit's folder, and a local socket's path has a limit of about 100 bytes.
TOP = tempfile.mkdtemp(prefix="nbt_ssh_")
HU = os.path.join(TOP, "hu")
SOCK_DIR = os.path.join(TOP, "sock")
SOCKET = os.path.join(SOCK_DIR, "agent.sock")
PIDFILE = os.path.join(SOCK_DIR, "agent.pid")
CLIENT = os.path.join(TOP, "client")
LOG = os.path.join(TOP, "agent.log")
TMP = os.path.join(HU, "tmp", PACKAGE)     # the package's ${tmp} on this head unit
# What may run in the agent's container with nothing open: Docker's init, the shell that starts the agent again,
# and the agent.
IDLE = (re.compile(r"/sbin/docker-init\b"), re.compile(r"/bin/sh -c pidfile="), re.compile(r"/opt/nbtagent/nbtagent\b"))


class Failed(Exception):
    pass


def check(condition, what, detail=""):
    if not condition:
        raise Failed(what + ("\n" + detail if detail else ""))


def ctl(*args, timeout=120):
    done = subprocess.run([NBTCTL, "--socket", SOCKET] + list(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=timeout)
    return done.returncode, done.stdout.decode("utf-8", "replace")


def ok(*args):
    code, out = ctl(*args)
    check(code == 0, "nbtctl %s: exit status %d" % (" ".join(args), code), out)
    return out


def docker(*args):
    done = subprocess.run(["docker"] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    return done.returncode, done.stdout.decode("utf-8", "replace")


def containers(kind, folder):
    """The ids of this test's containers of one kind: those tools/bench.py labelled so that have `folder` bound."""
    code, out = docker("ps", "-q", "--filter", "label=nbtapp-bench=" + kind, "--filter", "volume=" + folder)
    return out.split() if code == 0 else []


class Agent:
    """tools/bench.py agent, with --restart: the agent is started again whenever it ends."""
    started = []    # ended by cleanup(), whatever failed

    def __init__(self):
        self.log = open(LOG, "ab")
        self.process = subprocess.Popen(
            BENCH + ["agent", "--restart", PIDFILE, "--root", HU, "--socket", SOCKET,
                     "--test-key", os.path.join(ROOT, "tests", "keys", "test-package.pub")],
            stdout=self.log, stderr=self.log)
        Agent.started.append(self)
        self.wait_listening()
        # Docker lists a container's mounts a moment after it has started.
        for _ in range(100):
            found = containers("agent", HU)
            if found:
                break
            time.sleep(0.1)
        check(len(found) == 1, "the agent's container is not found", str(found) + "\n" + docker("ps", "--no-trunc")[1])
        self.id = found[0]

    def wait_listening(self):
        for _ in range(400):    # a container takes a moment longer to start than a program
            if os.path.exists(SOCKET) and ctl("info", timeout=10)[0] == 0:
                return
            if self.process.poll() is not None:
                break
            time.sleep(0.05)
        raise Failed("nbtagent did not start listening")

    def processes(self):
        """[(pid, command line)] of what runs in the agent's container."""
        code, out = docker("top", self.id, "-o", "pid,args")
        check(code == 0, "docker top of the agent's container", out)
        rows = []
        for line in out.splitlines()[1:]:
            parts = line.strip().split(None, 1)
            if len(parts) == 2 and parts[0].isdigit():
                rows.append((int(parts[0]), parts[1]))
        return rows

    def running(self, word):
        return [args for _, args in self.processes() if word in args]

    def wait_idle(self, what, seconds=15):
        """Until nothing but the agent runs in its container."""
        until = time.time() + seconds
        while True:
            left = [args for _, args in self.processes() if not any(p.match(args) for p in IDLE)]
            if not left:
                return
            if time.time() > until:
                raise Failed("%s: left running in the agent's container:\n  %s" % (what, "\n  ".join(left)))
            time.sleep(0.2)

    def in_container(self, script):
        code, out = docker("exec", self.id, "/bin/sh", "-c", script)
        check(code == 0, "docker exec %s" % script, out)
        return out

    def restart(self):
        """The agent ended with SIGTERM, inside its container, and started again by the loop around it."""
        with open(PIDFILE) as f:
            pid = int(f.read().strip())
        self.in_container("kill -TERM %d" % pid)
        for _ in range(200):
            with open(PIDFILE) as f:
                if f.read().strip() not in ("", str(pid)):
                    break
            time.sleep(0.05)
        else:
            raise Failed("the agent was not started again")
        self.wait_listening()

    def stop(self):
        """SIGTERM to tools/bench.py, which Docker passes on to the loop around the agent; the container is killed
        if it is still there."""
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                pass
        for cid in containers("agent", HU):
            docker("kill", cid)
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        self.log.close()


class Session:
    """An SSH client of the image, in a container, as a process: its input is kept open until close_input(), and its
    output and errors are read as they come."""
    running = []

    def __init__(self, *command):
        self.process = subprocess.Popen(BENCH + ["client", CLIENT, SOCK_DIR, "--"] + list(command),
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        Session.running.append(self)
        self.got, self.said = [], []
        self.readers = [threading.Thread(target=self._read, args=(self.process.stdout, self.got)),
                        threading.Thread(target=self._read, args=(self.process.stderr, self.said))]
        for reader in self.readers:
            reader.start()

    @staticmethod
    def _read(stream, into):
        while True:
            chunk = stream.read1(65536)
            if not chunk:
                return
            into.append(chunk)

    def send(self, text):
        self.process.stdin.write(text.encode())
        self.process.stdin.flush()

    def close_input(self):
        self.process.stdin.close()

    def output(self):
        return b"".join(self.got).decode("utf-8", "replace")

    def errors(self):
        return b"".join(self.said).decode("utf-8", "replace")

    def wait_for(self, text, seconds=60):
        until = time.time() + seconds
        while text not in self.output() and time.time() < until and self.process.poll() is None:
            time.sleep(0.05)
        return text in self.output()

    def wait(self, seconds=60):
        try:
            code = self.process.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            raise Failed("the client did not end:\n" + self.output() + self.errors())
        for reader in self.readers:
            reader.join(timeout=10)
        if not self.process.stdin.closed:
            try:
                self.process.stdin.close()
            except OSError:
                pass
        Session.running.remove(self)
        return code

    def kill(self):
        self.process.kill()
        self.wait()


def client(*command, seconds=120):
    """A program of the image in a client's container, to its end: (exit status, output, errors)."""
    done = subprocess.run(BENCH + ["client", CLIENT, SOCK_DIR, "--"] + list(command), stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=seconds)
    return done.returncode, done.stdout.decode("utf-8", "replace"), done.stderr.decode("utf-8", "replace")


def key_pair(name):
    """An ed25519 key for the client, in OpenSSH's file format, and its public line."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    key = Ed25519PrivateKey.generate()
    path = os.path.join(CLIENT, name)
    with open(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.OpenSSH,
                                  serialization.NoEncryption()))
    public = key.public_key().public_bytes(serialization.Encoding.OpenSSH, serialization.PublicFormat.OpenSSH).decode()
    return path, public + " " + name


def ssh_options(identity, known_hosts):
    proxy = os.path.join(CLIENT, "proxy.sh")
    return ["-F", "/dev/null", "-i", identity, "-o", "IdentitiesOnly=yes", "-o", "IdentityAgent=none",
            "-o", "ProxyCommand=" + proxy, "-o", "HostKeyAlias=headunit", "-o", "UserKnownHostsFile=" + known_hosts,
            "-o", "GlobalKnownHostsFile=/dev/null", "-o", "StrictHostKeyChecking=yes", "-o", "UpdateHostKeys=no",
            "-o", "BatchMode=yes", "-o", "LogLevel=ERROR"]


def answer_lines():
    """The lines --skip-first has written so far: each connection's answer."""
    try:
        with open(os.path.join(CLIENT, "proxy.err")) as f:
            return [l for l in f.read().splitlines() if l.startswith("{")]
    except OSError:
        return []


def plain_open(keys):
    """The channel opened with BIN's nbtctl, the keys message sent: the relay's answer, its first message. Then the
    channel is closed with SIGTERM."""
    proc = subprocess.Popen([NBTCTL, "--socket", SOCKET, "channel", PACKAGE, CHANNEL, "--send", keys],
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    line = []
    reader = threading.Thread(target=lambda: line.append(proc.stdout.readline()))
    reader.start()
    reader.join(timeout=30)
    proc.send_signal(signal.SIGTERM)
    try:
        code = proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        raise Failed("nbtctl channel did not end at SIGTERM")
    reader.join(timeout=5)
    errors = proc.stderr.read().decode("utf-8", "replace")
    check(code == 0 and line and line[0], "the channel opened with nbtctl: exit status %d" % code, errors)
    return json.loads(line[0].decode())


def run():
    for folder in (HU, SOCK_DIR, CLIENT):
        os.makedirs(folder)
    stick, stick2 = os.path.join(TOP, "stick"), os.path.join(TOP, "stick2")
    os.makedirs(stick)
    os.makedirs(stick2)
    shutil.copy(os.path.join(DATA, "good", PACKAGE + "-1.0.0.nbtpkg"), stick)
    shutil.copy(os.path.join(DATA, "good", PACKAGE + "-1.0.1.nbtpkg"), stick2)

    identity, public = key_pair("e2e_key")
    wrong, _ = key_pair("other_key")
    keys = json.dumps({"v": 1, "keys": [public]}, separators=(",", ":"))
    check("'" not in keys, "the keys message can stand in single quotes")
    with open(os.path.join(CLIENT, "proxy.sh"), "w") as f:
        f.write("#!/bin/sh\n# The SSH client's proxy command: the channel, with the keys message first and the relay's\n"
                "# answer to it on standard error, here kept in proxy.err.\n"
                "exec /opt/nbtagent/nbtctl --socket %s channel %s %s --raw --send '%s' --skip-first 2>>%s\n"
                % (SOCKET, PACKAGE, CHANNEL, keys, os.path.join(CLIENT, "proxy.err")))
    os.chmod(os.path.join(CLIENT, "proxy.sh"), 0o755)
    known = os.path.join(CLIENT, "known_hosts")
    opts = ssh_options(identity, known)
    target = USER + "@headunit"

    try:
        agent = Agent()
        out = ok("install", "--from", stick, PACKAGE)
        check("job succeeded" in out, "the install of the SSH package", out)

        # The switch: off, and refused before anything starts; then on, and the relay's answer to the keys message.
        check(ok("channel", "status", PACKAGE, CHANNEL) == "switched true\non false\nopen 0\n", "the switch is off")
        code, out = ctl("channel", PACKAGE, CHANNEL)
        check(code == 1 and "off" in out, "an open while it is off", out)
        check(not agent.running("sshrelay"), "a relay runs while the switch is off")
        check(ok("channel", "on", PACKAGE, CHANNEL) == "switched true\non true\nopen 0\n", "switched on")
        answer = plain_open(keys)
        check(answer.get("v") == 1 and answer.get("pty") is True and len(answer.get("hostKeys", [])) == 1 and
              re.fullmatch(r"ssh-ed25519 [A-Za-z0-9+/]+=*", answer["hostKeys"][0]) is not None,
              "the relay's answer: one ed25519 host key, and a pty", json.dumps(answer))
        host_key = answer["hostKeys"][0]
        with open(known, "w") as f:
            f.write("headunit %s\n" % host_key)
        keyfiles = [os.path.join(d, n) for d, _, ns in os.walk(HU) for n in ns if n == "hostkey_ed25519"]
        check(len(keyfiles) == 1 and "/data/" in keyfiles[0] and PACKAGE in keyfiles[0],
              "the host key is in the package's data folder", str(keyfiles))
        host_key_file = keyfiles[0]

        # A login with the key the keys message names; a command and its exit status.
        code, out, err = client("ssh", *opts, target, "echo hello from $(id -un); exit 7")
        check(code == 7 and out == "hello from %s\n" % USER, "a login and a command: exit status %d" % code, out + err)
        check(answer_lines() and json.loads(answer_lines()[-1]) == answer,
              "--skip-first's line is the relay's answer, the same host key", "\n".join(answer_lines()))
        # A login as root, as the app logs in to a head unit: the bench's root is the container's user, not real root.
        code, out, err = client("ssh", *opts, "root@headunit", "echo $LOGNAME $(id -u); exit 5")
        check(code == 5 and out == "root %d\n" % os.getuid(), "a login as root: exit status %d" % code, out + err)
        code, out, err = client("ssh", *ssh_options(wrong, known), target, "echo in")
        check(code == 255 and "in" not in out.split() and "Permission denied" in err,
              "a key that the keys message does not name: exit status %d" % code, out + err)
        other_known = os.path.join(CLIENT, "known_hosts_other")
        _, other_public = key_pair("not_the_host")
        with open(other_known, "w") as f:
            f.write("headunit %s\n" % " ".join(other_public.split()[:2]))
        code, out, err = client("ssh", *ssh_options(identity, other_known), target, "echo in")
        check(code == 255 and "Host key verification failed" in err, "another known host key: exit status %d" % code,
              out + err)

        # A login on a pty, its input kept open while the shell runs: Dropbear closes a session's pty when the
        # client's input ends. What is typed is echoed, so the answers are worked out by the shell.
        tty = Session("ssh", "-tt", *opts, target)
        tty.send("tty; stty size; echo pty-done-$((6 * 7))\n")
        check(tty.wait_for("pty-done-42"), "a shell on a pty did not answer", tty.output() + tty.errors())
        tty.send("exit 3\n")
        code = tty.wait()
        # The shell's prompt comes before the first answer, on its line; stty size gives the rows and columns.
        lines = tty.output().replace("\r", "").splitlines()
        check(code == 3 and any(re.search(r"(^|\s)/dev/pts/\d+$", l) for l in lines) and
              any(re.fullmatch(r"\d+ \d+", l) for l in lines), "a shell on a pty: exit status %d" % code, tty.output())

        # 1 MB each way, with scp's own protocol and with sftp.
        up = os.path.join(CLIENT, "up.bin")
        with open(up, "wb") as f:
            f.write(os.urandom(1000003))
        code, out, err = client("scp", "-O", "-q", *opts, up, target + ":/tmp/up.bin")
        check(code == 0, "scp -O to the head unit: exit status %d" % code, out + err)
        down = os.path.join(CLIENT, "scp-back.bin")
        code, out, err = client("scp", "-O", "-q", *opts, target + ":/tmp/up.bin", down)
        check(code == 0 and open(down, "rb").read() == open(up, "rb").read(), "scp -O back: exit status %d" % code, out + err)
        batch = os.path.join(CLIENT, "sftp.batch")
        back = os.path.join(CLIENT, "sftp-back.bin")
        with open(batch, "w") as f:
            f.write("put %s /tmp/sftp.bin\nget /tmp/sftp.bin %s\n" % (up, back))
        code, out, err = client("sftp", "-q", "-b", batch, *opts, target)
        check(code == 0 and open(back, "rb").read() == open(up, "rb").read(), "sftp put and get: exit status %d" % code,
              out + err)

        # The switch off with a session open: the client ends, and so does everything of the package.
        held = Session("ssh", "-tt", *opts, target)
        held.send("echo held-$((1 + 1)); sleep 7777\n")
        check(held.wait_for("held-2"), "a session to switch off", held.output() + held.errors())
        check(agent.running("sleep 7777") and agent.running("sshrelay") and agent.running("dropbear"),
              "the session's processes", "\n".join(a for _, a in agent.processes()))
        check(os.path.isfile(os.path.join(TMP, "sshrelay.state")), "the relay's state file")
        check(ok("channel", "off", PACKAGE, CHANNEL) == "switched true\non false\nopen 0\n", "switched off")
        code = held.wait(30)
        check(code != 0, "the client of a session that was switched off: exit status %d" % code, held.errors())
        agent.wait_idle("the switch off with a session open")
        check(not os.path.exists(os.path.join(TMP, "sshrelay.state")), "the relay's state file is left")

        # An upgrade with a session open: the session ends, the switch stays on, and a relay of the new version
        # serves the next login, with the same host key.
        check(ok("channel", "on", PACKAGE, CHANNEL) == "switched true\non true\nopen 0\n", "switched on again")
        held = Session("ssh", "-tt", *opts, target)
        held.send("echo held-$((2 + 2)); sleep 7777\n")
        check(held.wait_for("held-4"), "a session to upgrade under", held.output() + held.errors())
        out = ok("install", "--from", stick2, PACKAGE)
        check("job succeeded" in out, "the upgrade to 1.0.1 with a session open", out)
        code = held.wait(30)
        check(code != 0, "the client of a session the upgrade ended: exit status %d" % code, held.errors())
        agent.wait_idle("an upgrade with a session open")
        check("1.0.1" in ok("list"), "1.0.1 is installed", ok("list"))
        check(ok("channel", "status", PACKAGE, CHANNEL) == "switched true\non true\nopen 0\n", "the switch after the upgrade")
        before = len(answer_lines())
        code, out, err = client("ssh", *opts, target, "echo upgraded")
        check(code == 0 and out == "upgraded\n", "a login after the upgrade: exit status %d" % code, out + err)
        check(len(answer_lines()) == before + 1 and json.loads(answer_lines()[-1])["hostKeys"] == [host_key],
              "the host key after the upgrade", "\n".join(answer_lines()))
        env = agent.in_container("for d in /proc/[0-9]*; do if tr '\\000' ' ' < $d/cmdline 2>/dev/null | "
                                 "grep -q '^/opt/nbtagent/ssh/sshrelay'; then tr '\\000' '\\n' < $d/environ; fi; done")
        check("NBT_PKG_VERSION=1.0.1" in env.split(), "the relay that served it was started for 1.0.1", env)

        # The agent started again while the relay runs: it knows the relay again, so an open starts no second one,
        # a login works, and the switch off ends the relay.
        relays = agent.running("/opt/nbtagent/ssh/sshrelay")
        check(len(relays) == 1, "one relay before the agent is started again", "\n".join(relays))
        relay_pid = [p for p, a in agent.processes() if a.startswith("/opt/nbtagent/ssh/sshrelay")]
        agent.restart()
        check([p for p, a in agent.processes() if a.startswith("/opt/nbtagent/ssh/sshrelay")] == relay_pid,
              "the relay ran on while the agent was started again", "\n".join(a for _, a in agent.processes()))
        code, out, err = client("ssh", *opts, target, "echo again")
        check(code == 0 and out == "again\n", "a login after the agent was started again: exit status %d" % code, out + err)
        check([p for p, a in agent.processes() if a.startswith("/opt/nbtagent/ssh/sshrelay")] == relay_pid,
              "the agent started a second relay", "\n".join(a for _, a in agent.processes()))
        with open(LOG, "rb") as f:
            log = f.read().decode("utf-8", "replace")
        check("the program of org.nbtapp.ssh ssh, process " in log and "was started by an agent before this one and "
              "still runs" in log, "the agent that was started again does not say it knows the relay")
        ok("channel", "off", PACKAGE, CHANNEL)
        agent.wait_idle("the switch off after the agent was started again")

        # Removal: nothing of the package runs, and its data, the host key, stays until it is asked to go.
        ok("channel", "on", PACKAGE, CHANNEL)
        code, out, err = client("ssh", *opts, target, "true")
        check(code == 0, "a login before the removal: exit status %d" % code, out + err)
        check(agent.running("/opt/nbtagent/ssh/sshrelay"), "the relay before the removal")
        out = ok("remove", PACKAGE)
        check("job succeeded" in out, "the removal", out)
        agent.wait_idle("the removal")
        check(os.path.isfile(host_key_file), "the host key went with a removal that keeps the data")
        out = ok("install", "--from", stick2, PACKAGE)
        check("job succeeded" in out, "the install again", out)
        check(ok("channel", "status", PACKAGE, CHANNEL) == "switched true\non false\nopen 0\n",
              "the switch of a package installed again is off")
        out = ok("remove", "--delete-data", PACKAGE)
        check("job succeeded" in out and not os.path.exists(host_key_file), "the removal with the data", out)
        agent.wait_idle("the removal with the data")
    finally:
        cleanup()
    check(not containers("agent", HU) and not containers("client", CLIENT), "a container of the test is left")
    left = []
    for pid in os.listdir("/proc"):
        if pid.isdigit() and int(pid) != os.getpid():
            try:
                with open("/proc/%s/cmdline" % pid, "rb") as f:
                    command = f.read().replace(b"\0", b" ").decode("utf-8", "replace")
            except OSError:
                continue
            if TOP in command:
                left.append(command)
    check(not left, "processes of the test are left on this machine", "\n".join(left))


def cleanup():
    """Every client and the agent ended, and their containers too: a client that a timeout killed leaves its
    container, which Docker then lists with the client's folder."""
    for left in list(Session.running):
        left.kill()
    for cid in containers("client", CLIENT):
        docker("kill", cid)
    for agent in Agent.started:
        agent.stop()
    del Agent.started[:]


if __name__ == "__main__":
    try:
        run()
    except (Failed, subprocess.TimeoutExpired) as failure:
        print("ssh e2e: FAILED: %s" % failure, file=sys.stderr)
        for name in (LOG, os.path.join(TMP, "channels", "ssh.log"), os.path.join(CLIENT, "proxy.err")):
            if os.path.exists(name):
                print("---- %s\n%s" % (name, open(name, "rb").read().decode("utf-8", "replace")), file=sys.stderr)
        cleanup()
        shutil.rmtree(TOP, ignore_errors=True)
        sys.exit(1)
    shutil.rmtree(TOP, ignore_errors=True)
    print("ssh e2e: ok")
