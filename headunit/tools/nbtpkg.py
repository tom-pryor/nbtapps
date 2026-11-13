#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""Builds, signs and verifies .nbtpkg packages. Needs python3 and, for anything with a key, the `cryptography` module.

  nbtpkg.py keygen NAME [-o DIR]               NAME.key (private: keep it offline) and NAME.pub
  nbtpkg.py build DIR [-o OUTDIR] [--key KEY]  DIR -> OUTDIR/<id>-<version>.nbtpkg, signed if a key is given
  nbtpkg.py sign PKG --key KEY [-o OUT]        adds or replaces manifest.sig (in place without -o)
  nbtpkg.py sign-file FILE --key KEY           FILE.sig: a detached signature, as index.json.sig is
  nbtpkg.py verify PKG [--key PUB]... [--unsigned] [--signature HEX] [--json]

A package source DIR holds manifest.json without `contents`, and every other file as it sits in the archive
(payload/..., scripts/..., icon.png). `build` fills in `contents`, checks the manifest and writes the archive. The
same input gives the same bytes.

`verify` applies the checks the Android app and the head-unit agent apply, and names a failure with the code the
agent reports for it: bad-archive, bad-signature, bad-manifest or unsupported-manifest. Without --key the signature is
not checked, and the output says so.

The format, as the plan document sets it:
  - a zip archive under a strict profile: manifest.json first, manifest.sig second, stored or deflated entries one
    after another in directory order, local headers equal to the central directory, no data descriptors, zip64,
    encryption, duplicate names or archive comment, and no name longer than 255 bytes
  - manifest.sig is the 64-byte Ed25519 signature over the exact bytes of manifest.json
  - a catalogue may instead give a detached signature over those bytes, for a package whose author signed it with a
    key of their own (verify --signature): the app and the agent then check that one and do not read manifest.sig
  - manifest.json follows the strict JSON profile and spec/manifest.schema.json, and its `contents` lists every
    other file with its size and the SHA-256 of its uncompressed bytes

Key files are text: one line of 64 hex characters (the 32-byte Ed25519 seed or public key); lines starting with #
are comments. A key's fingerprint is the first 16 hex characters of the SHA-256 of the 32 public key bytes.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import zlib

MANIFEST = "manifest.json"
SIGNATURE = "manifest.sig"
SIGNATURE_BYTES = 64
MAX_MANIFEST_BYTES = 1 << 20
MAX_PATH_BYTES = 255
MAX_DEPTH = 16
MAX_INT = (1 << 53) - 1
SUPPORTED_MANIFEST_VERSION = 1

STORED, DEFLATED = 0, 8
# Deflating these again gains nothing, and the plugin DB zips must stay as the HMI expects them.
ALREADY_COMPRESSED = (".zip", ".gz", ".xz", ".bz2", ".png", ".jpg", ".jpeg")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA_PATH = os.path.join(ROOT, "spec", "manifest.schema.json")
OWN_DIRS = ("${app}/", "${data}/", "${plugins}/")


