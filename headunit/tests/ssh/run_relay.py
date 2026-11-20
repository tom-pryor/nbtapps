#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""`make ssh-test`: the relay (ssh/relay/, ssh/README "The relay") on this machine.

  python3 tests/ssh/run_relay.py build/ssh

  - test_key_lines and test_processes, the C++ tests of the key-line rule and of the walk of /proc
  - the protocol, with a stand-in for the server (standin_server.py, through the relay's test hook): no keys
    message in time, an agent's line that cannot be read, a keys message that is not JSON and bad ones, each
    answered bad-keys with no server and no keys folder; a good one answered with the host key, with the keys
    written as sent, in the right modes, and the server started as it should be; a key tool or a server that
    cannot be started (failed); the host key made once and kept
  - the bridge: 1 MiB each way in messages of every size, compared; a server and a client that read slowly, with
    the relay's memory looked at meanwhile
  - the ends: the client goes away; the server exits with bytes still to come; what a server leaves behind (in its
    session, ignoring SIGHUP; in a session of its own that it started) is ended, and what is not of its sessions
    is left; SIGTERM with two connections open
  - the pty driver, with a stand-in (standin_pty.cpp, copied as devc-pty, through the relay's test hooks, its 30 s
    shortened): started at the first connection and its pid found, not for a second, kept by a connection within
    its time, ended after it and not before, started again, ended at the relay's end, another of the same name
    left alone; connections that wait for a late one; one that never appears, none there; one that goes by
    itself; one that ignores SIGTERM; pty false with no ptys, true with another driver's names
  - the limit: four connections, a fifth too-many with nothing made for it, a slot free again after an end
  - the state file, with start times of the size QNX gives (START_OFFSET): a relay killed with SIGKILL, and the
    next start ending its servers, their leftovers and its driver and removing the keys folders; unrelated
    processes named in it, and nonsense (also start times in any form but the one written), signalling none;
    start times up to 2^64 - 1 read; an older relay still running ended before the new one listens, also one that
    does not end at SIGTERM, timed
  - with this machine's Dropbear (build/ssh/host, make ssh-host) and its OpenSSH client, through ssh_proxy.py as
    the proxy command: a login with a command and its exit status, one on a pty, a wrong key refused, the host key
    of the answer the one the client sees, a wrong one refused. Skipped, and said so, when there is no ssh.

Each relay runs in a folder of its own under a temporary folder, which is removed at the end. No process of the
tests is left: at the end every process whose command line names that folder, and every relay this run started,
is looked for, ended and counted as a failure. The last line says how many passed, failed and were skipped.
"""
import json, os, random, shlex, shutil, signal, socket, stat, struct, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SSH = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "ssh"))
RELAY = os.path.join(SSH, "host", "sshrelay")
MULTI = os.path.join(SSH, "host", "dropbearmulti")
SFTP = os.path.join(SSH, "host", "sftp-server")
TESTS = os.path.join(SSH, "test")
STANDIN = os.path.join(HERE, "standin_server.py")
STANDIN_PTY = os.path.join(TESTS, "standin_pty")
PROXY = os.path.join(HERE, "ssh_proxy.py")

sys.path.insert(0, HERE)
from standin_server import pattern  # noqa: E402

results = {"passed": [], "failed": [], "skipped": []}
relays = []          # every relay process started, ended at the end whatever happened
work_root = None


class Failure(Exception):
    pass


def check(ok, what):
    if not ok:
        raise Failure(what)


def wait_for(condition, timeout, step=0.02):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if condition():
            return True
        time.sleep(step)
    return condition()


def alive(pid):
    """Running, and not a zombie."""
    try:
        with open("/proc/%d/stat" % pid) as f:
            return f.read().rsplit(")", 1)[1].split()[0] != "Z"
    except (OSError, IndexError):
        return False


# Added by every relay of the tests to the start times it reads (NBT_SSHRELAY_TEST_START_OFFSET), so that its state
# file holds start times of the size QNX gives (nanoseconds since 1970: 19 digits, far above the 2^53 - 1 the JSON
# reader takes as a number).
START_OFFSET = 1700000000000000000


def start_time(pid):
    """A process's start time as the relay reads it: the 22nd field of /proc/<pid>/stat, with START_OFFSET."""
    with open("/proc/%d/stat" % pid) as f:
        return int(f.read().rsplit(")", 1)[1].split()[19]) + START_OFFSET


def rec(pid, start):
    """A process as the state file records it."""
    return {"pid": pid, "start": str(start)}


class Driver:
    """A copy of the stand-in pty driver (standin_pty.cpp) under the name devc-pty, in a folder of its own."""

    def __init__(self, folder, mode="run"):
        os.makedirs(folder, exist_ok=True)
        self.path = os.path.join(folder, "devc-pty")
        shutil.copy(STANDIN_PTY, self.path)
        self.set_mode(mode)

    def set_mode(self, mode):
        with open(self.path + ".mode", "w") as f:
            f.write(mode + "\n")

    def pids(self):
        """The drivers that run and have not ended by SIGTERM or by themselves, by their markers."""
        folder, base = os.path.split(self.path)
        return sorted(int(n[len(base) + 5:]) for n in os.listdir(folder) if n.startswith(base + ".pid-"))

    def events(self, kind):
        try:
            with open(self.path + ".log") as f:
                return [int(line.split()[1]) for line in f if line.split()[0] == kind]
        except OSError:
            return []

    def args(self, pid):
        with open("%s.pid-%d" % (self.path, pid)) as f:
            return f.read()

    def run_directly(self):
        """Started as the debug set-up would start one: the pid of the driver that runs on."""
        before = set(self.pids())
        subprocess.run([self.path, "-n", "8"], check=True, timeout=10)
        check(wait_for(lambda: set(self.pids()) - before, 5), "the stand-in driver did not start")
        pid, = set(self.pids()) - before
        return pid


class Relay:
    """A relay in a folder of its own: tmp/ (channels/, auth/), data/, app/bin/ with this machine's Dropbear. With
    `driver`, a stand-in pty driver in drv/ in that mode; with `dev`, an empty folder dev/ as the folder of pty names;
    with `driver_ms`, the driver's 30 s shortened."""

    def __init__(self, name, mode=None, mode_args=(), multi=MULTI, data=None, driver=None, dev=False, driver_ms=None):
        self.dir = os.path.join(work_root, name)
        self.tmp = os.path.join(self.dir, "tmp")
        self.data = data or os.path.join(self.dir, "data")
        self.app = os.path.join(self.dir, "app")
        self.state = os.path.join(self.dir, "state")
        for d in (os.path.join(self.tmp, "channels"), self.data, os.path.join(self.app, "bin"), self.state):
            os.makedirs(d, exist_ok=True)
        os.chmod(os.path.join(self.tmp, "channels"), 0o700)
        bin_dir = os.path.join(self.app, "bin")
        if multi:
            os.symlink(multi, os.path.join(bin_dir, "dropbearmulti"))
            os.symlink("dropbearmulti", os.path.join(bin_dir, "scp"))
            os.symlink(SFTP, os.path.join(bin_dir, "sftp-server"))
        self.socket = os.path.join(self.tmp, "channels", "ssh")
        self.auth = os.path.join(self.tmp, "auth")
        self.log_path = os.path.join(self.tmp, "channels", "ssh.log")
        self.server = None
        if mode:
            self.server = os.path.join(self.dir, "standin.sh")
            with open(self.server, "w") as f:
                f.write("#!/bin/sh\nexec %s -I %s %s %s %s \"$@\"\n" % (
                    shlex.quote(sys.executable), shlex.quote(STANDIN), shlex.quote(mode), shlex.quote(self.state),
                    " ".join(shlex.quote(str(a)) for a in mode_args)))
            os.chmod(self.server, 0o755)
        self.driver = Driver(os.path.join(self.dir, "drv"), driver) if driver else None
        self.dev = os.path.join(self.dir, "dev") if dev else None
        if self.dev:
            os.makedirs(self.dev, exist_ok=True)
        self.driver_ms = driver_ms
        self.state_path = os.path.join(self.tmp, "sshrelay.state")
        self.proc = None
        self.start_took = None

    def start(self, env_extra=None):
        """Started as the agent starts it, also over a relay that runs or died in the same folder: returns once
        this one says it listens."""
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "NBT_TMP_DIR": self.tmp, "NBT_DATA_DIR": self.data,
               "NBT_APP_DIR": self.app, "NBT_CHANNEL": "ssh", "NBT_SSHRELAY_TEST_START_OFFSET": str(START_OFFSET)}
        if self.server:
            env["NBT_SSHRELAY_TEST_SERVER"] = self.server
            env["NBT_SSHRELAY_TEST_SERVER_NAME"] = os.path.basename(os.path.realpath(sys.executable))
        if self.driver:
            env["NBT_SSHRELAY_TEST_PTY_DRIVER"] = self.driver.path
        if self.dev:
            env["NBT_SSHRELAY_TEST_DEV"] = self.dev
        if self.driver_ms:
            env["NBT_SSHRELAY_TEST_DRIVER_MS"] = str(self.driver_ms)
        env.update(env_extra or {})
        log = open(self.log_path, "ab")
        started = time.monotonic()
        # As the agent starts it: standard input from /dev/null, its output to the log, a process group of its own.
        self.proc = subprocess.Popen([RELAY], stdin=subprocess.DEVNULL, stdout=log, stderr=log, env=env,
                                     cwd=self.app, start_new_session=True)
        log.close()
        relays.append(self.proc)
        listening = "started, pid %d, listening" % self.proc.pid
        check(wait_for(lambda: listening in self.log() or self.proc.poll() is not None, 5),
              "the relay did not listen within 5 s")
        self.start_took = time.monotonic() - started
        check(self.proc.poll() is None and os.path.exists(self.socket), "the relay exited at its start: " + self.log())
        return self

    def state_file(self):
        with open(self.state_path) as f:
            return json.load(f)

    def log(self):
        try:
            with open(self.log_path, errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_log(self, text, timeout=5):
        check(wait_for(lambda: text in self.log(), timeout), "the log does not say %r:\n%s" % (text, self.log()))

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

    def servers(self):
        """What each stand-in recorded at its start, by pid."""
        out = {}
        for name in os.listdir(self.state):
            if name.startswith("server-") and name.endswith(".json"):
                with open(os.path.join(self.state, name)) as f:
                    info = json.load(f)
                out[info["pid"]] = info
        return out

    def left_behind(self, pid):
        path = os.path.join(self.state, "pids-%d.json" % pid)
        check(wait_for(lambda: os.path.exists(path), 5), "the stand-in did not say what it left behind")
        with open(path) as f:
            return json.load(f)

    def keys_dirs(self):
        return sorted(os.listdir(self.auth)) if os.path.isdir(self.auth) else []


class Client:
    """The agent and the phone on one connection: the agent's line, then two-byte-length messages."""

    def __init__(self, relay, max_message=4096, line=None):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(relay.socket)
        self.sock.settimeout(15)
        self.max_message = max_message
        self.buffer = bytearray()
        if line is None:
            line = json.dumps({"v": 1, "channel": "ssh", "access": "both", "caller": "nbtapps",
                               "phone": "Test Phone", "maxMessage": max_message})
        if line is not False:
            self.sock.sendall(line.encode() + b"\n")

    def send(self, payload):
        self.sock.sendall(struct.pack(">H", len(payload)) + payload)

    def keys(self, lines, version=1):
        self.send(json.dumps({"v": version, "keys": lines}).encode())

    def receive(self, timeout=15):
        """The next message, or None when the relay has closed the connection."""
        self.sock.settimeout(timeout)
        while True:
            if len(self.buffer) >= 2:
                length = struct.unpack(">H", bytes(self.buffer[:2]))[0]
                check(1 <= length <= self.max_message, "a message of %d bytes from the relay" % length)
                if len(self.buffer) >= 2 + length:
                    message = bytes(self.buffer[2:2 + length])
                    del self.buffer[:2 + length]
                    return message
            try:
                part = self.sock.recv(65536)
            except ConnectionResetError:
                # A close with bytes of ours left unread is a reset on this side.
                part = b""
            if not part:
                check(not self.buffer, "the relay closed the connection inside a message")
                return None
            self.buffer += part

    def answer(self):
        message = self.receive()
        check(message is not None, "the relay closed the connection with no answer")
        return json.loads(message)

    def closed(self, timeout=15):
        """True if the relay closes the connection with nothing more sent."""
        return self.receive(timeout) is None

    def close(self):
        self.sock.close()


ED = "AAAAC3NzaC1lZDI1NTE5AAAAIAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8g"
EC = ("AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBAABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4fICEiIy"
      "QlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj8=")
GOOD = ["ssh-ed25519 %s laptop" % ED, "ecdsa-sha2-nistp256 %s" % EC]


def host_key_line(relay):
    out = subprocess.run([MULTI, "dropbearkey", "-y", "-f", os.path.join(relay.data, "hostkey_ed25519")],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, universal_newlines=True, check=True).stdout
    line = [l for l in out.splitlines() if l.startswith("ssh-ed25519 ")][0]
    return " ".join(line.split()[:2])


# ---- protocol ---------------------------------------------------------------------------------------------------

def test_no_keys_in_time():
    r = Relay("nokeys", "echo").start()
    c = Client(r)
    started = time.monotonic()
    check(c.closed(timeout=14), "the relay sent something")
    took = time.monotonic() - started
    check(9.5 <= took <= 12, "the connection was dropped after %.1f s, not 10" % took)
    r.wait_log("no keys message within 10 s")
    check(not r.servers(), "a server was started")
    check(r.keys_dirs() == [], "a keys folder is left: %s" % r.keys_dirs())
    r.stop()


def test_agent_line_unreadable():
    r = Relay("badline", "echo").start()
    c = Client(r, line='{"v":1,"caller":"nbtapps"}')
    try:
        c.keys(GOOD)
    except (BrokenPipeError, ConnectionResetError):
        pass   # the relay may have closed the connection already
    check(c.closed(), "the relay answered a connection whose line has no maxMessage")
    c = Client(r, line="not json")
    check(c.closed(), "the relay answered a connection whose line is not JSON")
    r.wait_log("the agent's line cannot be read")
    check(not r.servers(), "a server was started")
    r.stop()


def test_bad_keys():
    r = Relay("badkeys", "echo").start()
    cases = [
        ("not JSON", b"ssh-ed25519 " + ED.encode()),
        ("an option before the type", json.dumps({"v": 1, "keys": ["no-pty ssh-ed25519 " + ED]}).encode()),
        ("an RSA key", json.dumps({"v": 1, "keys": ["ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAAQE= x"]}).encode()),
        ("nine keys", json.dumps({"v": 1, "keys": [GOOD[0]] * 9}).encode()),
        ("no key", json.dumps({"v": 1, "keys": []}).encode()),
        ("v 2", json.dumps({"v": 2, "keys": GOOD}).encode()),
        ("a good line and a bad one", json.dumps({"v": 1, "keys": [GOOD[0], "ssh-ed25519 " + ED + "\nno-pty"]}).encode()),
    ]
    for what, message in cases:
        c = Client(r)
        c.send(message)
        answer = c.answer()
        check(answer == {"error": "bad-keys"}, "%s: answered %r" % (what, answer))
        check(c.closed(), "%s: the connection stayed open after the error" % what)
        c.close()
    check(not r.servers(), "a server was started for a bad keys message")
    check(r.keys_dirs() == [], "a keys folder is left: %s" % r.keys_dirs())
    check(r.log().count("refused, bad-keys") == len(cases), "the log does not name each refusal:\n" + r.log())
    check(ED not in r.log(), "the log holds key material")
    r.stop()


def test_good_keys():
    r = Relay("goodkeys", "echo").start()
    c = Client(r)
    c.keys(GOOD)
    answer = c.answer()
    # No pty driver on this machine, where ptys are always there.
    check(answer == {"v": 1, "hostKeys": [host_key_line(r)], "pty": True}, "the answer is %r" % answer)
    check(wait_for(lambda: r.servers(), 5), "no server was started")
    (pid, info), = r.servers().items()
    keys = info["keysDir"]
    check(os.path.dirname(keys) == r.auth, "the keys folder %s is not in %s" % (keys, r.auth))
    check(info["args"] == ["-i", "-E", "-s", "-e", "-j", "-k", "-I", "1800", "-r",
                           os.path.join(r.data, "hostkey_ed25519"), "-D", keys], "the server's arguments: %r" % info["args"])
    check(info["keys"] == "".join(line + "\n" for line in GOOD), "authorized_keys holds %r" % info["keys"])
    check(info["keysFiles"] == ["authorized_keys"], "the keys folder holds %r" % info["keysFiles"])
    check(info["keysDirMode"] == 0o700 and info["keysMode"] == 0o600,
          "modes %o and %o, not 700 and 600" % (info["keysDirMode"], info["keysMode"]))
    check(info["stdinSocket"] and info["sameInOut"], "the server's input and output are not one socket")
    check(info["session"] == pid and info["group"] == pid, "the server does not lead a session of its own")
    others = [fd for fd in info["fds"] if fd not in ("0", "1", "2")]
    check(len(others) <= 1, "the server has files of the relay's: %r" % info["fds"])
    env = dict(info["env"])
    env.pop("PWD", None)
    for k in ("PYTHONHASHSEED", "LC_CTYPE", "SHLVL", "_"):
        env.pop(k, None)
    check(env == {"PATH": os.path.join(r.app, "bin") + ":" + os.environ.get("PATH", "/usr/bin:/bin")},
          "the server's environment: %r" % env)
    mode = stat.S_IMODE(os.stat(os.path.join(r.data, "hostkey_ed25519")).st_mode)
    check(mode == 0o600, "the host key's mode is %o" % mode)
    r.wait_log("server started, pid %d" % pid)
    log = r.log()
    check("caller nbtapps, phone 'Test Phone', 2 key(s): ssh-ed25519 'laptop', ecdsa-sha2-nistp256 (no comment)" in log,
          "the log does not describe the connection:\n" + log)
    check("host key ssh-ed25519 SHA256:" in log, "the log does not give the host key's fingerprint:\n" + log)
    # Early bytes: what the phone sends right behind the keys message reaches the server after it.
    c.send(b"behind the keys")
    check(c.receive() == b"behind the keys", "the echo did not come back")
    c.close()
    check(wait_for(lambda: os.path.exists(os.path.join(r.state, "eof-%d" % pid)), 5),
          "the server did not see its input end")
    check(wait_for(lambda: not os.path.exists(keys), 5), "the keys folder is still there")
    r.stop()


def test_failed():
    # A key tool that cannot make a key: every connection is answered failed, and nothing is started.
    r = Relay("nokeytool", "echo", multi=None)
    with open(os.path.join(r.app, "bin", "dropbearmulti"), "w") as f:
        f.write("#!/bin/sh\necho 'cannot' >&2\nexit 1\n")
    os.chmod(os.path.join(r.app, "bin", "dropbearmulti"), 0o755)
    r.start()
    r.wait_log("connections are answered failed")
    c = Client(r)
    c.keys(GOOD)
    check(c.answer() == {"error": "failed"}, "a relay with no host key did not answer failed")
    check(c.closed(), "the connection stayed open")
    check(not r.servers() and r.keys_dirs() == [], "a server or a keys folder is there")
    r.stop()
    # A server that cannot be started.
    r = Relay("noserver")
    r.start({"NBT_SSHRELAY_TEST_SERVER": os.path.join(r.dir, "no-such-program")})
    c = Client(r)
    c.keys(GOOD)
    check(c.answer() == {"error": "failed"}, "a server that cannot start did not give failed")
    check(c.closed(), "the connection stayed open")
    check(r.keys_dirs() == [], "a keys folder is left: %s" % r.keys_dirs())
    r.wait_log("refused, failed: cannot start")
    r.stop()


def test_host_key_kept():
    first = Relay("hostkey1", "echo").start()
    key = os.path.join(first.data, "hostkey_ed25519")
    check(wait_for(lambda: "host key ssh-ed25519" in first.log(), 10), "no host key: " + first.log())
    with open(key, "rb") as f:
        made = f.read()
    stamp = os.stat(key).st_mtime_ns
    c = Client(first)
    c.keys(GOOD)
    answer = c.answer()
    c.close()
    first.stop()
    check("made the host key" in first.log(), "the first relay did not say it made the key")
    second = Relay("hostkey2", "echo", data=first.data).start()
    c = Client(second)
    c.keys(GOOD)
    check(c.answer() == answer, "a second start answers another host key")
    c.close()
    second.stop()
    with open(key, "rb") as f:
        check(f.read() == made and os.stat(key).st_mtime_ns == stamp, "the host key was made again")
    check("made the host key" not in second.log(), "the second relay made a host key")


# ---- the bridge ---------------------------------------------------------------------------------------------------

def rss_kib(pid):
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    return 0


class Sampler(threading.Thread):
    """The relay's resident memory, every 20 ms, while a test runs."""

    def __init__(self, pid):
        threading.Thread.__init__(self, daemon=True)
        self.pid, self.most, self.done = pid, 0, False

    def run(self):
        while not self.done:
            try:
                self.most = max(self.most, rss_kib(self.pid))
            except OSError:
                return
            time.sleep(0.02)


def through(relay, payload, sizes, reader_delay=0.0, max_message=4096):
    """Sends payload cut at `sizes` while reading back the echo; what came back."""
    c = Client(relay, max_message=max_message)
    c.keys(GOOD)
    check("hostKeys" in c.answer(), "no host keys in the answer")
    error = []

    def send():
        try:
            at, i = 0, 0
            while at < len(payload):
                n = sizes[i % len(sizes)]
                c.send(payload[at:at + n])
                at, i = at + n, i + 1
        except Exception as e:  # reported by the reader's check
            error.append(e)

    sender = threading.Thread(target=send, daemon=True)
    sender.start()
    back = bytearray()
    while len(back) < len(payload):
        message = c.receive(timeout=30)
        check(message is not None, "the relay closed the connection after %d bytes" % len(back))
        back += message
        if reader_delay:
            time.sleep(reader_delay)
    sender.join(10)
    check(not error, "sending failed: %r" % error)
    c.close()
    return bytes(back)


def test_echo_every_size():
    r = Relay("echo", "echo").start()
    payload = random.Random(15).randbytes(1 << 20)
    sizes = list(range(1, 4097))
    back = through(r, payload, sizes)
    check(back == payload, "1 MiB came back different")
    # A smaller message size from the agent's line: the relay's messages keep to it.
    back = through(r, payload[:200000], [517, 1, 300], max_message=517)
    check(back == payload[:200000], "200 KB in messages of up to 517 bytes came back different")
    r.stop()


def test_slow_server():
    r = Relay("slowserver", "slow").start()
    sampler = Sampler(r.proc.pid)
    sampler.start()
    time.sleep(0.1)
    before = rss_kib(r.proc.pid)
    payload = random.Random(16).randbytes(4 << 20)
    back = through(r, payload, [4096])
    sampler.done = True
    check(back == payload, "4 MiB through a server that reads slowly came back different")
    check(sampler.most - before < 1024, "the relay grew by %d KiB" % (sampler.most - before))
    r.stop()


def test_slow_client():
    size = 4 << 20
    r = Relay("slowclient", "source", [size]).start()
    sampler = Sampler(r.proc.pid)
    sampler.start()
    c = Client(r)
    c.keys(GOOD)
    check("hostKeys" in c.answer(), "no host keys in the answer")
    time.sleep(0.1)
    before = rss_kib(r.proc.pid)
    got = bytearray()
    while len(got) < size:
        message = c.receive(timeout=30)
        check(message is not None, "closed after %d bytes" % len(got))
        got += message
        if len(got) % (64 * 4096) < 4096:
            time.sleep(0.05)
    sampler.done = True
    check(bytes(got) == pattern(size), "4 MiB from the server, read slowly, came out different")
    check(sampler.most - before < 1024, "the relay grew by %d KiB" % (sampler.most - before))
    c.close()
    r.stop()


# ---- ends ---------------------------------------------------------------------------------------------------------

def test_client_goes_away():
    r = Relay("clientgone", "echo").start()
    c = Client(r)
    c.keys(GOOD)
    c.answer()
    check(wait_for(lambda: r.servers(), 5), "no server")
    (pid, info), = r.servers().items()
    c.close()
    check(wait_for(lambda: os.path.exists(os.path.join(r.state, "eof-%d" % pid)), 5), "the server's input did not end")
    check(wait_for(lambda: not alive(pid), 5), "the server is still there")
    check(wait_for(lambda: r.keys_dirs() == [], 5), "the keys folder is left")
    r.wait_log("ended: the agent's side closed")
    r.stop()


def test_server_exits_with_bytes():
    for size, delay in ((60000, 0.5), (1 << 20, 0.0)):
        r = Relay("exitbytes%d" % size, "burst-exit", [size]).start()
        c = Client(r)
        c.keys(GOOD)
        c.answer()
        time.sleep(delay)
        got = bytearray()
        while True:
            message = c.receive(timeout=15)
            if message is None:
                break
            got += message
            if size > 100000 and len(got) < size // 2:
                time.sleep(0.002)
        check(bytes(got) == pattern(size), "%d bytes from a server that exited: %d came" % (size, len(got)))
        r.wait_log("ended: the server")
        check(wait_for(lambda: r.keys_dirs() == [], 5), "the keys folder is left")
        r.stop()


def ended(pids, names, timeout):
    return wait_for(lambda: not any(alive(pids[n]) for n in names), timeout)


def kill_quietly(pid):
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass


def test_leftovers_client_closes():
    r = Relay("leftovers", "leftovers").start()
    c = Client(r)
    c.keys(GOOD)
    c.answer()
    check(wait_for(lambda: r.servers(), 5), "no server")
    (pid, info), = r.servers().items()
    left = r.left_behind(pid)
    try:
        check(all(alive(p) for p in left.values()), "what the stand-in left is not all there: %r" % left)
        c.close()
        check(ended(left, ["plain", "leader"], 1.0), "SIGHUP did not end the server's group and the login's leader")
        time.sleep(1.0)
        check(alive(left["inSession"]) and alive(left["leaderChild"]),
              "what ignores SIGHUP was ended before the SIGKILL was due")
        check(ended(left, ["inSession", "leaderChild"], 2.0), "what ignores SIGHUP is still there after the SIGKILL")
        check(alive(left["outsider"]), "a process outside the server's sessions was ended")
        r.wait_log("SIGKILL to 2 still there")
    finally:
        for p in left.values():
            kill_quietly(p)
    r.stop()


def test_leftovers_server_exits():
    r = Relay("leftoversexit", "leftovers-exit").start()
    c = Client(r)
    c.keys(GOOD)
    c.answer()
    check(wait_for(lambda: r.servers(), 5), "no server")
    (pid, info), = r.servers().items()
    left = r.left_behind(pid)
    try:
        # The server exits by itself after 1.5 s; one it left still holds its end of the socket pair.
        check(c.closed(timeout=5), "the connection was not closed after the server exited")
        r.wait_log("ended: the server exited")
        check(ended(left, ["plain", "leader"], 1.0), "SIGHUP did not end what heeds it")
        check(ended(left, ["inSession", "leaderChild"], 3.0), "what ignores SIGHUP is still there after the SIGKILL")
        check(alive(left["outsider"]), "a process outside the server's sessions was ended")
    finally:
        for p in left.values():
            kill_quietly(p)
    r.stop()


def test_sigterm():
    r = Relay("sigterm", "leftovers").start()
    clients, lefts = [], []
    try:
        for i in range(2):
            c = Client(r)
            c.keys(GOOD)
            c.answer()
            clients.append(c)
        check(wait_for(lambda: len(r.servers()) == 2, 5), "two servers were not started")
        for pid in r.servers():
            lefts.append(r.left_behind(pid))
        check(len(r.keys_dirs()) == 2, "two keys folders are not there: %s" % r.keys_dirs())
        started = time.monotonic()
        r.proc.send_signal(signal.SIGTERM)
        status = r.proc.wait(5)
        took = time.monotonic() - started
        check(status == 0, "the relay exited with %d" % status)
        check(took < 1.2, "the relay took %.2f s to exit" % took)
        for left in lefts:
            check(ended(left, ["plain", "leader", "inSession", "leaderChild"], 0.3),
                  "something of a server was left after the relay exited: %r" % left)
            check(alive(left["outsider"]), "a process outside the servers' sessions was ended")
        for pid in r.servers():
            check(not alive(pid), "a server is still there")
        check(not os.path.exists(r.socket), "the socket file is left")
        check(not os.path.exists(r.auth) or r.keys_dirs() == [], "keys folders are left: %s" % r.keys_dirs())
        for c in clients:
            check(c.closed(timeout=2), "a client's connection was not closed")
        r.wait_log("SIGTERM: ending 2 connection(s)")
        r.wait_log("exiting")
    finally:
        for left in lefts:
            for p in left.values():
                kill_quietly(p)


# ---- the pty driver -----------------------------------------------------------------------------------------------

def keyed(relay):
    """A connection with its keys message sent and its answer read: the client and the answer."""
    c = Client(relay)
    c.keys(GOOD)
    return c, c.answer()


def works(c, what):
    c.send(b"through " + what.encode())
    check(c.receive() == b"through " + what.encode(), "%s: the echo did not come back" % what)


def ended_count(relay, n):
    check(wait_for(lambda: relay.log().count("ended: ") >= n, 5), "%d connection(s) did not end:\n%s" % (n, relay.log()))


def new_driver(relay, besides=()):
    """The pid of the one stand-in driver that runs, other than `besides`; its marker may come just after the
    relay has found its process."""
    def others():
        return [p for p in relay.driver.pids() if p not in besides]
    check(wait_for(lambda: len(others()) == 1, 3), "not one driver: %r\n%s" % (relay.driver.pids(), relay.log()))
    return others()[0]


def test_driver_lifecycle():
    r = Relay("driver", "echo", driver="run", dev=True, driver_ms=1500)
    other = Driver(os.path.join(r.dir, "other"))
    o = other.run_directly()   # another driver of the same name, there before the relay
    try:
        r.start()
        c1, a = keyed(r)
        check(a == {"v": 1, "hostKeys": [host_key_line(r)], "pty": True}, "the first answer: %r" % a)
        d = new_driver(r)
        check(d != o and r.driver.args(d) == "-n 4", "the driver %d (the other is %d) has %r" % (d, o, r.driver.args(d)))
        r.wait_log("pty driver started, pid %d" % d)
        works(c1, "the first")
        c2, a = keyed(r)
        check(a.get("pty") is True, "the second answer: %r" % a)
        check(r.log().count("starting the pty driver") == 1 and r.driver.pids() == [d], "a second driver was started")
        c1.close()
        c2.close()
        ended_count(r, 2)
        time.sleep(0.8)
        check(alive(d), "the driver was ended 0.8 s after the last connection")
        # A connection within the time keeps it, and the time begins again at its end.
        c3, a = keyed(r)
        check(a.get("pty") is True and r.log().count("starting the pty driver") == 1, "the third: %r" % a)
        c3.close()
        ended_count(r, 3)
        since = time.monotonic()
        check(wait_for(lambda: not alive(d), 4), "the driver was not ended after the last connection")
        took = time.monotonic() - since
        check(took >= 1.2, "the driver was ended %.2f s after the last connection, not 1.5" % took)
        check(r.driver.events("TERM") == [d], "the driver did not get SIGTERM")
        r.wait_log("the pty driver, pid %d: SIGTERM, no connection for" % d)
        r.wait_log("the pty driver, pid %d, has ended" % d)
        # The next connection starts it again; the relay's end ends it.
        c4, a = keyed(r)
        check(a.get("pty") is True and r.log().count("starting the pty driver") == 2, "the fourth: %r" % a)
        d2 = new_driver(r, [d])
        check(d2 != d, "the same pid again")
        works(c4, "the fourth")
        r.stop()
        check(r.proc.returncode == 0, "the relay exited with %r" % r.proc.returncode)
        check(not alive(d2) and d2 in r.driver.events("TERM"), "the relay's end did not end its driver")
        check(alive(o) and not other.events("TERM") and not other.events("TERM-ignored"),
              "the other driver was signalled")
    finally:
        kill_quietly(o)


def test_driver_waits():
    # A driver that appears 0.5 s after its command: both connections wait for it, and one start is made.
    r = Relay("driverlate", "echo", driver="late", dev=True).start()
    started = time.monotonic()
    c1 = Client(r)
    c1.keys(GOOD)
    time.sleep(0.15)
    c2 = Client(r)
    c2.keys(GOOD)
    a1, a2 = c1.answer(), c2.answer()
    took = time.monotonic() - started
    check(a1.get("pty") is True and a2.get("pty") is True, "answers %r, %r" % (a1, a2))
    check(0.4 <= took <= 1.5, "answered after %.2f s, with a driver that takes 0.5 s" % took)
    check(r.log().count("starting the pty driver") == 1 and r.log().count("waits for the pty driver") == 2,
          "not one start for two waiting connections:\n" + r.log())
    works(c1, "the first")
    works(c2, "the second")
    d = new_driver(r)
    r.stop()
    check(not alive(d), "the driver is still there after the relay's end")


def test_driver_never_appears():
    r = Relay("drivernone", "echo", driver="none", dev=True).start()
    started = time.monotonic()
    c1, a = keyed(r)
    took = time.monotonic() - started
    check("hostKeys" in a and a.get("pty") is False, "the answer: %r" % a)
    check(1.9 <= took <= 3.0, "answered after %.2f s, not after the 2 s look" % took)
    r.wait_log("no pty driver: none appeared within 2 s")
    works(c1, "without a pty")
    # Not tried again within 30 s.
    started = time.monotonic()
    c2, a = keyed(r)
    check(a.get("pty") is False and time.monotonic() - started < 1.0, "the second waited, or: %r" % a)
    check(r.log().count("starting the pty driver") == 1, "a second start within 30 s")
    r.wait_log("no pty driver started: the last start")
    r.stop()
    # A driver that is not there: nothing waits.
    r = Relay("drivermissing", "echo", driver="run", dev=True)
    os.remove(r.driver.path)
    r.start()
    started = time.monotonic()
    c, a = keyed(r)
    check(a.get("pty") is False and time.monotonic() - started < 1.0, "with no driver: %r" % a)
    r.wait_log("no pty driver started: %s: No such file" % r.driver.path)
    works(c, "with no driver")
    r.stop()


def test_driver_dies():
    r = Relay("driverdies", "echo", driver="die", dev=True).start()
    c1, a = keyed(r)
    check(a.get("pty") is True, "the answer: %r" % a)
    d = new_driver(r)
    check(wait_for(lambda: not alive(d), 3), "the stand-in did not end by itself")
    c2, a = keyed(r)
    check(a.get("pty") is True, "the answer after the driver went: %r" % a)
    r.wait_log("the pty driver, pid %d, has gone" % d)
    check(r.log().count("starting the pty driver") == 2, "the driver was not started again:\n" + r.log())
    check(d not in r.driver.events("TERM"), "the driver that had gone was signalled")
    r.stop()


def test_driver_stubborn():
    # A driver that ignores SIGTERM gets SIGKILL 2 s later, or 0.5 s later at the relay's end.
    r = Relay("driverstubborn", "echo", driver="stubborn", dev=True, driver_ms=500).start()
    c, a = keyed(r)
    d = new_driver(r)
    c.close()
    r.wait_log("the pty driver, pid %d: SIGTERM" % d)
    since = time.monotonic()
    check(wait_for(lambda: not alive(d), 4), "the driver is still there")
    took = time.monotonic() - since
    check(1.7 <= took <= 2.7, "SIGKILL %.2f s after the SIGTERM, not 2" % took)
    r.wait_log("the pty driver, pid %d, still there: SIGKILL" % d)
    check(d in r.driver.events("TERM-ignored"), "the stand-in did not see the SIGTERM")
    c, a = keyed(r)
    d2 = new_driver(r, [d])
    since = time.monotonic()
    r.proc.send_signal(signal.SIGTERM)
    r.proc.wait(5)
    check(time.monotonic() - since < 1.6 and not alive(d2), "the relay's end did not end a driver that ignores SIGTERM")
    r.wait_log("the pty driver, pid %d, still there: SIGKILL" % d2)


def test_no_pty():
    r = Relay("nopty", "echo", dev=True).start()
    c, a = keyed(r)
    check("hostKeys" in a and a.get("pty") is False, "with no pty: %r" % a)
    works(c, "with no pty")
    r.wait_log("pty no")
    # The names of another driver's ptys.
    open(os.path.join(r.dev, "ptyp3"), "w").close()
    c2, a = keyed(r)
    check(a.get("pty") is True, "with another driver's ptys: %r" % a)
    r.stop()


# ---- the limit ------------------------------------------------------------------------------------------------------

def test_too_many():
    r = Relay("toomany", "echo").start()
    clients = []
    for i in range(4):
        c, a = keyed(r)
        check("hostKeys" in a, "connection %d: %r" % (i + 1, a))
        clients.append(c)
    check(wait_for(lambda: len(r.servers()) == 4, 5), "four servers were not started")
    c5 = Client(r)
    c5.keys(GOOD)
    check(c5.answer() == {"error": "too-many"}, "the fifth was not answered too-many")
    check(c5.closed(), "the fifth stayed open")
    time.sleep(0.3)
    check(len(r.servers()) == 4 and len(r.keys_dirs()) == 4, "something was made for the fifth")
    r.wait_log("refused, too-many")
    clients[0].close()
    ended_count(r, 2)
    c6, a = keyed(r)
    check("hostKeys" in a, "a connection after one ended: %r" % a)
    works(c6, "the sixth")
    r.stop()


# ---- the state file -------------------------------------------------------------------------------------------------

def stranger():
    """An unrelated process that leads a session of its own, with this run's folder in its command line."""
    proc = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(120)", work_root],
                            start_new_session=True)
    check(wait_for(lambda: alive(proc.pid), 5), "a stranger did not start")
    return proc


