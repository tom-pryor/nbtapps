#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""`make test-pkg`: host test of tools/nbtpkg.py and of the test packages it is used to build. Needs python3 with the
`cryptography` module. Exits non-zero on the first failure.

  1. the strict JSON profile agrees with every line of spec/json-profile.vectors
  2. the manifest schema uses only keywords the tool implements, agrees with every line of spec/log-paths.vectors,
     and names the tool's folder variables (FOLDER_VARIABLES) in its patterns for a destination and a link's path
  3. every good test package verifies and every bad or skipped one is refused with the code vectors.json gives it,
     and each detached-signature case gets the verdict it gives
  4. the same source builds the same bytes, and `build` + `sign` equals a signed `build`
  5. Python's own zip reader accepts a good package, so ordinary tools can inspect one
  6. the command line: keygen never overwrites, sign-file signs, verify reports and fails as documented
"""
import hashlib
import io
import json
import os
import re
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


def vector_cases(name, verdicts=("ok", "bad")):
    """spec/NAME, in the form json-profile.vectors and log-paths.vectors share -> (line number, verdict, bytes)."""
    with open(os.path.join(ROOT, "spec", name), "r", encoding="ascii") as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            line = line.split("  #", 1)[0]
            verdict, _, text = line.partition(" ")
            assert verdict in verdicts, "%s line %d" % (name, number)
            data = bytearray()
            i = 0
            while i < len(text):
                if text[i] == "%":
                    data.append(int(text[i + 1:i + 3], 16))
                    i += 3
                else:
                    data.append(ord(text[i]))
                    i += 1
            yield number, verdict, bytes(data)


def json_cases():
    """spec/json-profile.vectors -> (line number, expected ok, input bytes)."""
    for number, verdict, data in vector_cases("json-profile.vectors"):
        yield number, verdict == "ok", data


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
    # The path of a logs entry, as the agent and the phone app read it: the schema takes only "ok".
    count = 0
    for number, verdict, data in vector_cases("log-paths.vectors", ("ok", "skip", "bad")):
        path = data.decode("utf-8")
        form = nbtpkg.log_path_form(path)
        assert form == verdict, "log-paths.vectors line %d: %s, not %s" % (number, form, verdict)
        try:
            nbtpkg.validate({"path": path}, schema["$defs"]["log"], schema, "log")
            accepted = True
        except nbtpkg.SchemaError:
            accepted = False
        assert accepted == (verdict == "ok"), "log-paths.vectors line %d" % number
        count += 1
    assert count > 40
    # 255 characters at most, a skipped path too.
    for path, form in (("${data}/" + "x" * 247, "ok"), ("${home}/" + "x" * 247, "skip"), ("/" + "x" * 255, "bad")):
        assert nbtpkg.log_path_form(path) == form, path[:20]
    print("[ok] log paths: %d cases" % count)
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
    # A destination and a link's path: in one of the four folders a package places files in, or absolute.
    dest = schema["$defs"]["file"]["properties"]["dest"]
    link = schema["$defs"]["link"]["properties"]["path"]
    for good in ("${app}/b", "${plugins}/A/", "${var_plugins}/A/", "${var_plugins}/A/a.so", "${var_plugins}/", "/x"):
        nbtpkg.validate(good, dest, schema, "dest")
    for wrong in ("${var_plugins}", "${var_plugins}x/a", "${var_pluginsx}/a", "${VAR_PLUGINS}/a", "${var_plugins}//a",
                  "${var_plugins}/../a", "${tmp}/a"):
        try:
            nbtpkg.validate(wrong, dest, schema, "dest")
            raise AssertionError("dest %r accepted" % wrong)
        except nbtpkg.SchemaError:
            pass
    for good in ("${plugins}/A/b", "${var_plugins}/A/b", "/x"):
        nbtpkg.validate(good, link, schema, "link")
    for wrong in ("${var_plugins}", "${var_plugins}/", "${var_plugins}/A/", "${var_pluginsx}/a"):
        try:
            nbtpkg.validate(wrong, link, schema, "link")
            raise AssertionError("link path %r accepted" % wrong)
        except nbtpkg.SchemaError:
            pass
    # Both patterns name exactly the tool's folder variables, the table OWN_DIRS and VARIABLE_FIRST_AGENT come from.
    names = set(v[2:-1] for v in nbtpkg.FOLDER_VARIABLES)
    for where, pattern in (("dest", dest["pattern"]), ("link path", link["pattern"])):
        alternatives = re.findall(r"\\\$\\\{\(([a-z_|]+)\)\\\}", pattern)
        assert len(alternatives) == 1 and set(alternatives[0].split("|")) == names, (where, alternatives)
    print("[ok] manifest schema")

    # 2b. Settings: what a text option takes, and the edges the test packages do not reach.
    for value in ("a", "Z9", "lamp-1.example:80/x@y+z,w_v"):
        assert nbtpkg.setting_text_ok(value, 255) and nbtpkg.setting_text_ok(value, 255, "any"), value
    for value in ("", "a b", "a=b", "a#b", "é", "a\tb", "a;b"):
        assert not nbtpkg.setting_text_ok(value, 255), value
    for value in ("a b", "a=b;c#d", "é x", "!"):
        assert nbtpkg.setting_text_ok(value, 255, "any"), value
    for value in ("", " a", "a ", " ", "a\tb", "a\nb", "a\x7fb"):
        assert not nbtpkg.setting_text_ok(value, 255, "any"), value
    assert nbtpkg.setting_text_ok("éé", 4, "any") and not nbtpkg.setting_text_ok("éé", 3, "any")

    def settings(*options):
        manifest = {"manifestVersion": 1, "id": "org.nbtapp.t", "name": "T", "version": "1.0.0", "contents": [],
                    "settings": {"options": list(options)}}
        nbtpkg.check_manifest(manifest)
    lamp = {"key": "LAMP", "type": "bool"}
    settings(dict(lamp, label="L" * 20, description="é" * 500))            # 1,024 bytes
    refused("bad-manifest", settings, dict(lamp, label="L" * 21, description="é" * 500))
    settings(dict(lamp, label="L" * 22, unit="U", description="é" * 499))  # a unit on a bool does not count
    refused("bad-manifest", settings, dict(lamp, label="L" * 22, unset="U", description="é" * 499))
    settings(dict(lamp, label="L", type="colour", description="é" * 600))  # nor anything on an unknown type
    refused("bad-manifest", settings, dict(lamp, label="L"), dict(lamp, label="L", type="colour"))
    print("[ok] settings")

    # 2c. A file or a link in ${var_plugins}, which an agent knows from 1.6.0, needs a requires.agent that leaves
    # the older agents out; the tool is stricter here than the agent, which would refuse such a package as broken.
    def placing(requirement, files=(), links=()):
        manifest = {"manifestVersion": 1, "id": "org.nbtapp.t", "name": "T", "version": "1.0.0",
                    "contents": [{"path": "payload/a", "size": 1, "sha256": "0" * 64}],
                    "install": {"files": [{"src": "payload/a", "dest": d, "mode": "0644"} for d in files],
                                "links": [{"path": p, "target": "a"} for p in links]}}
        if requirement is not None:
            manifest["requires"] = {"agent": requirement}
        nbtpkg.check_manifest(manifest)
    for where in ({"files": ["${var_plugins}/A/a"]}, {"files": ["${app}/a"], "links": ["${var_plugins}/A/l"]}):
        for requirement in (None, ">=1.5.1", "<2.0.0", "<=1.6.0", ">=1.5.9 <1.6.0", "1.5.1", ">1.5.9"):
            message = refused("bad-manifest", placing, requirement, **where)
            assert "${var_plugins}" in message and "\"requires\": {\"agent\": \">=1.6.0\"}" in message, message
            assert "agent-too-old" in message, message
        for requirement in (">=1.6.0", ">=1.6.0-beta.1", "=1.7.0", ">=1.6.0 <2.0.0", "<2.0.0 >=1.6.0", "1.6.0",
                            ">1.6.0", ">=2.0.0+build.1"):
            placing(requirement, **where)
    # Elsewhere nothing is asked. The numbers are compared as numbers.
    placing(None, files=["${plugins}/A/a", "/opt/x"], links=["${data}/l"])
    assert nbtpkg.agent_requirement_ok(">=1.6.0", (1, 6, 0)) and not nbtpkg.agent_requirement_ok(None, (1, 6, 0))
    assert not nbtpkg.agent_requirement_ok(">=1.10.0", (2, 0, 0)) and nbtpkg.agent_requirement_ok(">=1.10.0", (1, 9, 9))
    print("[ok] requires.agent for ${var_plugins}")

    # 2d. requires.app, the phone app's, which an agent knows from 1.6.0: a range, and the same rule as above.
    def requiring(requires):
        nbtpkg.check_manifest({"manifestVersion": 1, "id": "org.nbtapp.t", "name": "T", "version": "1.0.0",
                               "contents": [], "requires": requires})
    for requirement in (None, ">=1.5.1", "<2.0.0", "1.5.1"):
        requires = {"app": ">=0.8.0"} if requirement is None else {"agent": requirement, "app": ">=0.8.0"}
        message = refused("bad-manifest", requiring, requires)
        assert "requires.app" in message and "\"requires\": {\"agent\": \">=1.6.0\"}" in message, message
        assert "agent-too-old" in message, message
    for app in (">=0.8.0", "<1.0.0", ">=0.8.0-beta.1 <2.0.0", "0.8.0"):
        requiring({"agent": ">=1.6.0", "app": app})
        requiring({"app": app, "agent": ">=1.6.0 <2.0.0"})
    for app in (">=0.8", "", 8, "^0.8.0", None):
        refused("bad-manifest", requiring, {"agent": ">=1.6.0", "app": app})
    refused("unsupported-manifest", requiring, {"agent": ">=1.6.0", "phone": ">=0.8.0"})
    print("[ok] requires.app")

    # 2e. channels, which an agent knows from 1.7.0 and an older one ignores: the same rule again. An apps that names
    # a phone app is a later format's, as an unknown key is; an apps that is no list is broken.
    def channelling(requirement, channel=None):
        manifest = {"manifestVersion": 1, "id": "org.nbtapp.t", "name": "T", "version": "1.0.0", "contents": [],
                    "channels": [channel or {"name": "a", "apps": [], "access": "both"}]}
        if requirement is not None:
            manifest["requires"] = {"agent": requirement}
        nbtpkg.check_manifest(manifest)
    for requirement in (None, ">=1.6.0", "1.6.0", "<2.0.0"):
        message = refused("bad-manifest", channelling, requirement)
        assert "has channels" in message and "\"requires\": {\"agent\": \">=1.7.0\"}" in message, message
    for requirement in (">=1.7.0", ">=1.7.0-beta.1", "1.7.0", ">=1.7.0 <2.0.0"):
        channelling(requirement)
    refused("unsupported-manifest", channelling, ">=1.7.0", {"name": "a", "apps": ["org.example.app"], "access": "both"})
    refused("bad-manifest", channelling, ">=1.7.0", {"name": "a", "apps": "org.example.app", "access": "both"})
    print("[ok] channels")

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
        # What this tool refuses and the agent and the app take: a log in a folder the schema does not know, which
        # they skip, and requires.app with no requires.agent that keeps out the agents that do not know it.
        assert len(vectors["skipped"]) >= 4
        for entry in vectors["skipped"]:
            message = refused(entry["code"], nbtpkg.verify_package, data_of(entry), [public])
            assert entry["names"] in message and ("does not know" in message or "knows from" in message), \
                "%s: %s" % (entry["file"], message)
        assert {e["names"] for e in vectors["skipped"]} == {"logs[1].path", "requires.app", "channels"}
        hello = next(e for e in vectors["good"] if e["file"] == "good/org.nbtapp.hello-1.2.0.nbtpkg")
        assert hello["flags"] == ["runs-scripts"]
        print("[ok] test packages: %d good, %d bad, %d skipped"
              % (len(vectors["good"]), len(vectors["bad"]), len(vectors["skipped"])))

        # Detached signatures: the one given is checked, manifest.sig is not read.
        assert len(vectors["detached"]) >= 7
        for entry in vectors["detached"]:
            data, signature = data_of(entry), bytes.fromhex(entry["signature"])
            try:
                if entry["code"]:
                    refused(entry["code"], nbtpkg.verify_package, data, [public], signature=signature)
                    continue
                info = nbtpkg.verify_package(data, [other, public], signature=signature)
                assert info["key"] == public.hex() and info["signature"] == entry["signature"]
                assert info["manifestSha256"] == hello["manifestSha256"]
                refused("bad-signature", nbtpkg.verify_package, data, [other], signature=signature)
            except AssertionError as e:
                raise AssertionError("%s (%s): %s" % (entry["file"], entry["what"], e))
        print("[ok] detached signatures: %d cases" % len(vectors["detached"]))

        # Risk flags come from the manifest alone.
        assert nbtpkg.risk_flags({"install": {"files": [{"dest": "/etc/x"}], "reboot": "hmi-restart"}}) == \
            ["modifies-system", "needs-reboot"]
        assert nbtpkg.risk_flags({"install": {"files": [{"dest": "${plugins}/A/a", "replaces": {}}]}}) == ["modifies-system"]
        assert nbtpkg.risk_flags({"install": {"links": [{"path": "/bin/x"}]}}) == ["modifies-system"]
        assert nbtpkg.risk_flags({"install": {"files": [{"dest": "${data}/x"}], "hooks": {}}}) == []
        # Both plugin folders are the package's own places; a variable that only starts like one is not.
        assert nbtpkg.risk_flags({"install": {"files": [{"dest": "${plugins}/A/a"}, {"dest": "${var_plugins}/B/"}],
                                              "links": [{"path": "${var_plugins}/B/l"}]}}) == []
        assert nbtpkg.risk_flags({"install": {"files": [{"dest": "${var_pluginsx}/a"}]}}) == ["modifies-system"]
        assert nbtpkg.risk_flags({}) == []
        # A channel whose program the agent starts; one with no start starts nothing.
        assert nbtpkg.risk_flags({"channels": [{"name": "a"}, {"name": "b", "start": {"program": "x"}}]}) == ["starts-program"]
        assert nbtpkg.risk_flags({"channels": [{"name": "a"}]}) == []
        chan = next(e for e in vectors["good"] if e["file"] == "good/org.nbtapp.chan-1.0.0.nbtpkg")
        assert chan["flags"] == ["starts-program"], chan["flags"]

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
        # Signed with k, read with the test package key's detached signature over the same manifest.
        countersigned = nbtpkg.sign(seed, nbtpkg.read_zip(signed)[0]["data"]).hex()
        detached = run("verify", package, "--key", os.path.join(KEYS, "test-package.pub"), "--signature", countersigned,
                       "--json")
        assert detached.returncode == 0 and json.loads(detached.stdout)["signature"] == countersigned
        assert run("verify", package, "--key", pub, "--signature", countersigned).returncode != 0
        assert run("verify", package, "--signature", countersigned.upper()).returncode != 0

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
