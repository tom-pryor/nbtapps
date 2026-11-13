#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""Whether two builds of the app are the same, leaving their signatures aside.

  tools/compare_apks.py A.apk B.apk

Either may be signed or unsigned: the APK Signing Block (v2 and later signatures), with the zeros apksigner puts
before it, and the files of a v1 signature are not compared. That is what makes a build of CI, which is never
signed, comparable with one signed here. There are three answers, each with its exit status:

  0  the same bytes        the two are one file once the signature is taken out, so a signature made for one is
                           a signature of the other as well. A signed APK keeps the unsigned one's bytes only when
                           apksigner is told to leave its layout alone (--alignment-preserved) and to make no v1
                           signature
  3  the same contents     the same files with the same bytes once unpacked, but the APKs are written differently:
                           compressed otherwise, other timestamps or another order. The same code; the lines after
                           this answer say what differs
  1  different contents    the files that differ, are in one only, or are not in the same place

and 2 when an APK can't be read. Only Python's standard library is used.
"""

import re
import struct
import sys
import zipfile

EOCD = b"PK\x05\x06"
SIGNING_BLOCK_MAGIC = b"APK Sig Block 42"
LOCAL_HEADER = b"PK\x03\x04"
# The files of a v1 (JAR) signature. MANIFEST.MF goes with them only where there is one.
V1_SIGNATURE = re.compile(r"META-INF/[^/]+\.(SF|RSA|DSA|EC)")
SHOWN = 20


class Unreadable(Exception):
    pass


class Apk:
    def __init__(self, path):
        self.path = path
        try:
            with open(path, "rb") as source:
                self.data = source.read()
            self.zip = zipfile.ZipFile(path)
        except (OSError, zipfile.BadZipFile) as e:
            raise Unreadable(f"{path}: {e}") from e
        self.entries = self.zip.infolist()
        names = [entry.filename for entry in self.entries]
        if len(set(names)) != len(names):
            raise Unreadable(f"{path}: a file name is in it twice")
        self.by_name = dict(zip(names, self.entries))
        self.v1_signed = any(V1_SIGNATURE.fullmatch(name) for name in names)
        self.unsigned = self._without_signing_block()

    def _without_signing_block(self):
        """The APK's bytes with the APK Signing Block taken out: what it was before it was signed."""
        data = self.data
        end = data.rfind(EOCD, max(0, len(data) - 0xFFFF - 22))
        if end < 0 or len(data) < end + 22:
            raise Unreadable(f"{self.path}: no end of central directory")
        cd_size, cd_offset = struct.unpack_from("<II", data, end + 12)
        if cd_offset == 0xFFFFFFFF or cd_offset + cd_size != end:
            raise Unreadable(f"{self.path}: the central directory is not where the end record says")
        start = cd_offset
        if cd_offset >= 32 and data[cd_offset - 16:cd_offset] == SIGNING_BLOCK_MAGIC:
            (size,) = struct.unpack_from("<Q", data, cd_offset - 24)
            start = cd_offset - size - 8
            if start < 0 or struct.unpack_from("<Q", data, start)[0] != size:
                raise Unreadable(f"{self.path}: the APK Signing Block's two sizes differ")
            # apksigner puts the block at a multiple of 4096 bytes, with zeros before it: they go with it.
            entries_end = max((self._entry_end(entry) for entry in self.entries), default=0)
            if entries_end < start and not data[entries_end:start].strip(b"\0"):
                start = entries_end
        record = bytearray(data[end:])
        struct.pack_into("<I", record, 16, start)
        return data[:start] + data[cd_offset:end] + bytes(record)

    def _entry_end(self, entry):
        """Where the entry's local header, its data and any data descriptor after it end."""
        offset = entry.header_offset
        name_length, extra_length = struct.unpack_from("<HH", self.data, offset + 26)
        end = offset + 30 + name_length + extra_length + entry.compress_size
        if entry.flag_bits & 0x08:
            end += 16 if self.data[end:end + 4] == b"PK\x07\x08" else 12
        return end

    def raw(self, entry):
        """The entry's bytes as stored: compressed, if it is."""
        offset = entry.header_offset
        if self.data[offset:offset + 4] != LOCAL_HEADER:
            raise Unreadable(f"{self.path}: no local header for {entry.filename}")
        name_length, extra_length = struct.unpack_from("<HH", self.data, offset + 26)
        start = offset + 30 + name_length + extra_length
        return self.data[start:start + entry.compress_size]

    def local_extra(self, entry):
        offset = entry.header_offset
        name_length, extra_length = struct.unpack_from("<HH", self.data, offset + 26)
        return self.data[offset + 30 + name_length:offset + 30 + name_length + extra_length]

    def compared_names(self, other):
        """The names that count: without a v1 signature's files, and its MANIFEST.MF where either has one."""
        v1 = self.v1_signed or other.v1_signed
        return [
            entry.filename
            for entry in self.entries
            if not V1_SIGNATURE.fullmatch(entry.filename) and not (v1 and entry.filename == "META-INF/MANIFEST.MF")
        ]


