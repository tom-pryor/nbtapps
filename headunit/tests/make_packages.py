#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""Builds the test packages: `make testdata`, or python3 tests/make_packages.py [OUTDIR] (default build/testdata).

  OUTDIR/good/*.nbtpkg   the sources in tests/packages, signed with tests/keys/test-package.key, plus variants a
                         reader must also accept
  OUTDIR/bad/*.nbtpkg    hello 1.2.0 with one defect each; every reader must refuse each of them
  OUTDIR/detached/*.nbtpkg  hello 1.2.0 as an author might sign it, each read with a detached signature over its
                         manifest instead of its manifest.sig
  OUTDIR/vectors.json    what each file is: for a good one the values an index entry or a job.plan offer carries,
                         for a bad one the code the refusal has (bad-archive, bad-signature, bad-manifest or
                         unsupported-manifest) and what is wrong with it, for a detached one the signature to read it
                         with and the code ("" when it is accepted, trusting the package key only)

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

    # ---- logs: the files a controller may pull ----
    every_form = [{"path": "${data}/log/*.log"}, {"path": "${app}/state.txt"}, {"path": "${tmp}/crash-?.txt"},
                  {"path": "/var/log/hello.log", "label": "an unknown key in an entry is ignored"},
                  {"path": "/tmp/hello.log"}]
    good["logs"] = (with_changed_manifest(field(["logs"], every_form)),
                    "logs in each place one may be, one entry with a key this version does not know")
    for label, logs, why in (
            ("logs-not-a-list", {"path": "/tmp/hello.log"}, "logs is one entry, not a list"),
            ("logs-entry-not-object", ["/tmp/hello.log"], "a logs entry that is a string"),
            ("logs-no-path", [{"label": "hello"}], "a logs entry with no path"),
            ("logs-too-many", [{"path": "/tmp/hello-%d.log" % i} for i in range(17)], "17 logs entries"),
            ("logs-twice", [{"path": "/tmp/hello.log"}, {"path": "/tmp/hello.log"}], "the same log named twice"),
            ("logs-dotdot", [{"path": "${data}/../../var/opt/nbtapp/phones.json"}], "a log path with `..`"),
            ("logs-relative", [{"path": "log/hello.log"}], "a log path that is not absolute"),
            ("logs-unknown-variable", [{"path": "${home}/hello.log"}], "a log path with an unknown variable"),
            ("logs-plugins", [{"path": "${plugins}/Hello/hello.log"}], "a log path in the HMI's plugin folder"),
            ("logs-folder", [{"path": "${data}/log/"}], "a log path that ends in a slash"),
            ("logs-wildcard-in-folder", [{"path": "${data}/*/hello.log"}], "a wildcard outside the last segment"),
            ("logs-absolute-elsewhere", [{"path": "/etc/passwd"}], "a log path outside /tmp and /var"),
            ("logs-agent-state", [{"path": "/var/opt/nbtapp/phones.json"}], "a log path in the agent's state folder"),
            ("logs-agent-run", [{"path": "/tmp/nbtapp/log/agent.log"}], "a log path in the agent's folder on the RAM disk"),
            ("logs-other-tmp", [{"path": "/tmp/nbtapp/apps/org.other.radio/radio.log"}],
             "another package's ${tmp}, named by its path")):
        bad[label] = (with_changed_manifest(field(["logs"], logs)), "bad-manifest", why)
    bad["dest-tmp"] = (with_changed_manifest(field(["install", "files", 0, "dest"], "${tmp}/bin/hello")), "bad-manifest",
                       "a destination in ${tmp}, which is for logs only")

    # ---- settings: what a phone shows and changes ----
    choices = [{"value": "round", "label": "Round"}, {"value": "square", "label": "Square"}]
    lamp = {"key": "LAMP", "label": "Lamp", "type": "bool"}
    every_setting = {
        "comment": "an unknown key in settings is ignored",
        "options": [
            dict(lamp, description="", default=False, applies="now", advanced=False, min="not looked at",
                 note="an unknown key in an option is ignored"),
            {"key": "CHIME", "label": "Chime", "type": "bool", "unset": "As the chime has it", "advanced": True},
            {"key": "SPEED", "label": "Speed", "description": "How fast.", "type": "int", "min": -2147483648,
             "max": 2147483647, "unit": "°/s", "default": -5, "applies": "app-start", "maxLength": "not looked at"},
            {"key": "STEPS", "label": "Steps", "type": "int", "min": 3, "max": 3, "default": 3},
            {"key": "LEVEL", "label": "Level", "type": "int", "min": 0, "max": 9, "unset": "Off"},
            {"key": "HOST", "label": "Host", "type": "text", "maxLength": 255, "default": "lamp-1.example:80/a@b+c,d_e"},
            {"key": "GREETING", "label": "Greeting", "type": "text", "maxLength": 6, "characters": "any",
             "default": "é é!", "applies": "reboot"},
            {"key": "NAME", "label": "Name", "type": "text", "maxLength": 1, "characters": "safe"},
            {"key": "FACE", "label": "Face", "type": "select", "values": choices, "default": "square",
             "applies": "hmi-restart"},
            {"key": "FACE_2", "label": "Second face", "type": "select",
             "values": [{"value": "v%d" % i, "label": "Value %d" % i, "note": "ignored"} for i in range(16)],
             "unset": "Automatic"},
            {"key": "COLOUR", "label": "Colour", "type": "colour", "default": "#ffffff", "min": "not looked at"},
            {"key": "Z", "label": "x" * 64, "type": "TEXT", "description": 7},
        ]}
    good["settings"] = (with_changed_manifest(field(["settings"], every_setting)),
                        "settings of each type, with and without a default or an unset text, in the default file, "
                        "with keys this version does not know and two options of a type it does not know")
    # 64 options, the last at exactly 1,024 bytes of text, two bytes to each character of its description.
    most = [dict(lamp, key="LAMP_%d" % i) for i in range(63)]
    most.append({"key": "K" * 64, "label": "L" * 64, "type": "bool", "description": "é" * 448})
    good["settings-most"] = (with_changed_manifest(field(["settings"], {"file": "${data}/conf/dial settings.txt",
                                                                        "options": most})),
                             "64 options, one with 1,024 bytes of text, kept in a file of the package's own choice")

    def one(**changes):
        """A settings block with the single option `lamp`, with these members changed (None removes one)."""
        option = dict(lamp)
        for key, value in changes.items():
            if value is None:
                option.pop(key)
            else:
                option[key] = value
        return {"options": [option]}
    number = dict(lamp, type="int", min=0, max=10)
    text = dict(lamp, type="text", maxLength=8)
    select = dict(lamp, type="select", values=choices)
    for label, settings, why in (
            ("settings-not-object", [lamp], "settings is a list, not an object"),
            ("settings-no-options", {"file": "${data}/settings"}, "settings with no options"),
            ("settings-options-empty", {"options": []}, "settings with no option in its list"),
            ("settings-options-not-list", {"options": lamp}, "options is one option, not a list"),
            ("settings-options-too-many", {"options": [dict(lamp, key="L%d" % i) for i in range(65)]}, "65 options"),
            ("settings-option-not-object", {"options": ["LAMP"]}, "an option that is a string"),
            ("settings-file-not-string", dict(one(), file=7), "a settings file that is a number"),
            ("settings-file-app", dict(one(), file="${app}/settings"), "a settings file in ${app}"),
            ("settings-file-absolute", dict(one(), file="/tmp/settings"), "a settings file outside ${data}"),
            ("settings-file-dotdot", dict(one(), file="${data}/../org.other.radio/settings"),
             "a settings file with `..`"),
            ("settings-file-folder", dict(one(), file="${data}/conf/"), "a settings file that ends in a slash"),
            ("settings-file-data", dict(one(), file="${data}/"), "the data folder itself as the settings file"),
            ("settings-file-too-long", dict(one(), file="${data}/" + "f" * 248), "a settings file of 256 characters"),
            ("settings-no-key", one(key=None), "an option with no key"),
            ("settings-no-label", one(label=None), "an option with no label"),
            ("settings-no-type", one(type=None), "an option with no type"),
            ("settings-key-lowercase", one(key="Lamp"), "a key with a small letter"),
            ("settings-key-digit-first", one(key="1LAMP"), "a key that starts with a digit"),
            ("settings-key-too-long", one(key="K" * 65), "a key of 65 characters"),
            ("settings-key-twice", {"options": [lamp, dict(lamp, label="Lamp again")]}, "the same key twice"),
            ("settings-key-twice-skipped", {"options": [lamp, dict(lamp, type="colour")]},
             "the same key twice, once on an option of a type no reader knows"),
            ("settings-unknown-type-bad-label", {"options": [dict(lamp, type="colour", label="")]},
             "an option of a type no reader knows with an empty label"),
            ("settings-label-empty", one(label=""), "an empty label"),
            ("settings-label-too-long", one(label="x" * 65), "a label of 65 characters"),
            ("settings-label-control", one(label="La\tmp"), "a label with a tab"),
            ("settings-type-not-string", one(type=1), "a type that is a number"),
            ("settings-description-too-long", one(description="x" * 501), "a description of 501 characters"),
            ("settings-description-control", one(description="On\nor off"), "a description with a line end"),
            ("settings-bool-default-string", one(default="true"), "a bool whose default is a string"),
            ("settings-unset-with-default", one(default=True, unset="Off"), "both a default and an unset text"),
            ("settings-unset-empty", one(unset=""), "an empty unset text"),
            ("settings-applies-unknown", one(applies="later"), "an unknown applies"),
            ("settings-advanced-not-bool", one(advanced="yes"), "advanced as a string"),
            ("settings-int-no-min", {"options": [{k: v for k, v in number.items() if k != "min"}]},
             "an int with no min"),
            ("settings-int-no-max", {"options": [{k: v for k, v in number.items() if k != "max"}]},
             "an int with no max"),
            ("settings-int-min-above-max", {"options": [dict(number, min=11)]}, "an int whose min is above its max"),
            ("settings-int-min-string", {"options": [dict(number, min="0")]}, "an int whose min is a string"),
            ("settings-int-out-of-range", {"options": [dict(number, max=2147483648)]}, "an int whose max is 2^31"),
            ("settings-int-default-below", {"options": [dict(number, default=-1)]}, "a default below min"),
            ("settings-int-default-above", {"options": [dict(number, default=11)]}, "a default above max"),
            ("settings-int-default-string", {"options": [dict(number, default="5")]}, "an int's default as a string"),
            ("settings-int-unit-empty", {"options": [dict(number, unit="")]}, "an empty unit"),
            ("settings-int-unit-too-long", {"options": [dict(number, unit="u" * 17)]}, "a unit of 17 characters"),
            ("settings-text-no-max-length", {"options": [dict(lamp, type="text")]}, "a text with no maxLength"),
            ("settings-text-max-length-zero", {"options": [dict(text, maxLength=0)]}, "a maxLength of 0"),
            ("settings-text-max-length-256", {"options": [dict(text, maxLength=256)]}, "a maxLength of 256"),
            ("settings-text-characters-unknown", {"options": [dict(text, characters="ascii")]},
             "an unknown set of characters"),
            ("settings-text-default-empty", {"options": [dict(text, default="")]}, "a text's default that is empty"),
            ("settings-text-default-bytes", {"options": [dict(text, characters="any", maxLength=5, default="ééé")]},
             "a text's default of 3 characters in 6 bytes, over a maxLength of 5"),
            ("settings-text-default-unsafe", {"options": [dict(text, default="a b")]},
             "a space in a safe text's default"),
            ("settings-text-default-space-end", {"options": [dict(text, characters="any", default="a ")]},
             "a space at the end of a text's default"),
            ("settings-text-default-control", {"options": [dict(text, characters="any", default="a\x7fb")]},
             "a control character in a text's default"),
            ("settings-text-default-number", {"options": [dict(text, default=5)]}, "a text's default as a number"),
            ("settings-select-no-values", {"options": [dict(lamp, type="select")]}, "a select with no values"),
            ("settings-select-values-not-list", {"options": [dict(select, values=choices[0])]},
             "a select's values as one object"),
            ("settings-select-one-value", {"options": [dict(select, values=choices[:1])]}, "a select of one value"),
            ("settings-select-too-many-values",
             {"options": [dict(select, values=[{"value": "v%d" % i, "label": "V"} for i in range(17)])]},
             "a select of 17 values"),
            ("settings-select-value-twice", {"options": [dict(select, values=[choices[0], dict(choices[1], value="round")])]},
             "a select with one value twice"),
            ("settings-select-value-form", {"options": [dict(select, values=[choices[0], dict(choices[1], value="a b")])]},
             "a select value with a space"),
            ("settings-select-value-too-long", {"options": [dict(select, values=[choices[0], dict(choices[1], value="v" * 65)])]},
             "a select value of 65 characters"),
            ("settings-select-value-no-label", {"options": [dict(select, values=[choices[0], {"value": "x"}])]},
             "a select value with no label"),
            ("settings-select-default-other", {"options": [dict(select, default="hex")]},
             "a select's default that is not one of its values"),
            ("settings-bytes-description", one(label="L" * 21, description="é" * 500),
             "an option of 1,025 bytes of text: a description of 500 characters in 1,000 bytes, its key and label"),
            ("settings-bytes-select",
             {"options": [dict(select, values=[{"value": "%02d" % i + "v" * 59, "label": "V" * (26 if i == 0 else 1)}
                                               for i in range(16)])]},
             "a select of 1,025 bytes of text: 16 values of 61 characters, their labels, its key and label")):
        bad[label] = (with_changed_manifest(field(["settings"], settings)), "bad-manifest", why)

    # ---- detached signatures: a package its author signed, with the catalogue's signature over its manifest ----
    countersigned = nbtpkg.sign(seed, manifest_bytes)
    flipped = bytearray(countersigned)
    flipped[10] ^= 0x01
    authors = with_manifest(manifest_bytes, signer=other_seed)
    def moved_signature(ms):
        ms.append(ms.pop(1))
    detached = {
        "author-signed": (authors, countersigned, "", "signed by its author's key, with the catalogue's signature"),
        "unsigned": (with_members(lambda ms: ms.pop(1) and None), countersigned, "",
                     "no manifest.sig, with the catalogue's signature"),
        "author-signature-short": (with_members(short_signature), countersigned, "",
                                   "a manifest.sig of 63 bytes, which is not read"),
        "package-signed-other-detached": (base, nbtpkg.sign(other_seed, manifest_bytes), "bad-signature",
                                          "a manifest.sig the reader trusts does not stand in for a detached "
                                          "signature it does not"),
        "detached-flipped": (authors, bytes(flipped), "bad-signature", "one bit of the detached signature is changed"),
        "detached-short": (authors, countersigned[:63], "bad-signature", "a detached signature of 63 bytes"),
        "signature-not-second": (with_members(moved_signature), countersigned, "bad-archive",
                                 "manifest.sig is there but not the second entry"),
    }

    # ---- write ----
    vectors = {"keys": {"package": public.hex(), "other": nbtpkg.public_key(other_seed).hex()}, "good": [], "bad": [],
               "detached": []}
    for kind in ("good", "bad", "detached"):
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
    for label, (data, signature, code, why) in sorted(detached.items()):
        path = "detached/%s.nbtpkg" % label
        with open(os.path.join(out_dir, path), "wb") as f:
            f.write(data)
        vectors["detached"].append({"file": path, "signature": signature.hex(), "code": code, "what": why})
    with open(os.path.join(out_dir, "vectors.json"), "w") as f:
        json.dump(vectors, f, indent=2)
        f.write("\n")
    return vectors


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "testdata")
    result = build_all(out)
    print("[DONE] %s: %d good, %d bad packages" % (out, len(result["good"]), len(result["bad"])))
