#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""`make test-ssh-pkg`: host test of tools/ssh_pkg.py, which builds the SSH package and signs it for a release.
Needs python3 with the `cryptography` module, and git. Exits non-zero on the first failure.

Everything here is a throwaway folder: the three programs as a few bytes each, an export of the sources that holds
only the files the package takes from it, keys made for the run, and a header that has such a key in the form
agent/embedded_keys.hpp keeps. No key of this repo or of the catalogue is read, and nothing is built.

  1. build: the archive is the template with the version filled in, the three programs as payload/bin/ and the
     licence texts as payload/licenses/, verifies as a package that is not signed, with starts-program as its only
     flag, and is the same bytes every time; the manifest places bin/ in ${app}/bin/ (0755), the licences in
     ${app}/licenses/ (0644), scp as a link to dropbearmulti, the switched channel ssh whose program is
     bin/sshrelay, and the channel's two logs; the licence files are the export's byte for byte, and the notices
     the comments at the head of each file and none of its code; what it refuses
  2. release: signs with a key the header holds, to signed/, and the result verifies under that key; what it
     refuses, each by itself, among them programs of other sources than the checkout's
  3. this repo's sources: ssh/relay/version.hpp names a version, as spec/manifest.schema.json has one, and has the
     line the relay is asked for; ssh/README names every file whose notice the package ships; the record of the
     sources the checkout gives names both submodules' pins
