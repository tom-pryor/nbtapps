#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""`make test-pkg`: host test of tools/nbtpkg.py and of the test packages it is used to build. Needs python3 with the
`cryptography` module. Exits non-zero on the first failure.

  1. the strict JSON profile agrees with every line of spec/json-profile.vectors
  2. the manifest schema uses only keywords the tool implements
  3. every good test package verifies and every bad one is refused with the code vectors.json gives it
  4. the same source builds the same bytes, and `build` + `sign` equals a signed `build`
  5. Python's own zip reader accepts a good package, so ordinary tools can inspect one
  6. the command line: keygen never overwrites, sign-file signs, verify reports and fails as documented
"""
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import make_packages
import nbtpkg

TOOL = os.path.join(ROOT, "tools", "nbtpkg.py")
KEYS = os.path.join(ROOT, "tests", "keys")


def json_cases():
    """spec/json-profile.vectors -> (line number, expected ok, input bytes)."""
    with open(os.path.join(ROOT, "spec", "json-profile.vectors"), "r", encoding="ascii") as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            line = line.split("  #", 1)[0]
            verdict, _, text = line.partition(" ")
            assert verdict in ("ok", "bad"), "line %d" % number
            data = bytearray()
            i = 0
            while i < len(text):
                if text[i] == "%":
                    data.append(int(text[i + 1:i + 3], 16))
                    i += 3
                else:
                    data.append(ord(text[i]))
                    i += 1
            yield number, verdict == "ok", bytes(data)


def refused(code, call, *args, **kwargs):
    try:
        call(*args, **kwargs)
    except nbtpkg.PkgError as e:
        assert e.code == code, "expected %s, got %s: %s" % (code, e.code, e)
        return str(e)
    raise AssertionError("expected %s, but it was accepted" % code)


def run(*args):
    return subprocess.run([sys.executable, TOOL] + list(args), capture_output=True, text=True)


def main():
    # 1. JSON profile.
    count = 0
    for number, ok, data in json_cases():
        try:
            nbtpkg.parse_json(data)
            accepted = True
        except ValueError:
            accepted = False
        assert accepted == ok, "json-profile.vectors line %d: %s" % (number, "refused" if ok else "accepted")
        count += 1
    assert count >= 100
    print("[ok] JSON profile: %d cases" % count)

    # 2. Schema.
    schema = nbtpkg.manifest_schema()
    assert schema["properties"]["manifestVersion"]["const"] == nbtpkg.SUPPORTED_MANIFEST_VERSION
    # `$` must not accept a trailing newline, which Python's own `$` would.
    for good in ("1.2.0", "1.2.0-beta.1", "10.20.30+build.5"):
        nbtpkg.validate(good, schema["$defs"]["version"], schema, "version")
    for wrong in ("1.2.0\n", "1.2", "01.2.0", "1.2.0-", "v1.2.0", " 1.2.0", "1.2.0 "):
        try:
            nbtpkg.validate(wrong, schema["$defs"]["version"], schema, "version")
            raise AssertionError("version %r accepted" % wrong)
        except nbtpkg.SchemaError:
            pass
    for good in (">=1.0.0", "1.0.0", ">=2.0.0 <3.0.0", "=1.2.3 <=1.2.3 >1.0.0-rc.1"):
        nbtpkg.validate(good, schema["$defs"]["range"], schema, "range")
    for wrong in ("", ">= 1.0.0", ">=1.0.0  <2.0.0", "^1.0.0", "~1.0.0", "1.x", ">=1.0.0 ", "*", ">=1.0.0,<2.0.0"):
        try:
            nbtpkg.validate(wrong, schema["$defs"]["range"], schema, "range")
            raise AssertionError("range %r accepted" % wrong)
        except nbtpkg.SchemaError:
            pass
    print("[ok] manifest schema")

    with tempfile.TemporaryDirectory() as tmp:
        # 3. The test packages.
        vectors = make_packages.build_all(os.path.join(tmp, "data"))
        public = bytes.fromhex(vectors["keys"]["package"])
        other = bytes.fromhex(vectors["keys"]["other"])
        assert len(vectors["good"]) >= 6 and len(vectors["bad"]) >= 50

        def data_of(entry):
            with open(os.path.join(tmp, "data", entry["file"]), "rb") as f:
                return f.read()

        for entry in vectors["good"]:
            data = data_of(entry)
            info = nbtpkg.verify_package(data, [other, public])
            assert info["key"] == public.hex() and info["sha256"] == hashlib.sha256(data).hexdigest()
            assert all(info[k] == entry[k] for k in ("id", "version", "size", "manifestSha256", "signature", "flags"))
            refused("bad-signature", nbtpkg.verify_package, data, [other])
            refused("bad-signature", nbtpkg.verify_package, data, [])
        for entry in vectors["bad"]:
            try:
                refused(entry["code"], nbtpkg.verify_package, data_of(entry), [public])
            except AssertionError as e:
                raise AssertionError("%s (%s): %s" % (entry["file"], entry["what"], e))
        hello = next(e for e in vectors["good"] if e["file"] == "good/org.nbtapp.hello-1.2.0.nbtpkg")
        assert hello["flags"] == ["runs-scripts"]
        print("[ok] test packages: %d good, %d bad" % (len(vectors["good"]), len(vectors["bad"])))

        # Risk flags come from the manifest alone.
        assert nbtpkg.risk_flags({"install": {"files": [{"dest": "/etc/x"}], "reboot": "hmi-restart"}}) == \
            ["modifies-system", "needs-reboot"]
        assert nbtpkg.risk_flags({"install": {"files": [{"dest": "${plugins}/A/a", "replaces": {}}]}}) == ["modifies-system"]
        assert nbtpkg.risk_flags({"install": {"links": [{"path": "/bin/x"}]}}) == ["modifies-system"]
        assert nbtpkg.risk_flags({"install": {"files": [{"dest": "${data}/x"}], "hooks": {}}}) == []
        assert nbtpkg.risk_flags({}) == []

        # 4. Reproducible, and signing afterwards gives the same archive as building signed.
        seed = nbtpkg.read_key(os.path.join(KEYS, "test-package.key"))
        source = os.path.join(ROOT, "tests", "packages", "hello-1.2.0")
        _, signed = nbtpkg.build_package(source, seed)
        _, unsigned = nbtpkg.build_package(source)
        assert signed == nbtpkg.build_package(source, seed)[1] == data_of(hello)
        refused("bad-signature", nbtpkg.verify_package, unsigned, [public])
        assert "signature" not in nbtpkg.verify_package(unsigned, [public], allow_unsigned=True)
        assert nbtpkg.sign_package(unsigned, seed) == signed
        assert nbtpkg.sign_package(signed, seed) == signed
        resigned = nbtpkg.sign_package(signed, nbtpkg.read_key(os.path.join(KEYS, "test-other.key")))
        assert nbtpkg.verify_package(resigned, [other])["key"] == other.hex()
        bad = next(e for e in vectors["bad"] if e["file"] == "bad/file-changed.nbtpkg")
        refused("bad-archive", nbtpkg.sign_package, data_of(bad), seed)
        print("[ok] reproducible build and signing")

        # 5. An ordinary zip reader sees the same files.
        with zipfile.ZipFile(io.BytesIO(signed)) as archive:
            assert archive.testzip() is None
            names = archive.namelist()
            assert names[:2] == [nbtpkg.MANIFEST, nbtpkg.SIGNATURE]
            manifest = json.loads(archive.read(nbtpkg.MANIFEST))
            assert [c["path"] for c in manifest["contents"]] == names[2:]
            for content in manifest["contents"]:
                assert hashlib.sha256(archive.read(content["path"])).hexdigest() == content["sha256"]
        print("[ok] readable as a plain zip")

        # 6. Command line.
        assert run("keygen", "k", "-o", tmp).returncode == 0
        again = run("keygen", "k", "-o", tmp)
        assert again.returncode != 0 and "never overwritten" in again.stderr
        assert os.stat(os.path.join(tmp, "k.key")).st_mode & 0o077 == 0
        key, pub = os.path.join(tmp, "k.key"), os.path.join(tmp, "k.pub")
        assert nbtpkg.public_key(nbtpkg.read_key(key)) == nbtpkg.read_key(pub)

        built = run("build", source, "-o", tmp)
        package = os.path.join(tmp, "org.nbtapp.hello-1.2.0.nbtpkg")
        assert built.returncode == 0 and "NOT signed" in built.stdout
        failed = run("verify", package, "--key", pub)
        assert failed.returncode != 0 and "bad-signature" in failed.stderr
        assert run("verify", package, "--unsigned").returncode == 0
        assert run("sign", package, "--key", key).returncode == 0
        checked = run("verify", package, "--key", pub, "--json")
        assert checked.returncode == 0 and json.loads(checked.stdout)["key"] == nbtpkg.read_key(pub).hex()
        unchecked = run("verify", package)
        assert unchecked.returncode == 0 and "NOT checked" in unchecked.stdout
        wrong = run("verify", package, "--key", os.path.join(KEYS, "test-package.pub"))
        assert wrong.returncode != 0 and "bad-signature" in wrong.stderr

        index = os.path.join(tmp, "index.json")
        with open(index, "wb") as f:
            f.write(b'{"schema": 1}\n')
        assert run("sign-file", index, "--key", key).returncode == 0
        with open(index + ".sig", "rb") as f:
            signature = f.read()
        assert len(signature) == 64 and nbtpkg.signature_ok(nbtpkg.read_key(pub), b'{"schema": 1}\n', signature)
        assert not nbtpkg.signature_ok(nbtpkg.read_key(pub), b'{"schema": 2}\n', signature)
        print("[ok] command line")

    print("[DONE] nbtpkg tests passed")


if __name__ == "__main__":
    main()