def test_tidy_after_kill():
    r = Relay("tidy", "leftovers", driver="run").start()
    clients, lefts, extra = [], [], []
    try:
        for i in range(2):
            c, a = keyed(r)
            check(a.get("pty") is True, "connection %d: %r" % (i + 1, a))
            clients.append(c)
        check(wait_for(lambda: len(r.servers()) == 2, 5), "two servers were not started")
        servers = sorted(r.servers())
        for pid in servers:
            lefts.append(r.left_behind(pid))
        d = new_driver(r)
        extra += servers + [d]
        # The first server stays when its input ends, as one whose relay died might.
        open(os.path.join(r.state, "stay-%d" % servers[0]), "w").close()

        def recorded():
            try:
                s = r.state_file()
            except (OSError, ValueError):
                return False
            # Start times of QNX's size (START_OFFSET), as strings of digits.
            return (s["relay"] == rec(r.proc.pid, start_time(r.proc.pid)) and s.get("driver") == rec(d, start_time(d)) and
                    all(x["start"] == str(start_time(x["pid"])) for x in s["servers"]) and
                    sorted(x["pid"] for x in s["servers"]) == servers and
                    all(left["leader"] in [l["pid"] for l in x["logins"]]
                        for x, left in zip(sorted(s["servers"], key=lambda x: x["pid"]), lefts)))
        check(wait_for(recorded, 5), "the state file does not hold the servers, their logins and the driver: %r" % (
            open(r.state_path).read() if os.path.exists(r.state_path) else None))
        r.proc.kill()
        r.proc.wait()
        check(wait_for(lambda: not alive(servers[1]), 5), "the second server did not end with its input")
        check(alive(servers[0]) and alive(d) and all(alive(p) for left in lefts for p in left.values()),
              "what the relay left is not all there")
        check(len(r.keys_dirs()) == 2, "the keys folders are not there: %r" % r.keys_dirs())

        r.start()
        # The start removes them after it listens, which is what start() waits for.
        check(wait_for(lambda: r.keys_dirs() == [], 5), "keys folders are left: %r" % r.keys_dirs())
        r.wait_log("left by the relay before: 2 server(s) recorded, 1 still running; SIGHUP to")
        r.wait_log("its pty driver, pid %d: SIGTERM" % d)
        check(wait_for(lambda: not alive(servers[0]) and not alive(d), 1.0), "the server or the driver was not ended")
        check(d in r.driver.events("TERM"), "the driver did not get SIGTERM")
        for left in lefts:
            check(ended(left, ["plain", "leader"], 1.0), "SIGHUP did not reach what was left: %r" % left)
            check(alive(left["inSession"]) and alive(left["leaderChild"]), "what ignores SIGHUP was ended at once")
        for left in lefts:
            check(ended(left, ["inSession", "leaderChild"], 3.0), "what ignores SIGHUP is still there: %r" % left)
            check(alive(left["outsider"]), "a process outside the recorded sessions was ended")
        r.wait_log("left by the relay before: SIGKILL to")
        # Then a connection as any other.
        c, a = keyed(r)
        check(a.get("pty") is True and "hostKeys" in a, "a connection after the tidying: %r" % a)
        check(wait_for(lambda: len(r.servers()) == 3, 5), "no server for the new connection")
        lefts.append(r.left_behind(max(set(r.servers()) - set(servers))))
        works(c, "after the tidying")
        c.close()
        r.stop()
        check(not os.path.exists(r.state_path), "the state file is left after a clean exit")
    finally:
        for left in lefts:
            for p in left.values():
                kill_quietly(p)
        for p in extra:
            kill_quietly(p)