"""
import json
import os
import re
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import nbtpkg  # noqa: E402
import ssh_pkg  # noqa: E402

checks = 0
STAMP = ("28216cd9af822732a1549b78621c2ea9a76a0fe5  ssh/dropbear\n"
         "d449e662b6b8b8cb20e852d8084c269af1c5e8f1  ssh/openssh\n"
         + "%s  ssh/dropbear-qnx.patch\n" % ("1" * 64)
         + "%s  ssh/localoptions.h\n" % ("2" * 64))
BUILD_STAMP = STAMP + "%s  ssh/build-qnx.sh\n" % ("3" * 64)
LICENCES = ("dropbear-LICENSE", "libtomcrypt-LICENSE", "libtommath-LICENSE", "dropbear-notices", "openssh-LICENCE",
            "openssh-notices", "README")


def check(condition, what, detail=""):
    global checks
    checks += 1
    if not condition:
        print("FAIL  %s%s" % (what, "\n" + str(detail) if detail != "" else ""))
        sys.exit(1)


def refused(what, why, function, *args, **kwargs):
    try:
        function(*args, **kwargs)
    except ssh_pkg.Refused as e:
        check(why in str(e), what, e)
        return
    check(False, what, "it was not refused")


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def header(path, public):
    table = ", ".join("0x%02x" % b for b in public)
    put(path, "const uint8_t kThrowaway[32] = {\n    %s,\n};\n" % table)


def notice_of(name):
    return "/* $Id: %s $ */\n\n/*\n * Copyright (c) 2026 A. Holder of %s\n * Stand-in terms.\n */\n" % (name, name)


def programs(folder, build="abc1234", version="1.2.3", line=True, stamp=BUILD_STAMP):
    """The head unit's three programs as make ssh and make ssh-relay leave them, the relay saying which version it
    is and what it was built from, and the record of the sources."""
    said = ("sshrelay %s (build %s)" % (version, build)).encode() if line else b"a relay that says nothing"
    put(os.path.join(folder, "sshrelay"), b"\x7fELF relay \0" + said + b"\0")
    put(os.path.join(folder, "dropbearmulti"), b"\x7fELF stands in for dropbearmulti\0")
    put(os.path.join(folder, "sftp-server"), b"\x7fELF stands in for sftp-server\0")
    put(os.path.join(folder, "obj", "make.log"), b"not part of the package")
    if stamp is not None:
        put(os.path.join(folder, "sources"), stamp)


def export(folder, stamp=STAMP):
    """An export of the sources with the files the package takes from it: each notice file with its notice, then
    code that must stay out of the package."""
    for _, path in ssh_pkg.DROPBEAR_LICENCES + ssh_pkg.OPENSSH_LICENCES:
        # Not UTF-8 everywhere: a licence file is copied byte for byte.
        put(os.path.join(folder, path), ("The licence text of %s, Kungliga Tekniska H\xf6gskolan.\n" % path).encode("latin-1"))
    for base, names in (("dropbear", ssh_pkg.DROPBEAR_NOTICES), ("openssh", ssh_pkg.OPENSSH_NOTICES)):
        for name in names:
            put(os.path.join(folder, base, name), notice_of(name) + '#include "includes.h"\n\nint code_of_%d;\n' % len(name))
    put(os.path.join(folder, "dropbear", "src", "sysoptions.h"), '#define DROPBEAR_VERSION "2026.94"\n')
    put(os.path.join(folder, "openssh", "version.h"), '#define SSH_VERSION\t"OpenSSH_10.6"\n#define SSH_PORTABLE\t"p2"\n')
    put(os.path.join(folder, "sources"), stamp)


def main():
    work = tempfile.TemporaryDirectory()
    top = work.name

    # ---- 1. build
    built, src = os.path.join(top, "qnx"), os.path.join(top, "src")
    programs(built)
    export(src)
    name, data, manifest = ssh_pkg.build(built, src, "1.2.3")
    check(name == "org.nbtapp.ssh-1.2.3.nbtpkg", "the archive's name", name)
    info = nbtpkg.verify_package(data, None, allow_unsigned=True)
    check(info["id"] == "org.nbtapp.ssh" and info["version"] == "1.2.3" and info["name"] == "SSH access" and
          "signature" not in info, "the archive is the SSH package, version 1.2.3, not signed", info)
    check(info["flags"] == ["starts-program"], "its only risk flag is starts-program", info["flags"])
    source = json.loads(ssh_pkg.manifest_text("1.2.3"))
    check({k: v for k, v in manifest.items() if k != "contents"} == source, "its manifest is the template's", manifest)
    files = {e["name"]: e["data"] for e in nbtpkg.read_zip(data) if not e["dir"]}
    check(sorted(files) == sorted(["manifest.json"] + ["payload/bin/" + p for p in ssh_pkg.PROGRAMS] +
                                  ["payload/licenses/" + n for n in LICENCES]), "its files", sorted(files))
    for program in ssh_pkg.PROGRAMS:
        with open(os.path.join(built, program), "rb") as f:
            check(files["payload/bin/" + program] == f.read(), "payload/bin/%s is the build's" % program)
    install = manifest["install"]
    check(install == {"files": [{"src": "payload/bin/", "dest": "${app}/bin/", "mode": "0755"},
                                {"src": "payload/licenses/", "dest": "${app}/licenses/", "mode": "0644"}],
                      "links": [{"path": "${app}/bin/scp", "target": "dropbearmulti"}]},
          "it places the programs in ${app}/bin/, the licences in ${app}/licenses/ and scp as a link, and runs no hook",
          install)
    check(manifest["channels"] == [{"name": "ssh", "apps": [], "access": "both", "switch": True,
                                    "start": {"program": "bin/sshrelay"}}],
          "one channel, ssh, for NBTApps alone, switched, its program the relay", manifest["channels"])
    check(manifest["logs"] == [{"path": "${tmp}/channels/ssh.log"}, {"path": "${tmp}/channels/ssh.log.1"}],
          "its logs are the channel's two", manifest["logs"])
    check(manifest["requires"] == {"agent": ">=1.7.0-beta.1", "app": ">=0.9.0"},
          "it requires the first agent with channels, its pre-release too, and app 0.9.0", manifest["requires"])
    check(nbtpkg.agent_requirement_ok(manifest["requires"]["agent"], nbtpkg.KEY_FIRST_AGENT["channels"]),
          "and that requirement keeps out every agent without channels")
    for licence, path in ssh_pkg.DROPBEAR_LICENCES + ssh_pkg.OPENSSH_LICENCES:
        with open(os.path.join(src, path), "rb") as f:
            check(files["payload/licenses/" + licence] == f.read(), "%s is the export's %s, byte for byte" % (licence, path))
    for licence, base, names in (("dropbear-notices", "dropbear", ssh_pkg.DROPBEAR_NOTICES),
                                 ("openssh-notices", "openssh", ssh_pkg.OPENSSH_NOTICES)):
        text = files["payload/licenses/" + licence].decode()
        for n in names:
            check(("==== %s\n\n" % n + notice_of(n)) in text, "%s has the notice of %s/%s, as the file has it" % (licence, base, n), text)
        check("code_of_" not in text and "#include" not in text, "%s has none of the files' code" % licence, text)
    readme = files["payload/licenses/README"].decode()
    for n in LICENCES[:-1]:
        check(n in readme, "the licences' README names %s" % n, readme)
    check("Dropbear 2026.94" in readme and "OpenSSH 10.6p2" in readme and "dropbearmulti" in readme and
          "sftp-server" in readme, "and says which program and which release each belongs to", readme)
    check(ssh_pkg.build(built, src, "1.2.3")[1] == data, "the same programs and sources give the same bytes")
    check(ssh_pkg.build(built, src, "1.2.4")[0] == "org.nbtapp.ssh-1.2.4.nbtpkg", "--version names another version")

    refused("a version that is not one", "the package is refused", ssh_pkg.build, built, src, "1.2")
    refused("a version that cannot stand in the template", "is not a version", ssh_pkg.build, built, src, '1.0.0", "x": "')
    refused("no programs", "make ssh", ssh_pkg.build, os.path.join(top, "nowhere"), src, "1.2.3")
    lacking = os.path.join(top, "lacking")
    programs(lacking)
    os.remove(os.path.join(lacking, "sshrelay"))
    refused("no relay", "./build.sh ssh", ssh_pkg.build, lacking, src, "1.2.3")
    unrecorded = os.path.join(top, "unrecorded")
    programs(unrecorded, stamp=None)
    refused("programs with no record of their sources", "sources is not there: make ssh", ssh_pkg.build, unrecorded, src, "1.2.3")
    other = os.path.join(top, "other")
    programs(other, stamp=BUILD_STAMP.replace("28216cd9", "0000cd9a"))
    refused("programs of another pin than the export's", "name other sources", ssh_pkg.build, other, src, "1.2.3")
    unexported = os.path.join(top, "unexported")
    export(unexported)
    os.remove(os.path.join(unexported, "openssh", "LICENCE"))
    refused("an export without OpenSSH's LICENCE", "LICENCE is not there: make ssh-src", ssh_pkg.build, built, unexported, "1.2.3")
    moved = os.path.join(top, "moved")
    export(moved)
    put(os.path.join(moved, "dropbear", "src", "scp.c"), '#include "includes.h"\n/* Copyright later on */\n')
    refused("a file of the list with no notice at its head", "src/scp.c has no notice at its head", ssh_pkg.build, built, moved, "1.2.3")
    latin = os.path.join(top, "latin")
    export(latin)
    put(os.path.join(latin, "openssh", "misc.c"), b"/*\n * Copyright (c) 2026 H\xf6lder\n */\nint code;\n")
    refused("a notice that is not UTF-8", "misc.c is not UTF-8", ssh_pkg.build, built, latin, "1.2.3")

    # ---- 2. release
    seed, other_seed = os.urandom(32), os.urandom(32)
    key, other_key = os.path.join(top, "keys", "one.key"), os.path.join(top, "keys", "other.key")
    os.makedirs(os.path.dirname(key))
    nbtpkg.write_key(key, seed, "a test's key", True)
    nbtpkg.write_key(other_key, other_seed, "a test's other key", True)
    built_in = os.path.join(top, "embedded_keys.hpp")
    header(built_in, nbtpkg.public_key(seed))
    pkg = os.path.join(top, "pkg")

    def release(folder=built, key_file=key, checked_out="abc1234", version="1.2.3", sources=BUILD_STAMP):
        return ssh_pkg.release(folder, src, pkg, key_file, header=built_in, checked_out=checked_out, version=version,
                               sources=sources)

    refused("no archive", "make ssh-pkg", release)
    put(os.path.join(pkg, name), data)
    path, signed, public = release()
    check(path == os.path.join(pkg, "signed", name) and public == nbtpkg.public_key(seed), "where the signed archive goes", path)
    with open(path, "rb") as f:
        check(f.read() == signed, "the file is the signed archive")
    info = nbtpkg.verify_package(signed, [nbtpkg.public_key(seed)])
    check(info.get("key") == nbtpkg.public_key(seed).hex() and info["version"] == "1.2.3", "it verifies under the key", info)
    check(nbtpkg.sign_package(data, seed) == signed, "and is the archive, signed, with nothing else changed")
    check(release()[1] == signed, "signing again gives the same archive")

    refused("a key that is not built in", "not the private half of a key built into the agent", release, key_file=other_key)
    put(path, signed[:-1] + bytes([signed[-1] ^ 1]))
    refused("a signed archive of other bytes is there", "Raise the version in ssh/relay/version.hpp", release)
    put(path, signed)
    refused("a relay of another commit", "sshrelay was built from abc1234, and what is checked out is def5678", release,
            checked_out="def5678")
    refused("Dropbear and sftp-server of another patch than the checkout's",
            "dropbearmulti and sftp-server were built from other sources than the checkout's", release,
            sources=BUILD_STAMP.replace("1" * 64, "4" * 64))
    refused("Dropbear and sftp-server of another pin than the checkout's", "make ssh", release,
            sources=BUILD_STAMP.replace("d449e662", "d449e663"))
    refused("a relay of another version than the package", "says it is version 1.2.3, and the package is version 1.2.4",
            lambda: (put(os.path.join(pkg, "org.nbtapp.ssh-1.2.4.nbtpkg"), ssh_pkg.build(built, src, "1.2.4")[1]),
                     release(version="1.2.4")))

    for label, build, why in (("dirty", "abc1234-dirty", "changes that are not committed"),
                              ("unknown", "unknown", "changes that are not committed")):
        folder = os.path.join(top, label)
        programs(folder, build=build)
        put(os.path.join(pkg, name), ssh_pkg.build(folder, src, "1.2.3")[1])
        refused("a relay built from a %s checkout" % label, why, release, folder=folder, checked_out=build)
    silent = os.path.join(top, "silent")
    programs(silent, line=False)
    put(os.path.join(pkg, name), ssh_pkg.build(silent, src, "1.2.3")[1])
    refused("a relay that names no version and no build", "does not say which version it is", release, folder=silent)
    refused("an archive of other programs", "is not the package of the programs", release)

    # ---- 3. this repo's sources
    version = ssh_pkg.source_version()
    with open(os.path.join(ROOT, "spec", "manifest.schema.json")) as f:
        pattern = json.load(f)["$defs"]["version"]["pattern"]
    check(re.search(pattern, version) is not None, "ssh/relay/version.hpp names a version", version)
    with open(ssh_pkg.VERSION_HEADER) as f:
        line = re.search(r'^#define NBT_SSHRELAY_BUILD_LINE (.*)$', f.read(), re.M)
    check(line is not None and line.group(1) == '"sshrelay " NBT_SSH_VERSION " (build " NBT_APP_VERSION ")"',
          "ssh/relay/version.hpp has the line the relay is asked for, in the form tools/ssh_pkg.py reads",
          line and line.group(1))
    said = ssh_pkg.build_line(("sshrelay %s (build 1a2b3c4)" % version).encode())
    check(said == (version, "1a2b3c4"), "and that form is read back", said)
    bad = os.path.join(top, "version.hpp")
    put(bad, '#define NBT_SSH_VERSION "1.0"\n#define NBT_SSH_VERSION "1.1"\n')
    refused("a header that names two versions", "does not name the package's version", ssh_pkg.source_version, bad)
    with open(os.path.join(ROOT, "ssh", "README")) as f:
        readme = f.read()
    shipped = readme[readme.index("Licences to ship with the programs"):]
    for base, names in (("dropbear", ssh_pkg.DROPBEAR_NOTICES), ("openssh", ssh_pkg.OPENSSH_NOTICES)):
        for n in names:
            check(n in shipped, "ssh/README names %s/%s among the notices the package ships" % (base, n))
    now = ssh_pkg.stamp_now().splitlines()
    check(len(now) == 5 and all(re.fullmatch(r"[0-9a-f]{40}  ssh/(dropbear|openssh)", l) for l in now[:2]) and
          all(re.fullmatch(r"[0-9a-f]{64}  ssh/\S+", l) for l in now[2:]),
          "the record of this checkout's sources: the two pins, and the SHA-256 of the patch, the options and the "
          "build script", now)

    print("test_ssh_pkg: ok (%d checks)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
