#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""The iDrive plugin's own package, org.nbtapp.plugin: the archive an agent on a head unit installs and updates
the plugin with, as it does any package (README "The plugin's package").

  plugin_pkg.py build [FOLDER] [--version V] [-o OUTDIR]        `make plugin-pkg`
  plugin_pkg.py release --key KEY [FOLDER] [--pkg DIR]          `make plugin-release KEY=FILE`

`build` makes OUTDIR/org.nbtapp.plugin-<version>.nbtpkg, which is not signed. FOLDER (default release/NbtApp) is
the plugin's folder as the build leaves it: the library, the manifest, the UI description and the two DBs, and
nothing else. The package is plugin/package/manifest.json.in with the version filled in, and that folder as
payload/NbtApp/, which the manifest places as ${plugins}/NbtApp/. The version is --version, or the one
src/version.hpp names. The same folder and version give the same bytes. OUTDIR is release/pkg unless -o names
another.

`release` signs that archive for the catalogue: DIR/org.nbtapp.plugin-<version>.nbtpkg (DIR is release/pkg) gives
DIR/signed/org.nbtapp.plugin-<version>.nbtpkg, and the SHA-256 the catalogue records is printed. It refuses what
tools/agent_pkg.py refuses of the agent's package, and for the same reasons:

  - an archive that is not what `build` makes of FOLDER now
  - a library that does not say it is the version src/version.hpp names, or that was built from another commit
    than the one checked out, or from a checkout with changes that are not committed: the library holds the
    version and what `git describe --always --dirty` said when it was built (src/nbt_app.cpp)
  - a key whose public half is not built into the agent (agent/embedded_keys.hpp): an agent takes an
    org.nbtapp. id from those keys only
  - a signed archive of that version that is there already with other bytes: a published version is never
    changed, and a new archive needs a new version

Needs python3; `release` needs the `cryptography` module and git.
"""
import argparse
import hashlib
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import agent_pkg  # noqa: E402
import make_db  # noqa: E402
import nbtpkg  # noqa: E402
from agent_pkg import Refused  # noqa: E402

PACKAGE_ID = "org.nbtapp.plugin"
NAME = "NbtApp"                    # the plugin's folder under ${plugins}, and what its files are named after
LIB = "lib%s.so" % NAME
# What the plugin's folder holds, and all it may hold: iDrive loads what it finds there.
FILES = (LIB, NAME + ".manifest", NAME + ".xml", make_db.TEXTS_ZIP, make_db.IMAGES_ZIP)
TEMPLATE = os.path.join(ROOT, "plugin", "package", "manifest.json.in")
VERSION_HEADER = os.path.join(ROOT, "src", "version.hpp")
RELEASE = os.path.join(ROOT, "release", NAME)
PKG_DIR = os.path.join(ROOT, "release", "pkg")
VERSION_TEXT = agent_pkg.VERSION_TEXT
# The line the library logs when it is loaded: its version, and the commit it was built from.
BUILD_LINE = rb"NBTApp plugin ([0-9A-Za-z.+-]{1,64}) \(build ([^)%]{1,80})\)"


def archive_name(version):
    return "%s-%s.nbtpkg" % (PACKAGE_ID, version)


def source_version(header=VERSION_HEADER):
    """The version src/version.hpp names."""
    with open(header) as f:
        found = re.findall(r'^#define NBT_PLUGIN_VERSION "(.*)"$', f.read(), re.M)
    shown = os.path.relpath(header, ROOT) if header.startswith(ROOT + os.sep) else header
    if len(found) != 1 or not re.fullmatch(VERSION_TEXT, found[0]):
        raise Refused("%s does not name the plugin's version on one line: #define NBT_PLUGIN_VERSION \"x.y.z\"" % shown)
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


def build(folder, version):
    """(file name, archive bytes, manifest) of the package made of the plugin's folder. Not signed."""
    text = manifest_text(version)
    if not os.path.isdir(folder):
        raise Refused("%s is not there: build the plugin" % folder)
    present = set(os.listdir(folder))
    lacking, more = sorted(set(FILES) - present), sorted(present - set(FILES))
    if lacking:
        raise Refused("%s lacks %s" % (folder, ", ".join(lacking)))
    if more:
        raise Refused("%s holds %s, which is not the plugin's: iDrive loads what it finds in that folder" % (folder, ", ".join(more)))
    with tempfile.TemporaryDirectory() as stage:
        with open(os.path.join(stage, nbtpkg.MANIFEST), "w") as f:
            f.write(text)
        os.makedirs(os.path.join(stage, "payload", NAME))
        for name in FILES:
            source = os.path.join(folder, name)
            if not os.path.isfile(source):
                raise Refused("%s is not a file" % source)
            shutil.copyfile(source, os.path.join(stage, "payload", NAME, name))
        try:
            manifest, data = nbtpkg.build_package(stage)
        except nbtpkg.PkgError as e:
            raise Refused("the package is refused: %s" % e)
    if manifest["id"] != PACKAGE_ID:
        raise Refused("%s is not the manifest of %s" % (TEMPLATE, PACKAGE_ID))
    return archive_name(manifest["version"]), data, manifest


