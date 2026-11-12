#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""`make test-protocol`: reads spec/protocol/*.vectors with a frame codec of its own. The agent's tests write those
files with the agent's codec, so they cannot show that the codec is right; this can. It also reads
spec/protocol/constants.txt, which each side's own test compares with its code. Needs python3 only.

  1. frames.vectors: every good frame is what the framing rules give for its fields, decodes back to them, and cut
     off anywhere is "not yet"; every bad line is refused
  2. session.vectors: every line is one whole frame; every message is inside the strict JSON profile and is a
     request, a response or an event; each side numbers its requests from 1; every request has exactly one answer;
     the events of a job are numbered from 1 with no gap; and the DATA frames of each stream add up to the size its
     blob.fetch reply gave, ending on the frame that has the last-frame flag; the session has a pairing.issue
     that gives a key and an `auth` whose two proofs are the ones that key gives, worked out here from the bytes
     of the two hello payloads; until that `auth` is done the agent answers nothing but hello, ping, auth and
     pairing.issue with a result; and the session ends with the agent's link.closing, with a reason that is one
     of the five there are, and nothing after it
  3. constants.txt: every line is a name and a value, the names are the ones known here and each is there once,
     the numbers are numbers and the UUID a UUID, the serial frame lies between the least and the most a frame
     may be, the silence allowed has room for a ping and its answer, and the time a closed link's node is kept is
     a second at least and less than that silence; the frame size and the protocol version are the ones the
     vectors were written with
