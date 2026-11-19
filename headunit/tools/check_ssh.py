#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""`make ssh-check`: what can be said on this machine about the builds of Dropbear and OpenSSH's sftp-server
(ssh/README). Exits non-zero on any failure, and if a build is not there.

  python3 tools/check_ssh.py

  - build/ssh/qnx/ (`make ssh`): dropbearmulti and sftp-server are what the head unit starts: ELF32 little-endian
    ARM, EABI5, soft-float calls, an executable that is not position-independent, the QNX runtime linker as its
    interpreter, no library search path of its own, and exactly libc.so.3 and libsocket.so.3 in NEEDED (so no
    zlib: compression is built out)
  - build/ssh/host/ (`make ssh-host`): dropbearmulti's key tool makes an ed25519 host key, and its server, on one
    connection (-i, its standard input and output one end of a socket pair, as a program of the agent's gives it
    one), sends the SSH banner of the Dropbear version in build/ssh/src/dropbear before anything is said to it
  - the size of each of the four programs is printed

Nothing of the head unit's builds is run. Writes build/ssh/check/ only. Needs readelf (binutils).
"""
import os, re, shutil, socket, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SSH = os.path.join(ROOT, "build", "ssh")
PROGRAMS = ("dropbearmulti", "sftp-server")
NEEDED = {"libc.so.3", "libsocket.so.3"}
INTERPRETER = "/usr/lib/ldqnx.so.2"

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL  " + msg)


def ok(msg):
    print("ok    " + msg)


def readelf(*args):
    return subprocess.run(["readelf", "-W"] + list(args), check=True, stdout=subprocess.PIPE,
                          universal_newlines=True).stdout


def check_qnx(name):
    path = os.path.join(SSH, "qnx", name)
    shown = os.path.relpath(path, ROOT)
    if not os.path.isfile(path):
        fail("%s does not exist: make ssh builds it" % shown)
        return
    before = len(failures)
    header = readelf("-h", path)

    def field(label):
        m = re.search(r"^\s*%s:\s*(.*)$" % re.escape(label), header, re.M)
        return m.group(1).strip() if m else ""

    if field("Class") != "ELF32" or "little endian" not in field("Data") or field("Machine") != "ARM":
        fail("%s is not ELF32 little-endian ARM (%s, %s, %s)" % (shown, field("Class"), field("Data"), field("Machine")))
    if not field("Type").startswith("EXEC"):
        fail("%s is not a plain executable (%s)" % (shown, field("Type")))
    flags = int(field("Flags").split(",")[0], 16) if field("Flags") else 0
    if flags >> 24 != 5:
        fail("%s is not EABI version 5 (flags 0x%x)" % (shown, flags))
    if flags & 0x400 or "Tag_ABI_VFP_args" in readelf("-A", path):
        fail("%s uses the hard-float call ABI; the head unit is soft-float" % shown)
    interpreter = re.findall(r"Requesting program interpreter: ([^\]]+)\]", readelf("-l", path))
    if interpreter != [INTERPRETER]:
        fail("%s asks for the interpreter %s, expected %s" % (shown, interpreter, INTERPRETER))
    dynamic = readelf("-d", path)
    needed = set(re.findall(r"\(NEEDED\).*\[(.*)\]", dynamic))
    if needed != NEEDED:
        fail("%s NEEDED is %s, expected %s" % (shown, sorted(needed), sorted(NEEDED)))
    paths = re.findall(r"\((RPATH|RUNPATH)\).*\[(.*)\]", dynamic)
    if paths:
        fail("%s has a library search path of its own: %s" % (shown, paths))
    if len(failures) == before:
        ok("%s: %d bytes, ELF32 ARM EABI5 soft-float executable, not PIC, NEEDED %s"
           % (shown, os.path.getsize(path), " ".join(sorted(needed))))


def dropbear_version():
    path = os.path.join(SSH, "src", "dropbear", "src", "sysoptions.h")
    with open(path) as f:
        m = re.search(r'^#define DROPBEAR_VERSION "(.*)"$', f.read(), re.M)
    return m.group(1) if m else None


def check_host():
    host = os.path.join(SSH, "host")
    for name in PROGRAMS:
        path = os.path.join(host, name)
        if not os.path.isfile(path):
            fail("%s does not exist: make ssh-host builds it" % os.path.relpath(path, ROOT))
            return
        ok("%s: %d bytes" % (os.path.relpath(path, ROOT), os.path.getsize(path)))
    multi = os.path.join(host, "dropbearmulti")

    work = os.path.join(SSH, "check")
    os.makedirs(work, exist_ok=True)
    key = os.path.join(work, "hostkey_ed25519")
    if os.path.exists(key):
        os.remove(key)
    made = subprocess.run([multi, "dropbearkey", "-t", "ed25519", "-f", key], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, universal_newlines=True, timeout=30)
    public = re.search(r"^ssh-ed25519 \S+", made.stdout, re.M)
    if made.returncode != 0 or not os.path.isfile(key) or not public:
        fail("dropbearkey did not make an ed25519 key (exit %d): %s" % (made.returncode, made.stdout.strip()))
        return
    ok("dropbearkey made an ed25519 host key: %s..." % public.group(0)[:40])

    version = dropbear_version()
    want = "SSH-2.0-dropbear_%s" % version
    ours, theirs = socket.socketpair()
    server = subprocess.Popen([multi, "dropbear", "-i", "-E", "-r", key], stdin=theirs, stdout=theirs,
                              stderr=subprocess.PIPE, universal_newlines=True, close_fds=True)
    theirs.close()
    ours.settimeout(10)
    banner = b""
    try:
        while b"\n" not in banner and len(banner) < 256:
            got = ours.recv(256)
            if not got:
                break
            banner += got
    except socket.timeout:
        pass
    ours.close()
    try:
        _, log = server.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        server.kill()
        _, log = server.communicate()
    line = banner.split(b"\r\n")[0].decode("ascii", "replace")
    if line != want:
        fail("the server on a socket pair said %r, expected %r; its log: %s" % (line, want, log.strip()))
    else:
        ok("the server on one connection (-i, a socket pair) said %s" % line)


def main():
    if not shutil.which("readelf"):
        print("FAIL  readelf not found: cannot check the programs")
        return 1
    for name in PROGRAMS:
        check_qnx(name)
    check_host()
    if failures:
        print("%d check(s) failed" % len(failures))
        return 1
    print("ssh-check: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
