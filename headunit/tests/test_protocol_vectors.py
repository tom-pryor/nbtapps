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
     blob.fetch reply gave, ending on the frame that has the last-frame flag; those of a files.fetch, from the
     agent, are the size its reply gave once inflated (zlib's own inflate, when they are deflated), end on the
     frame with that flag, and come before the answer to a files.cancel of the stream; a channel's DATA frames, either
     way, are on the stream its channel.open answer named, from 32,768 up, each one message of 1 to maxMessage bytes
     with the last-frame flag, and no side sends more than the other granted, in that open (the request's credit and
     the answer's) and with channel.credit, until a channel.close; the session has a pairing.issue
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
# As constants.txt has them, which check_constants reads; the session is read first.
CHANNEL_FIRST_STREAM = 32768
CHANNEL_MAX_MESSAGE = 4096
CHANNEL_CREDIT = 16


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
    files = {}                          # open files streams, agent to phone: number -> [size, inflater or None, bytes]
    fetching = {}                       # files.fetch requests of the phone with no answer yet: id -> compress
    cancelling = {}                     # files.cancel requests of the phone with no answer yet: id -> stream
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
    opening = {}                        # channel.open requests of the phone with no answer yet: id -> credit asked
    channels = {}                       # open channels: stream -> {">": what the phone may send, "<": the agent's, "max"}
    closed_channels = set()             # streams of channels that were closed: a frame that crossed the close is dropped
    channel_messages = {">": 0, "<": 0}  # channels' messages each way

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
        if kind == DATA and stream >= CHANNEL_FIRST_STREAM and stream not in streams and stream not in files:
            # A channel's message, either way: within what the other side granted.
            if stream in closed_channels and stream not in channels:
                continue
            channel = channels.get(stream)
            if channel is None:
                fail("%s: a DATA frame for a channel that is not open" % where)
            if not flags & LAST or not 1 <= len(payload) <= channel["max"]:
                fail("%s: a channel's DATA frame that is not one message of 1 to %d bytes with the last-frame flag"
                     % (where, channel["max"]))
            channel[direction] -= 1
            channel_messages[direction] += 1
            if channel[direction] < 0:
                fail("%s: a channel's message over the credit the other side granted" % where)
            continue
        if kind == DATA and direction == "<":
            # A files stream: the bytes it holds are those of the file once inflated.
            if stream not in files:
                fail("%s: a DATA frame for a files stream that is not open" % where)
            size, inflater, got = files[stream]
            got += inflater.decompress(payload) if inflater else payload
            files[stream][2] = got
            if len(got) > size or (flags & LAST and (len(got) != size or (inflater and (not inflater.eof or inflater.unused_data)))):
                fail("%s: the files stream is not as long as its reply said, or does not end with its last frame" % where)
            if flags & LAST:
                del files[stream]
            continue
        if kind == DATA:
            if stream not in streams:
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
            if message["method"] == "files.fetch" and direction == ">":
                fetching[message["id"]] = message["params"].get("compress")
            if message["method"] == "files.cancel" and direction == ">":
                cancelling[message["id"]] = message["params"].get("stream")
            if message["method"] == "channel.open" and direction == ">":
                opening[message["id"]] = message["params"].get("credit", CHANNEL_CREDIT)
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
            if message["event"] in ("channel.credit", "channel.close"):
                number = message["params"].get("stream")
                if "seq" in message or not isinstance(number, int):
                    fail("%s: a channel's event with a seq, or with no stream" % where)
                if number in channels and message["event"] == "channel.credit":
                    granted = message["params"].get("messages")
                    if not isinstance(granted, int) or not 1 <= granted <= 65535:
                        fail("%s: a channel.credit with no messages from 1 to 65535" % where)
                    channels[number][other] += granted
                elif number in channels:
                    if not isinstance(message["params"].get("reason"), str):
                        fail("%s: a channel.close with no reason" % where)
                    del channels[number]
                    closed_channels.add(number)
                elif number not in closed_channels:
                    fail("%s: %s for a channel that was never open" % (where, message["event"]))
        elif ("result" in message) != ("error" in message):
            method = waiting[other].pop(message.get("id"), None)
            if method is None:
                fail("%s: an answer to no request" % where)
            if "error" in message:
                error = message["error"]
                if method == "channel.open" and direction == "<":
                    opening.pop(message["id"], None)
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
            elif method == "files.fetch" and direction == "<":
                compress = fetching.pop(message["id"])
                if message["result"].get("compress") != compress or compress not in ("deflate", "none"):
                    fail("%s: a files.fetch reply with another compress than was asked for" % where)
                if message["result"]["size"] > 0:
                    files[message["result"]["stream"]] = [message["result"]["size"],
                                                          zlib.decompressobj(-15) if compress == "deflate" else None, b""]
                elif "stream" in message["result"]:
                    fail("%s: a files.fetch reply of size 0 with a stream" % where)
            elif method == "files.cancel" and direction == "<":
                # Nothing more comes on the stream after this answer.
                files.pop(cancelling.pop(message["id"]), None)
            elif method == "channel.open" and direction == "<":
                result = message["result"]
                number, credit, most = result.get("stream"), result.get("credit"), result.get("maxMessage")
                if (not isinstance(number, int) or not CHANNEL_FIRST_STREAM <= number <= 65535 or number in channels
                        or not isinstance(credit, int) or credit < 0 or most != CHANNEL_MAX_MESSAGE
                        or not isinstance(result.get("version"), str)):
                    fail("%s: a channel.open answer with no channel's stream, credit, maxMessage or version" % where)
                channels[number] = {">": credit, "<": opening.pop(message["id"]), "max": most}
                closed_channels.discard(number)
            elif method == "hello":
                versions.add((message["result"].get("protocolMin"), message["result"].get("protocolMax")))
                hello[direction] = payload
        else:
            fail("%s: not a request, a response or an event" % where)
    if not saw_magic or waiting[">"] or waiting["<"] or streams or files:
        fail("session.vectors ends with a request unanswered or a stream open")
    if frames < 40 or len(job_seq) < 2 or not trusted or not closing or not all(channel_messages.values()):
        fail("session.vectors holds less than expected")
    return frames, versions


CONSTANTS = {
    "protocol.version": "number", "frame.maxPayload": "number", "frame.minPayload": "number",
    "serial.serviceName": "name", "serial.serviceUuid": "uuid", "serial.maxFrame": "number",
    "serial.pingAfterMs": "number", "serial.silenceMs": "number", "serial.decideMs": "number",
    "serial.lingerMs": "number",
    "serial.knockMagic": "name", "serial.knockAttempts": "number", "serial.knockGapMs": "number",
    "serial.knockHoldMs": "number",
    "pairing.codeDigits": "number", "pairing.windowSeconds": "number", "pairing.tries": "number",
    "pairing.pauseSeconds": "number", "auth.nonceBytes": "number",
    "capability.autostart": "name",
    "capability.detachedSignature": "name",
    "capability.files": "name",
    "capability.checks": "name",
    "capability.serialReset": "name",
    "capability.serialDial": "name",
    "capability.serialKnock": "name",
    "capability.settings": "name",
    "settings.maxValueBytes": "number",
    "capability.channels": "name",
    "stream.last": "number", "channel.firstStream": "number", "channel.maxMessage": "number",
    "channel.credit": "number", "channel.perLink": "number", "channel.startWaitMs": "number",
    "channel.startRetryMs": "number",
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
    if len(values["serial.knockMagic"]) != 4 or values["serial.knockMagic"] == "NBTL":
        fail("constants.txt: serial.knockMagic is not four bytes other than the link's magic bytes")
    if values["serial.knockAttempts"] < 2:
        fail("constants.txt: a knock is one connect, which may fail on the phone while another app's link is up")
    if values["serial.knockHoldMs"] <= values["serial.decideMs"]:
        fail("constants.txt: serial.knockHoldMs ends a knock link before the agent has decided on it")
    if values["stream.last"] + 1 != values["channel.firstStream"] or values["channel.firstStream"] > 65535:
        fail("constants.txt: the channels' stream numbers do not follow on from the other streams'")
    if not values["frame.minPayload"] <= values["channel.maxMessage"] <= values["serial.maxFrame"]:
        fail("constants.txt: channel.maxMessage does not fit a serial link's frame")
    return values


def main():
    good, bad = check_frames()
    frames, versions = check_session()
    constants = check_constants()
    if versions != {(constants["protocol.version"], constants["protocol.version"])}:
        fail("session.vectors: hello does not carry protocol.version of constants.txt")
    if (constants["channel.firstStream"], constants["channel.maxMessage"], constants["channel.credit"]) != (
            CHANNEL_FIRST_STREAM, CHANNEL_MAX_MESSAGE, CHANNEL_CREDIT):
        fail("constants.txt: the channels' numbers are not the ones session.vectors was read with")
    print("test-protocol: ok (%d good and %d bad frames, a session of %d frames, %d constants)"
          % (good, bad, frames, len(constants)))


if __name__ == "__main__":
    main()
