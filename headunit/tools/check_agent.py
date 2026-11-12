#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""`make agent-check`: what can be said on this machine about release/agent/, the folder that goes to a head unit,
and about the agent's own package beside it in release/pkg/. Exits non-zero on any failure, and if the folder
is not there.

  python3 tools/check_agent.py [FOLDER]      (default release/agent)

  - the folder holds nbtagent, nbtctl, version and the scripts hu_probe.sh, hu_install.sh, nbtlaunch.sh and
    nbtstart.sh and nothing else; each script is the one in agent/, and version holds the agent's version as
    agent/version.hpp has it, which is also what hu_install.sh and nbtstart.sh take for a version's name
  - hu_install.sh, nbtlaunch.sh and nbtstart.sh are scripts that sh can read (sh -n). They are written for the
    head unit's ksh and for sh alike; hu_probe.sh is ksh's only
  - each program is what the head unit starts: ELF32 little-endian ARM, EABI5, soft-float calls, an executable
    that is not position-independent (a PIC executable does not start there), the QNX runtime linker as its
    interpreter, no library search path of its own, and exactly the shared libraries in NEEDED below
  - neither program holds anything of the test bench: not the options --test-key and --debug-tcp, and not a key
    from tests/keys/, whose private halves are public
  - nbtagent holds each package key of agent/embedded_keys.hpp, of which there is at least one and none is a key
    from tests/keys/. Without it the program would start and refuse every package
  - release/pkg/org.nbtapp.agent-<version>.nbtpkg (`make agent-pkg`; the folder is pkg/ beside FOLDER) is a
    package, not signed, and byte for byte what tools/agent_pkg.py makes of the folder now: the version in the
    folder, the two programs, and agent/package/manifest.json.in. That manifest places the two programs, the
    version's launcher (nbtlaunch.sh) and the starter it comes with (nbtstart.sh) in ${app} and does nothing
    else, as an agent demands of its own package, and names the agent it requires. Its payload folder holds
    those four and the rest of what a first install needs, hu_install.sh and version, and nothing else:
    unpacked onto a USB stick, that folder is what hu_install.sh is run from