def build_line(library):
    """(version, build) as the library's bytes name them; None when they hold no such line."""
    found = re.search(BUILD_LINE, library)
    return (found.group(1).decode("ascii", "replace"), found.group(2).decode("ascii", "replace")) if found else None


def release(folder, pkg_dir, key_path, header=agent_pkg.HEADER, checked_out=None, version=None):
    """Signs the archive `build` left in pkg_dir. Returns (path of the signed archive, its bytes, the key).
    `version` is the plugin's version, which is src/version.hpp's unless a test says another."""
    version = source_version() if version is None else version
    name, fresh, _ = build(folder, version)
    path = os.path.join(pkg_dir, name)
    if not os.path.isfile(path):
        raise Refused("%s is not there: make plugin-pkg" % path)
    with open(path, "rb") as f:
        data = f.read()
    if data != fresh:
        raise Refused("%s is not the package of the plugin in %s: make plugin-pkg" % (path, folder))
    with open(os.path.join(folder, LIB), "rb") as f:
        said = build_line(f.read())
    if said is None:
        raise Refused("%s does not say which version it is and which commit it was built from" % LIB)
    if said[0] != version:
        raise Refused("%s says it is version %s, and the package is version %s: build again" % (LIB, said[0], version))
    return agent_pkg.sign_release(name, data, said[1], LIB, key_path, pkg_dir, header, checked_out, "src/version.hpp")


def main(argv=None):
    parser = argparse.ArgumentParser(description="The iDrive plugin's own package: built, and signed for a release.")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("build", help="the plugin's folder -> org.nbtapp.plugin-<version>.nbtpkg, not signed")
    p.add_argument("folder", nargs="?", default=RELEASE, help="the plugin's folder, as the build leaves it")
    p.add_argument("--version", help="the package's version; default: the one src/version.hpp names")
    p.add_argument("-o", "--out", default=PKG_DIR, help="folder for the archive")
    p = sub.add_parser("release", help="sign the archive `build` made, for the catalogue")
    p.add_argument("folder", nargs="?", default=RELEASE, help="the folder the archive was built from")
    p.add_argument("--pkg", default=PKG_DIR, help="the folder `build` wrote to")
    p.add_argument("--key", required=True, help="the private half of the package key")
    args = parser.parse_args(argv)
    try:
        if args.command == "build":
            name, data, _ = build(args.folder, args.version or source_version())
            os.makedirs(args.out, exist_ok=True)
            path = os.path.join(args.out, name)
            with open(path, "wb") as f:
                f.write(data)
            print("%s  %d bytes, NOT signed" % (os.path.relpath(path), len(data)))
        else:
            path, signed, public = release(args.folder, args.pkg, args.key)
            print("%s  %d bytes, signed, key %s" % (os.path.relpath(path), len(signed), nbtpkg.fingerprint(public)))
            print("sha256  %s" % hashlib.sha256(signed).hexdigest())
    except Refused as e:
        print("plugin_pkg: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