class PkgError(Exception):
    """A package or manifest that is refused. `code` is the agent's code for the refusal."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


# ---- strict JSON profile --------------------------------------------------------------------------------------

def parse_json(data):
    """Parses `data` (bytes) under the strict JSON profile shared by manifest.json and the protocol's messages:
    UTF-8 with no byte-order mark, one object at the top level, no duplicate key, integers only (no fraction or
    exponent, at most 2^53 - 1 in size), no unpaired surrogate escape, at most 16 levels. Raises ValueError."""
    if data.startswith(b"\xef\xbb\xbf"):
        raise ValueError("starts with a byte-order mark")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as e:
        raise ValueError("is not valid UTF-8 (byte %d)" % e.start)

    def pairs(items):
        obj = {}
        for key, value in items:
            if key in obj:
                raise ValueError("has the key %r twice in one object" % key)
            obj[key] = value
        return obj

    def integer(digits):
        if len(digits) > 17 or abs(int(digits)) > MAX_INT:
            raise ValueError("has a number out of range: %s" % digits[:24])
        return int(digits)

    def refuse(token):
        raise ValueError("has a number that is not an integer: %s" % token[:24])

    try:
        value = json.loads(text, object_pairs_hook=pairs, parse_int=integer, parse_float=refuse,
                           parse_constant=refuse)
    except RecursionError:
        raise ValueError("is nested too deeply")
    except json.JSONDecodeError as e:
        raise ValueError("is not valid JSON: %s" % e)
    if not isinstance(value, dict):
        raise ValueError("is not an object at the top level")

    def walk(node, level):
        if isinstance(node, str):
            try:
                node.encode("utf-8")
            except UnicodeEncodeError:
                raise ValueError("has an unpaired surrogate escape in a string")
        elif isinstance(node, (dict, list)):
            if level > MAX_DEPTH:
                raise ValueError("is nested more than %d levels deep" % MAX_DEPTH)
            for child in (node.items() if isinstance(node, dict) else enumerate(node)):
                if isinstance(node, dict):
                    walk(child[0], level)
                walk(child[1], level + 1)

    walk(value, 1)
    return value


# ---- the subset of JSON Schema that spec/manifest.schema.json uses ---------------------------------------------

_SCHEMA_KEYWORDS = {"$schema", "$defs", "$ref", "title", "description", "type", "required", "properties",
                    "additionalProperties", "items", "minItems", "minLength", "maxLength", "pattern", "minimum",
                    "maximum", "const", "enum"}
_patterns = {}


class SchemaError(ValueError):
    def __init__(self, where, message, unknown_key=False):
        super().__init__("%s %s" % (where, message))
        self.unknown_key = unknown_key


def _pattern(text):
    if text not in _patterns:
        # In Python `$` also matches before a final newline; in the schema's dialect it does not.
        anchored = text[:-1] + r"\Z" if text.endswith("$") and not text.endswith(r"\$") else text
        _patterns[text] = re.compile(anchored)
    return _patterns[text]


def check_schema_keywords(node):
    """Fails loudly on a schema keyword `validate` does not implement, so no rule is skipped in silence."""
    if isinstance(node, dict):
        unknown = set(node) - _SCHEMA_KEYWORDS
        if unknown:
            raise RuntimeError("manifest schema uses unsupported keyword(s): %s" % ", ".join(sorted(unknown)))
        for key in ("properties", "$defs"):
            for child in node.get(key, {}).values():
                check_schema_keywords(child)
        if isinstance(node.get("items"), dict):
            check_schema_keywords(node["items"])


def validate(value, schema, root, where):
    if "$ref" in schema:
        return validate(value, root["$defs"][schema["$ref"].rsplit("/", 1)[1]], root, where)
    if "const" in schema and (type(value) is not type(schema["const"]) or value != schema["const"]):
        raise SchemaError(where, "must be %r" % (schema["const"],))
    if "enum" in schema and (not isinstance(value, str) or value not in schema["enum"]):
        raise SchemaError(where, "must be one of: %s" % ", ".join(schema["enum"]))
    kind = schema.get("type")
    if kind == "object":
        if not isinstance(value, dict):
            raise SchemaError(where, "must be an object")
        for key in schema.get("required", []):
            if key not in value:
                raise SchemaError(where, "has no %r" % key)
        properties = schema.get("properties", {})
        for key, child in value.items():
            if key in properties:
                validate(child, properties[key], root, "%s.%s" % (where, key))
            elif schema.get("additionalProperties") is False:
                raise SchemaError(where, "has the unknown key %r" % key, unknown_key=True)
    elif kind == "array":
        if not isinstance(value, list):
            raise SchemaError(where, "must be an array")
        if len(value) < schema.get("minItems", 0):
            raise SchemaError(where, "needs at least %d item(s)" % schema["minItems"])
        for i, child in enumerate(value):
            validate(child, schema.get("items", {}), root, "%s[%d]" % (where, i))
    elif kind == "string":
        if not isinstance(value, str):
            raise SchemaError(where, "must be a string")
        if len(value) < schema.get("minLength", 0) or len(value) > schema.get("maxLength", len(value)):
            raise SchemaError(where, "has a length that is not allowed")
        if "pattern" in schema and not _pattern(schema["pattern"]).search(value):
            raise SchemaError(where, "has a form that is not allowed: %r" % value[:80])
    elif kind == "integer":
        if not isinstance(value, int) or isinstance(value, bool):
            raise SchemaError(where, "must be an integer")
        if value < schema.get("minimum", value) or value > schema.get("maximum", value):
            raise SchemaError(where, "is out of range: %d" % value)
    elif kind == "boolean":
        if not isinstance(value, bool):
            raise SchemaError(where, "must be true or false")
    elif kind is not None:
        raise RuntimeError("manifest schema uses unsupported type %r" % kind)


_schema = None


def manifest_schema():
    global _schema
    if _schema is None:
        with open(SCHEMA_PATH, "rb") as f:
            _schema = json.load(f)
        check_schema_keywords(_schema)
    return _schema


# ---- manifest --------------------------------------------------------------------------------------------------

def check_manifest(manifest):
    """Checks a parsed manifest against the schema and the cross-field rules. Raises PkgError."""
    version = manifest.get("manifestVersion")
    if not isinstance(version, int) or isinstance(version, bool):
        raise PkgError("bad-manifest", "manifest has no integer manifestVersion")
    if version != SUPPORTED_MANIFEST_VERSION:
        raise PkgError("unsupported-manifest", "manifestVersion %d is not supported" % version)
    try:
        validate(manifest, manifest_schema(), manifest_schema(), "manifest")
    except SchemaError as e:
        raise PkgError("unsupported-manifest" if e.unknown_key else "bad-manifest", str(e))

    def bad(message):
        raise PkgError("bad-manifest", "manifest " + message)

    paths = set()
    for content in manifest["contents"]:
        path = content["path"]
        if path in (MANIFEST, SIGNATURE):
            bad("lists %s in contents" % path)
        if len(path.encode("utf-8")) > MAX_PATH_BYTES:
            bad("lists a path longer than %d bytes: %s" % (MAX_PATH_BYTES, path))
        if path in paths:
            bad("lists %s twice" % path)
        paths.add(path)

    install = manifest.get("install", {})
    placed = set()

    def place(dest):
        if len(dest.encode("utf-8")) > MAX_PATH_BYTES:
            bad("places a path longer than %d bytes: %s" % (MAX_PATH_BYTES, dest))
        if dest in placed:
            bad("places %s twice" % dest)
        placed.add(dest)

    for entry in install.get("files", []):
        src, dest = entry["src"], entry["dest"]
        if src.endswith("/") != dest.endswith("/"):
            bad("maps %s to %s: a folder goes to a folder and a file to a file" % (src, dest))
        if src.endswith("/"):
            under = sorted(p for p in paths if p.startswith(src))
            if not under:
                bad("installs the folder %s, which holds nothing listed in contents" % src)
            if "config" in entry or "replaces" in entry:
                bad("sets config or replaces on the folder %s; they need a single file" % src)
            for path in under:
                place(dest + path[len(src):])
        else:
            if src not in paths:
                bad("installs %s, which is not listed in contents" % src)
            place(dest)
    for link in install.get("links", []):
        place(link["path"])
    for name, hook in sorted(install.get("hooks", {}).items()):
        if hook["script"] not in paths:
            bad("names %s as %s, which is not listed in contents" % (hook["script"], name))
    services = set()
    for service in install.get("serialServices", []):
        if service["name"] in services:
            bad("declares the serial service %s twice" % service["name"])
        services.add(service["name"])


def load_manifest(data):
    """manifest.json bytes -> the checked manifest. Raises PkgError."""
    if len(data) > MAX_MANIFEST_BYTES:
        raise PkgError("bad-manifest", "manifest is larger than %d bytes" % MAX_MANIFEST_BYTES)
    try:
        manifest = parse_json(data)
    except ValueError as e:
        raise PkgError("bad-manifest", "manifest %s" % e)
    check_manifest(manifest)
    return manifest


def risk_flags(manifest):
    """The flags the app shows before an install, worked out from the manifest so a source cannot understate them."""
    install = manifest.get("install", {})
    files = install.get("files", [])
    flags = []
    if install.get("hooks"):
        flags.append("runs-scripts")
    placed = [f["dest"] for f in files] + [link["path"] for link in install.get("links", [])]
    if any(not p.startswith(OWN_DIRS) for p in placed) or any("replaces" in f for f in files):
        flags.append("modifies-system")
    if install.get("reboot", "none") != "none":
        flags.append("needs-reboot")
    return flags


# ---- keys and signatures ---------------------------------------------------------------------------------------

def _ed25519():
    try:
        from cryptography.hazmat.primitives.asymmetric import ed25519
    except ImportError:
        sys.exit("nbtpkg: this needs the python module `cryptography`")
    return ed25519


def public_key(seed):
    from cryptography.hazmat.primitives import serialization
    key = _ed25519().Ed25519PrivateKey.from_private_bytes(seed)
    return key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def sign(seed, message):
    return _ed25519().Ed25519PrivateKey.from_private_bytes(seed).sign(message)


def signature_ok(public, message, signature):
    from cryptography.exceptions import InvalidSignature
    try:
        _ed25519().Ed25519PublicKey.from_public_bytes(public).verify(signature, message)
        return True
    except (InvalidSignature, ValueError):
        return False


def fingerprint(public):
    digest = hashlib.sha256(public).hexdigest()[:16]
    return " ".join(digest[i:i + 4] for i in range(0, 16, 4))


def read_key(path):
    with open(path, "r", encoding="ascii") as f:
        lines = [line.strip() for line in f if line.strip() and not line.startswith("#")]
    if len(lines) != 1 or not re.fullmatch(r"[0-9a-f]{64}", lines[0]):
        sys.exit("nbtpkg: %s is not a key file (one line of 64 hex characters)" % path)
    return bytes.fromhex(lines[0])


def write_key(path, raw, comment, private):
    # O_EXCL: a key is never overwritten. A private key is readable by its owner only.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600 if private else 0o644)
    with os.fdopen(fd, "w", encoding="ascii") as f:
        f.write("# %s\n%s\n" % (comment, raw.hex()))


# ---- zip container ---------------------------------------------------------------------------------------------

_LOCAL = struct.Struct("<IHHHHHIIIHH")
_CENTRAL = struct.Struct("<IHHHHHHIIIHHHHHII")
_END = struct.Struct("<IHHHHIIH")
_LOCAL_SIG, _CENTRAL_SIG, _END_SIG = 0x04034B50, 0x02014B50, 0x06054B50
_DOS_DATE = 0x0021        # 1980-01-01: every archive gets the same timestamps
_FLAG_ENCRYPTED = 0x2041  # bit 0 encrypted, bit 6 strong encryption, bit 13 masked directory
_FLAG_DESCRIPTOR = 0x0008
_FLAG_UTF8 = 0x0800


def make_member(name, data, store=None):
    """One archive member. Deflated at the highest level unless that gains nothing or the type is compressed already."""
    raw, method = data, STORED
    if store is None:
        store = name.lower().endswith(ALREADY_COMPRESSED)
    if not store and data:
        packer = zlib.compressobj(9, zlib.DEFLATED, -15)
        packed = packer.compress(data) + packer.flush()
        if len(packed) < len(data):
            raw, method = packed, DEFLATED
    return {"name": name.encode("utf-8"), "method": method, "crc": zlib.crc32(data) & 0xFFFFFFFF,
            "usize": len(data), "raw": raw}


def write_zip(members, comment=b"", trailer=b"", end=None):
    """Writes members in order. Each is a dict from make_member. The tests bend the layout with extra keys:
    `local` / `central` (dicts that override name, flags, method, crc, csize or usize in that header only), `pre`
    (bytes in front of the local header), `descriptor` (bytes after the data) and `offset` (what the directory
    records); `end` overrides count, size or offset in the end record."""
    out = bytearray()
    directory = bytearray()
    for member in members:
        out += member.get("pre", b"")
        offset = len(out)
        for header in ("local", "central"):
            field = dict(member, csize=len(member["raw"]))
            field.setdefault("flags", _FLAG_UTF8 if any(b > 0x7F for b in member["name"]) else 0)
            field.update(member.get(header, {}))
            name = field["name"]
            if header == "local":
                out += _LOCAL.pack(_LOCAL_SIG, 20, field["flags"], field["method"], 0, _DOS_DATE, field["crc"],
                                   field["csize"], field["usize"], len(name), 0) + name
                out += member["raw"] + member.get("descriptor", b"")
            else:
                directory += _CENTRAL.pack(_CENTRAL_SIG, 20, 20, field["flags"], field["method"], 0, _DOS_DATE,
                                           field["crc"], field["csize"], field["usize"], len(name), 0, 0, 0, 0, 0,
                                           member.get("offset", offset)) + name
    record = {"count": len(members), "size": len(directory), "offset": len(out)}
    record.update(end or {})
    out += directory
    out += _END.pack(_END_SIG, 0, 0, record["count"], record["count"], record["size"], record["offset"], len(comment))
    return bytes(out + comment + trailer)


def _valid_name(name):
    return (not any(c == "\\" or c < " " for c in name)
            and all(part not in ("", ".", "..") for part in name.split("/")))


def read_zip(data):
    """Reads an archive under the container profile. Returns its entries in order, each a dict with `name`, `dir`,
    `data` (the uncompressed bytes) and what write_zip needs to copy it. Raises PkgError("bad-archive")."""

    def bad(message):
        raise PkgError("bad-archive", message)

    if len(data) < _END.size:
        bad("the file is too short to be a package")
    # With no archive comment the end record is exactly the last 22 bytes.
    sig, disk, dir_disk, on_disk, count, dir_size, dir_offset, comment = _END.unpack_from(data, len(data) - _END.size)
    if sig != _END_SIG:
        bad("the file is not a package, is cut short, or has an archive comment or bytes after its end")
    if count == 0xFFFF or dir_size == 0xFFFFFFFF or dir_offset == 0xFFFFFFFF:
        bad("the archive uses zip64")
    if disk or dir_disk or on_disk != count or comment:
        bad("the archive layout is not allowed")
    if dir_offset + dir_size != len(data) - _END.size:
        bad("the archive's file list is damaged")

    entries = []
    names = set()
    at = dir_offset
    for _ in range(count):
        if at + _CENTRAL.size > dir_offset + dir_size:
            bad("the archive's file list is damaged")
        (sig, _made, _need, flags, method, _time, _date, crc, csize, usize, name_len, extra_len, note_len, start_disk,
         _internal, _external, offset) = _CENTRAL.unpack_from(data, at)
        at += _CENTRAL.size
        if sig != _CENTRAL_SIG or at + name_len + extra_len + note_len > dir_offset + dir_size:
            bad("the archive's file list is damaged")
        name_bytes = data[at:at + name_len]
        at += name_len + extra_len + note_len
        if name_len > MAX_PATH_BYTES:
            bad("a path in the archive is longer than %d bytes" % MAX_PATH_BYTES)
        try:
            name = name_bytes.decode("utf-8")
        except UnicodeDecodeError:
            bad("a file name is not valid UTF-8")
        is_dir = name.endswith("/")
        if not _valid_name(name[:-1] if is_dir else name):
            bad("a path is not allowed: %r" % name)
        if name in names:
            bad("%s is in the archive twice" % name)
        names.add(name)
        if flags & _FLAG_ENCRYPTED:
            bad("%s is encrypted" % name)
        if flags & _FLAG_DESCRIPTOR:
            bad("%s uses a data descriptor" % name)
        if method not in (STORED, DEFLATED):
            bad("%s uses a compression that is not allowed" % name)
        if csize == 0xFFFFFFFF or usize == 0xFFFFFFFF or offset == 0xFFFFFFFF:
            bad("the archive uses zip64")
        if start_disk:
            bad("the archive layout is not allowed")
        if is_dir and (method != STORED or csize or usize):
            bad("the folder entry %s holds data" % name)
        entries.append({"name": name, "name_bytes": name_bytes, "dir": is_dir, "flags": flags, "method": method,
                        "crc": crc, "csize": csize, "usize": usize, "offset": offset})
    if at != dir_offset + dir_size:
        bad("the archive's file list is damaged")

    # Entries sit one after another in directory order, from byte 0 up to the directory. A reader that walks the
    # local headers of a stream and a reader that starts from the directory then see the same files.
    expected = 0
    for entry in entries:
        name = entry["name"]
        if entry["offset"] != expected:
            bad("%s is not where the entry before it ends" % name)
        if expected + _LOCAL.size > dir_offset:
            bad("%s is damaged" % name)
        sig, _need, flags, method, _time, _date, crc, csize, usize, name_len, extra_len = _LOCAL.unpack_from(data, expected)
        start = expected + _LOCAL.size + name_len + extra_len
        if sig != _LOCAL_SIG or start + entry["csize"] > dir_offset:
            bad("%s is damaged" % name)
        local_name = data[expected + _LOCAL.size:expected + _LOCAL.size + name_len]
        if ((flags, method, crc, csize, usize, local_name)
                != (entry["flags"], entry["method"], entry["crc"], entry["csize"], entry["usize"], entry["name_bytes"])):
            bad("%s is described in two different ways" % name)
        raw = data[start:start + entry["csize"]]
        expected = start + entry["csize"]
        if method == STORED:
            if entry["csize"] != entry["usize"]:
                bad("%s is damaged" % name)
            plain = raw
        else:
            unpacker = zlib.decompressobj(-15)
            try:
                plain = unpacker.decompress(raw, entry["usize"] + 1)
            except zlib.error:
                bad("%s is damaged" % name)
            if not unpacker.eof or unpacker.unused_data or len(plain) != entry["usize"]:
                bad("%s is damaged" % name)
        if zlib.crc32(plain) & 0xFFFFFFFF != entry["crc"]:
            bad("%s is damaged" % name)
        entry["raw"], entry["data"] = raw, plain
    if expected != dir_offset:
        bad("the archive holds bytes that belong to no file")
    return entries


# ---- packages --------------------------------------------------------------------------------------------------

def verify_package(data, keys=None, allow_unsigned=False, signature=None):
    """Checks a whole package. `keys` is a list of trusted public keys; None skips the signature check. Returns
    what the index and a job.plan offer say about the package. Raises PkgError.

    `signature` is a detached signature over the manifest (64 bytes), as a catalogue gives one for a package whose
    author signed it with a key of their own. With it, that signature is the one checked: manifest.sig is not read,
    may be signed by any key or missing, and only has to be the second entry if it is there."""
    entries = read_zip(data)
    if not entries or entries[0]["name"] != MANIFEST:
        raise PkgError("bad-archive", "the package does not start with its manifest")
    manifest_bytes = entries[0]["data"]
    if len(manifest_bytes) > MAX_MANIFEST_BYTES:
        raise PkgError("bad-manifest", "manifest is larger than %d bytes" % MAX_MANIFEST_BYTES)
    signed = len(entries) > 1 and entries[1]["name"] == SIGNATURE
    if not signed and any(e["name"] == SIGNATURE for e in entries):
        raise PkgError("bad-archive", "manifest.sig is not the second entry")

    # The signature is checked before the manifest is parsed, so only authenticated bytes reach the parser.
    key = None
    if signature is not None:
        if len(signature) != SIGNATURE_BYTES:
            raise PkgError("bad-signature", "the signature given is not %d bytes" % SIGNATURE_BYTES)
    elif signed:
        signature = entries[1]["data"]
        if len(signature) != SIGNATURE_BYTES:
            raise PkgError("bad-signature", "manifest.sig is not %d bytes" % SIGNATURE_BYTES)
    elif not allow_unsigned:
        raise PkgError("bad-signature", "the package is not signed")
    if signature is not None and keys is not None:
        key = next((k for k in keys if signature_ok(k, manifest_bytes, signature)), None)
        if key is None:
            raise PkgError("bad-signature", "no trusted key verifies the signature")

    manifest = load_manifest(manifest_bytes)

    files = {e["name"]: e for e in entries[2 if signed else 1:] if not e["dir"]}
    for content in manifest["contents"]:
        entry = files.pop(content["path"], None)
        if entry is None:
            raise PkgError("bad-archive", "%s is listed in the manifest but is not in the package" % content["path"])
        if len(entry["data"]) != content["size"] or hashlib.sha256(entry["data"]).hexdigest() != content["sha256"]:
            raise PkgError("bad-archive", "%s is not the file the manifest describes" % content["path"])
    if files:
        raise PkgError("bad-archive", "%s is in the package but not in its manifest" % sorted(files)[0])

    info = {"id": manifest["id"], "version": manifest["version"], "name": manifest["name"],
            "sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
            "manifestSha256": hashlib.sha256(manifest_bytes).hexdigest(), "flags": risk_flags(manifest)}
    if signature is not None:
        info["signature"] = signature.hex()
    if key is not None:
        info["key"] = key.hex()
    return info


def build_package(src_dir, seed=None):
    """Package source folder -> (manifest, archive bytes). Signed when `seed` is given."""
    try:
        with open(os.path.join(src_dir, MANIFEST), "rb") as f:
            manifest = parse_json(f.read())
    except ValueError as e:
        raise PkgError("bad-manifest", "%s %s" % (os.path.join(src_dir, MANIFEST), e))

    files = {}
    for folder, subfolders, names in os.walk(src_dir):
        subfolders.sort()
        for name in names:
            full = os.path.join(folder, name)
            path = os.path.relpath(full, src_dir).replace(os.sep, "/")
            if path in (MANIFEST, SIGNATURE):
                continue
            if os.path.islink(full) or not os.path.isfile(full):
                raise PkgError("bad-archive", "%s is not a regular file; declare links in the manifest" % full)
            with open(full, "rb") as f:
                files[path] = f.read()
        for name in subfolders:
            if os.path.islink(os.path.join(folder, name)):
                raise PkgError("bad-archive", "%s is a link; declare links in the manifest" % os.path.join(folder, name))
    order = sorted(files, key=lambda p: p.encode("utf-8"))
    contents = [{"path": p, "size": len(files[p]), "sha256": hashlib.sha256(files[p]).hexdigest()} for p in order]

    # `contents` goes in front of `install`; every other key keeps the place the author gave it.
    manifest.pop("contents", None)
    ordered = {}
    for key, value in manifest.items():
        if key == "install":
            ordered["contents"] = contents
        ordered[key] = value
    ordered.setdefault("contents", contents)
    check_manifest(ordered)

    manifest_bytes = (json.dumps(ordered, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    if len(manifest_bytes) > MAX_MANIFEST_BYTES:
        raise PkgError("bad-manifest", "manifest is larger than %d bytes" % MAX_MANIFEST_BYTES)
    members = [make_member(MANIFEST, manifest_bytes)]
    if seed is not None:
        members.append(make_member(SIGNATURE, sign(seed, manifest_bytes), store=True))
    members += [make_member(p, files[p]) for p in order]
    return ordered, write_zip(members)


def sign_package(data, seed):
    """Adds manifest.sig as the second entry, replacing one that is there. Every other entry is copied as it is."""
    verify_package(data, keys=None, allow_unsigned=True)  # a broken package is never signed
    entries = [e for e in read_zip(data) if e["name"] != SIGNATURE]
    members = [{"name": e["name_bytes"], "method": e["method"], "crc": e["crc"], "usize": e["usize"],
                "raw": e["raw"], "flags": e["flags"]} for e in entries]
    members.insert(1, make_member(SIGNATURE, sign(seed, entries[0]["data"]), store=True))
    return write_zip(members)


# ---- command line ----------------------------------------------------------------------------------------------

def _write(path, data):
    with open(path, "wb") as f:
        f.write(data)


def cmd_keygen(args):
    seed = os.urandom(32)
    public = public_key(seed)
    base = os.path.join(args.out, args.name)
    try:
        write_key(base + ".key", seed, "%s: Ed25519 PRIVATE key. Keep it offline." % args.name, private=True)
        write_key(base + ".pub", public, "%s: Ed25519 public key, fingerprint %s" % (args.name, fingerprint(public)),
                  private=False)
    except FileExistsError as e:
        sys.exit("nbtpkg: %s exists; a key is never overwritten" % e.filename)
    print("%s.key  private key" % base)
    print("%s.pub  public key, fingerprint %s" % (base, fingerprint(public)))


def cmd_build(args):
    manifest, data = build_package(args.dir, read_key(args.key) if args.key else None)
    path = os.path.join(args.out, "%s-%s.nbtpkg" % (manifest["id"], manifest["version"]))
    _write(path, data)
    print("%s  %d bytes, %s" % (path, len(data), "signed" if args.key else "NOT signed"))


def cmd_sign(args):
    with open(args.pkg, "rb") as f:
        data = sign_package(f.read(), read_key(args.key))
    _write(args.out or args.pkg, data)
    print("%s  signed, key %s" % (args.out or args.pkg, fingerprint(public_key(read_key(args.key)))))


def cmd_sign_file(args):
    with open(args.file, "rb") as f:
        _write(args.file + ".sig", sign(read_key(args.key), f.read()))
    print("%s.sig  key %s" % (args.file, fingerprint(public_key(read_key(args.key)))))


def _signature_hex(text):
    if not re.fullmatch(r"[0-9a-f]{%d}" % (SIGNATURE_BYTES * 2), text):
        raise argparse.ArgumentTypeError("a signature is %d lowercase hex characters" % (SIGNATURE_BYTES * 2))
    return bytes.fromhex(text)


def cmd_verify(args):
    keys = [read_key(k) for k in args.key] if args.key else None
    with open(args.pkg, "rb") as f:
        info = verify_package(f.read(), keys, allow_unsigned=args.unsigned, signature=args.signature)
    if args.json:
        print(json.dumps(info, indent=2))
        return
    print("%s %s  \"%s\"" % (info["id"], info["version"], info["name"]))
    print("archive    %s  %d bytes" % (info["sha256"], info["size"]))
    print("manifest   %s" % info["manifestSha256"])
    if "key" in info:
        print("signature  ok, key %s" % fingerprint(bytes.fromhex(info["key"])))
    elif "signature" in info:
        print("signature  present, NOT checked (no --key given)")
    else:
        print("signature  none: the package is NOT signed")
    print("flags      %s" % (", ".join(info["flags"]) or "none"))


def main(argv=None):
    parser = argparse.ArgumentParser(prog="nbtpkg.py", description="Build, sign and verify .nbtpkg packages.")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("keygen", help="make an Ed25519 key pair")
    p.add_argument("name")
    p.add_argument("-o", "--out", default=".", help="folder for NAME.key and NAME.pub")
    p.set_defaults(run=cmd_keygen)

    p = sub.add_parser("build", help="package source folder -> .nbtpkg")
    p.add_argument("dir")
    p.add_argument("-o", "--out", default=".", help="folder for the archive")
    p.add_argument("--key", help="private key file; without it the archive is not signed")
    p.set_defaults(run=cmd_build)

    p = sub.add_parser("sign", help="add or replace manifest.sig")
    p.add_argument("pkg")
    p.add_argument("--key", required=True, help="private key file")
    p.add_argument("-o", "--out", help="write here instead of in place")
    p.set_defaults(run=cmd_sign)

    p = sub.add_parser("sign-file", help="detached signature of any file -> FILE.sig")
    p.add_argument("file")
    p.add_argument("--key", required=True, help="private key file")
    p.set_defaults(run=cmd_sign_file)

    p = sub.add_parser("verify", help="check a package as the app and the agent do")
    p.add_argument("pkg")
    p.add_argument("--key", action="append", help="trusted public key file; may be given more than once")
    p.add_argument("--unsigned", action="store_true", help="accept a package with no manifest.sig")
    p.add_argument("--signature", type=_signature_hex, metavar="HEX",
                   help="a detached signature over the manifest (128 hex characters), checked instead of manifest.sig")
    p.add_argument("--json", action="store_true", help="print the result as JSON")
    p.set_defaults(run=cmd_verify)

    args = parser.parse_args(argv)
    try:
        args.run(args)
    except PkgError as e:
        sys.exit("nbtpkg: %s: %s" % (e.code, e))
    except OSError as e:
        sys.exit("nbtpkg: %s" % e)


if __name__ == "__main__":
    main()