What it cannot say is that the programs run on a car. NEEDED is what they ask for, which is what the Bluetooth
tools ask for too; whether a head unit has libcpp.so.4 is not recorded in this repo, and hu_probe.sh lists it.
Needs readelf (binutils).
"""
import glob, hashlib, os, re, shutil, subprocess, sys

import agent_pkg
import nbtpkg

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASE = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "release", "agent")
SHOWN = os.path.relpath(RELEASE, ROOT) if len(sys.argv) <= 1 else RELEASE
PKG = os.path.join(os.path.dirname(RELEASE), "pkg")
SHOWN_PKG = os.path.join(os.path.dirname(SHOWN), "pkg")
# What the agent's package places: (src, dest, mode) of each file, and nothing but files.
PACKAGE_FILES = [("payload/nbtagent", "${app}/nbtagent", "0755"), ("payload/nbtctl", "${app}/nbtctl", "0755"),
                 ("payload/nbtlaunch.sh", "${app}/nbtlaunch.sh", "0755"), ("payload/nbtstart.sh", "${app}/nbtstart.sh", "0755")]
PACKAGE_KEYS = {"manifestVersion", "id", "name", "version", "requires", "contents", "install"}
# What the agent's package holds: the files it places, and those a first install needs beside them.
PACKAGE_CONTENTS = ["payload/hu_install.sh", "payload/nbtagent", "payload/nbtctl", "payload/nbtlaunch.sh",
                    "payload/nbtstart.sh", "payload/version"]
PROGRAMS = ("nbtagent", "nbtctl")
SCRIPTS = ("hu_probe.sh", "hu_install.sh", "nbtlaunch.sh", "nbtstart.sh")
FOR_SH = ("hu_install.sh", "nbtlaunch.sh", "nbtstart.sh")
VERSION = "version"
# A version's name as the two scripts take it: it names a folder on the head unit.
VERSION_NAME = r"[0-9][0-9A-Za-z.+-]{0,63}"
NEEDED = {"libsocket.so.3", "libcpp.so.4", "libc.so.3"}
INTERPRETER = "/usr/lib/ldqnx.so.2"
BENCH_ONLY = (b"--test-key", b"--debug-tcp", b"TEST KEY", b"DEBUG TCP")

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL  " + msg)


def ok(msg):
    print("ok    " + msg)


def readelf(*args):
    return subprocess.run(["readelf", "-W"] + list(args), check=True, stdout=subprocess.PIPE,
                          universal_newlines=True).stdout


def test_keys():
    """name -> the 32 bytes of each public key in tests/keys/: comment lines, then 64 hex characters."""
    keys = {}
    for path in sorted(glob.glob(os.path.join(ROOT, "tests", "keys", "*.pub"))):
        with open(path) as f:
            lines = [line.strip() for line in f if line.strip() and not line.startswith("#")]
        if len(lines) == 1 and re.fullmatch(r"[0-9a-fA-F]{64}", lines[0]):
            keys[os.path.basename(path)] = bytes.fromhex(lines[0])
        else:
            fail("tests/keys/%s is not a public key file: cannot look for it in the programs" % os.path.basename(path))
    if not keys:
        fail("tests/keys/ holds no public key: cannot tell that the programs hold no test key")
    return keys


def built_in_keys(test):
    """name -> the 32 bytes of each key table in agent/embedded_keys.hpp, which nbtagent has to hold."""
    keys, problems = agent_pkg.built_in_keys()
    for problem in problems:
        fail(problem)
    for name, key in keys.items():
        for test_name, test_key in test.items():
            if key == test_key:
                fail("agent/embedded_keys.hpp: %s is the key of tests/keys/%s, whose private half is public"
                     % (name, test_name))
    return keys


def check_package():
    """The agent's own package, as `make agent-pkg` left it beside the folder."""
    before = len(failures)
    try:
        name, fresh, manifest = agent_pkg.build(RELEASE, agent_pkg.read_version(RELEASE))
    except agent_pkg.Refused as e:
        fail("the agent's package cannot be made of %s: %s" % (SHOWN, e))
        return
    template = "agent/package/manifest.json.in"
    install = manifest.get("install", {})
    files = sorted((f.get("src"), f.get("dest"), f.get("mode")) for f in install.get("files", []))
    if set(install) != {"files"} or files != PACKAGE_FILES:
        fail("%s: the agent's package places nbtagent, nbtctl, nbtlaunch.sh and nbtstart.sh in ${app} with mode "
             "0755, and does nothing else" % template)
    if set(manifest) - PACKAGE_KEYS:
        fail("%s: %s: the agent's package is alone in its job, and depends on nothing"
             % (template, ", ".join(sorted(set(manifest) - PACKAGE_KEYS))))
    if "agent" not in manifest.get("requires", {}):
        fail("%s: requires.agent is missing: it says which agents can place this one" % template)
    if sorted(c.get("path") for c in manifest.get("contents", [])) != PACKAGE_CONTENTS:
        fail("tools/agent_pkg.py: the agent's package holds %s and nothing else: the two programs, the launcher, "
             "the starter, and what a first install needs beside them" % ", ".join(PACKAGE_CONTENTS))

    shown = os.path.join(SHOWN_PKG, name)
    path = os.path.join(PKG, name)
    if not os.path.isfile(path):
        fail("%s is missing: make agent-pkg" % shown)
        return
    with open(path, "rb") as f:
        data = f.read()
    try:
        info = nbtpkg.verify_package(data, None, allow_unsigned=True)
    except nbtpkg.PkgError as e:
        fail("%s is not a package: %s" % (shown, e))
        return
    if "signature" in info:
        fail("%s is signed: `make agent-release` puts the signed archive in %s/signed" % (shown, SHOWN_PKG))
    elif data != fresh:
        fail("%s is not the package of the programs in %s: make agent-pkg" % (shown, SHOWN))
    if len(failures) == before:
        ok("%s: version %s, the two programs of %s, the two scripts and the first install's files, %d bytes, not signed, sha256 %s"
           % (shown, info["version"], SHOWN, len(data), hashlib.sha256(data).hexdigest()))


def check_folder():
    before = len(failures)
    want = set(PROGRAMS) | set(SCRIPTS) | {VERSION}
    present = set(os.listdir(RELEASE))
    for name in sorted(want - present):
        fail("%s/%s is missing" % (SHOWN, name))
    for name in sorted(present - want):
        fail("%s/%s should not be there" % (SHOWN, name))
    for name in SCRIPTS:
        released = os.path.join(RELEASE, name)
        if not os.path.isfile(released):
            continue
        with open(released, "rb") as a, open(os.path.join(ROOT, "agent", name), "rb") as b:
            if a.read() != b.read():
                fail("%s/%s differs from agent/%s: build again" % (SHOWN, name, name))
        if name in FOR_SH:
            read = subprocess.run(["sh", "-n", released], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  universal_newlines=True)
            if read.returncode != 0:
                fail("sh cannot read %s/%s: %s" % (SHOWN, name, read.stdout.strip()))
    if len(failures) == before:
        ok("%s: %s, %s and the scripts %s, which are agent/'s" % (SHOWN, ", ".join(PROGRAMS), VERSION, ", ".join(SCRIPTS)))