def content_differences(a, b):
    names_a, names_b = a.compared_names(b), b.compared_names(a)
    in_a, in_b = set(names_a), set(names_b)
    found = [f"only in A: {name}" for name in names_a if name not in in_b]
    found += [f"only in B: {name}" for name in names_b if name not in in_a]
    for name in names_a:
        if name in in_b:
            content_a, content_b = a.zip.read(name), b.zip.read(name)
            if content_a != content_b:
                found.append(f"differs: {name} ({len(content_a)} and {len(content_b)} bytes unpacked)")
    return found


def layout_differences(a, b):
    """Why the bytes differ when the contents don't: how each file is stored."""
    names_a, names_b = a.compared_names(b), b.compared_names(a)
    found = []
    if a.v1_signed or b.v1_signed:
        where = "both" if a.v1_signed and b.v1_signed else "A" if a.v1_signed else "B"
        found.append(f"a v1 (JAR) signature is in {where}")
    if names_a != names_b:
        found.append("the files are in another order")
    for name in names_a:
        x, y = a.by_name[name], b.by_name[name]
        what = []
        if x.compress_type != y.compress_type:
            what.append(f"compression method {x.compress_type} and {y.compress_type}")
        elif a.raw(x) != b.raw(y):
            what.append(f"compressed otherwise ({x.compress_size} and {y.compress_size} bytes)")
        if x.date_time != y.date_time:
            what.append(f"timestamps {x.date_time} and {y.date_time}")
        if a.local_extra(x) != b.local_extra(y) or x.extra != y.extra:
            what.append("extra fields (alignment)")
        if (x.external_attr, x.create_system, x.flag_bits) != (y.external_attr, y.create_system, y.flag_bits):
            what.append("attributes or flags")
        if x.comment != y.comment:
            what.append("comment")
        if what:
            found.append(f"{name}: " + ", ".join(what))
    if a.zip.comment != b.zip.comment:
        found.append("the APK's comment")
    if not found:
        found.append("the APKs are laid out otherwise (offsets or headers), each file the same")
    return found


def show(lines):
    for line in lines[:SHOWN]:
        print(f"  {line}")
    if len(lines) > SHOWN:
        print(f"  and {len(lines) - SHOWN} more")


def main(args):
    if len(args) != 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    try:
        a, b = Apk(args[0]), Apk(args[1])
        print(f"A: {a.path}\nB: {b.path}")
        if a.unsigned == b.unsigned:
            print("the same bytes, leaving the signatures aside")
            return 0
        differences = content_differences(a, b)
        if differences:
            print("different contents:")
            show(differences)
            return 1
        print("the same contents, but not the same bytes:")
        show(layout_differences(a, b))
        return 3
    except Unreadable as e:
        print(f"compare_apks.py: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
