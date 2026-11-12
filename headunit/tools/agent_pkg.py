#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""The agent's own package, org.nbtapp.agent: the archive an agent on a head unit updates itself with
(agent/engine.hpp, README "The agent updates itself").

  agent_pkg.py build [PROGRAMS] [--version V] [-o OUTDIR]       `make agent-pkg`
  agent_pkg.py release --key KEY [PROGRAMS] [--pkg DIR]         `make agent-release KEY=FILE`

`build` makes OUTDIR/org.nbtapp.agent-<version>.nbtpkg, which is not signed. PROGRAMS (default release/agent) is
the folder that holds nbtagent and nbtctl. The package is agent/package/manifest.json.in with the version filled
in, and a payload folder. What the manifest places, all of it in the version's own folder: the two programs as
payload/nbtagent and payload/nbtctl, the version's launcher, agent/nbtlaunch.sh, and the starter the version
comes with, agent/nbtstart.sh, under their own names. What it lists and places nowhere, because only a first
install needs it: agent/hu_install.sh, and payload/version, the version on one line. The version is --version, or
the one line of PROGRAMS/version. The same programs, scripts and version give the same bytes. OUTDIR is
release/pkg unless -o names another.

So the archive is also what puts the agent onto a head unit for the first time. It is a zip: unpacked onto a USB
stick, its payload folder holds what agent/hu_install.sh asks for beside itself, and that script is run from
there. An agent that updates itself with the package places the two programs and the two scripts, and leaves
the rest in the archive.

`release` signs that archive for the catalogue: DIR/org.nbtapp.agent-<version>.nbtpkg (DIR is release/pkg) gives
DIR/signed/org.nbtapp.agent-<version>.nbtpkg, and the SHA-256 the catalogue records is printed. It refuses:

  - an archive that is not what `build` makes of PROGRAMS now
  - programs built from another commit than the one checked out, or from a checkout with changes that are not
    committed: nbtagent holds what `git describe --always --dirty` said when it was built (build.sh), and a
    release is the commit it is tagged with
  - a key whose public half is not built into the agent (agent/embedded_keys.hpp): every agent would refuse what
    it signed
  - a signed archive of that version that is there already with other bytes: a published version is never
    changed, and a new archive needs a new version

Needs python3; `release` needs the `cryptography` module and git.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import nbtpkg  # noqa: E402

PACKAGE_ID = "org.nbtapp.agent"
PROGRAMS = ("nbtagent", "nbtctl")
# The scripts, out of agent/: the version's launcher and the starter it comes with, which an update places beside
# the programs, and the first install's own. And the file that names the version, which hu_install.sh reads; an
# update places neither that nor hu_install.sh.
SCRIPTS = ("hu_install.sh", "nbtlaunch.sh", "nbtstart.sh")
SCRIPTS_DIR = os.path.join(ROOT, "agent")
VERSION_FILE = "version"
TEMPLATE = os.path.join(ROOT, "agent", "package", "manifest.json.in")
HEADER = os.path.join(ROOT, "agent", "embedded_keys.hpp")
RELEASE = os.path.join(ROOT, "release", "agent")
PKG_DIR = os.path.join(ROOT, "release", "pkg")
# What can stand for a version in the template's text. Whether it is a version is the manifest check's to say.
VERSION_TEXT = r"[0-9A-Za-z.+-]{1,64}"
BUILD_STRING = rb"\(build ([^)%]{1,80})\), head unit"


class Refused(Exception):
    pass


def archive_name(version):
    return "%s-%s.nbtpkg" % (PACKAGE_ID, version)


def read_version(programs):
    """The version PROGRAMS/version names: what `make agent` wrote there from agent/version.hpp."""
    path = os.path.join(programs, "version")
    try:
        with open(path) as f:
            text = f.read()
    except OSError:
        raise Refused("%s is not there: build the agent, or say the version with --version" % path)
    if not re.fullmatch(VERSION_TEXT + r"\n", text):
        raise Refused("%s does not hold a version on one line" % path)
    return text.strip()


