#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""`make test-plugin-pkg`: host test of tools/plugin_pkg.py, which builds the iDrive plugin's own package and
signs it for a release. Needs python3 with the `cryptography` module. Exits non-zero on the first failure.

Everything here is a throwaway folder: a plugin's folder whose files are a few bytes, keys made for the run, and a
header that has such a key in the form agent/embedded_keys.hpp keeps. No key of this repo or of the catalogue is
read.

  1. build: the archive is the template with the version filled in and the plugin's five files as payload/NbtApp/,
     verifies as a package that is not signed, and is the same bytes every time; the manifest places that folder
     as ${plugins}/NbtApp/ and asks for an HMI restart; what it refuses
  2. release: signs with a key the header holds, to signed/, and the result verifies under that key; what it
     refuses, each by itself
  3. this repo's src/version.hpp names a version, and src/nbt_app.cpp has the line the library is asked for
"""
import json
import os
import re
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import agent_pkg  # noqa: E402
import nbtpkg  # noqa: E402
import plugin_pkg  # noqa: E402

checks = 0


def check(condition, what, detail=""):
    global checks
    checks += 1
    if not condition:
        print("FAIL  %s%s" % (what, "\n" + str(detail) if detail != "" else ""))
        sys.exit(1)


def refused(what, why, function, *args, **kwargs):
    try:
        function(*args, **kwargs)
    except plugin_pkg.Refused as e:
        check(why in str(e), what, e)
        return
    check(False, what, "it was not refused")


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def header(path, public):
    table = ", ".join("0x%02x" % b for b in public)
    put(path, ("const uint8_t kThrowaway[32] = {\n    %s,\n};\n" % table).encode())


def plugin(folder, build="abc1234", version="1.2.3", line=True):
    """A plugin's folder: the five files, with a library that says which version it is and what it was built from."""
    said = ("NBTApp plugin %s (build %s)" % (version, build)).encode() if line else b"a library that says nothing"
    put(os.path.join(folder, plugin_pkg.LIB), b"\x7fELF plugin \0" + said + b"\0")
    for name in plugin_pkg.FILES[1:]:
        put(os.path.join(folder, name), ("stands in for %s\n" % name).encode())