def write_state(relay, value):
    with open(relay.state_path, "w") as f:
        f.write(value if isinstance(value, str) else json.dumps(value))


def test_state_strangers():
    r = Relay("strangers", "echo", driver="run")
    other = Driver(os.path.join(r.dir, "other"))
    o = other.run_directly()
    procs = [stranger() for i in range(4)]
    p = [x.pid for x in procs]
    try:
        # Each pid now someone else's: a wrong start time, or the right one with another program, or (p[3]) a
        # leader with the right start time named as a server, which runs another program.
        write_state(r, {"v": 1, "relay": rec(p[0], start_time(p[0])),
                        "driver": rec(p[1], start_time(p[1])),
                        "servers": [dict(rec(o, start_time(o)), logins=[rec(p[2], start_time(p[2]) + 1)]),
                                    dict(rec(p[3], start_time(p[3]) + 7), logins=[])]})
        r.start()
        r.wait_log("left by the relay before: 2 server(s) recorded, 0 still running; SIGHUP to 0 process group(s) "
                   "of 0 session(s)")
        r.wait_log("its pty driver, pid %d, is not there" % p[1])
        time.sleep(2.5)
        check(all(alive(x) for x in p) and alive(o), "a stranger was ended")
        check(not other.events("TERM"), "the other driver was signalled")
        check(r.state_file()["relay"]["pid"] == r.proc.pid, "the state file was not replaced")
        c, a = keyed(r)
        works(c, "with strangers")
        r.stop()
    finally:
        kill_quietly(o)
        for x in procs:
            x.kill()
            x.wait()


