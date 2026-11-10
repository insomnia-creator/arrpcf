#!/usr/bin/env python3
"""End-to-end protocol tests for arrpc (stdlib only, no dependencies).

Usage: tools/test_server.py [path/to/arrpc]

Covers: IPC framing + handshake, READY packet, SET_ACTIVITY translation
(buttons, timestamps, pid), activity clearing, socket close, socket probing,
WebSocket transport (handshake, origin checks, version/encoding rejection),
bridge broadcast + catch-up, and process-scanning (via the fake-process hook).
"""

from __future__ import annotations

import base64
import json
import re
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "arrpc")

PASS = 0
FAIL = 0
FAILURES: list[str] = []


def check(name: str, condition, detail: str = "") -> None:
    global PASS, FAIL
    if condition:
        PASS += 1
        print(f"  ok   {name}")
    else:
        FAIL += 1
        FAILURES.append(name)
        print(f"  FAIL {name} {detail}")


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# ---------------------------------------------------------------------------
# Transports
# ---------------------------------------------------------------------------


class IPCClient:
    HANDSHAKE, FRAME, CLOSE, PING, PONG = 0, 1, 2, 3, 4

    def __init__(self, path: str):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(5)
        self.sock.connect(path)
        self.buf = b""

    def _read(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("ipc socket closed")
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

    def handshake(self, client_id: str = "1234567890", version: int = 1) -> None:
        payload = {"v": version, "client_id": client_id}
        self.send(self.HANDSHAKE, payload)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


class WSClient:
    def __init__(self, port: int, query: str = "v=1&encoding=json&client_id=456",
                 origin: str | None = "https://discord.com", path: str = "/"):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET {path}?{query} HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
        )
        if origin:
            request += f"Origin: {origin}\r\n"
        request += "\r\n"
        self.sock.sendall(request.encode())

        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("closed during websocket handshake")
            self.buf += chunk
        head, _, self.buf = self.buf.partition(b"\r\n\r\n")
        self.status_line = head.split(b"\r\n")[0].decode()
        self.headers = {}
        for line in head.split(b"\r\n")[1:]:
            if b":" in line:
                k, v = line.split(b":", 1)
                self.headers[k.strip().lower().decode()] = v.strip().decode()

    def _read(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("websocket closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send_frame(self, opcode: int, payload: bytes = b"") -> None:
        header = bytes([0x80 | opcode])
        mask = os.urandom(4)
        n = len(payload)
        if n < 126:
            header += bytes([0x80 | n])
        elif n < 65536:
            header += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", n)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def send_json(self, obj) -> None:
        self.send_frame(0x1, json.dumps(obj).encode())

    def recv_frame(self):
        b0, b1 = self._read(2)
        opcode = b0 & 0x0F
        masked = b1 & 0x80
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", self._read(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self._read(8))[0]
        mask = self._read(4) if masked else None
        payload = self._read(n)
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return opcode, payload

    def recv_json(self, timeout: float = 5.0):
        self.sock.settimeout(timeout)
        while True:
            opcode, payload = self.recv_frame()
            if opcode == 0x1:
                return json.loads(payload)
            if opcode == 0x8:
                return None
            if opcode == 0x9:
                self.send_frame(0xA, payload)

    def close(self) -> None:
        try:
            self.send_frame(0x8, struct.pack(">H", 1000))
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------


class Server:
    def __init__(self, base_dir: str, fake_processes: str | None = None):
        self.fake_file = os.path.join(base_dir, "fake_processes.txt")
        if fake_processes is not None:
            self.write_fake_processes(fake_processes)
        self.base = os.path.join(base_dir, "discord-ipc")
        self.bridge_port = free_port()
        self.ws_first = free_port()
        self.ws_last = self.ws_first + 4
        self.log_path = os.path.join(base_dir, "arrpc.log")
        self.log = open(self.log_path, "w")

        env = dict(os.environ)
        env.pop("ARRPC_FAKE_PROCESSES", None)
        env.pop("ARRPC_FAKE_PROCESSES_FILE", None)
        if fake_processes is not None:
            env["ARRPC_FAKE_PROCESSES_FILE"] = self.fake_file

        self.proc = subprocess.Popen(
            [
                BINARY,
                "--ipc-base", self.base,
                "--ws-ports", f"{self.ws_first}-{self.ws_last}",
                "--bridge-port", str(self.bridge_port),
                "--scan-interval", "1",
                "--debug",
                "--no-color",
            ],
            stdout=self.log,
            stderr=subprocess.STDOUT,
            env=env,
        )

    def write_fake_processes(self, spec: str) -> None:
        with open(self.fake_file, "w") as handle:
            for item in spec.split(";"):
                if item:
                    handle.write(item + "\n")

    def wait_ready(self, timeout: float = 10.0) -> None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(self.logs())
            try:
                sock = socket.create_connection(("127.0.0.1", self.bridge_port), timeout=0.2)
                sock.close()
                if os.path.exists(self.base + "-0"):
                    time.sleep(0.2)
                    return
            except OSError:
                pass
            time.sleep(0.05)
        raise RuntimeError("server did not become ready\n" + self.logs())

    def logs(self) -> str:
        try:
            self.log.flush()
            with open(self.log_path, "r") as f:
                return f.read()
        except OSError:
            return ""

    def stop(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log.close()


class Bridge:
    def __init__(self, port: int):
        self.client = WSClient(port, query="", origin=None, path="/")

    def next_activity(self, timeout: float = 5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                message = self.client.recv_json(timeout=max(0.1, min(0.5, deadline - time.time())))
            except (socket.timeout, TimeoutError):
                continue
            return message
        raise TimeoutError("no bridge message")

    def drain(self, quiet: float = 0.25) -> int:
        dropped = 0
        while True:
            try:
                self.client.recv_json(timeout=quiet)
                dropped += 1
            except (socket.timeout, TimeoutError):
                return dropped
        return dropped

    def wait_for(self, predicate, timeout: float = 5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                message = self.client.recv_json(timeout=0.5)
            except (socket.timeout, TimeoutError):
                continue
            if message is None:
                return None
            if predicate(message):
                return message
        return None

    def close(self):
        self.client.close()


def ipc_path(server: Server, index: int = 0) -> str:
    return f"{server.base}-{index}"


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_ipc(server: Server, bridge: Bridge) -> None:
    print("ipc transport")
    bridge.drain()

    client = IPCClient(ipc_path(server))
    client.handshake("1234567890")

    packet_type, ready = client.recv()
    check("handshake answered with a FRAME", packet_type == IPCClient.FRAME, str(packet_type))
    check("READY cmd/evt", ready["cmd"] == "DISPATCH" and ready["evt"] == "READY", json.dumps(ready)[:200])
    check("READY nonce is null", ready["nonce"] is None)
    check("READY config", ready["data"]["config"] == {
        "cdn_host": "cdn.discordapp.com",
        "api_endpoint": "//discord.com/api",
        "environment": "production",
    }, json.dumps(ready["data"]["config"]))
    check("READY user", ready["data"]["user"]["id"] == "1045800378228281345"
          and ready["data"]["user"]["global_name"] == "arRPC")

    # ping -> pong
    client.send(IPCClient.PING, 42)
    packet_type, pong = client.recv()
    check("ping answered with pong", packet_type == IPCClient.PONG and pong == 42, f"{packet_type} {pong}")

    activity = {
        "details": "In a menu",
        "state": "Waiting",
        "timestamps": {"start": 1700000000},  # seconds -> ms
        "buttons": [{"label": "Website", "url": "https://example.com"}],
        "instance": True,
    }
    client.send(IPCClient.FRAME, {
        "cmd": "SET_ACTIVITY",
        "args": {"pid": 999, "activity": activity},
        "nonce": "nonce-1",
    })

    packet_type, response = client.recv()
    check("activity answered", response["cmd"] == "SET_ACTIVITY" and response["nonce"] == "nonce-1",
          json.dumps(response)[:200])
    check("activity response evt is null", response["evt"] is None)
    check("activity response data", response["data"]["application_id"] == "1234567890"
          and response["data"]["name"] == "" and response["data"]["type"] == 0,
          json.dumps(response["data"])[:300])
    check("seconds timestamps became milliseconds",
          response["data"]["timestamps"]["start"] == 1700000000000,
          str(response["data"]["timestamps"]))

    bridged = bridge.next_activity()
    check("bridge received activity", bridged is not None and bridged["activity"] is not None,
          json.dumps(bridged)[:200] if bridged else "none")
    act = bridged["activity"]
    check("bridge activity application_id", act["application_id"] == "1234567890", str(act.get("application_id")))
    check("bridge activity metadata button_urls",
          act["metadata"]["button_urls"] == ["https://example.com"], str(act["metadata"]))
    check("bridge activity buttons labels", act["buttons"] == ["Website"], str(act.get("buttons")))
    check("bridge activity flag for instance", act["flags"] == 1, str(act.get("flags")))
    check("bridge activity pid", bridged.get("pid") == 999, str(bridged.get("pid")))
    check("bridge activity socketId", bridged.get("socketId") == "0", str(bridged.get("socketId")))

    # large payload: exercises extended WebSocket lengths on the bridge
    big_state = "ü" + "x" * 70000
    client.send(IPCClient.FRAME, {
        "cmd": "SET_ACTIVITY",
        "args": {"pid": 999, "activity": {"state": big_state, "details": "Tëst 🎮"}},
        "nonce": "big",
    })
    _, big_response = client.recv()
    check("large activity echoed back intact", big_response["data"]["state"] == big_state
          and big_response["data"]["details"] == "Tëst 🎮", str(len(big_response["data"].get("state", ""))))
    big_bridged = bridge.next_activity()
    check("large activity bridged intact", big_bridged["activity"]["state"] == big_state,
          str(len(big_bridged["activity"].get("state", ""))))

    # clearing
    client.send(IPCClient.FRAME, {
        "cmd": "SET_ACTIVITY",
        "args": {"pid": 999, "activity": None},
        "nonce": "nonce-2",
    })
    _, cleared = client.recv()
    check("clear answered with null data", cleared["data"] is None and cleared["nonce"] == "nonce-2",
          json.dumps(cleared)[:200])
    bridged = bridge.next_activity()
    check("bridge received clear", bridged["activity"] is None and bridged["socketId"] == "0",
          json.dumps(bridged)[:200])

    # close -> clear with last pid
    client.send(IPCClient.FRAME, {
        "cmd": "SET_ACTIVITY",
        "args": {"pid": 1001, "activity": {"details": "again", "instance": False}},
        "nonce": None,
    })
    client.recv()
    bridge.next_activity()
    client.close()
    bridged = bridge.next_activity()
    check("close clears activity", bridged["activity"] is None, json.dumps(bridged)[:200])
    check("close keeps last pid", bridged.get("pid") == 1001, str(bridged.get("pid")))


def test_ipc_handshake_errors(server: Server) -> None:
    print("ipc handshake validation")
    client = IPCClient(ipc_path(server))
    client.handshake("", version=1)
    packet_type, payload = client.recv()
    check("missing client_id closes with 4000",
          packet_type == IPCClient.CLOSE and payload["code"] == 4000, f"{packet_type} {payload}")
    client.close()

    client = IPCClient(ipc_path(server))
    client.handshake("abc", version=2)
    packet_type, payload = client.recv()
    check("bad version closes with 4004",
          packet_type == IPCClient.CLOSE and payload["code"] == 4004, f"{packet_type} {payload}")
    client.close()

    client = IPCClient(ipc_path(server))
    client.send(IPCClient.FRAME, {"cmd": "SET_ACTIVITY", "args": {}, "nonce": 1})
    packet_type, payload = client.recv()
    check("frame before handshake closes with 1003",
          packet_type == IPCClient.CLOSE and payload["code"] == 1003, f"{packet_type} {payload}")
    client.close()


def test_control_commands(server: Server, bridge: Bridge) -> None:
    print("control commands")
    bridge.drain()
    client = IPCClient(ipc_path(server))
    client.handshake("control-test")
    client.recv()

    client.send(IPCClient.FRAME, {"cmd": "CONNECTIONS_CALLBACK", "args": {"code": "x"}, "nonce": "cb"})
    _, callback = client.recv()
    check("CONNECTIONS_CALLBACK answers like arRPC",
          callback["data"] == {"code": 1000} and callback["evt"] == "ERROR" and callback["nonce"] == "cb",
          json.dumps(callback))

    client.send(IPCClient.FRAME, {"cmd": "INVITE_BROWSER", "args": {"code": "abcdef"}, "nonce": 1})
    _, invite = client.recv()
    check("valid invite accepted", invite["data"] == {"code": "abcdef"} and invite["evt"] is None,
          json.dumps(invite))

    client.send(IPCClient.FRAME, {"cmd": "INVITE_BROWSER", "args": {"code": ""}, "nonce": 2})
    _, bad_invite = client.recv()
    check("invalid invite rejected with 4011",
          bad_invite["data"]["code"] == 4011 and bad_invite["evt"] == "ERROR"
          and bad_invite["data"]["message"] == "Invalid invite id: ", json.dumps(bad_invite))

    client.send(IPCClient.FRAME, {"cmd": "GUILD_TEMPLATE_BROWSER", "args": {"code": "nope!"}, "nonce": 3})
    _, bad_template = client.recv()
    check("invalid guild template rejected with 4017",
          bad_template["data"]["code"] == 4017
          and bad_template["data"]["message"] == "Invalid guild template id: nope!", json.dumps(bad_template))

    client.send(IPCClient.FRAME, {"cmd": "DEEP_LINK", "args": {"url": "https://discord.com/x"}, "nonce": 4})
    _, deep = client.recv()
    check("deep link answered", deep["data"] is None and deep["evt"] is None and deep["nonce"] == 4,
          json.dumps(deep))

    client.send(IPCClient.FRAME, {"cmd": "NOT_A_COMMAND", "args": {}, "nonce": 5})
    client.sock.settimeout(0.5)
    try:
        unexpected = client.recv()
        no_reply = False
    except (socket.timeout, TimeoutError):
        unexpected = None
        no_reply = True
    check("unknown command is ignored", no_reply, str(unexpected))
    client.close()


def test_websocket(server: Server, bridge: Bridge) -> None:
    print("websocket transport")
    bridge.drain()
    ws = WSClient(server.ws_first)
    check("handshake accepted", "101" in ws.status_line, ws.status_line)
    check("accept header present", "sec-websocket-accept" in ws.headers, str(ws.headers))

    ready = ws.recv_json()
    check("READY over websocket", ready["cmd"] == "DISPATCH" and ready["evt"] == "READY", str(ready)[:120])

    ws.send_json({
        "cmd": "SET_ACTIVITY",
        "args": {"pid": 555, "activity": {"details": "ws"}},
        "nonce": 7,
    })
    response = ws.recv_json()
    check("ws activity response", response["nonce"] == 7 and response["data"]["application_id"] == "456",
          json.dumps(response)[:200])

    bridged = bridge.next_activity()
    check("ws activity bridged", bridged["activity"]["details"] == "ws"
          and bridged["socketId"] != "0", json.dumps(bridged)[:200])
    check("ws activity has metadata/flags", bridged["activity"]["metadata"] == {}
          and bridged["activity"]["flags"] == 0, str(bridged["activity"]))

    ws.close()

    print("websocket origin/version/encoding checks")
    bad_origin = WSClient(server.ws_first, origin="https://evil.example")
    check("disallowed origin closed", bad_origin.recv_json() is None, "expected close")
    bad_origin.close()

    allowed = WSClient(server.ws_first, origin="https://canary.discord.com")
    check("canary origin accepted", allowed.recv_json()["evt"] == "READY")
    allowed.close()

    no_origin = WSClient(server.ws_first, origin=None)
    check("empty origin accepted", no_origin.recv_json()["evt"] == "READY")
    no_origin.close()

    bad_version = WSClient(server.ws_first, query="v=9&client_id=x")
    check("bad version closed", bad_version.recv_json() is None)
    bad_version.close()

    bad_encoding = WSClient(server.ws_first, query="encoding=etf&client_id=x")
    check("etf encoding closed", bad_encoding.recv_json() is None)
    bad_encoding.close()

    # Plain HTTP request gets an upgrade hint instead of a hang.
    plain = socket.create_connection(("127.0.0.1", server.ws_first), timeout=5)
    plain.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    reply = plain.recv(4096).decode(errors="replace")
    plain.close()
    check("http request gets 426", reply.startswith("HTTP/1.1 426"), reply.split("\r\n")[0])


def test_bridge_catchup(server: Server) -> None:
    print("bridge catch-up")
    client = IPCClient(ipc_path(server))
    client.handshake("catchup")
    client.recv()
    client.send(IPCClient.FRAME, {
        "cmd": "SET_ACTIVITY",
        "args": {"pid": 321, "activity": {"details": "still here"}},
        "nonce": 1,
    })
    client.recv()

    late = Bridge(server.bridge_port)
    activity = late.wait_for(
        lambda m: bool(m.get("activity")) and m["activity"].get("details") == "still here",
        timeout=3,
    )
    check("late bridge client receives current activity", activity is not None,
          "no catch-up for the active socket")
    late.close()
    client.close()


def test_rejected_connections_do_not_clear(server: Server, bridge: Bridge) -> None:
    print("rejected connections leave activities alone")
    bridge.drain()
    client = IPCClient(ipc_path(server))
    client.handshake("keep-me")
    client.recv()
    client.send(IPCClient.FRAME, {
        "cmd": "SET_ACTIVITY",
        "args": {"pid": 777, "activity": {"details": "keep me"}},
        "nonce": 1,
    })
    client.recv()
    bridge.drain()

    # A rejected WebSocket (bad Origin) and a rejected IPC handshake (bad
    # version) must not publish a "clear" for socket 0.
    WSClient(server.ws_first, origin="https://evil.example").close()
    bad = IPCClient(ipc_path(server))
    bad.handshake("bad", version=9)
    try:
        bad.recv()
    except (socket.timeout, EOFError):
        pass
    bad.close()

    stale = bridge.wait_for(lambda m: m.get("activity") is None, timeout=1.5)
    check("no clear from rejected connections", stale is None,
          json.dumps(stale) if stale else "")

    late = Bridge(server.bridge_port)
    alive = late.wait_for(lambda m: bool(m.get("activity")) and m["activity"].get("details") == "keep me",
                          timeout=3)
    check("activity survives rejected connections", alive is not None)
    late.close()
    client.close()


def test_process_scanning(server: Server, bridge: Bridge) -> None:
    print("process scanning (fake process list)")
    bridge.drain()
    found = bridge.wait_for(
        lambda m: bool(m.get("activity")) and m["activity"].get("name") == "Brawlhalla",
        timeout=8,
    )
    check("fake game detected", found is not None, "no Brawlhalla activity seen")
    app_id = find_app_id("Brawlhalla")
    if found:
        check("detected activity has application_id",
              app_id == found["activity"]["application_id"],
              str(found["activity"].get("application_id")))
        check("detected activity has start timestamp",
              isinstance(found["activity"]["timestamps"]["start"], int),
              str(found["activity"].get("timestamps")))
        check("detected activity pid from fake entry", found.get("pid") == 12345, str(found.get("pid")))
        check("detected activity socketId is the app id",
              found.get("socketId") == found["activity"]["application_id"], str(found.get("socketId")))

    # The process disappears -> the scanner must clear the activity.
    server.write_fake_processes("")
    lost = bridge.wait_for(
        lambda m: m.get("activity") is None and m.get("socketId") == app_id,
        timeout=8,
    )
    check("process loss clears the activity", lost is not None,
          "no clear message for the detected game")

    # ...and it is re-detected with a fresh start timestamp when it comes back.
    server.write_fake_processes("/Applications/Brawlhalla.app/Contents/MacOS/Brawlhalla|12345")
    again = bridge.wait_for(
        lambda m: bool(m.get("activity")) and m["activity"].get("application_id") == app_id,
        timeout=8,
    )
    check("process reappearing is re-detected", again is not None)
    if again and found:
        check("re-detection has a fresh start timestamp",
              again["activity"]["timestamps"]["start"] >= found["activity"]["timestamps"]["start"])



def find_app_id(name: str) -> str | None:
    output = subprocess.run([BINARY, "--detect-path", f"/Applications/{name}.app/Contents/MacOS/{name}"],
                            capture_output=True, text=True)
    match = re.search(r"match: .*\(([0-9]+)\)", output.stdout)
    return match.group(1) if match else None


def test_probe_skips_used_socket(base_dir: str) -> None:
    print("ipc socket probing")
    base = os.path.join(base_dir, "probe", "discord-ipc")
    os.makedirs(os.path.dirname(base), exist_ok=True)

    # Pretend to be Discord: own -0 and answer PING with a PONG.
    squatter = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    squatter.bind(base + "-0")
    squatter.listen(4)
    squatter.settimeout(0.05)
    squatter_path = base + "-0"

    port = free_port()
    ws_base_guard = 31000 + os.getpid() % 5000
    log = open(os.path.join(base_dir, "probe.log"), "w")
    ws_base = 20000 + os.getpid() % 20000
    proc = subprocess.Popen([BINARY, "--ipc-base", base, "--bridge-port", str(port),
                             "--ws-ports", f"{ws_base}-{ws_base + 4}",
                             "--no-process-scanning", "--no-color"],
                            stdout=log, stderr=subprocess.STDOUT)
    try:
        got_pong = False
        deadline = time.time() + 6
        conn = None
        while time.time() < deadline:
            try:
                conn, _ = squatter.accept()
                conn.settimeout(1)
                header = conn.recv(8)
                if len(header) == 8:
                    _, length = struct.unpack("<ii", header)
                    conn.recv(length)
                    conn.sendall(struct.pack("<ii", 4, 2) + b" 1"[:2])
                    got_pong = True
            except socket.timeout:
                pass
            except OSError:
                break
            if got_pong and os.path.exists(base + "-1"):
                break

        check("server pinged the squatted socket", got_pong)
        check("server fell through to -1", os.path.exists(base + "-1"), str(os.listdir(os.path.dirname(base))))

        client = IPCClient(base + "-1")
        client.handshake("probe-test")
        _, ready = client.recv()
        check("handshake works on -1", ready.get("evt") == "READY", str(ready)[:120])
        client.close()

        # --ipc-socket must refuse to take over a socket that answers pings.
        guard_log = os.path.join(base_dir, "guard.log")
        with open(guard_log, "w") as handle:
            guard = subprocess.Popen([BINARY, "--ipc-socket", squatter_path,
                                      "--bridge-port", str(free_port()),
                                      "--ws-ports", f"{ws_base_guard}-{ws_base_guard + 1}",
                                      "--no-process-scanning", "--no-color"],
                                     stdout=handle, stderr=subprocess.STDOUT)
        try:
            deadline = time.time() + 8
            refused = False
            while time.time() < deadline:
                # Answer the availability probe so the squatter looks alive.
                try:
                    conn, _ = squatter.accept()
                    conn.settimeout(1)
                    header = conn.recv(8)
                    if len(header) == 8:
                        _, length = struct.unpack("<ii", header)
                        conn.recv(length)
                        conn.sendall(struct.pack("<ii", 4, 2) + b" 1")
                except socket.timeout:
                    pass
                except OSError:
                    break
                with open(guard_log) as handle:
                    if "already in use" in handle.read():
                        refused = True
                        break
            check("--ipc-socket refuses an active socket", refused,
                  open(guard_log).read()[-300:])
        finally:
            guard.terminate()
            try:
                guard.wait(timeout=5)
            except subprocess.TimeoutExpired:
                guard.kill()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        squatter.close()
        try:
            os.unlink(squatter_path)
        except OSError:
            pass


def test_options(base_dir: str) -> None:
    print("extra options")
    sock_path = os.path.join(base_dir, "custom.sock")
    bridge_port = free_port()
    ws_base = 30000 + os.getpid() % 10000
    fake_file = os.path.join(base_dir, "fake_options.txt")
    with open(fake_file, "w") as handle:
        handle.write("/Applications/Brawlhalla.app/Contents/MacOS/Brawlhalla|111\n")
    log = open(os.path.join(base_dir, "options.log"), "w")

    # Squat on the first port of the range, like Discord desktop does on 6463.
    squatter = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    squatter.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    squatter.bind(("127.0.0.1", ws_base))
    squatter.listen(4)

    proc = subprocess.Popen(
        [BINARY, "--ipc-socket", sock_path, "--ipc-base", "/nonexistent/ignored",
         "--ws-ports", f"{ws_base}-{ws_base + 2}", "--bridge-port", str(bridge_port),
         "--allow-origin", "https://custom.example", "--no-process-scanning",
         "--scan-interval", "1", "--no-color"],
        stdout=log, stderr=subprocess.STDOUT,
        env={**os.environ, "ARRPC_FAKE_PROCESSES_FILE": fake_file},
    )
    try:
        deadline = time.time() + 10
        while time.time() < deadline and not os.path.exists(sock_path):
            if proc.poll() is not None:
                raise RuntimeError("options server exited")
            time.sleep(0.05)

        check("--ipc-socket binds the exact path", os.path.exists(sock_path), sock_path)
        check("--ipc-socket ignores --ipc-base probing", not os.path.exists("/nonexistent/ignored-0"))

        client = IPCClient(sock_path)
        client.handshake("opt-test")
        _, ready = client.recv()
        check("exact socket accepts handshake", ready.get("evt") == "READY", str(ready)[:100])
        client.close()

        check("occupied ws port is skipped", socket.socket(socket.AF_INET, socket.SOCK_STREAM)
              .connect_ex(("127.0.0.1", ws_base + 1)) == 0)

        allowed = WSClient(ws_base + 1, origin="https://custom.example")
        check("--allow-origin accepts the extra origin", allowed.recv_json()["evt"] == "READY")
        allowed.close()

        rejected = WSClient(ws_base + 1, origin="https://other.example")
        check("--allow-origin still rejects others", rejected.recv_json() is None)
        rejected.close()

        bridge = Bridge(bridge_port)
        activity = bridge.wait_for(lambda m: m.get("activity") is not None, timeout=2.5)
        check("--no-process-scanning suppresses game detection", activity is None,
              json.dumps(activity)[:120] if activity else "")
        bridge.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        squatter.close()


def test_cli() -> None:
    print("cli helpers")
    out = subprocess.run([BINARY, "--detect-path", "/Applications/Brawlhalla.app/Contents/MacOS/Brawlhalla"],
                         capture_output=True, text=True)
    check("--detect-path matches a .app bundle", out.returncode == 0 and "Brawlhalla" in out.stdout,
          out.stdout + out.stderr)

    out = subprocess.run([BINARY, "--detect-path", "/Applications/Dota 2/osx64/dota2.app/Contents/MacOS/dota2"],
                         capture_output=True, text=True)
    check("--detect-path matches a nested suffix", out.returncode == 0 and "Dota 2" in out.stdout,
          out.stdout + out.stderr)

    out = subprocess.run([BINARY, "--detect-path", "/usr/bin/nope"], capture_output=True, text=True)
    check("--detect-path reports no match", out.returncode == 1 and "no match" in out.stdout, out.stdout)

    out = subprocess.run([BINARY, "--scan-once"], capture_output=True, text=True,
                         env={**os.environ, "ARRPC_FAKE_PROCESSES": "/tmp/hl2_osx|4242"})
    check("--scan-once runs", out.returncode == 0, out.stdout + out.stderr)

    out = subprocess.run([BINARY, "--bogus"], capture_output=True, text=True)
    check("unknown option rejected", out.returncode == 2 and "unknown option" in out.stderr, out.stderr)

    out = subprocess.run([BINARY, "--version"], capture_output=True, text=True)
    check("--version works", out.returncode == 0 and "1.0.0" in out.stdout, out.stdout)


def main() -> int:
    if not os.path.exists(BINARY):
        print(f"binary not found: {BINARY}", file=sys.stderr)
        return 1

    test_cli()

    base_dir = tempfile.mkdtemp(prefix="arrpc-test-")
    server = Server(base_dir, fake_processes="/Applications/Brawlhalla.app/Contents/MacOS/Brawlhalla|12345")
    try:
        server.wait_ready()
        bridge = Bridge(server.bridge_port)
        try:
            test_ipc(server, bridge)
            test_ipc_handshake_errors(server)
            test_control_commands(server, bridge)
            test_websocket(server, bridge)
            test_rejected_connections_do_not_clear(server, bridge)
            test_process_scanning(server, bridge)
            test_bridge_catchup(server)
        finally:
            bridge.close()
    except Exception as exc:  # noqa: BLE001
        print(f"\nharness error: {exc}")
        print(server.logs()[-4000:])
        FAILURES.append("harness")
        global FAIL
        FAIL += 1
    finally:
        server.stop()

    check("ipc socket unlinked on shutdown", not os.path.exists(server.base + "-0"),
          str(os.listdir(base_dir)))

    try:
        test_probe_skips_used_socket(base_dir)
        test_options(base_dir)
    finally:
        shutil.rmtree(base_dir, ignore_errors=True)

    print(f"\n{PASS} passed, {FAIL} failed")
    if FAILURES:
        print("failed:", ", ".join(FAILURES))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