"""
import hashlib
import hmac
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import nbtpkg

SPEC = os.path.join(ROOT, "spec", "protocol")
MAGIC = b"NBTL"
MSG, DATA = 1, 2
LAST = 2
MAX_PAYLOAD = 65536


def fail(message):
    print("test-protocol: FAILED: %s" % message, file=sys.stderr)
    sys.exit(1)


def encode(kind, flags, stream, payload):
    head = struct.pack("<BBHI", kind, flags, stream, len(payload))
    return head + payload + struct.pack("<I", zlib.crc32(head + payload) & 0xFFFFFFFF)


def decode(data):
    """("frame", kind, flags, stream, payload, bytes used), ("more",) or ("bad", why)."""
    if len(data) < 8:
        return ("more",)
    kind, flags, stream, length = struct.unpack_from("<BBHI", data)
    if kind not in (MSG, DATA):
        return ("bad", "type")
    if flags & 1:
        return ("bad", "compressed")
    if flags & ~3:
        return ("bad", "flags")
    if kind == MSG and (flags or stream):
        return ("bad", "a MSG frame with flags or a stream")
    if kind == DATA and stream == 0:
        return ("bad", "a DATA frame with no stream")
    if length > MAX_PAYLOAD:
        return ("bad", "length")
    if len(data) < 12 + length:
        return ("more",)
    if struct.unpack_from("<I", data, 8 + length)[0] != zlib.crc32(data[:8 + length]) & 0xFFFFFFFF:
        return ("bad", "check")
    return ("frame", kind, flags, stream, data[8:8 + length], 12 + length)


def lines_of(name):
    with open(os.path.join(SPEC, name), encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if line and not line.startswith("#"):
                yield number, line


def check_frames():
    good = bad = 0
    saw_magic = False
    for number, line in lines_of("frames.vectors"):
        where = "frames.vectors:%d" % number
        fields = line.split(" ")
        if fields[0] == "magic":
            if bytes.fromhex(fields[1]) != MAGIC:
                fail("%s: the magic bytes are not %r" % (where, MAGIC))
            saw_magic = True
        elif fields[0] == "frame":
            kind, flags, stream = int(fields[1]), int(fields[2]), int(fields[3])
            payload = b"" if fields[4] == "-" else bytes.fromhex(fields[4])
            frame = bytes.fromhex(fields[5])
            if encode(kind, flags, stream, payload) != frame:
                fail("%s: the frame is not what its fields encode to" % where)
            if decode(frame) != ("frame", kind, flags, stream, payload, len(frame)):
                fail("%s: the frame does not decode to its fields: %r" % (where, decode(frame)[:1]))
            for cut in range(len(frame)):
                if decode(frame[:cut]) != ("more",):
                    fail("%s: cut off after %d bytes it is not 'more'" % (where, cut))
            good += 1
        elif fields[0] == "bad":
            if decode(bytes.fromhex(fields[1]))[0] != "bad":
                fail("%s: a bad frame was not refused" % where)
            bad += 1
        else:
            fail("%s: unknown line" % where)
    if not saw_magic or good < 5 or bad < 10:
        fail("frames.vectors holds fewer cases than expected")
    return good, bad


CLOSING_REASONS = ("restarting", "stopping", "forgotten", "replaced", "silent")


def check_session():
    next_id = {">": 1, "<": 1}          # the next request id of each side
    waiting = {">": {}, "<": {}}        # requests of each side with no answer yet: id -> method
    streams = {}                        # open streams, phone to agent: number -> bytes still due
    job_seq = {}                        # planId -> last seq seen
    frames = 0
    versions = set()                    # (protocolMin, protocolMax) of every hello and of every answer to one
    saw_magic = False
    hello = {}                          # ">" and "<": the payloads of hello and of its answer, as they travelled
    key = None                          # the pairing key that pairing.issue gave
    nonces = {}                         # ">" and "<": the two nonces of auth
    proof_sent = None                   # the proof the phone sent
    trusted = False                     # auth is done
    closing = False                     # the agent has said link.closing

    def proof(label):
        transcript = nonces[">"] + nonces["<"] + hashlib.sha256(hello[">"]).digest() + hashlib.sha256(hello["<"]).digest()
        return hmac.new(key, label + transcript, hashlib.sha256).hexdigest()
    for number, line in lines_of("session.vectors"):
        where = "session.vectors:%d" % number
        direction, _, text = line.partition(" ")
        if direction == "magic":
            if bytes.fromhex(text) != MAGIC:
                fail("%s: the magic bytes are not %r" % (where, MAGIC))
            saw_magic = True
            continue
        if direction not in (">", "<"):
            fail("%s: unknown line" % where)
        raw = bytes.fromhex(text)
        result = decode(raw)
        if result[0] != "frame" or result[5] != len(raw):
            fail("%s: not exactly one frame" % where)
        _, kind, flags, stream, payload, _ = result
        if closing:
            fail("%s: a frame after link.closing, which is the last thing said on a link" % where)
        frames += 1
        other = "<" if direction == ">" else ">"
        if kind == DATA:
            if direction != ">" or stream not in streams:
                fail("%s: a DATA frame for a stream that is not open" % where)
            streams[stream] -= len(payload)
            if streams[stream] < 0 or (flags & LAST) != (LAST if streams[stream] == 0 else 0):
                fail("%s: the stream's length and its last-frame flag disagree" % where)
            if streams[stream] == 0:
                del streams[stream]
            continue
        try:
            message = nbtpkg.parse_json(payload)
        except Exception as error:
            fail("%s: the message is outside the JSON profile: %s" % (where, error))
        if "method" in message:
            if message.get("id") != next_id[direction] or not isinstance(message.get("params", {}), dict):
                fail("%s: a request with the wrong id, or params that are not an object" % where)
            next_id[direction] += 1
            waiting[direction][message["id"]] = message["method"]
            if message["method"] == "hello":
                versions.add((message["params"].get("protocolMin"), message["params"].get("protocolMax")))
                hello[direction] = payload
            if message["method"] == "auth" and direction == ">":
                if "nonce" in message["params"]:
                    nonces[">"] = bytes.fromhex(message["params"]["nonce"])
                else:
                    proof_sent = message["params"].get("proof")
        elif "event" in message:
            if not isinstance(message.get("params"), dict):
                fail("%s: an event with no params" % where)
            if message["event"].startswith("job."):
                plan = message["params"].get("planId")
                if direction != "<" or message.get("seq") != job_seq.get(plan, 0) + 1:
                    fail("%s: a job event out of sequence" % where)
                job_seq[plan] = message["seq"]
            if message["event"] == "link.closing":
                if direction != "<" or message["params"].get("reason") not in CLOSING_REASONS or "seq" in message:
                    fail("%s: link.closing is the agent's, with a reason it knows and no seq" % where)
                closing = True
        elif ("result" in message) != ("error" in message):
            method = waiting[other].pop(message.get("id"), None)
            if method is None:
                fail("%s: an answer to no request" % where)
            if "error" in message:
                error = message["error"]
                if not isinstance(error.get("code"), str) or not isinstance(error.get("message"), str):
                    fail("%s: an error with no code or message" % where)
            elif not isinstance(message["result"], dict):
                fail("%s: a result that is not an object" % where)
            elif direction == "<" and not trusted and method not in ("hello", "ping", "auth", "pairing.issue"):
                fail("%s: the agent answered %s before auth was done" % (where, method))
            elif method == "pairing.issue":
                key = bytes.fromhex(message["result"]["key"])
                if len(key) != 32:
                    fail("%s: a pairing key that is not 32 bytes" % where)
            elif method == "auth" and "nonce" in message["result"]:
                nonces["<"] = bytes.fromhex(message["result"]["nonce"])
                if key is None or len(nonces[">"]) != 16 or len(nonces["<"]) != 16:
                    fail("%s: auth with no pairing key before it, or a nonce that is not 16 bytes" % where)
                if message["result"].get("proof") != proof(b"nbt-auth-1"):
                    fail("%s: the agent's proof is not HMAC-SHA256(K, \"nbt-auth-1\" || T)" % where)
            elif method == "auth":
                if proof_sent != proof(b"nbt-auth-2"):
                    fail("%s: the agent took a proof that is not HMAC-SHA256(K, \"nbt-auth-2\" || T)" % where)
                trusted = True
            elif method == "blob.fetch":
                streams[message["result"]["stream"]] = message["result"]["size"]
            elif method == "hello":
                versions.add((message["result"].get("protocolMin"), message["result"].get("protocolMax")))
                hello[direction] = payload
        else:
            fail("%s: not a request, a response or an event" % where)
    if not saw_magic or waiting[">"] or waiting["<"] or streams:
        fail("session.vectors ends with a request unanswered or a stream open")
    if frames < 40 or len(job_seq) < 2 or not trusted or not closing:
        fail("session.vectors holds less than expected")
    return frames, versions


CONSTANTS = {
    "protocol.version": "number", "frame.maxPayload": "number", "frame.minPayload": "number",
    "serial.serviceName": "name", "serial.serviceUuid": "uuid", "serial.maxFrame": "number",
    "serial.pingAfterMs": "number", "serial.silenceMs": "number", "serial.decideMs": "number",
    "serial.lingerMs": "number",
    "pairing.codeDigits": "number", "pairing.windowSeconds": "number", "pairing.tries": "number",
    "pairing.pauseSeconds": "number", "auth.nonceBytes": "number",
    "capability.autostart": "name",
}


def check_constants():
    values = {}
    for number, line in lines_of("constants.txt"):
        where = "constants.txt:%d" % number
        fields = line.split(" ")
        if len(fields) != 2 or not fields[0] or not fields[1] or line != line.strip():
            fail("%s: not a name, a space and a value" % where)
        name, value = fields
        if name not in CONSTANTS or name in values:
            fail("%s: %s is unknown, or there twice" % (where, name))
        kind = CONSTANTS[name]
        if kind == "number":
            if not re.fullmatch(r"[1-9][0-9]{0,8}", value):
                fail("%s: %s is not a number" % (where, name))
            value = int(value)
        elif kind == "uuid":
            # Upper case: the form the car's Bluetooth stack is given.
            if not re.fullmatch(r"[0-9A-F]{8}(-[0-9A-F]{4}){3}-[0-9A-F]{12}", value):
                fail("%s: %s is not a UUID in upper case" % (where, name))
        elif not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", value):
            fail("%s: %s is not a name" % (where, name))
        values[name] = value
    missing = sorted(set(CONSTANTS) - set(values))
    if missing:
        fail("constants.txt has no %s" % ", ".join(missing))
    if values["frame.maxPayload"] != MAX_PAYLOAD:
        fail("constants.txt: frame.maxPayload is not the %d the vectors were written with" % MAX_PAYLOAD)
    if not values["frame.minPayload"] <= values["serial.maxFrame"] <= values["frame.maxPayload"]:
        fail("constants.txt: serial.maxFrame is outside what a frame may be")
    if 2 * values["serial.pingAfterMs"] > values["serial.silenceMs"]:
        fail("constants.txt: serial.silenceMs leaves no time for a ping and its answer")
    if values["serial.decideMs"] >= values["serial.pingAfterMs"]:
        fail("constants.txt: serial.decideMs is not well below the time a link may be silent")
    if not 1000 <= values["serial.lingerMs"] < values["serial.silenceMs"]:
        fail("constants.txt: serial.lingerMs gives a phone no time to end its link, or more than a silent link gets")
    return values


def main():
    good, bad = check_frames()
    frames, versions = check_session()
    constants = check_constants()
    if versions != {(constants["protocol.version"], constants["protocol.version"])}:
        fail("session.vectors: hello does not carry protocol.version of constants.txt")
    print("test-protocol: ok (%d good and %d bad frames, a session of %d frames, %d constants)"
          % (good, bad, frames, len(constants)))


if __name__ == "__main__":
    main()