def main():
    work = tempfile.TemporaryDirectory()
    top = work.name

    # ---- 1. build
    src = os.path.join(top, "NbtApp")
    plugin(src)
    name, data, manifest = plugin_pkg.build(src, "1.2.3")
    check(name == "org.nbtapp.plugin-1.2.3.nbtpkg", "the archive's name", name)
    info = nbtpkg.verify_package(data, None, allow_unsigned=True)
    check(info["id"] == "org.nbtapp.plugin" and info["version"] == "1.2.3" and "signature" not in info,
          "the archive is the plugin's package, version 1.2.3, not signed", info)
    source = json.loads(plugin_pkg.manifest_text("1.2.3"))
    check({k: v for k, v in manifest.items() if k != "contents"} == source, "its manifest is the template's", manifest)
    files = {e["name"]: e["data"] for e in nbtpkg.read_zip(data) if not e["dir"]}
    check(sorted(files) == sorted(["manifest.json"] + ["payload/NbtApp/" + f for f in plugin_pkg.FILES]), "its files", sorted(files))
    for wanted in plugin_pkg.FILES:
        with open(os.path.join(src, wanted), "rb") as f:
            check(files["payload/NbtApp/" + wanted] == f.read(), "payload/NbtApp/%s is the folder's" % wanted)
    install = manifest["install"]
    check(install["files"] == [{"src": "payload/NbtApp/", "dest": "${plugins}/NbtApp/", "mode": "0644"}] and
          install["reboot"] == "hmi-restart" and set(install) == {"files", "reboot"},
          "it places the folder as ${plugins}/NbtApp/, asks for an HMI restart, and does nothing else", install)
    check("agent" in manifest["requires"], "and names the agent it requires", manifest["requires"])
    check("needs-reboot" in info["flags"], "which the catalogue shows as a package that needs a restart", info["flags"])
    check(plugin_pkg.build(src, "1.2.3")[1] == data, "the same folder gives the same bytes")
    check(plugin_pkg.build(src, "1.2.4")[0] == "org.nbtapp.plugin-1.2.4.nbtpkg", "--version names another version")

    refused("a version that is not one", "the package is refused", plugin_pkg.build, src, "1.2")
    refused("a version that cannot stand in the template", "is not a version", plugin_pkg.build, src, '1.0.0", "x": "')
    refused("no folder", "is not there", plugin_pkg.build, os.path.join(top, "nowhere"), "1.2.3")
    lacking = os.path.join(top, "lacking")
    plugin(lacking)
    os.remove(os.path.join(lacking, "NbtApp.xml"))
    refused("a folder without the UI description", "lacks NbtApp.xml", plugin_pkg.build, lacking, "1.2.3")
    more = os.path.join(top, "more")
    plugin(more)
    put(os.path.join(more, "libNbtApp.so.old"), b"left from a build before")
    refused("a folder with a file more", "which is not the plugin's", plugin_pkg.build, more, "1.2.3")

    # ---- 2. release
    seed, other = os.urandom(32), os.urandom(32)
    key, other_key = os.path.join(top, "keys", "one.key"), os.path.join(top, "keys", "other.key")
    os.makedirs(os.path.dirname(key))
    nbtpkg.write_key(key, seed, "a test's key", True)
    nbtpkg.write_key(other_key, other, "a test's other key", True)
    built_in = os.path.join(top, "embedded_keys.hpp")
    header(built_in, nbtpkg.public_key(seed))
    pkg = os.path.join(top, "pkg")

    def release(folder=src, key_file=key, checked_out="abc1234", version="1.2.3"):
        return plugin_pkg.release(folder, pkg, key_file, header=built_in, checked_out=checked_out, version=version)

    refused("no archive", "make plugin-pkg", release)
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
    refused("a signed archive of other bytes is there", "Raise the version in src/version.hpp", release)
    put(path, signed)
    refused("a library of another commit", "libNbtApp.so was built from abc1234, and what is checked out is def5678", release,
            checked_out="def5678")
    refused("a library of another version than the package", "says it is version 1.2.3, and the package is version 1.2.4",
            lambda: (put(os.path.join(pkg, "org.nbtapp.plugin-1.2.4.nbtpkg"), plugin_pkg.build(src, "1.2.4")[1]),
                     release(version="1.2.4")))

    for label, build, why in (("dirty", "abc1234-dirty", "changes that are not committed"),
                              ("unknown", "unknown", "changes that are not committed")):
        folder = os.path.join(top, label)
        plugin(folder, build=build)
        put(os.path.join(pkg, name), plugin_pkg.build(folder, "1.2.3")[1])
        refused("a library built from a %s checkout" % label, why, release, folder=folder, checked_out=build)
    silent = os.path.join(top, "silent")
    plugin(silent, line=False)
    put(os.path.join(pkg, name), plugin_pkg.build(silent, "1.2.3")[1])
    refused("a library that names no version and no build", "does not say which version it is", release, folder=silent)
    refused("an archive of another folder", "is not the package of the plugin", release)

    # ---- 3. this repo's sources
    version = plugin_pkg.source_version()
    check(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is not None, "src/version.hpp names a version", version)
    with open(os.path.join(ROOT, "src", "nbt_app.cpp")) as f:
        line = re.search(r'^const char kBuildLine\[\] = (.*);$', f.read(), re.M)
    check(line is not None and line.group(1) == '"NBTApp plugin " NBT_PLUGIN_VERSION " (build " NBT_APP_VERSION ")"',
          "src/nbt_app.cpp has the line the library is asked for, in the form tools/plugin_pkg.py reads", line and line.group(1))
    said = plugin_pkg.build_line(("NBTApp plugin %s (build 1a2b3c4)" % version).encode())
    check(said == (version, "1a2b3c4"), "and that form is read back", said)
    bad = os.path.join(top, "version.hpp")
    put(bad, b'#define NBT_PLUGIN_VERSION "1.0"\n#define NBT_PLUGIN_VERSION "1.1"\n')
    refused("a header that names two versions", "does not name the plugin's version", plugin_pkg.source_version, bad)
    refused("the agent's signing, for a program that names no build", "does not say which commit",
            agent_pkg.sign_release, name, data, None, "libNbtApp.so", key, pkg, built_in, "abc1234")

    print("test_plugin_pkg: ok (%d checks)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