def test_state_nonsense():
    victim = stranger()
    try:
        good = {"v": 1, "relay": rec(99999999, 5), "servers": [
            dict(rec(99999998, 5), logins=[rec(victim.pid, start_time(victim.pid))])]}

        def driver_start(text):
            return json.dumps(dict(good, driver={"pid": 4242, "start": text}))
        cases = [
            ("not JSON", "pid 123\n"),
            ("half a file", json.dumps(good)[:40]),
            ("no servers", json.dumps({"v": 1, "relay": rec(99999999, 5)})),
            ("v 2", json.dumps(dict(good, v=2))),
            ("a relay of pid 1", json.dumps(dict(good, relay=rec(1, 5)))),
            ("a key too many", json.dumps(dict(good, more=1))),
            ("a start as a number", driver_start(5)),
            ("a negative start", driver_start("-1")),
            ("a start with a sign", driver_start("+5")),
            ("a start with a leading zero", driver_start("05")),
            ("an empty start", driver_start("")),
            ("a start with a space", driver_start("5 ")),
            ("a start above 2^64 - 1", driver_start("18446744073709551616")),
            ("a start of 21 digits", driver_start("100000000000000000000")),
        ]
        for i, (what, text) in enumerate(cases):
            r = Relay("nonsense%d" % i, "echo")
            write_state(r, text)
            r.start()
            r.wait_log("cannot be read: ignored")
            time.sleep(0.2)
            check(alive(victim.pid), "%s: a process it names was signalled" % what)
            check(r.state_file()["relay"]["pid"] == r.proc.pid, "%s: the state file was not replaced" % what)
            c, a = keyed(r)
            check("hostKeys" in a, "%s: %r" % (what, a))
            r.stop()
        # Start times of every size up to 2^64 - 1, and 0, are read; none of these pids is there.
        for i, text in enumerate(["0", "315500000000000000", "18446744073709551615"]):
            r = Relay("startsize%d" % i, "echo")
            write_state(r, {"v": 1, "relay": rec(99999999, text), "driver": {"pid": 4242, "start": text}, "servers": []})
            r.start()
            r.wait_log("its pty driver, pid 4242, is not there")
            check("cannot be read" not in r.log() and alive(victim.pid), "a start of %s: %s" % (text, r.log()))
            r.stop()
        # The same file whole: that session leader, with its start time, is ended.
        r = Relay("nonsense-control", "echo")
        write_state(r, good)
        r.start()
        check(wait_for(lambda: not alive(victim.pid), 2), "a recorded session leader was not ended")
        r.stop()
    finally:
        victim.kill()
        victim.wait()