def manifest_text(version):
    """The package's manifest.json as the source has it: the template, with the version filled in."""
    if not re.fullmatch(VERSION_TEXT, version):
        raise Refused("%r is not a version" % version)
    with open(TEMPLATE) as f:
        template = f.read()
    if template.count("@VERSION@") != 1:
        raise Refused("%s does not have @VERSION@ exactly once" % TEMPLATE)
    return template.replace("@VERSION@", version)


def build(programs, version):
    """(file name, archive bytes, manifest) of the package made of the two programs in `programs`, agent/'s three
    scripts and the version's file. Not signed."""
    text = manifest_text(version)
    with tempfile.TemporaryDirectory() as stage:
        with open(os.path.join(stage, nbtpkg.MANIFEST), "w") as f:
            f.write(text)
        os.mkdir(os.path.join(stage, "payload"))
        for name in PROGRAMS:
            source = os.path.join(programs, name)
            if not os.path.isfile(source):
                raise Refused("%s is not there" % source)
            shutil.copyfile(source, os.path.join(stage, "payload", name))
        for name in SCRIPTS:
            shutil.copyfile(os.path.join(SCRIPTS_DIR, name), os.path.join(stage, "payload", name))
        with open(os.path.join(stage, "payload", VERSION_FILE), "w") as f:
            f.write(version + "\n")
        try:
            manifest, data = nbtpkg.build_package(stage)
        except nbtpkg.PkgError as e:
            raise Refused("the package is refused: %s" % e)
    if manifest["id"] != PACKAGE_ID:
        raise Refused("%s is not the manifest of %s" % (TEMPLATE, PACKAGE_ID))
    return archive_name(manifest["version"]), data, manifest


def built_in_keys(header=HEADER):
    """({name: the 32 bytes of each key table in agent/embedded_keys.hpp}, [what is wrong with that file]). A
    table is `const uint8_t kName[32] = { 0x.., ... };`, as that file says."""
    with open(header) as f:
        code = re.sub(r"//.*", "", f.read())
    shown = os.path.relpath(header, ROOT) if header.startswith(ROOT + os.sep) else header
    keys, problems = {}, []
    for name, body in re.findall(r"^const uint8_t (k\w+)\[32\] = \{([^}]*)\};", code, re.M):
        values = [v.strip() for v in body.split(",") if v.strip()]
        if len(values) != 32 or not all(re.fullmatch(r"0x[0-9a-fA-F]{2}", v) for v in values):
            problems.append("%s: %s is not 32 bytes written as 0x.." % (shown, name))
            continue
        keys[name] = bytes(int(v, 16) for v in values)
    if not keys:
        problems.append("%s holds no package key: an agent built from it installs nothing" % shown)
    if len(re.findall(r"\buint8_t\b[^;(]*\[", code)) != len(keys):
        problems.append("%s has a byte table that is not read as a key: see the note above its key" % shown)
    return keys, problems


def build_string(program):
    """What NBT_APP_VERSION was when nbtagent was built, out of the program's bytes; None when it is not found."""
    found = re.search(BUILD_STRING, program)
    return found.group(1).decode("ascii", "replace") if found else None


def describe():
    """What build.sh would hand a build of the commit that is checked out, as things are now."""
    done = subprocess.run(["git", "-C", ROOT, "describe", "--always", "--dirty"], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True)
    if done.returncode != 0:
        raise Refused("git describe: %s" % done.stderr.strip())
    return done.stdout.strip()


