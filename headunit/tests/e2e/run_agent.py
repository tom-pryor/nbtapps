#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""`make agent-e2e`: nbtagent and nbtctl end to end, as two processes, with no car.

  python3 tests/e2e/run_agent.py [BIN] [TESTDATA]      (default build/agent-host and build/testdata)

The agent runs on a folder that stands in for a head unit and trusts the test key. It runs in a Docker container
(tools/bench.py, which needs `make bench-image` first), because it runs the packages' hook scripts: the container
is handed the head unit's folder, the socket's folder and the key, and nothing else of this machine. nbtctl is BIN's,
runs here, and reaches the agent through the socket. It plays the phone: it offers
the packages of a folder (the "stick") and serves them when the agent asks. The script drives a plan, an install
with a dependency, an upgrade, an upgrade that fails and is undone, a fetch that the controller cancels, a fetch
whose link drops and is resumed, removals, and a restart of the agent, and after each looks at what the agent says
and at what is on disk. It also is a phone on the agent's debug TCP link, which the phone app on an emulator uses:
refused until it is paired, paired by the code `nbtctl pair` prints, let in by `auth`, and forgotten again, and
told with link.closing why its link closes when it is forgotten and when the agent stops.

One package is made here: 2.5 MiB that do not compress, so that its archive takes three ranges of the 1 MiB the
agent asks for at a time and many frames. The others come from tests/make_packages.py.
"""
import hashlib, hmac, json, os, re, shutil, signal, socket, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import test_protocol_vectors as wire    # a frame codec that is not the agent's
BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "agent-host"))
DATA = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "testdata"))
WORK = os.path.join(BIN, "e2e")
HU = os.path.join(WORK, "hu")
# The socket goes into a folder with a short name: a local socket's path has a limit of about 100 bytes, which a
# checkout further down the tree would pass.
# The agent's container is handed this folder, so it holds nothing but the socket.
SOCKET_DIR = tempfile.mkdtemp(prefix="nbt_e2e_")
SOCKET = os.path.join(SOCKET_DIR, "agent.sock")
STICK = os.path.join(WORK, "stick")
STICK2 = os.path.join(WORK, "stick2")
BULK_BYTES = 2500000
BENCH = [sys.executable, os.path.join(ROOT, "tools", "bench.py"), "agent"]
IMAGE = os.environ.get("NBT_BENCH_IMAGE", "nbtapp-bench")    # as tools/bench.py names it
# What the agent says it is: its version as agent/version.hpp has it, from which the image's nbtagent is built.
with open(os.path.join(ROOT, "agent", "version.hpp")) as _f:
    VERSION = re.search(r'^const char kAgentVersion\[\] = "(.*)";$', _f.read(), re.M).group(1)


class Failed(Exception):
    pass


def check(condition, what, detail=""):
    if not condition:
        raise Failed(what + ("\n" + detail if detail else ""))


class Agent:
    def __init__(self):
        self.log = open(os.path.join(WORK, "agent.log"), "ab")
        self.log_start = self.log.tell()
        self.process = subprocess.Popen(
            BENCH + ["--root", HU, "--socket", SOCKET, "--debug-tcp", "0",
                     "--test-key", os.path.join(ROOT, "tests", "keys", "test-package.pub")],
            stdout=self.log, stderr=self.log)
        for _ in range(400):    # a container takes a moment longer to start than a program
            if os.path.exists(SOCKET):
                return
            if self.process.poll() is not None:
                break
            time.sleep(0.05)
        raise Failed("nbtagent did not start listening")

    def tcp_port(self):
        """The port of the debug TCP link, which this run of the agent wrote to its log. The address it names
        there is the one inside its container; on this machine the port is on 127.0.0.1."""
        for _ in range(100):
            with open(os.path.join(WORK, "agent.log"), "rb") as f:
                f.seek(self.log_start)
                found = re.search(rb"DEBUG TCP link at [0-9.]+:(\d+)", f.read())
            if found:
                return int(found.group(1))
            time.sleep(0.05)
        raise Failed("nbtagent did not say where its debug TCP link is")

    def stop(self):
        self.process.send_signal(signal.SIGTERM)
        code = self.process.wait(timeout=20)
        self.log.close()
        return code


def ctl(*args):
    """Runs nbtctl; returns (exit status, what it printed)."""
    done = subprocess.run([os.path.join(BIN, "nbtctl"), "--socket", SOCKET] + list(args),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    return done.returncode, done.stdout.decode("utf-8", "replace")


def ok(*args):
    code, out = ctl(*args)
    check(code == 0, "nbtctl %s: exit status %d" % (" ".join(args), code), out)
    return out


HELLO = {"protocolMin": 1, "protocolMax": 1, "role": "phone", "phoneId": "5f2c9a1e07b34d6688a0c1d2e3f40516", "name": "e2e phone",
         "maxFrame": 65536, "capabilities": []}


class TcpPhone:
    """A phone on the agent's debug TCP link: the frames and the proofs of `auth` are worked out here, with none
    of the agent's code."""
    ID = "5f2c9a1e07b34d6688a0c1d2e3f40516"

    def __init__(self, port):
        self.link = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.link.sendall(wire.MAGIC)
        self.data = b""
        self.next_id = 1

    def call(self, method, params):
        """Sends a request; returns (its answer, the request's payload, the answer's payload)."""
        number = self.next_id
        self.next_id += 1
        request = json.dumps({"id": number, "method": method, "params": params}, separators=(",", ":")).encode()
        self.link.sendall(wire.encode(wire.MSG, 0, 0, request))
        while True:
            got = wire.decode(self.data)
            if got[0] == "more":
                chunk = self.link.recv(65536)
                check(chunk, "the agent closed the TCP link before it answered %s" % method)
                self.data += chunk
                continue
            check(got[0] == "frame" and got[1] == wire.MSG, "the agent sent a bad frame over TCP: %s" % (got[1:],))
            self.data = self.data[got[5]:]
            reply = json.loads(got[4].decode("utf-8"))
            if reply.get("id") == number and "method" not in reply:
                return reply, request, got[4]

    def hello(self):
        return self.call("hello", dict(HELLO))

    def error(self, method, params):
        """The error code a request is answered with, or "" for a result."""
        return self.call(method, params)[0].get("error", {}).get("code", "")

    def ended(self):
        """True if the agent has closed the link."""
        try:
            return self.link.recv(65536) == b""
        except socket.timeout:
            return False

    def last_words(self):
        """Every message the agent still sends on the link, until it closes it."""
        said = []
        while True:
            got = wire.decode(self.data)
            if got[0] == "more":
                try:
                    chunk = self.link.recv(65536)
                except socket.timeout:
                    raise Failed("the agent did not close the TCP link")
                if not chunk:
                    check(not self.data, "the agent closed the TCP link in the middle of a frame")
                    return said
                self.data += chunk
                continue
            check(got[0] == "frame" and got[1] == wire.MSG, "the agent sent a bad frame over TCP: %s" % (got[1:],))
            self.data = self.data[got[5]:]
            said.append(json.loads(got[4].decode("utf-8")))

    def close(self):
        self.link.close()