def test_older_relay():
    r = Relay("older", "echo").start()
    first = r.proc
    c, a = keyed(r)
    r.start()
    check(first.wait(3) == 0, "the older relay did not end with 0")
    r.wait_log("an older relay, pid %d, still runs: SIGTERM" % first.pid)
    r.wait_log("the older relay has ended")
    check(c.closed(timeout=3), "the older relay's connection stayed open")
    c, a = keyed(r)
    works(c, "after an older relay")
    c.close()
    # One that does not end at SIGTERM (stopped): SIGKILL after 2 s, and the new one listens within 2.5 s.
    second = r.proc
    os.kill(second.pid, signal.SIGSTOP)
    r.start()
    check(second.wait(3) == -signal.SIGKILL, "the stopped relay did not get SIGKILL")
    check(1.9 <= r.start_took <= 2.6, "the start took %.2f s; about 2.0 to 2.3 s expected" % r.start_took)
    r.wait_log("the older relay is still there after 2 s: SIGKILL")
    c, a = keyed(r)
    works(c, "after a stopped relay")
    r.stop()


# ---- with Dropbear and OpenSSH -----------------------------------------------------------------------------------

def ssh_keygen(path):
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "relay-test", "-f", path], check=True)
    with open(path + ".pub") as f:
        return f.read().strip()