def release(programs, pkg_dir, key_path, header=HEADER, checked_out=None):
    """Signs the archive `build` left in pkg_dir. Returns (path of the signed archive, its bytes, the key)."""
    version = read_version(programs)
    name, fresh, _ = build(programs, version)
    path = os.path.join(pkg_dir, name)
    if not os.path.isfile(path):
        raise Refused("%s is not there: make agent-pkg" % path)
    with open(path, "rb") as f:
        data = f.read()
    if data != fresh:
        raise Refused("%s is not the package of the programs in %s: make agent-pkg" % (path, programs))

    with open(os.path.join(programs, "nbtagent"), "rb") as f:
        built = build_string(f.read())
    return sign_release(name, data, built, "nbtagent", key_path, pkg_dir, header, checked_out, "agent/version.hpp")


def sign_release(name, data, built, program, key_path, pkg_dir, header=HEADER, checked_out=None,
                 version_file="agent/version.hpp"):
    """Signs `data`, the archive `name` that is not signed, for the catalogue: to pkg_dir/signed/. `built` is what
    `program`, which the archive holds, says it was built from. Returns (path of the signed archive, its bytes,
    the key). For the agent's own package and for the iDrive plugin's (tools/plugin_pkg.py), which are released
    by the same rules; `version_file` is where that package's version is raised."""
    now = checked_out if checked_out is not None else describe()
    if built is None:
        raise Refused("%s does not say which commit it was built from" % program)
    if built.endswith("-dirty") or built == "unknown":
        raise Refused("%s was built from a checkout with changes that are not committed (build %s): "
                      "commit, and build again" % (program, built))
    if built != now:
        raise Refused("%s was built from %s, and what is checked out is %s: build again" % (program, built, now))

    keys, problems = built_in_keys(header)
    if problems:
        raise Refused("; ".join(problems))
    seed = nbtpkg.read_key(key_path)
    public = nbtpkg.public_key(seed)
    if public not in keys.values():
        raise Refused("%s is not the private half of a key built into the agent (%s): no agent would take what "
                      "it signs" % (key_path, ", ".join(nbtpkg.fingerprint(k) for k in keys.values())))
    signed = nbtpkg.sign_package(data, seed)
    try:
        nbtpkg.verify_package(signed, list(keys.values()))
    except nbtpkg.PkgError as e:
        raise Refused("the signed archive does not verify: %s" % e)

    out = os.path.join(pkg_dir, "signed", name)
    if os.path.isfile(out):
        with open(out, "rb") as f:
            if f.read() != signed:
                raise Refused("%s is there already, and is another archive: a version that may have been "
                              "published is never changed. Raise the version in %s" % (out, version_file))
    else:
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(signed)
    return out, signed, public


def main(argv=None):
    parser = argparse.ArgumentParser(description="The agent's own package: built, and signed for a release.")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("build", help="the two programs, the launcher, the starter and the first install's files -> "
                                     "org.nbtapp.agent-<version>.nbtpkg, not signed")
    p.add_argument("programs", nargs="?", default=RELEASE, help="the folder with nbtagent and nbtctl")
    p.add_argument("--version", help="the package's version; default: the line in PROGRAMS/version")
    p.add_argument("-o", "--out", default=PKG_DIR, help="folder for the archive")
    p = sub.add_parser("release", help="sign the archive `build` made, for the catalogue")
    p.add_argument("programs", nargs="?", default=RELEASE, help="the folder the archive was built from")
    p.add_argument("--pkg", default=PKG_DIR, help="the folder `build` wrote to")
    p.add_argument("--key", required=True, help="the private half of the package key")
    args = parser.parse_args(argv)
    try:
        if args.command == "build":
            name, data, _ = build(args.programs, args.version or read_version(args.programs))
            os.makedirs(args.out, exist_ok=True)
            path = os.path.join(args.out, name)
            with open(path, "wb") as f:
                f.write(data)
            print("%s  %d bytes, NOT signed" % (os.path.relpath(path), len(data)))
        else:
            path, signed, public = release(args.programs, args.pkg, args.key)
            print("%s  %d bytes, signed, key %s" % (os.path.relpath(path), len(signed), nbtpkg.fingerprint(public)))
            print("sha256  %s" % hashlib.sha256(signed).hexdigest())
    except Refused as e:
        print("agent_pkg: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
