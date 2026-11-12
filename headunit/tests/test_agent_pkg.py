#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""`make test-agent-pkg`: host test of tools/agent_pkg.py, which builds the agent's own package and signs it for a
release. Needs python3 with the `cryptography` module. Exits non-zero on the first failure.

Everything here is a throwaway folder: programs that are a few bytes, keys made for the run, and a header that
has such a key in the form agent/embedded_keys.hpp keeps. No key of this repo or of the catalogue is read.

  1. build: the archive is the template with the version filled in, the two programs, the version's launcher and
     the starter it comes with, and the rest of what a first install needs (agent/hu_install.sh and the version's
     file), verifies as a package that is not signed, and is the same bytes every time; its payload folder holds
     every file agent/hu_install.sh asks for, and the manifest places the two programs and the two scripts and
     nothing else; what it refuses
  2. release: signs with a key the header holds, to signed/, and the result verifies under that key; what it
     refuses, each by itself
  3. the header of this repo is read as one key and nothing wrong
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
    except agent_pkg.Refused as e:
        check(why in str(e), what, e)
        return
    check(False, what, "it was not refused")


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def header(path, public):
    table = ", ".join("0x%02x" % b for b in public)
    put(path, ("// a test's header: const uint8_t kInAComment[32] = { 0x00 };\n"
               "const uint8_t kThrowaway[32] = {\n    %s,\n};\n" % table).encode())


def programs(folder, build="abc1234", version="1.2.3"):
    put(os.path.join(folder, "nbtagent"), b"\x7fELF agent " + ("%s (build %s), head unit " % (version, build)).encode())
    put(os.path.join(folder, "nbtctl"), b"\x7fELF ctl")
    put(os.path.join(folder, "version"), (version + "\n").encode())