def ssh(relay, key, offered, known_hosts, command, tty=False):
    proxy = "%s -I %s %s %s" % (shlex.quote(sys.executable), shlex.quote(PROXY), shlex.quote(relay.socket),
                                shlex.quote(offered))
    args = ["ssh"] + (["-tt"] if tty else []) + [
            "-F", "/dev/null", "-o", "ProxyCommand=" + proxy, "-o", "UserKnownHostsFile=" + known_hosts,
            "-o", "GlobalKnownHostsFile=/dev/null", "-o", "StrictHostKeyChecking=yes", "-o", "HostKeyAlias=headunit",
            "-o", "IdentitiesOnly=yes", "-o", "IdentityAgent=none", "-o", "BatchMode=yes",
            "-o", "PasswordAuthentication=no", "-o", "KbdInteractiveAuthentication=no", "-o", "ConnectTimeout=20",
            "-o", "LogLevel=ERROR", "-i", key, "-l", os.environ.get("USER") or __import__("getpass").getuser(),
            "headunit", command]
    env = dict(os.environ, SHELL="/bin/sh")
    if not tty:
        return subprocess.run(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True, timeout=60, env=env)
    # With a pty the client's input is held open until the command has ended: Dropbear closes a session's pty when
    # the client's input ends, which with no input at all is before the command has run.
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            universal_newlines=True, env=env)
    timer = threading.Timer(60, proc.kill)
    timer.start()
    try:
        out = proc.stdout.read()
        proc.stdin.close()
        err = proc.stderr.read()
        proc.wait()
    finally:
        timer.cancel()
    return subprocess.CompletedProcess(args, proc.returncode, out, err)


