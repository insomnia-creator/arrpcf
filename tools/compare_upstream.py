#!/usr/bin/env python3
"""Differential test: run the same RPC session against upstream arRPC (Node) and
this C++ implementation, then compare what each one replies and what each one
publishes on its bridge.

Usage:
    git clone https://github.com/OpenAsar/arrpc /tmp/arrpc-ref
    (cd /tmp/arrpc-ref && npm install)
    tools/compare_upstream.py /tmp/arrpc-ref [build/arrpcf]

Exits 0 when the bridge payloads, the READY packet and every SET_ACTIVITY
response are identical. The three commands arRPC never answers
(INVITE_BROWSER, GUILD_TEMPLATE_BROWSER, DEEP_LINK) are reported separately,
they are an intentional difference (see README).
"""

from __future__ import annotations

import base64
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UPSTREAM = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("ARRPCF_UPSTREAM", os.environ.get("ARRPC_UPSTREAM", "/tmp/arrpc-ref"))
BINARY = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "arrpcf")

SESSION = [
    {"cmd": "SET_ACTIVITY", "nonce": "n1", "args": {"pid": 4242, "activity": {
        "details": "Details", "state": "State",
        "timestamps": {"start": 1700000000, "end": 1700003600},
        "buttons": [{"label": "One", "url": "https://one.example"},
                    {"label": "Two", "url": "https://two.example"}],
        "assets": {"large_image": "logo", "large_text": "big"},
        "instance": True, "party": {"size": [1, 4]}}}},
    {"cmd": "SET_ACTIVITY", "nonce": None, "args": {"pid": 4242, "activity": {"details": "second"}}},
    {"cmd": "SET_ACTIVITY", "nonce": 5, "args": {"pid": 4242, "activity": None}},
    {"cmd": "CONNECTIONS_CALLBACK", "nonce": "cb", "args": {"code": "x"}},
    {"cmd": "INVITE_BROWSER", "nonce": "inv", "args": {"code": "abcdef"}},
    {"cmd": "GUILD_TEMPLATE_BROWSER", "nonce": "tpl", "args": {"code": ""}},
    {"cmd": "DEEP_LINK", "nonce": "dl", "args": {"url": "https://discord.com/channels/@me"}},
    {"cmd": "BOGUS_COMMAND", "nonce": "bogus", "args": {}},
]

# Commands upstream arRPC leaves unanswered (no bridge listener for the callbacks).
UPSTREAM_SILENT = {"INVITE_BROWSER", "GUILD_TEMPLATE_BROWSER", "DEEP_LINK"}


def free_port() -> int:
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