def main():
    work = tempfile.TemporaryDirectory()
    top = work.name

    # ---- 1. build
    src = os.path.join(top, "programs")
    programs(src)
    name, data, manifest = agent_pkg.build(src, agent_pkg.read_version(src))
    check(name == "org.nbtapp.agent-1.2.3.nbtpkg", "the archive's name", name)
    info = nbtpkg.verify_package(data, None, allow_unsigned=True)
    check(info["id"] == "org.nbtapp.agent" and info["version"] == "1.2.3" and "signature" not in info,
          "the archive is the agent's package, version 1.2.3, not signed", info)
    source = json.loads(agent_pkg.manifest_text("1.2.3"))
    check({k: v for k, v in manifest.items() if k != "contents"} == source, "its manifest is the template's", manifest)
    files = {e["name"]: e["data"] for e in nbtpkg.read_zip(data)}
    check(sorted(files) == ["manifest.json", "payload/hu_install.sh", "payload/nbtagent", "payload/nbtctl",
                            "payload/nbtlaunch.sh", "payload/nbtstart.sh", "payload/version"], "its files", sorted(files))
    for program in agent_pkg.PROGRAMS:
        with open(os.path.join(src, program), "rb") as f:
            check(files["payload/" + program] == f.read(), "payload/%s is the program" % program)
    for script in agent_pkg.SCRIPTS:
        with open(os.path.join(ROOT, "agent", script), "rb") as f:
            check(files["payload/" + script] == f.read(), "payload/%s is agent/'s" % script)
    check(files["payload/version"] == b"1.2.3\n", "payload/version is the version on one line", files["payload/version"])
    # The payload folder, unpacked onto a stick, is a first install: hu_install.sh finds beside itself what it
    # asks for. The names are read out of the script, so one that comes to ask for more fails here.
    with open(os.path.join(ROOT, "agent", "hu_install.sh")) as f:
        asked = re.search(r"^for name in ([^;\n]+); do$", f.read(), re.M)
    check(asked is not None, "agent/hu_install.sh names the files it needs beside it on one `for name in` line")
    check(all("payload/" + wanted in files for wanted in asked.group(1).split()),
          "the payload holds every file hu_install.sh asks for", asked.group(1))
    check([f["src"] for f in manifest["install"]["files"]] == ["payload/nbtagent", "payload/nbtctl", "payload/nbtlaunch.sh",
                                                               "payload/nbtstart.sh"]
          and all(f["dest"] == "${app}/" + f["src"][len("payload/"):] and f["mode"] == "0755" for f in manifest["install"]["files"])
          and set(manifest["install"]) == {"files"},
          "an update places the two programs, the version's launcher and its starter, in the version's folder, and nothing else",
          manifest["install"])
    check(agent_pkg.build(src, "1.2.3")[1] == data, "the same programs give the same bytes")
    name_4, data_4, _ = agent_pkg.build(src, "1.2.4")
    check(name_4 == "org.nbtapp.agent-1.2.4.nbtpkg", "--version names another version")
    check({e["name"]: e["data"] for e in nbtpkg.read_zip(data_4)}["payload/version"] == b"1.2.4\n",
          "and payload/version follows it")

    refused("a version that is not one", "the package is refused", agent_pkg.build, src, "1.2")
    refused("a version that cannot stand in the template", "is not a version", agent_pkg.build, src, '1.0.0", "x": "')
    lacking = os.path.join(top, "lacking")
    programs(lacking)
    os.remove(os.path.join(lacking, "nbtctl"))
    refused("a folder without nbtctl", "nbtctl is not there", agent_pkg.build, lacking, "1.2.3")
    os.remove(os.path.join(lacking, "version"))
    refused("a folder without version", "is not there", agent_pkg.read_version, lacking)
    put(os.path.join(lacking, "version"), b"1.2.3\n1.2.4\n")
    refused("a version file of two lines", "does not hold a version", agent_pkg.read_version, lacking)

    # ---- 2. release
    seed, other = os.urandom(32), os.urandom(32)
    key, other_key = os.path.join(top, "keys", "one.key"), os.path.join(top, "keys", "other.key")
    os.makedirs(os.path.dirname(key))
    nbtpkg.write_key(key, seed, "a test's key", True)
    nbtpkg.write_key(other_key, other, "a test's other key", True)
    built_in = os.path.join(top, "embedded_keys.hpp")
    header(built_in, nbtpkg.public_key(seed))
    pkg = os.path.join(top, "pkg")

    def release(folder=src, key_file=key, checked_out="abc1234"):
        return agent_pkg.release(folder, pkg, key_file, header=built_in, checked_out=checked_out)

    refused("no archive", "make agent-pkg", release)
    put(os.path.join(pkg, name), data)
    path, signed, public = release()
    check(path == os.path.join(pkg, "signed", name) and public == nbtpkg.public_key(seed), "where the signed archive goes", path)
    with open(path, "rb") as f:
        check(f.read() == signed, "the file is the signed archive")
    info = nbtpkg.verify_package(signed, [nbtpkg.public_key(seed)])
    check(info.get("key") == nbtpkg.public_key(seed).hex() and info["version"] == "1.2.3", "it verifies under the key", info)
    check(nbtpkg.sign_package(data, seed) == signed, "and is the archive, signed, with nothing else changed")
    with open(os.path.join(pkg, name), "rb") as f:
        check(f.read() == data, "the archive that is not signed stays as it was")
    check(release()[1] == signed, "signing again gives the same archive")

    refused("a key that is not built in", "not the private half of a key built into the agent", release, key_file=other_key)
    put(path, signed[:-1] + bytes([signed[-1] ^ 1]))
    refused("a signed archive of other bytes is there", "is there already", release)
    put(path, signed)
    refused("programs of another commit", "what is checked out is def5678", release, checked_out="def5678")

    for label, build, why in (("dirty", "abc1234-dirty", "changes that are not committed"),
                              ("unknown", "unknown", "changes that are not committed")):
        folder = os.path.join(top, label)
        programs(folder, build=build)
        put(os.path.join(pkg, name), agent_pkg.build(folder, "1.2.3")[1])
        refused("programs built from a %s checkout" % label, why, release, folder=folder, checked_out=build)
    silent = os.path.join(top, "silent")
    programs(silent)
    put(os.path.join(silent, "nbtagent"), b"\x7fELF agent that names no build")
    put(os.path.join(pkg, name), agent_pkg.build(silent, "1.2.3")[1])
    refused("an nbtagent that names no build", "does not say which commit", release, folder=silent)
    refused("an archive of other programs", "is not the package of the programs", release)

    empty = os.path.join(top, "empty.hpp")
    put(empty, b"// no key\n")
    put(os.path.join(pkg, name), data)
    refused("a header with no key", "holds no package key", agent_pkg.release, src, pkg, key, header=empty, checked_out="abc1234")
    odd = os.path.join(top, "odd.hpp")
    put(odd, b"const uint8_t kShort[32] = { 0x01, 0x02 };\nstatic const uint8_t kOther[4] = {1, 2, 3, 4};\n")
    keys, problems = agent_pkg.built_in_keys(odd)
    check(keys == {} and len(problems) == 3, "a table of two bytes and one of another form are both named", problems)

    # ---- 3. this repo's header
    keys, problems = agent_pkg.built_in_keys()
    check(problems == [] and len(keys) == 1 and all(len(k) == 32 for k in keys.values()),
          "agent/embedded_keys.hpp is one key", (keys, problems))

    print("test_agent_pkg: ok (%d checks)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