def answer_of(relay, public):
    c = Client(relay)
    c.keys([public])
    answer = c.answer()
    c.close()
    return answer


def test_openssh():
    r = Relay("openssh").start()
    keys = os.path.join(r.dir, "keys")
    os.makedirs(keys)
    key, other = os.path.join(keys, "id"), os.path.join(keys, "other")
    public, other_public = ssh_keygen(key), ssh_keygen(other)
    offered = os.path.join(keys, "offered")
    with open(offered, "w") as f:
        f.write(public + "\n")
    answer = answer_of(r, public)
    check("hostKeys" in answer and len(answer["hostKeys"]) == 1 and answer.get("pty") is True,
          "the answer is %r" % answer)
    check(answer["hostKeys"][0] == host_key_line(r), "the answer's host key is not the key file's")
    known = os.path.join(keys, "known_hosts")
    with open(known, "w") as f:
        f.write("headunit %s\n" % answer["hostKeys"][0])

    done = ssh(r, key, offered, known, "echo relay-ok; exit 3")
    check(done.returncode == 3 and done.stdout.strip() == "relay-ok",
          "the login: exit %d, output %r, errors %r\n%s" % (done.returncode, done.stdout, done.stderr, r.log()))
    check("Pubkey auth succeeded" in r.log(), "the server's own log lines are not in the relay's log:\n" + r.log())

    # A login with a pty.
    done = ssh(r, key, offered, known, "tty; exit 4", tty=True)
    check(done.returncode == 4 and "/dev/" in done.stdout and "not a tty" not in done.stdout,
          "a login with a pty: exit %d, output %r, errors %r" % (done.returncode, done.stdout, done.stderr))

    # The client's key is not among those the connection was given.
    done = ssh(r, other, offered, known, "echo should-not-run")
    check(done.returncode == 255 and "should-not-run" not in done.stdout and "Permission denied" in done.stderr,
          "a wrong key: exit %d, output %r, errors %r" % (done.returncode, done.stdout, done.stderr))

    # A known host key other than the server's: the client refuses before it logs in.
    wrong = os.path.join(keys, "known_wrong")
    with open(wrong, "w") as f:
        f.write("headunit %s\n" % " ".join(other_public.split()[:2]))
    done = ssh(r, key, offered, wrong, "echo should-not-run")
    check(done.returncode == 255 and "should-not-run" not in done.stdout and "verification failed" in done.stderr,
          "a wrong host key: exit %d, output %r, errors %r" % (done.returncode, done.stdout, done.stderr))

    # Every connection's keys folder is gone once its server has ended.
    check(wait_for(lambda: r.keys_dirs() == [], 5), "keys folders are left: %s" % r.keys_dirs())
    r.stop()


