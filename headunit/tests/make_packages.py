#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""Builds the test packages: `make testdata`, or python3 tests/make_packages.py [OUTDIR] (default build/testdata).

  OUTDIR/good/*.nbtpkg   the sources in tests/packages, signed with tests/keys/test-package.key, plus variants a
                         reader must also accept
  OUTDIR/bad/*.nbtpkg    hello 1.2.0 with one defect each; every reader must refuse each of them
  OUTDIR/vectors.json    what each file is: for a good one the values an index entry or a job.plan offer carries,
                         for a bad one the code the refusal has (bad-archive, bad-signature, bad-manifest or
                         unsupported-manifest) and what is wrong with it

The same sources give the same files, byte for byte. Every implementation that reads packages runs against this
set: tools/nbtpkg.py here (tests/test_nbtpkg.py), the agent's reader and the Android app's.
"""
import json
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import tempfile

import nbtpkg
import plugin_pkg
from nbtpkg import MANIFEST, SIGNATURE, make_member, write_zip

SOURCES = os.path.join(ROOT, "tests", "packages")
KEYS = os.path.join(ROOT, "tests", "keys")
BASE = "hello-1.2.0"   # the package every bad one is derived from


def members_of(data):
    return [{"name": e["name_bytes"], "method": e["method"], "crc": e["crc"], "usize": e["usize"], "raw": e["raw"],
             "flags": e["flags"]} for e in nbtpkg.read_zip(data)]


def member(members, name):
    return next(m for m in members if m["name"] == name.encode())


def crc(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def swap_directory_records(data, a, b):
    """The same archive with directory records `a` and `b` swapped: the directory then disagrees with file order."""
    _sig, _d, _dd, _n, count, size, offset, _c = struct.unpack_from("<IHHHHIIH", data, len(data) - 22)
    records, at = [], offset
    for _ in range(count):
        name_len, extra_len, note_len = struct.unpack_from("<HHH", data, at + 28)
        end = at + 46 + name_len + extra_len + note_len
        records.append(data[at:end])
        at = end
    records[a], records[b] = records[b], records[a]
    return data[:offset] + b"".join(records) + data[offset + size:]


def own_plugin(version, seed):
    """org.nbtapp.plugin as tools/plugin_pkg.py makes it for a head unit, signed with the test key: of the
    plugin's manifest and UI description as they are in plugin/, and stand-ins for the library and the two DBs
    that say which version they are of."""
    with tempfile.TemporaryDirectory() as folder:
        for name in plugin_pkg.FILES:
            source = os.path.join(ROOT, "plugin", name)
            if os.path.isfile(source):
                with open(source, "rb") as f:
                    data = f.read()
            else:
                data = ("Test package: stands in for %s of version %s of the plugin.\n" % (name, version)).encode()
            with open(os.path.join(folder, name), "wb") as f:
                f.write(data)
        _, data, _ = plugin_pkg.build(folder, version)
    return nbtpkg.sign_package(data, seed)


def build_all(out_dir):
    seed = nbtpkg.read_key(os.path.join(KEYS, "test-package.key"))
    other_seed = nbtpkg.read_key(os.path.join(KEYS, "test-other.key"))
    public = nbtpkg.public_key(seed)
    good, bad = {}, {}

    for source in sorted(os.listdir(SOURCES)):
        if os.path.isdir(os.path.join(SOURCES, source)):
            manifest, data = nbtpkg.build_package(os.path.join(SOURCES, source), seed)
            good["%s-%s" % (manifest["id"], manifest["version"])] = (data, "built from tests/packages/" + source)
            if source == BASE:
                base, base_manifest = data, manifest

    # The iDrive plugin's own package, in three versions: an install, an upgrade over it, and one from before
    # the versions that an agent takes for a plugin which starts it with the head unit.
    for version in ("0.9.0", "1.0.0", "1.1.0"):
        good["org.nbtapp.plugin-" + version] = (own_plugin(version, seed), "made by tools/plugin_pkg.py of plugin/'s "
                                                "manifest and UI description, with stand-ins for the library and the DBs")

    base_members = members_of(base)
    manifest_bytes = nbtpkg.read_zip(base)[0]["data"]
    payload = base_members[2:]

    def with_manifest(manifest_data, files=None, signer=seed):
        """The base package with other manifest bytes, signed properly, so only the manifest is at fault."""
        return write_zip([make_member(MANIFEST, manifest_data),
                          make_member(SIGNATURE, nbtpkg.sign(signer, manifest_data), store=True)]
                         + (payload if files is None else files))

    def with_changed_manifest(change, files=None):
        manifest = json.loads(manifest_bytes)
        change(manifest)
        return with_manifest((json.dumps(manifest, indent=2) + "\n").encode(), files)

    def with_members(change):
        """The base package with its member list bent by `change`, which edits or returns the list."""
        members = [dict(m) for m in base_members]
        return write_zip(change(members) or members)

    def with_text(old, new):
        assert manifest_bytes.count(old) == 1, old
        return with_manifest(manifest_bytes.replace(old, new))

    # ---- good variants: things a reader must not trip over ----
    def unknown_key(m):
        m["futureField"] = {"ignored": [1, 2, 3]}
        m["contents"][0]["note"] = "ignored too"
    good["unknown-keys"] = (with_changed_manifest(unknown_key), "unknown keys outside requires and install are ignored")
    folder = {"name": b"payload/", "method": 0, "crc": 0, "usize": 0, "raw": b""}
    good["folder-entries"] = (with_members(lambda ms: ms.insert(2, folder)), "folder entries are ignored")
    stored = [make_member(m["name"].decode(), nbtpkg.read_zip(base)[i + 2]["data"], store=True)
              for i, m in enumerate(payload)]
    good["all-stored"] = (with_manifest(manifest_bytes, stored), "every payload file stored instead of deflated")

    # ---- signature ----
    def flip_signature(ms):
        sig = bytearray(ms[1]["raw"])
        sig[10] ^= 0x01
        ms[1].update(raw=bytes(sig), crc=crc(bytes(sig)))
    bad["signature-flipped"] = (with_members(flip_signature), "bad-signature", "one bit of manifest.sig is changed")
    bad["signature-other-key"] = (with_manifest(manifest_bytes, signer=other_seed), "bad-signature",
                                  "signed with a key the reader does not trust")
    # The agent's own package from the second key: an agent that has both keys built in takes it as an update of
    # a version the first key signed.
    bad["agent-other-key"] = (nbtpkg.build_package(os.path.join(SOURCES, "agent-1.2.0"), other_seed)[1], "bad-signature",
                              "the agent's own package, signed with a key the reader does not trust")
    bad["unsigned"] = (with_members(lambda ms: ms.pop(1) and None), "bad-signature", "there is no manifest.sig")
    def short_signature(ms):
        ms[1].update(raw=ms[1]["raw"][:63], usize=63, crc=crc(ms[1]["raw"][:63]))
    bad["signature-short"] = (with_members(short_signature), "bad-signature", "manifest.sig is 63 bytes")
    def changed_manifest_byte(ms):
        text = manifest_bytes.replace(b'"Hello NBT"', b'"Hello NBS"')
        ms[0] = make_member(MANIFEST, text)
    bad["manifest-changed-after-signing"] = (with_members(changed_manifest_byte), "bad-signature",
                                             "one character of the manifest changed after it was signed")

    # ---- container ----
    bad["signature-last"] = (with_members(lambda ms: ms.append(ms.pop(1))), "bad-archive",
                             "manifest.sig is the last entry, not the second")
    bad["manifest-second"] = (with_members(lambda ms: ms.insert(0, ms.pop(1))), "bad-archive",
                              "manifest.sig comes before manifest.json")
    def change_payload(ms):
        m = member(ms, "scripts/post-remove.sh")
        assert m["method"] == 0
        data = m["raw"].replace(b"done", b"gone")
        m.update(raw=data, crc=crc(data))
    bad["file-changed"] = (with_members(change_payload), "bad-archive",
                           "a file differs from its hash in the manifest; its zip CRC is right")
    extra = make_member("payload/extra.txt", b"not in the manifest\n")
    bad["file-unlisted"] = (with_members(lambda ms: ms.append(extra)), "bad-archive", "a file the manifest does not list")
    bad["file-missing"] = (with_members(lambda ms: ms.pop() and None), "bad-archive", "a file the manifest lists is absent")
    bad["name-twice"] = (with_members(lambda ms: ms.append(dict(ms[-1]))), "bad-archive", "the same name twice")
    def descriptor(ms):
        m = ms[2]
        m["flags"] = m["flags"] | 0x0008
        m["local"] = {"crc": 0, "csize": 0, "usize": 0}
        m["descriptor"] = struct.pack("<IIII", 0x08074B50, m["crc"], len(m["raw"]), m["usize"])
    bad["data-descriptor"] = (with_members(descriptor), "bad-archive",
                              "an entry with a data descriptor and no sizes in its local header")
    def local_size(ms):
        ms[2]["local"] = {"usize": ms[2]["usize"] + 1}
    bad["local-size-differs"] = (with_members(local_size), "bad-archive", "local header and directory give different sizes")
    def local_name(ms):
        ms[2]["local"] = {"name": ms[2]["name"][:-1] + b"X"}
    bad["local-name-differs"] = (with_members(local_name), "bad-archive", "local header and directory give different names")
    def zip64(ms):
        ms[2]["central"] = {"csize": 0xFFFFFFFF, "usize": 0xFFFFFFFF}
    bad["zip64-sizes"] = (with_members(zip64), "bad-archive", "directory sizes hold the zip64 marker")
    bad["zip64-count"] = (write_zip([dict(m) for m in base_members], end={"count": 0xFFFF}), "bad-archive",
                          "the entry count holds the zip64 marker")
    def encrypted(ms):
        ms[2]["flags"] = ms[2]["flags"] | 0x0001
    bad["encrypted"] = (with_members(encrypted), "bad-archive", "an entry flagged as encrypted")
    def method(ms):
        ms[2]["method"] = 12
    bad["method-bzip2"] = (with_members(method), "bad-archive", "an entry with a compression method other than deflate")
    bad["archive-comment"] = (write_zip([dict(m) for m in base_members], comment=b"hello"), "bad-archive",
                              "an archive comment")
    bad["bytes-after-end"] = (write_zip([dict(m) for m in base_members], trailer=b"\0"), "bad-archive",
                              "one byte after the end record")
    def leading(ms):
        ms[0]["pre"] = b"junk"
    bad["bytes-before-start"] = (with_members(leading), "bad-archive", "four bytes in front of the manifest")
    def gap(ms):
        ms[3]["pre"] = b"\0" * 16
    bad["gap-between-entries"] = (with_members(gap), "bad-archive", "bytes between two entries that belong to no file")
    bad["directory-order"] = (swap_directory_records(base, 3, 4), "bad-archive",
                              "the directory lists two entries in the other order than the file holds them")
    for name, label, why in ((b"payload/../evil", "name-dotdot", "a `..` segment"),
                             (b"/etc/evil", "name-absolute", "an absolute path"),
                             (b"payload\\evil", "name-backslash", "a backslash"),
                             (b"payload//evil", "name-empty-segment", "an empty segment"),
                             (b"payload/\xff", "name-not-utf8", "a byte that is not UTF-8")):
        odd = dict(make_member("x", b"x"), name=name)
        bad[label] = (with_members(lambda ms, odd=odd: ms.append(odd)), "bad-archive", "an entry name with " + why)
    def bad_crc(ms):
        ms[2]["crc"] ^= 1
    bad["crc-wrong"] = (with_members(bad_crc), "bad-archive", "an entry whose zip CRC is wrong")
    bad["cut-short"] = (base[:-10], "bad-archive", "the last ten bytes are missing")
    def deflate_tail(ms):
        m = member(ms, "payload/etc/hello.conf")
        assert m["method"] == 8
        m["raw"] = m["raw"] + b"\0"
    bad["deflate-trailing-byte"] = (with_members(deflate_tail), "bad-archive",
                                    "a byte after the end of an entry's deflate stream")
    long_folder = {"name": b"d" * 255 + b"/", "method": 0, "crc": 0, "usize": 0, "raw": b""}
    bad["name-too-long"] = (with_members(lambda ms: ms.append(long_folder)), "bad-archive",
                            "a folder entry whose name is 256 bytes")
    folder_data = {"name": b"payload/odd/", "method": 0, "crc": crc(b"x"), "usize": 1, "raw": b"x"}
    bad["folder-with-data"] = (with_members(lambda ms: ms.append(folder_data)), "bad-archive", "a folder entry that holds data")

    # ---- manifest: each of these is signed correctly, so only the manifest is at fault ----
    bad["json-duplicate-key"] = (with_text(b'"conflicts": [],', b'"conflicts": [], "conflicts": [],'), "bad-manifest",
                                 "the manifest has a key twice")
    bad["json-fraction"] = (with_text(b'"timeout": 60', b'"timeout": 60.0'), "bad-manifest", "a number with a fraction")
    bad["json-byte-order-mark"] = (with_manifest(b"\xef\xbb\xbf" + manifest_bytes), "bad-manifest",
                                   "the manifest starts with a byte-order mark")
    bad["json-not-utf8"] = (with_text(b'"Hello NBT"', b'"Hello \xff"'), "bad-manifest", "the manifest is not UTF-8")
    def deep(m):
        node = m
        for _ in range(17):
            node["deeper"] = {}
            node = node["deeper"]
    bad["json-too-deep"] = (with_changed_manifest(deep), "bad-manifest", "nested more than 16 levels, in an unknown key")
    def huge(m):
        m["padding"] = "x" * (1 << 20)
    bad["manifest-too-large"] = (with_changed_manifest(huge), "bad-manifest", "the manifest is larger than 1 MiB")

    def version2(m):
        m["manifestVersion"] = 2
    bad["manifest-version-2"] = (with_changed_manifest(version2), "unsupported-manifest", "manifestVersion 2")
    def patch_key(m):
        m["install"]["files"][0]["patch"] = "bsdiff"
    bad["install-unknown-key"] = (with_changed_manifest(patch_key), "unsupported-manifest",
                                  "a key in install.files that this version does not know")
    def requires_key(m):
        m["requires"]["firmware"] = ">=1.0.0"
    bad["requires-unknown-key"] = (with_changed_manifest(requires_key), "unsupported-manifest",
                                   "a requirement this version cannot check")

    def field(path, value):
        def change(m):
            node = m
            for key in path[:-1]:
                node = node[key]
            node[path[-1]] = value
        return change
    for label, path, value, why in (
            ("id-uppercase", ["id"], "Org.NBTApp.Hello", "an id with capitals"),
            ("id-one-label", ["id"], "hello", "an id with a single label"),
            ("version-two-parts", ["version"], "1.2", "a version that is not semver"),
            ("version-leading-zero", ["version"], "1.02.0", "a version with a leading zero"),
            ("name-empty", ["name"], "", "an empty name"),
            ("range-bad", ["depends", 0, "version"], ">=2.0", "a range that is not a list of comparators"),
            ("mode-three-digits", ["install", "files", 0, "mode"], "755", "a mode without the leading 0"),
            ("dest-dotdot", ["install", "files", 0, "dest"], "${app}/../../etc/passwd", "a destination with `..`"),
            ("dest-relative", ["install", "files", 0, "dest"], "bin/hello", "a destination that is not absolute"),
            ("dest-unknown-variable", ["install", "files", 0, "dest"], "${home}/bin/hello", "an unknown path variable"),
            ("dest-twice", ["install", "files", 1, "dest"], "${app}/bin/hello", "two files placed at one destination"),
            ("dest-folder-for-file", ["install", "files", 0, "dest"], "${app}/bin/", "a file mapped to a folder"),
            ("src-unlisted", ["install", "files", 0, "src"], "payload/bin/other", "a src that contents does not list"),
            ("link-over-file", ["install", "links", 0, "path"], "${app}/bin/hello", "a link at a file's destination"),
            ("hook-timeout-zero", ["install", "hooks", "preInstall", "timeout"], 0, "a hook timeout of 0"),
            ("hook-script-unlisted", ["install", "hooks", "preInstall", "script"], "scripts/none.sh",
             "a hook script that contents does not list"),
            ("reboot-unknown", ["install", "reboot"], "maybe", "an unknown reboot value"),
            ("contents-hash-uppercase", ["contents", 0, "sha256"], base_manifest["contents"][0]["sha256"].upper(),
             "a hash in capitals")):
        bad[label] = (with_changed_manifest(field(path, value)), "bad-manifest", why)
    def lists_manifest(m):
        m["contents"].append({"path": MANIFEST, "size": 1, "sha256": "0" * 64})
    bad["contents-lists-manifest"] = (with_changed_manifest(lists_manifest), "bad-manifest", "contents lists manifest.json")
    def wrong_size(m):
        m["contents"][0]["size"] += 1
    bad["contents-size-wrong"] = (with_changed_manifest(wrong_size), "bad-archive",
                                  "contents gives a file's size wrongly; its hash is right")

    # ---- install.serialServices ----
    radio = {"name": "TestRadio", "program": "radiod"}
    for label, services, code, why in (
            ("serial-service-twice", [radio, dict(radio, program="otherd")], "bad-manifest",
             "the same serial service declared twice"),
            ("serial-service-agents-name", [dict(radio, name="NBTApp")], "bad-manifest",
             "a serial service under the agent's own name"),
            ("serial-service-cars-name", [dict(radio, name="SPP")], "bad-manifest",
             "a serial service under the name of the car's own"),
            ("serial-service-name-space", [dict(radio, name="Test Radio")], "bad-manifest",
             "a serial service name with a space"),
            ("serial-service-name-long", [dict(radio, name="R" * 33)], "bad-manifest",
             "a serial service name of 33 characters"),
            ("serial-service-program-folder", [dict(radio, program="bin/radiod")], "bad-manifest",
             "a serial service's program given with its folder"),
            ("serial-service-no-program", [{"name": "TestRadio"}], "bad-manifest",
             "a serial service with no program"),
            ("serial-service-not-a-list", radio, "bad-manifest", "serialServices is one service, not a list"),
            ("serial-service-unknown-key", [dict(radio, uuid="0123ABCD-89EF-4A5B-8C7D-0123456789AB")],
             "unsupported-manifest", "a key in a serial service that this version does not know")):
        bad[label] = (with_changed_manifest(field(["install", "serialServices"], services)), code, why)

    # ---- write ----
    vectors = {"keys": {"package": public.hex(), "other": nbtpkg.public_key(other_seed).hex()}, "good": [], "bad": []}
    for kind in ("good", "bad"):
        os.makedirs(os.path.join(out_dir, kind), exist_ok=True)
        for stale in os.listdir(os.path.join(out_dir, kind)):
            os.remove(os.path.join(out_dir, kind, stale))
    for label, (data, why) in sorted(good.items()):
        path = "good/%s.nbtpkg" % label
        with open(os.path.join(out_dir, path), "wb") as f:
            f.write(data)
        vectors["good"].append(dict(nbtpkg.verify_package(data, [public]), file=path, what=why))
    for label, (data, code, why) in sorted(bad.items()):
        path = "bad/%s.nbtpkg" % label
        with open(os.path.join(out_dir, path), "wb") as f:
            f.write(data)
        vectors["bad"].append({"file": path, "code": code, "what": why})
    with open(os.path.join(out_dir, "vectors.json"), "w") as f:
        json.dump(vectors, f, indent=2)
        f.write("\n")
    return vectors


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "testdata")
    result = build_all(out)
    print("[DONE] %s: %d good, %d bad packages" % (out, len(result["good"]), len(result["bad"])))
