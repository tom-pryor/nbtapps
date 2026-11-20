#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""The SSH package, org.nbtapp.ssh: Dropbear's server, OpenSSH's sftp-server and the relay that starts a server for
each connection on the channel "ssh" (ssh/README, README "The SSH package").

  ssh_pkg.py build [PROGRAMS] [--src SRC] [--version V] [-o OUTDIR]      `make ssh-pkg`
  ssh_pkg.py release --key KEY [PROGRAMS] [--src SRC] [--pkg DIR]        `make ssh-release KEY=FILE`

`build` makes OUTDIR/org.nbtapp.ssh-<version>.nbtpkg, which is not signed. PROGRAMS (default build/ssh/qnx) is
where `make ssh` and `make ssh-relay` leave the head unit's programs: dropbearmulti, sftp-server and sshrelay, and
the file `sources`, which says what the first two were built from. SRC (default build/ssh/src) is the export of
the sources that `make ssh-src` made, with its own `sources`; the licence texts are taken from it, and the two
files have to agree on the sources. The package is ssh/package/manifest.json.in with the version filled in, the
three programs as payload/bin/, which the manifest places in ${app}/bin/ (with ${app}/bin/scp a link to
dropbearmulti), and the licence texts as payload/licenses/, placed in ${app}/licenses/ with a README that says
which program each belongs to. The version is --version, or the one ssh/relay/version.hpp names. The same
programs, sources and version give the same bytes. OUTDIR is release/pkg unless -o names another.

The licence texts: for dropbearmulti, Dropbear's LICENSE, LibTomCrypt's and LibTomMath's, and the notices at the
head of the source files listed in DROPBEAR_NOTICES; for sftp-server, OpenSSH's LICENCE and the notices of the
files in OPENSSH_NOTICES. Those are the files compiled and linked into the head unit's build whose holders or terms
the licence file does not give in full (ssh/README, "Licences to ship with the programs"). A licence file is copied
byte for byte. A notice is the comments at the head of its file, up to its first line of code, as the file has them;
a file of the list that is not UTF-8, or has no notice there, is refused.

`release` signs that archive for the catalogue: DIR/org.nbtapp.ssh-<version>.nbtpkg (DIR is release/pkg) gives
DIR/signed/org.nbtapp.ssh-<version>.nbtpkg, and the SHA-256 the catalogue records is printed. It refuses what
tools/plugin_pkg.py refuses of the plugin's package, and for the same reasons:

  - an archive that is not what `build` makes of PROGRAMS and SRC now
  - a relay that does not say it is the version ssh/relay/version.hpp names, or that was built from another
    commit than the one checked out, or from a checkout with changes that are not committed: the relay holds the
    version and what `git describe --always --dirty` said when it was built (ssh/relay/version.hpp)
  - a key whose public half is not built into the agent (agent/embedded_keys.hpp): an agent takes an
    org.nbtapp. id from those keys only
  - a signed archive of that version that is there already with other bytes: a published version is never
    changed, and a new archive needs a new version