class BridgeClient:
    def __init__(self, port: int):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(
            (f"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("bridge closed during handshake")
            self.buf += chunk
        _, _, self.buf = self.buf.partition(b"\r\n\r\n")

    def _read(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv_json(self, timeout: float = 1.0):
        self.sock.settimeout(timeout)
        b0, b1 = self._read(2)
        opcode = b0 & 0x0F
        length = b1 & 0x7F
        if length == 126:
            length = struct.unpack(">H", self._read(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", self._read(8))[0]
        if b1 & 0x80:
            self._read(4)
        payload = self._read(length)
        return json.loads(payload) if opcode == 0x1 else None

    def close(self):
        self.sock.close()


class IPCClient:
    def __init__(self, path: str):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(5)
        self.sock.connect(path)
        self.buf = b""

    def _read(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, packet_type: int, payload) -> None:
        data = json.dumps(payload).encode()
        self.sock.sendall(struct.pack("<ii", packet_type, len(data)) + data)

    def recv(self):
        packet_type, length = struct.unpack("<ii", self._read(8))
        data = self._read(length)
        return packet_type, (json.loads(data) if length else None)

    def close(self):
        self.sock.close()


def run(kind: str, command: list[str], cwd: str, workdir: str):
    port = free_port()
    tmpdir = os.path.join(workdir, kind + "-tmp")
    os.makedirs(tmpdir, exist_ok=True)
    log_path = os.path.join(workdir, kind + ".log")

    # Upstream Node arRPC only reads ARRPC_BRIDGE_PORT; set both prefixes so the
    # same session can be replayed against either implementation.
    env = dict(os.environ, TMPDIR=tmpdir,
               ARRPC_BRIDGE_PORT=str(port), ARRPCF_BRIDGE_PORT=str(port))
    for name in ("ARRPC_FAKE_PROCESSES", "ARRPC_FAKE_PROCESSES_FILE",
                 "ARRPCF_FAKE_PROCESSES", "ARRPCF_FAKE_PROCESSES_FILE"):
        env.pop(name, None)

    with open(log_path, "w") as log:
        proc = subprocess.Popen(command, env=env, cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
        try:
            socket_path = os.path.join(tmpdir, "discord-ipc-0")
            deadline = time.time() + 15
            while time.time() < deadline and not os.path.exists(socket_path):
                if proc.poll() is not None:
                    raise RuntimeError(f"{kind} exited:\n" + open(log_path).read())
                time.sleep(0.05)
            if not os.path.exists(socket_path):
                raise RuntimeError(f"{kind} never created {socket_path}:\n" + open(log_path).read())

            bridge = BridgeClient(port)
            ipc = IPCClient(socket_path)
            ipc.send(0, {"v": 1, "client_id": "123456789012345678"})
            ready = ipc.recv()[1]

            responses = []
            bridged = []
            for message in SESSION:
                ipc.send(1, message)
                try:
                    responses.append(ipc.recv()[1])
                except (socket.timeout, TimeoutError):
                    responses.append("NO_RESPONSE")
                time.sleep(0.15)
                while True:
                    try:
                        item = bridge.recv_json(timeout=0.15)
                    except (socket.timeout, TimeoutError):
                        break
                    if item is None:
                        break
                    bridged.append(item)

            ipc.close()
            time.sleep(0.3)
            while True:  # the close-triggered clear
                try:
                    item = bridge.recv_json(timeout=0.3)
                except (socket.timeout, TimeoutError):
                    break
                if item is None:
                    break
                bridged.append(item)

            bridge.close()
            return ready, responses, bridged, log_path
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()


def normalize(message):
    if not isinstance(message, dict):
        return message
    message = json.loads(json.dumps(message))
    message.pop("socketId", None)
    return message


def main() -> int:
    if not os.path.isdir(UPSTREAM):
        print(f"upstream arRPC checkout not found: {UPSTREAM}", file=sys.stderr)
        return 2
    if not os.path.exists(os.path.join(UPSTREAM, "node_modules", "ws")):
        print("run `npm install` in the upstream checkout first", file=sys.stderr)
        return 2
    if not os.path.exists(BINARY):
        print(f"binary not found: {BINARY}", file=sys.stderr)
        return 2

    workdir = tempfile.mkdtemp(prefix="arrpcf-compare-")
    failures = 0
    try:
        ours = run("cpp", [BINARY, "--no-process-scanning", "--no-color"], ROOT, workdir)
        theirs = run("node", ["node", "src/index.js", "--no-process-scanning"], UPSTREAM, workdir)

        print(f"READY identical: {ours[0] == theirs[0]}")
        failures += 0 if ours[0] == theirs[0] else 1

        for index, (mine, upstream) in enumerate(zip(ours[1], theirs[1])):
            command = SESSION[index]["cmd"]
            if isinstance(upstream, str) and upstream == "NO_RESPONSE" and command in UPSTREAM_SILENT:
                print(f"response[{index}] {command}: intentional difference "
                      f"(upstream silent, we answer {json.dumps(mine)[:90]})")
                continue
            same = mine == upstream
            print(f"response[{index}] {command}: {'identical' if same else 'DIFFERS'}")
            if not same:
                failures += 1
                print(f"  ours    : {json.dumps(mine)[:400]}")
                print(f"  upstream: {json.dumps(upstream)[:400]}")

        # Only compare messages that belong to our test connection: other RPC
        # clients on the machine may connect to / disconnect from the same
        # servers while the comparison runs.
        def session_id(messages):
            for message in messages:
                if isinstance(message, dict) and message.get("activity") is not None:
                    return message.get("socketId")
            return None

        ours_id = session_id(ours[2])
        theirs_id = session_id(theirs[2])

        def relevant(messages, socket_id):
            return [normalize(item) for item in messages
                    if not isinstance(item, dict) or item.get("socketId") == socket_id]

        mine = relevant(ours[2], ours_id)
        upstream = relevant(theirs[2], theirs_id)
        same = mine == upstream
        print(f"bridge payloads identical ({len(mine)} messages): {same}")
        if not same:
            failures += 1
            for index in range(max(len(mine), len(upstream))):
                a = mine[index] if index < len(mine) else None
                b = upstream[index] if index < len(upstream) else None
                if a != b:
                    print(f"  bridge[{index}] ours    : {json.dumps(a)[:400]}")
                    print(f"  bridge[{index}] upstream: {json.dumps(b)[:400]}")

        if failures:
            print(f"\nserver logs: {ours[3]}, {theirs[3]}")
        print("\nPARITY OK" if failures == 0 else f"\nPARITY FAILURES: {failures}")
        return 0 if failures == 0 else 1
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