def start_pairing():
    """`nbtctl pair`, left running: (the process, the code it printed first)."""
    pairing = subprocess.Popen([os.path.join(BIN, "nbtctl"), "--socket", SOCKET, "pair"],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    first = pairing.stdout.readline().decode()
    found = re.fullmatch(r"code (\d{6})\n", first)
    if not found:
        pairing.kill()
    check(found, "nbtctl pair did not print a code first", first)
    return pairing, found.group(1)


def pair_over_tcp(port, head_unit_id):
    """A phone on the debug TCP link: refused until it is paired, paired by the code `nbtctl pair` prints, let in
    by `auth`, and put out again when the head unit forgets it."""
    phone = TcpPhone(port)
    hello, hello_sent, hello_reply = phone.hello()
    check(hello.get("result", {}).get("headUnitId") == head_unit_id, "the head-unit id over TCP", repr(hello))
    check(phone.error("info.get", {}) == "unauthenticated", "info.get before auth was not refused")
    check(phone.error("pairing.issue", {"code": "000000"}) == "forbidden", "a key was to be had with no window open")
    check(phone.error("auth", {"proof": "00" * 32}) == "bad-state", "a proof was taken before a nonce")
    check(ok("phones") == "", "a new head unit has paired phones")

    # The pairing window: nbtctl stands in for the car's screen, and says the code first.
    pairing, code = start_pairing()
    try:
        wrong = code[:5] + ("0" if code[5] != "0" else "1")
        check(phone.error("pairing.issue", {"code": wrong}) == "rejected", "a wrong code was not rejected")
        issued = phone.call("pairing.issue", {"code": code})[0]
        key = bytes.fromhex(issued.get("result", {}).get("key", ""))
        check(len(key) == 32, "pairing.issue gave no key", repr(issued))
        out = pairing.communicate(timeout=20)[0].decode()
    finally:
        if pairing.poll() is None:
            pairing.kill()
    check(pairing.returncode == 0 and out.endswith("paired\n"), "nbtctl pair: exit status %d" % pairing.returncode, out)
    listed = ok("phones").split("\n")
    check(len(listed) == 2 and listed[0].startswith(TcpPhone.ID + " 20") and listed[0].endswith("Z e2e phone"),
          "nbtctl phones", repr(listed))
    with open(os.path.join(WORK, "agent.log"), "rb") as f:
        log = f.read().decode("utf-8", "replace")
    check(key.hex() not in log and code not in log, "the pairing code or the pairing key is in the agent's log")

    # auth: the agent proves that it holds the key, and then the phone does.
    check(phone.error("info.get", {}) == "unauthenticated", "a key alone opened the link")
    mine = os.urandom(16)
    first = phone.call("auth", {"nonce": mine.hex()})[0].get("result", {})
    transcript = (mine + bytes.fromhex(first.get("nonce", "")) + hashlib.sha256(hello_sent).digest() +
                  hashlib.sha256(hello_reply).digest())
    check(first.get("proof") == hmac.new(key, b"nbt-auth-1" + transcript, hashlib.sha256).hexdigest(),
          "the agent's proof over TCP is not the one the key gives", repr(first))
    done = phone.call("auth", {"proof": hmac.new(key, b"nbt-auth-2" + transcript, hashlib.sha256).hexdigest()})[0]
    check(done.get("result") == {}, "auth over TCP", repr(done))
    info = phone.call("info.get", {})[0]
    check(info.get("result", {}).get("headUnitId") == head_unit_id, "info.get after auth", repr(info))

    # A wrong proof ends the link, and costs the paired phone nothing.
    other = TcpPhone(port)
    other.hello()
    check("nonce" in other.call("auth", {"nonce": mine.hex()})[0].get("result", {}), "auth's first call on a second link")
    check(other.error("auth", {"proof": "00" * 32}) == "auth-failed" and other.ended(), "a wrong proof was let in")
    other.close()
    check(phone.error("ping", {}) == "", "the paired phone's link went with another link's wrong proof")

    # Forgotten: the link ends, and the key opens nothing any more.
    status, out = ctl("forget", "0" * 32)
    check(status == 1 and "not-found" in out, "nbtctl forget of a phone that is not paired", out)
    check(ok("forget", "--all") == "1 phone forgotten\n", "nbtctl forget --all")
    # The phone is told that its link closes, and why, and that is the last thing on the link.
    said = phone.last_words()
    check(said == [{"event": "link.closing", "params": {"reason": "forgotten"}}],
          "what a forgotten phone was told before its link closed", repr(said))
    phone.close()
    check(ok("phones") == "", "a phone is still paired after forget --all")
    again = TcpPhone(port)
    again.hello()
    check(again.error("auth", {"nonce": mine.hex()}) == "auth-failed", "a forgotten phone was let begin auth")
    again.close()

    # A window nobody uses goes with the controller that showed its code.
    pairing, code = start_pairing()
    pairing.terminate()
    pairing.wait(timeout=20)
    late = TcpPhone(port)
    late.hello()
    # The controller's link had ended before this phone's began, so the agent has seen that end by now.
    check(late.error("pairing.issue", {"code": code}) == "forbidden", "a pairing window outlived the controller that opened it")
    late.close()
    check(ok("phones") == "", "a phone was paired by a code nobody showed any more")


def installed():
    return ok("list").split("\n")[:-1]


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for piece in iter(lambda: f.read(1 << 20), b""):
            h.update(piece)
    return h.hexdigest()


def make_bulk():
    """A package of 2.5 MiB of bytes that do not compress; returns the payload's SHA-256."""
    source = os.path.join(WORK, "bulk-1.0.0")
    os.makedirs(os.path.join(source, "payload"))
    with open(os.path.join(source, "manifest.json"), "w") as f:
        f.write('{ "manifestVersion": 1, "id": "org.nbtapp.bulk", "name": "Bulk", "version": "1.0.0",\n'
                '  "install": { "files": [{ "src": "payload/bulk.bin", "dest": "${app}/bulk.bin", "mode": "0644" }] } }\n')
    with open(os.path.join(source, "payload", "bulk.bin"), "wb") as f:
        block, written = b"", 0
        while written < BULK_BYTES:
            block = hashlib.sha256(block + b"nbtapp bulk").digest()
            f.write(block[:min(32, BULK_BYTES - written)])
            written += 32
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "nbtpkg.py"), "build", source, "-o", STICK,
                           "--key", os.path.join(ROOT, "tests", "keys", "test-package.key")], stdout=subprocess.DEVNULL)
    return sha256_of(os.path.join(source, "payload", "bulk.bin"))