and, for the two third-party programs, which hold no version or build of ours:
  - programs built from other sources than the checkout's. What ties them to the checkout is what they are built
    from: the commits the submodules ssh/dropbear and ssh/openssh are pinned at, exported by `make ssh-src` (never
    the submodules' working trees, which no build reads), ssh/dropbear-qnx.patch, ssh/localoptions.h and
    ssh/build-qnx.sh, in the pinned SDP image. The same of those give the same bytes in any checkout (ssh/README,
    "Provenance"). `make ssh` records them in PROGRAMS/sources (each pin, and the SHA-256 of each file), and
    `release` holds that record to the checkout as it is now, whose commit is the relay's.

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
import agent_pkg  # noqa: E402
import nbtpkg  # noqa: E402
from agent_pkg import Refused  # noqa: E402

PACKAGE_ID = "org.nbtapp.ssh"
RELAY = "sshrelay"
PROGRAMS = ("dropbearmulti", "sftp-server", RELAY)
TEMPLATE = os.path.join(ROOT, "ssh", "package", "manifest.json.in")
VERSION_HEADER = os.path.join(ROOT, "ssh", "relay", "version.hpp")
BUILT = os.path.join(ROOT, "build", "ssh", "qnx")
SOURCES = os.path.join(ROOT, "build", "ssh", "src")
PKG_DIR = os.path.join(ROOT, "release", "pkg")
VERSION_TEXT = agent_pkg.VERSION_TEXT
# The line the relay prints for --version and logs first: its version, and the commit it was built from.
BUILD_LINE = rb"sshrelay ([0-9A-Za-z.+-]{1,64}) \(build ([^)%]{1,80})\)"
# What `make ssh-src` and `make ssh` record: each submodule's pinned commit, then the SHA-256 of each file of ours
# that goes into the build, in the form sha256sum prints. SRC/sources has the first four lines, PROGRAMS/sources all.
SUBMODULES = ("ssh/dropbear", "ssh/openssh")
SOURCE_FILES = ("ssh/dropbear-qnx.patch", "ssh/localoptions.h")
BUILD_FILES = ("ssh/build-qnx.sh",)
STAMP = "sources"

# The licence texts, as (name in payload/licenses/, path in SRC), and the files whose notices are shipped as well.
DROPBEAR_LICENCES = (("dropbear-LICENSE", "dropbear/LICENSE"),
                     ("libtomcrypt-LICENSE", "dropbear/libtomcrypt/LICENSE"),
                     ("libtommath-LICENSE", "dropbear/libtommath/LICENSE"))
DROPBEAR_NOTICES = ("src/atomicio.c", "src/chachapoly.c", "src/compat.c", "src/libcrux_mlkem768_sha3.h",
                    "src/loginrec.c", "src/mlkem768.c", "src/scp.c", "src/scpmisc.c", "src/sshpty.c",
                    "src/svr-authpubkey.c", "src/svr-authpubkeyoptions.c")
OPENSSH_LICENCES = (("openssh-LICENCE", "openssh/LICENCE"),)
OPENSSH_NOTICES = ("atomicio.c", "misc.c", "openbsd-compat/fmt_scaled.c", "openbsd-compat/freezero.c",
                   "openbsd-compat/memmem.c", "openbsd-compat/reallocarray.c", "openbsd-compat/recallocarray.c",
                   "openbsd-compat/strtonum.c", "platform-pledge.c")


def archive_name(version):
    return "%s-%s.nbtpkg" % (PACKAGE_ID, version)


def source_version(header=VERSION_HEADER):
    """The version ssh/relay/version.hpp names."""
    with open(header) as f:
        found = re.findall(r'^#define NBT_SSH_VERSION "(.*)"$', f.read(), re.M)
    shown = os.path.relpath(header, ROOT) if header.startswith(ROOT + os.sep) else header
    if len(found) != 1 or not re.fullmatch(VERSION_TEXT, found[0]):
        raise Refused("%s does not name the package's version on one line: #define NBT_SSH_VERSION \"x.y.z\"" % shown)
    return found[0]


def manifest_text(version):
    """The package's manifest.json as the source has it: the template, with the version filled in."""
    if not re.fullmatch(VERSION_TEXT, version):
        raise Refused("%r is not a version" % version)
    with open(TEMPLATE) as f:
        template = f.read()
    if template.count("@VERSION@") != 1:
        raise Refused("%s does not have @VERSION@ exactly once" % TEMPLATE)
    return template.replace("@VERSION@", version)


def read_bytes(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        raise Refused("%s is not there: make ssh-src" % path)


def read_text(path):
    """A source file's text. One that is not UTF-8 is refused: a notice is never shipped other than as it is."""
    try:
        return read_bytes(path).decode("utf-8")
    except UnicodeDecodeError:
        raise Refused("%s is not UTF-8: tools/ssh_pkg.py would not ship its notice as it is" % path)


def head_notice(text):
    """The comments at the head of a C source file, up to its first line of code: where its notice is."""
    kept, inside = [], False
    for line in text.splitlines():
        stripped = line.strip()
        if inside:
            kept.append(line)
            inside = "*/" not in stripped
        elif stripped == "" or stripped.startswith("//"):
            kept.append(line)
        elif stripped.startswith("/*"):
            kept.append(line)
            inside = "*/" not in stripped[2:]
        else:
            break
    return "\n".join(kept).strip("\n") + "\n"


def define(path, name):
    """The string a `#define NAME "..."` line of a header in SRC gives."""
    found = re.search(r'^#define[ \t]+%s[ \t]+"([^"]*)"' % name, read_text(path), re.M)
    if not found:
        raise Refused("%s does not define %s" % (path, name))
    return found.group(1)


def notices(src, base, files, what):
    """One text of the notices at the head of `files` of SRC/base, each under a line that names its file."""
    parts = ["The notices at the head of these source files of %s, as each file has them: files compiled into the\n"
             "head unit's build whose holders or terms the licence file beside this one does not give in full.\n" % what]
    for name in files:
        notice = head_notice(read_text(os.path.join(src, base, name)))
        if not re.search(r"copyright|public domain", notice, re.I):
            raise Refused("%s has no notice at its head: the list in tools/ssh_pkg.py is not of this source" % os.path.join(base, name))
        parts.append("==== %s\n\n%s" % (name, notice))
    return "\n".join(parts)


def licence_files(src):
    """{name in payload/licenses/: its bytes, or its text} of the export in SRC. A licence file is copied byte for
    byte."""
    dropbear = "Dropbear " + define(os.path.join(src, "dropbear", "src", "sysoptions.h"), "DROPBEAR_VERSION")
    openssh = (define(os.path.join(src, "openssh", "version.h"), "SSH_VERSION").replace("_", " ") +
               define(os.path.join(src, "openssh", "version.h"), "SSH_PORTABLE"))
    files = {}
    for name, path in DROPBEAR_LICENCES + OPENSSH_LICENCES:
        files[name] = read_bytes(os.path.join(src, path))
    files["dropbear-notices"] = notices(src, "dropbear", DROPBEAR_NOTICES, dropbear)
    files["openssh-notices"] = notices(src, "openssh", OPENSSH_NOTICES, openssh)
    files["README"] = (
        "The licences of the third-party programs in ../bin/, as their sources give them.\n"
        "\n"
        "dropbearmulti: %s, its server, its key tool and scp (bin/scp is a link to it)\n"
        "  dropbear-LICENSE     Dropbear's licence: MIT, and the notices of the parts from OpenSSH, PuTTY and TweetNaCl\n"
        "  libtomcrypt-LICENSE  LibTomCrypt's, which Dropbear brings with it and is built with\n"
        "  libtommath-LICENSE   LibTomMath's, the same\n"
        "  dropbear-notices     the notices of its source files that dropbear-LICENSE does not give in full\n"
        "\n"
        "sftp-server: %s (portable), its SFTP server\n"
        "  openssh-LICENCE      OpenSSH's licence, which collects the notices of its files\n"
        "  openssh-notices      the notices of its source files that openssh-LICENCE does not name\n"
        "\n"
        "sshrelay is NBTApps' own, under the PolyForm Strict License 1.0.0.\n" % (dropbear, openssh))
    return files


def read_stamp(folder, builder):
    path = os.path.join(folder, STAMP)
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        raise Refused("%s is not there: %s" % (path, builder))


def stamp_now():
    """What `make ssh` would record of the checkout as it is now: PROGRAMS/sources."""
    lines = []
    for path in SUBMODULES:
        done = subprocess.run(["git", "-C", ROOT, "rev-parse", ":./" + path], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, universal_newlines=True)
        if done.returncode != 0:
            raise Refused("git rev-parse :./%s: %s" % (path, done.stderr.strip()))
        lines.append("%s  %s" % (done.stdout.strip(), path))
    for path in SOURCE_FILES + BUILD_FILES:
        with open(os.path.join(ROOT, path), "rb") as f:
            lines.append("%s  %s" % (hashlib.sha256(f.read()).hexdigest(), path))
    return "\n".join(lines) + "\n"


def build(programs, src, version):
    """(file name, archive bytes, manifest) of the package made of the three programs in `programs` and the
    licence texts of the export in `src`. Not signed."""
    text = manifest_text(version)
    built, exported = read_stamp(programs, "make ssh"), read_stamp(src, "make ssh-src")
    if not built.startswith(exported) or not exported:
        raise Refused("%s/%s and %s/%s name other sources: the licence texts would not be those of the programs. "
                      "make ssh" % (programs, STAMP, src, STAMP))
    licences = licence_files(src)
    with tempfile.TemporaryDirectory() as stage:
        with open(os.path.join(stage, nbtpkg.MANIFEST), "w") as f:
            f.write(text)
        os.makedirs(os.path.join(stage, "payload", "bin"))
        os.makedirs(os.path.join(stage, "payload", "licenses"))
        for name in PROGRAMS:
            source = os.path.join(programs, name)
            if not os.path.isfile(source):
                raise Refused("%s is not there: %s" % (source, "./build.sh ssh" if name == RELAY else "make ssh"))
            shutil.copyfile(source, os.path.join(stage, "payload", "bin", name))
        for name, data in licences.items():
            with open(os.path.join(stage, "payload", "licenses", name), "wb") as f:
                f.write(data if isinstance(data, bytes) else data.encode("utf-8"))
        try:
            manifest, data = nbtpkg.build_package(stage)
        except nbtpkg.PkgError as e:
            raise Refused("the package is refused: %s" % e)
    if manifest["id"] != PACKAGE_ID:
        raise Refused("%s is not the manifest of %s" % (TEMPLATE, PACKAGE_ID))
    return archive_name(manifest["version"]), data, manifest


def build_line(relay):
    """(version, build) as the relay's bytes name them; None when they hold no such line."""
    found = re.search(BUILD_LINE, relay)
    return (found.group(1).decode("ascii", "replace"), found.group(2).decode("ascii", "replace")) if found else None


def release(programs, src, pkg_dir, key_path, header=agent_pkg.HEADER, checked_out=None, version=None, sources=None):
    """Signs the archive `build` left in pkg_dir. Returns (path of the signed archive, its bytes, the key).
    `version` is the package's version, which is ssh/relay/version.hpp's, `checked_out` what git describe says and
    `sources` what stamp_now() gives, unless a test says another."""
    version = source_version() if version is None else version
    name, fresh, _ = build(programs, src, version)
    path = os.path.join(pkg_dir, name)
    if not os.path.isfile(path):
        raise Refused("%s is not there: make ssh-pkg" % path)
    with open(path, "rb") as f:
        data = f.read()
    if data != fresh:
        raise Refused("%s is not the package of the programs in %s: make ssh-pkg" % (path, programs))
    with open(os.path.join(programs, RELAY), "rb") as f:
        said = build_line(f.read())
    if said is None:
        raise Refused("%s does not say which version it is and which commit it was built from" % RELAY)
    if said[0] != version:
        raise Refused("%s says it is version %s, and the package is version %s: build again" % (RELAY, said[0], version))
    now = stamp_now() if sources is None else sources
    if read_stamp(programs, "make ssh") != now:
        raise Refused("dropbearmulti and sftp-server were built from other sources than the checkout's (%s/%s against "
                      "the pins of %s and %s): make ssh" % (programs, STAMP, ", ".join(SUBMODULES),
                                                           ", ".join(SOURCE_FILES + BUILD_FILES)))
    return agent_pkg.sign_release(name, data, said[1], RELAY, key_path, pkg_dir, header, checked_out,
                                  "ssh/relay/version.hpp")


def main(argv=None):
    parser = argparse.ArgumentParser(description="The SSH package: built, and signed for a release.")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("build", help="the head unit's three programs and the licence texts -> "
                                     "org.nbtapp.ssh-<version>.nbtpkg, not signed")
    p.add_argument("programs", nargs="?", default=BUILT, help="the folder with the programs, as make ssh leaves it")
    p.add_argument("--src", default=SOURCES, help="the export of the sources, as make ssh-src leaves it")
    p.add_argument("--version", help="the package's version; default: the one ssh/relay/version.hpp names")
    p.add_argument("-o", "--out", default=PKG_DIR, help="folder for the archive")
    p = sub.add_parser("release", help="sign the archive `build` made, for the catalogue")
    p.add_argument("programs", nargs="?", default=BUILT, help="the folder the archive was built from")
    p.add_argument("--src", default=SOURCES, help="the export the licence texts were taken from")
    p.add_argument("--pkg", default=PKG_DIR, help="the folder `build` wrote to")
    p.add_argument("--key", required=True, help="the private half of the package key")
    args = parser.parse_args(argv)
    try:
        if args.command == "build":
            name, data, _ = build(args.programs, args.src, args.version or source_version())
            os.makedirs(args.out, exist_ok=True)
            path = os.path.join(args.out, name)
            with open(path, "wb") as f:
                f.write(data)
            print("%s  %d bytes, NOT signed" % (os.path.relpath(path), len(data)))
        else:
            path, signed, public = release(args.programs, args.src, args.pkg, args.key)
            print("%s  %d bytes, signed, key %s" % (os.path.relpath(path), len(signed), nbtpkg.fingerprint(public)))
            print("sha256  %s" % hashlib.sha256(signed).hexdigest())
    except Refused as e:
        print("ssh_pkg: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
