#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""An OpenSSH client's proxy command in the relay's tests (run_relay.py): it speaks to the relay's socket as the
agent and the phone do together, then is the SSH byte stream on its standard input and output.

  ssh_proxy.py SOCKET PUBLIC_KEY_FILE [ANSWER_FILE]

It writes the agent's line, sends the keys message with the lines of PUBLIC_KEY_FILE, and reads the answer: an
error goes to standard error with exit status 1; the host keys are written to ANSWER_FILE when one is named. Then
standard input is cut into messages of up to 4,096 bytes for the relay, and the relay's messages go to standard
output as bytes, until the relay closes the connection. When its input ends it stops reading it and goes on
writing; the client ends it with SIGHUP.
"""
import json, os, select, socket, struct, sys

MAX_MESSAGE = 4096


def read_exact(sock, n, buffer):
    while len(buffer) < n:
        part = sock.recv(65536)
        if not part:
            return None
        buffer += part
    return buffer


def main():
    path, key_file = sys.argv[1], sys.argv[2]
    answer_file = sys.argv[3] if len(sys.argv) > 3 else None
    with open(key_file) as f:
        lines = [line.strip() for line in f if line.strip()]
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(path)
    line = {"v": 1, "channel": "ssh", "access": "both", "caller": "cli", "maxMessage": MAX_MESSAGE}
    sock.sendall(json.dumps(line).encode() + b"\n")
    keys = json.dumps({"v": 1, "keys": lines}).encode()
    sock.sendall(struct.pack(">H", len(keys)) + keys)

    buffer = bytearray()
    if read_exact(sock, 2, buffer) is None:
        sys.exit("ssh_proxy.py: the relay closed the connection before its answer")
    length = struct.unpack(">H", bytes(buffer[:2]))[0]
    if read_exact(sock, 2 + length, buffer) is None:
        sys.exit("ssh_proxy.py: the relay closed the connection in its answer")
    answer = json.loads(bytes(buffer[2:2 + length]))
    del buffer[:2 + length]
    if "error" in answer:
        sys.exit("ssh_proxy.py: the relay answered %s" % answer["error"])
    if answer_file:
        with open(answer_file, "w") as f:
            f.write("\n".join(answer["hostKeys"]) + "\n")

    stdin_open = True
    while True:
        readable = [sock] + ([0] if stdin_open else [])
        ready, _, _ = select.select(readable, [], [])
        if 0 in ready:
            data = os.read(0, MAX_MESSAGE)
            if data:
                sock.sendall(struct.pack(">H", len(data)) + data)
            else:
                stdin_open = False
        if sock in ready:
            part = sock.recv(65536)
            if not part:
                return
            buffer += part
            while len(buffer) >= 2:
                length = struct.unpack(">H", bytes(buffer[:2]))[0]
                if len(buffer) < 2 + length:
                    break
                data = bytes(buffer[2:2 + length])
                del buffer[:2 + length]
                view = memoryview(data)
                while view:
                    view = view[os.write(1, view):]


if __name__ == "__main__":
    main()
