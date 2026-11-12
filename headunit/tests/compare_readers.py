#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""Holds the two package readers together: tools/nbtpkg.py and the agent's (agent/package.cpp, through
build/agent-host/test_package --codes). Makes changed copies of the test packages and checks that both readers give
every one of them the same verdict: accepted, or refused with the same code.

  python3 tests/compare_readers.py [COUNT] [SEED]      (default 4000 cases, seed 1; needs `make build/agent-host/test_package testdata` first)

Two kinds of change:
  bytes      a good or bad package with bytes flipped, removed, inserted, repeated or cut off
  manifest   a good package whose manifest has a member removed, replaced, renamed or added, signed again with the
             test key, so the change reaches the manifest checks instead of stopping at the signature

Not part of `make agent-test`: it is slow and random. Run it after changing either reader or the schema.
"""
import json
import os
import random
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import make_packages
import nbtpkg

CHECKER = os.path.join(ROOT, "build", "agent-host", "test_package")

STRINGS = ["", "a", "a.b", "org.nbtapp.hello", "Org.x", "1.0.0", "1.0", "01.0.0", "1.0.0-rc.1", ">=1.0.0", ">=1.0.0 <2.0.0",
           ">= 1.0.0", "none", "required", "hmi-restart", "maybe", "0644", "0755", "644", "0999", "payload/bin/hello",
           "payload/", "payload/bin/", "payload/etc/hello.conf", "scripts/pre-install.sh", "scripts/", "../x", "a/../b",
           "a//b", "/abs", "/abs/", "/", "${app}/x", "${app}/", "${app}", "${data}/x/", "${plugins}/A/b", "${home}/x",
           "${app}/bin/hello", "${app}/../x", "a\\b", "a\nb", "a\u0000b", "café", "\U0001f600", "x" * 64, "x" * 65,
           "x" * 255, "x" * 256, "é" * 128, "é" * 255, "é" * 256, "manifest.json", "manifest.sig",
           "0" * 64, "f" * 64, "F" * 64, "0" * 63, "hello", "x\u007fy", ".", "..", "...", "a/.", "a/.b"]
VALUES = STRINGS + [0, 1, 2, -1, 30, 600, 601, 4294967294, 4294967295, 2 ** 53 - 1, True, False, None, [], {}, [1], ["a"],
                    [{}], {"a": 1}, [{"id": "a.b"}], {"sha256": ["0" * 64]}, {"sha256": []}, {"script": "scripts/pre-install.sh", "timeout": 5},
                    [{"path": "${app}/l", "target": "t"}], {"agent": ">=1.0.0"}]
KEYS = ["manifestVersion", "id", "name", "version", "requires", "depends", "conflicts", "contents", "install", "files",
        "links", "hooks", "reboot", "src", "dest", "mode", "config", "replaces", "sha256", "path", "target", "script",
        "timeout", "preInstall", "postInstall", "preRemove", "postRemove", "agent", "size", "patch", "extra", "firmware"]


def pool_value(rng):
    """A value from the pool, copied: the same list or object must not end up in a manifest twice."""
    return json.loads(json.dumps(rng.choice(VALUES)))


def nodes(value, found=None):
    """Every object and array inside `value`, itself included."""
    found = [] if found is None else found
    if isinstance(value, (dict, list)):
        found.append(value)
        for child in (value.values() if isinstance(value, dict) else value):
            nodes(child, found)
    return found


def change_manifest(manifest, rng):
    for _ in range(rng.choice((1, 1, 1, 2, 3))):
        node = rng.choice(nodes(manifest))
        kind = rng.randrange(6)
        if isinstance(node, dict):
            keys = list(node)
            if kind == 0 and keys:
                del node[rng.choice(keys)]
            elif kind == 1 and keys:
                node[rng.choice(keys)] = pool_value(rng)
            elif kind == 2 and keys:
                key = rng.choice(keys)
                node[rng.choice(KEYS)] = node.pop(key)
            elif kind == 3:
                node[rng.choice(KEYS)] = pool_value(rng)
            elif kind == 4 and keys:
                key = rng.choice(keys)
                if isinstance(node[key], str):
                    node[key] = node[key] + rng.choice(["/", "x", " ", "\n", ".", "/..", "é", "A"])
                else:
                    node[key] = pool_value(rng)
            elif keys:   # move a member to the front, so its fault is met first
                key = rng.choice(keys)
                moved = {key: node.pop(key)}
                moved.update(node)
                node.clear()
                node.update(moved)
        else:
            if kind == 0 and node:
                del node[rng.randrange(len(node))]
            elif kind == 1 and node:
                node[rng.randrange(len(node))] = pool_value(rng)
            elif kind == 2 and node:
                node.append(json.loads(json.dumps(rng.choice(node))))
            else:
                node.append(pool_value(rng))


def change_bytes(data, rng):
    data = bytearray(data)
    for _ in range(rng.choice((1, 1, 2, 4))):
        kind = rng.randrange(6)
        at = rng.randrange(len(data)) if data else 0
        # Half of the changes aim at the directory and the end record, where the structure is.
        if rng.random() < 0.5 and len(data) > 600:
            at = len(data) - 1 - rng.randrange(600)
        if kind == 0 and data:
            data[at] ^= 1 << rng.randrange(8)
        elif kind == 1 and data:
            data[at] = rng.choice((0, 0xFF, 0x50, 0x2F, data[at] ^ 0xFF))
        elif kind == 2 and data:
            del data[at:at + rng.choice((1, 2, 4, 30))]
        elif kind == 3:
            data[at:at] = bytes(rng.randrange(256) for _ in range(rng.choice((1, 2, 4, 22))))
        elif kind == 4 and data:
            data[at:at] = data[at:at + rng.choice((4, 30, 46))]
        elif data:
            del data[rng.randrange(len(data)):]
    return bytes(data)


def python_verdict(data, public):
    try:
        nbtpkg.verify_package(data, [public])
        return "ok"
    except nbtpkg.PkgError as e:
        return e.code


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 4000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    if not os.path.exists(CHECKER):
        sys.exit("compare_readers: %s is missing; run `make build/agent-host/test_package testdata` first" % CHECKER)
    rng = random.Random(seed)
    seed_key = nbtpkg.read_key(os.path.join(ROOT, "tests", "keys", "test-package.key"))
    public = nbtpkg.public_key(seed_key)

    with tempfile.TemporaryDirectory() as tmp:
        vectors = make_packages.build_all(os.path.join(tmp, "data"))
        originals = []
        for entry in vectors["good"] + vectors["bad"]:
            with open(os.path.join(tmp, "data", entry["file"]), "rb") as f:
                originals.append(f.read())
        good = originals[:len(vectors["good"])]
        parts = []   # (manifest, the members after the signature) of each good package
        for data in good:
            entries = nbtpkg.read_zip(data)
            members = [{"name": e["name_bytes"], "method": e["method"], "crc": e["crc"], "usize": e["usize"],
                        "raw": e["raw"], "flags": e["flags"]} for e in entries[2:]]
            parts.append((entries[0]["data"], members))

        cases, verdicts = [], {}
        os.mkdir(os.path.join(tmp, "cases"))
        for i in range(count):
            if i % 2:
                data = change_bytes(rng.choice(originals), rng)
            else:
                manifest_bytes, members = rng.choice(parts)
                manifest = json.loads(manifest_bytes)
                change_manifest(manifest, rng)
                text = (json.dumps(manifest, indent=rng.choice((None, 2)), ensure_ascii=rng.random() < 0.5) + "\n").encode()
                data = nbtpkg.write_zip([nbtpkg.make_member(nbtpkg.MANIFEST, text),
                                         nbtpkg.make_member(nbtpkg.SIGNATURE, nbtpkg.sign(seed_key, text), store=True)]
                                        + members)
            path = os.path.join(tmp, "cases", "%06d.nbtpkg" % i)
            with open(path, "wb") as f:
                f.write(data)
            cases.append(path)
            verdicts[path] = python_verdict(data, public)

        tally, differences = {}, 0
        for start in range(0, len(cases), 500):
            batch = cases[start:start + 500]
            out = subprocess.run([CHECKER, "--codes"] + batch, capture_output=True, text=True)
            if out.returncode != 0:
                sys.exit("compare_readers: the agent's reader stopped: %s" % out.stderr.strip())
            for line in out.stdout.splitlines():
                verdict, path = line.split(" ", 1)
                tally[verdict] = tally.get(verdict, 0) + 1
                if verdict != verdicts[path]:
                    differences += 1
                    keep = os.path.join(ROOT, "build", "compare-%s" % os.path.basename(path))
                    with open(path, "rb") as src, open(keep, "wb") as dst:
                        dst.write(src.read())
                    print("DIFFERENT: python %s, agent %s: kept as %s" % (verdicts[path], verdict, keep))
        summary = ", ".join("%d %s" % (n, v) for v, n in sorted(tally.items()))
        if differences:
            sys.exit("compare_readers: %d of %d cases differ (%s)" % (differences, count, summary))
        print("[DONE] %d cases, seed %d: both readers agree on every one (%s)" % (count, seed, summary))


if __name__ == "__main__":
    main()