def check_version():
    """release/agent/version names the folder hu_install.sh puts the programs in, so it has to be their version."""
    with open(os.path.join(ROOT, "agent", "version.hpp")) as f:
        found = re.search(r'^const char kAgentVersion\[\] = "(.*)";$', f.read(), re.M)
    if not found:
        fail("agent/version.hpp: kAgentVersion not found")
        return
    version = found.group(1)
    if not re.fullmatch(VERSION_NAME, version):
        fail("the agent's version %r is not a name hu_install.sh and nbtstart.sh take: a digit first, then "
             "letters, digits, '.', '+' and '-', 64 in all at most" % version)
        return
    path = os.path.join(RELEASE, VERSION)
    if not os.path.isfile(path):
        return
    with open(path) as f:
        text = f.read()
    if text != version + "\n":
        fail("%s/%s holds %r, and agent/version.hpp says %s: build again" % (SHOWN, VERSION, text, version))
    else:
        ok("%s/%s: %s, as agent/version.hpp has it" % (SHOWN, VERSION, version))


def check_program(name, keys, built_in):
    path = os.path.join(RELEASE, name)
    if not os.path.isfile(path):
        return
    before = len(failures)
    header = readelf("-h", path)

    def field(label):
        m = re.search(r"^\s*%s:\s*(.*)$" % re.escape(label), header, re.M)
        return m.group(1).strip() if m else ""

    if field("Class") != "ELF32" or "little endian" not in field("Data") or field("Machine") != "ARM":
        fail("%s is not ELF32 little-endian ARM (%s, %s, %s)" % (name, field("Class"), field("Data"), field("Machine")))
    if not field("Type").startswith("EXEC"):
        fail("%s is not a plain executable (%s); a position-independent one does not start on the head unit"
             % (name, field("Type")))
    flags = int(field("Flags").split(",")[0], 16) if field("Flags") else 0
    if flags >> 24 != 5:
        fail("%s is not EABI version 5 (flags 0x%x)" % (name, flags))
    if flags & 0x400 or "Tag_ABI_VFP_args" in readelf("-A", path):
        fail("%s uses the hard-float call ABI; the head unit is soft-float" % name)

    interpreter = re.findall(r"Requesting program interpreter: ([^\]]+)\]", readelf("-l", path))
    if interpreter != [INTERPRETER]:
        fail("%s asks for the interpreter %s, expected %s" % (name, interpreter, INTERPRETER))
    dynamic = readelf("-d", path)
    needed = set(re.findall(r"\(NEEDED\).*\[(.*)\]", dynamic))
    if needed != NEEDED:
        fail("%s NEEDED is %s, expected %s" % (name, sorted(needed), sorted(NEEDED)))
    paths = re.findall(r"\((RPATH|RUNPATH)\).*\[(.*)\]", dynamic)
    if paths:
        fail("%s has a library search path of its own: %s" % (name, paths))

    with open(path, "rb") as f:
        data = f.read()
    for text in BENCH_ONLY:
        if text in data:
            fail("%s holds %r, which only a build for the test bench has" % (name, text.decode()))
    for key_name, key in keys.items():
        if key in data:
            fail("%s holds the key of tests/keys/%s, whose private half is public" % (name, key_name))
    if name == "nbtagent":
        for key_name, key in built_in.items():
            if key not in data:
                fail("%s does not hold the package key %s of agent/embedded_keys.hpp: build again" % (name, key_name))
        with open(os.path.join(ROOT, "agent", "version.hpp")) as f:
            version = re.search(r'^const char kAgentVersion\[\] = "(.*)";$', f.read(), re.M)
        # What `nbtagent --version` prints, which hu_install.sh holds against the version file.
        if version and version.group(1).encode() + b"\0" not in data:
            fail("%s does not hold the version %s of agent/version.hpp: build again" % (name, version.group(1)))
    build = re.search(rb"\(build ([^)%]{1,80})\), head unit", data)
    if len(failures) == before:
        holds = ", the package key %s" % " ".join(sorted(built_in)) if name == "nbtagent" and built_in else ""
        ok("%s: ELF32 ARM EABI5 soft-float executable, not PIC, NEEDED %s, nothing of the test bench%s%s"
           % (name, " ".join(sorted(needed)), holds,
              (", build " + build.group(1).decode("ascii", "replace")) if build else ""))


def main():
    if not os.path.isdir(RELEASE):
        print("FAIL  %s does not exist: ./build.sh agent builds release/agent" % SHOWN)
        return 1
    if not shutil.which("readelf"):
        print("FAIL  readelf not found: cannot check the programs")
        return 1
    check_folder()
    check_version()
    keys = test_keys()
    built_in = built_in_keys(keys)
    for name in PROGRAMS:
        check_program(name, keys, built_in)
    check_package()
    if failures:
        print("\n%d check(s) failed" % len(failures))
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