def run():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(STICK)
    os.makedirs(STICK2)
    for name in ("org.nbtapp.runtime-2.1.0", "org.nbtapp.hello-1.1.0", "org.nbtapp.hello-1.2.0"):
        shutil.copy(os.path.join(DATA, "good", name + ".nbtpkg"), STICK)
    shutil.copy(os.path.join(DATA, "good", "org.nbtapp.hello-1.3.0.nbtpkg"), STICK2)
    shutil.copy(os.path.join(DATA, "bad", "signature-flipped.nbtpkg"), STICK2)
    bulk_hash = make_bulk()
    bulk_size = os.path.getsize(os.path.join(STICK, "org.nbtapp.bulk-1.0.0.nbtpkg"))
    check(bulk_size > BULK_BYTES, "the bulk archive is smaller than its payload: it compressed")

    agent = Agent()
    try:
        # ---- an empty head unit
        info = ok("info")
        check("agentVersion %s\n" % VERSION in info and "restartNeeded none" in info, "info", info)
        head_unit = [l for l in info.split("\n") if l.startswith("headUnitId ")][0]
        # The key that is built in, and the test key this agent was started with.
        sources = [line.split(None, 2)[1:] for line in ok("sources").strip().split("\n")]
        check(sources == [["embedded", "NBTApps"], ["embedded", "test key"]], "sources", str(sources))
        # ---- the debug TCP link reaches the same agent, for a phone that is paired with it
        pair_over_tcp(agent.tcp_port(), head_unit[len("headUnitId "):])
        check(installed() == [], "a new head unit has packages")
        check("no job on record" in ok("status"), "status with no job")

        # Nothing starts this agent, with the head unit or otherwise, and it says so when it is asked to be.
        state = ok("autostart")
        check(state == "autostart off\navailable no no-launcher\nstarts 0\n", "autostart", state)
        code, out = ctl("autostart", "on")
        check(code == 1 and "nbtctl: autostart.set: no-launcher: " in out, "autostart on, where no starter is", out)
        check(ok("autostart", "off") == state, "autostart off, which is what it was")
        check(ctl("autostart", "maybe")[0] == 2, "autostart with a word that is neither on nor off")

        # A second agent on the same socket is turned away, and the first keeps the socket.
        second = subprocess.run(BENCH + ["--root", HU, "--socket", SOCKET],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        # Its exit status is the one that tells a launcher not to start it again (agent/exit_status.hpp).
        check(second.returncode == 3 and b"already listening" in second.stdout, "a second agent", second.stdout.decode())
        ok("info")

        # This build has no mode that works on a head unit itself: with no --root it says how it is used, and
        # that is all. (tools/bench.py would not pass that on, so the container is started here.)
        bare = subprocess.run(["docker", "run", "--rm", "--network", "none", "--read-only", "--cap-drop", "ALL",
                               "--user", "%d:%d" % (os.getuid(), os.getgid()), IMAGE, "/opt/nbtagent/nbtagent"],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        check(bare.returncode == 2 and b"usage: nbtagent --root DIR" in bare.stdout and b"head unit" not in bare.stdout,
              "nbtagent with no --root", bare.stdout.decode())

        # ---- a plan changes nothing
        plan = ok("plan", "--from", STICK, "org.nbtapp.hello@1.1.0")
        check("install org.nbtapp.runtime 2.1.0\n  install org.nbtapp.hello 1.1.0" in plan and "\nplan " in plan, "plan", plan)
        check(installed() == [] and "no job on record" in ok("status"), "a plan changed something")

        # ---- install, with the dependency taken from the stick
        out = ok("install", "--from", STICK, "org.nbtapp.hello@1.1.0")
        check("job succeeded" in out and "postInstall| post-install: app dir " + HU in out, "install", out)
        check(installed() == ["org.nbtapp.hello 1.1.0 runs-scripts", "org.nbtapp.runtime 2.1.0 -"], "after install", "\n".join(installed()))
        hello = os.path.join(HU, "apps", "org.nbtapp.hello")
        check(os.access(os.path.join(hello, "bin", "hello"), os.X_OK), "hello's program is not there")
        check(os.listdir(os.path.join(HU, "staging", "blobs")) == [], "archives were left in staging")

        # ---- upgrade: with no version given, the highest on the stick
        out = ok("install", "--from", STICK, "org.nbtapp.hello")
        check("upgrade org.nbtapp.hello 1.2.0 (from 1.1.0)" in out and "job succeeded" in out, "upgrade", out)
        check("org.nbtapp.hello 1.2.0 runs-scripts" in installed(), "after upgrade")
        events = ok("status", "--events")
        check("succeeded" in events and "preInstall| pre-install: upgrade org.nbtapp.hello 1.2.0" in events, "status --events", events)

        # ---- an upgrade whose postInstall fails is undone; a package with a broken signature is not even offered
        code, out = ctl("install", "--from", STICK2, "org.nbtapp.hello")
        check(code == 1 and "job failed" in out and "hook-failed" in out, "a failing upgrade", out)
        check("skipped signature-flipped.nbtpkg: bad-signature" in out, "a package with a bad signature", out)
        check("org.nbtapp.hello 1.2.0 runs-scripts" in installed(), "the failed upgrade was not undone")

        # ---- the controller stops a fetch part-way
        code, out = ctl("install", "--from", STICK, "org.nbtapp.bulk", "--cancel-after", "1500000")
        check(code == 1 and "job cancelled" in out and "org.nbtapp.bulk 1.0.0: skipped" in out, "a cancelled fetch", out)
        check(not any(l.startswith("org.nbtapp.bulk") for l in installed()), "a cancelled job installed something")
        blobs = os.listdir(os.path.join(HU, "staging", "blobs"))
        check(len(blobs) == 1 and blobs[0].endswith(".part"), "the partial archive", str(blobs))
        check(os.path.getsize(os.path.join(HU, "staging", "blobs", blobs[0])) == 1500000, "the partial archive's size")

        # ---- the link drops in the middle of the next fetch, which had gone on from where the last one stopped
        code, out = ctl("install", "--from", STICK, "org.nbtapp.bulk", "--drop-after", "600000")
        check(code == 3, "a controller that drops out: exit status %d" % code, out)
        check("fetch %d bytes" % (bulk_size - 1500000) in out, "the plan did not count what staging holds", out)
        time.sleep(0.3)
        status = ok("status")
        check("paused" in status and "2100000 of %d bytes" % bulk_size in status, "a job whose link dropped", status)
        code, out = ctl("restart", "hmi")
        check(code == 0, "a restart is allowed while a job is paused", out)

        # ---- and is resumed over a new link
        out = ok("resume", "--from", STICK)
        check("job succeeded" in out and "org.nbtapp.bulk 1.0.0: done" in out, "resume", out)
        check(sha256_of(os.path.join(HU, "apps", "org.nbtapp.bulk", "bulk.bin")) == bulk_hash, "the installed file is not the packaged one")
        check(os.listdir(os.path.join(HU, "staging", "blobs")) == [], "archives were left in staging",
              str(os.listdir(os.path.join(HU, "staging", "blobs"))))

        # ---- removal
        code, out = ctl("remove", "org.nbtapp.runtime")
        check(code == 1 and "required-by" in out, "removing a package that is depended on", out)
        code, out = ctl("remove", "org.nbtapp.nothere")
        check(code == 1 and "not-installed" in out, "removing a package that is not there", out)
        out = ok("remove", "--delete-data", "org.nbtapp.runtime", "org.nbtapp.hello", "org.nbtapp.bulk")
        check("job succeeded" in out and "postRemove|" in out, "removal", out)
        check(installed() == [], "after removal", "\n".join(installed()))
        check(not os.path.exists(hello) and not os.path.exists(os.path.join(HU, "data", "org.nbtapp.hello")), "files were left behind")

        # ---- other requests
        code, out = ctl("call", "ping")
        check(code == 0 and out.strip() == "{}", "ping", out)
        code, out = ctl("call", "job.frobnicate")
        check(code == 1 and '"code":"unsupported"' in out, "a method the agent does not have", out)
        code, out = ctl("call", "pairing.issue", '{"code":"123456"}')
        check(code == 1 and '"code":"forbidden"' in out, "a pairing key asked for over the local socket", out)
        code, out = ctl("call", "job.status", '{"after":0}')
        check(code == 0 and '"event":"job.done"' in out, "call job.status", out)
        code, out = ctl("cancel")
        check(code == 1 and "too-late" in out, "cancelling a job that has ended", out)

        # ---- the agent stops and starts again: same head unit, and it still knows how the last job went. A phone
        # that is connected is told that the agent is stopping before its link closes.
        watcher = TcpPhone(agent.tcp_port())
        check("result" in watcher.hello()[0], "hello over TCP before the agent stops")
        check(agent.stop() == 0, "nbtagent's exit status")
        said = watcher.last_words()
        check(said == [{"event": "link.closing", "params": {"reason": "stopping"}}],
              "what a phone was told when the agent stopped", repr(said))
        watcher.close()
        check(not os.path.exists(SOCKET), "the socket was left behind")
        code, out = ctl("info")
        check(code == 3, "nbtctl with no agent: exit status %d" % code, out)
        agent = Agent()
        check(head_unit in ok("info"), "the head-unit id changed")
        status = ok("status")
        check("succeeded" in status and "remove org.nbtapp.bulk 1.0.0: done" in status, "the last job after a restart", status)
    finally:
        if agent.process.poll() is None:
            agent.stop()
    with open(os.path.join(WORK, "agent.log"), "rb") as f:
        log = f.read().decode("utf-8", "replace")
    check("TEST KEY" in log, "the agent did not say that it trusts a test key")
    check("DEBUG TCP" in log and "paired in the clear" in log, "the agent did not say what its TCP link is")
    shutil.rmtree(WORK, ignore_errors=True)


if __name__ == "__main__":
    try:
        run()
    except Failed as failure:
        print("agent e2e: FAILED: %s" % failure, file=sys.stderr)
        log = os.path.join(WORK, "agent.log")
        if os.path.exists(log):
            print("---- agent.log\n" + open(log, "rb").read().decode("utf-8", "replace"), file=sys.stderr)
        shutil.rmtree(SOCKET_DIR, ignore_errors=True)
        sys.exit(1)
    shutil.rmtree(SOCKET_DIR, ignore_errors=True)
    print("agent e2e: ok")