# ---- running --------------------------------------------------------------------------------------------------------

def run(name, function, skip=None):
    if skip:
        results["skipped"].append(name)
        print("SKIP  %s: %s" % (name, skip))
        return
    started = time.monotonic()
    first = len(relays)
    try:
        function()
    except Failure as e:
        results["failed"].append(name)
        print("FAIL  %s: %s" % (name, e))
        return
    except Exception as e:
        results["failed"].append(name)
        print("FAIL  %s: %s: %s" % (name, type(e).__name__, e))
        return
    finally:
        # A relay a failed test did not get to stop: ended as the agent ends one, so that it ends what it started.
        for proc in relays[first:]:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
    results["passed"].append(name)
    print("ok    %s (%.1f s)" % (name, time.monotonic() - started))


def run_program(name):
    done = subprocess.run([os.path.join(TESTS, name)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          universal_newlines=True, timeout=60)
    check(done.returncode == 0, done.stdout.strip())


def sweep():
    """Every process of this run that is still there: ended, and named."""
    left = []
    for proc in relays:
        if proc.poll() is None:
            left.append("relay %d" % proc.pid)
            proc.kill()
            proc.wait()
    found = subprocess.run(["pgrep", "-f", work_root], stdout=subprocess.PIPE, universal_newlines=True).stdout.split()
    for pid in found:
        if int(pid) != os.getpid():
            left.append("pid %s" % pid)
            kill_quietly(int(pid))
    return left


def main():
    global work_root
    sys.stdout.reconfigure(line_buffering=True)
    missing = [p for p in (RELAY, MULTI, SFTP, STANDIN_PTY, os.path.join(TESTS, "test_key_lines"),
                           os.path.join(TESTS, "test_processes")) if not os.path.isfile(p)]
    if missing:
        print("FAIL  missing: %s (make ssh-host builds Dropbear; make ssh-test the rest)" % ", ".join(missing))
        return 1
    # A short folder: the socket's path must fit a local socket's address.
    work_root = tempfile.mkdtemp(prefix="sshrelay-test-", dir="/tmp" if len(tempfile.gettempdir()) > 40 else None)
    no_ssh = None if shutil.which("ssh") and shutil.which("ssh-keygen") else "no OpenSSH client (ssh, ssh-keygen) here"
    try:
        run("key-line rule (test_key_lines)", lambda: run_program("test_key_lines"))
        run("process list (test_processes)", lambda: run_program("test_processes"))
        run("no keys message in time", test_no_keys_in_time)
        run("an agent's line that cannot be read", test_agent_line_unreadable)
        run("bad keys messages", test_bad_keys)
        run("a good keys message", test_good_keys)
        run("failed: no key tool, no server", test_failed)
        run("the host key made once and kept", test_host_key_kept)
        run("1 MiB echoed in messages of every size", test_echo_every_size)
        run("a server that reads slowly", test_slow_server)
        run("a client that reads slowly", test_slow_client)
        run("the client goes away", test_client_goes_away)
        run("the server exits with bytes still to come", test_server_exits_with_bytes)
        run("leftovers, the client closes", test_leftovers_client_closes)
        run("leftovers, the server exits", test_leftovers_server_exits)
        run("SIGTERM with two connections", test_sigterm)
        run("the pty driver: started, kept, ended, started again", test_driver_lifecycle)
        run("the pty driver: connections wait for it", test_driver_waits)
        run("the pty driver: none appears, or none is there", test_driver_never_appears)
        run("the pty driver: one that goes by itself", test_driver_dies)
        run("the pty driver: one that ignores SIGTERM", test_driver_stubborn)
        run("pty false with no ptys", test_no_pty)
        run("four connections at once, a fifth too-many", test_too_many)
        run("a relay killed: the next one tidies", test_tidy_after_kill)
        run("a state file naming strangers", test_state_strangers)
        run("a state file of nonsense", test_state_nonsense)
        run("an older relay still running", test_older_relay)
        run("OpenSSH through the relay to Dropbear", test_openssh, skip=no_ssh)
    finally:
        left = sweep()
        if left:
            results["failed"].append("processes left")
            print("FAIL  processes were left and have been ended: %s" % ", ".join(left))
        # The folder this run made, by the name mkdtemp gave it.
        shutil.rmtree(work_root, ignore_errors=True)
    line = "ssh-test: %d passed, %d failed, %d skipped" % (
        len(results["passed"]), len(results["failed"]), len(results["skipped"]))
    if results["skipped"]:
        line += " (skipped: %s)" % "; ".join(results["skipped"])
    print(line)
    return 1 if results["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
