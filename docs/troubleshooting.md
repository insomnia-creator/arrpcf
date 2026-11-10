---
title: Troubleshooting
description: Why nothing shows up in Discord, and how to find out quickly.
permalink: /troubleshooting/
---

## First, check that arrpc is doing something

```sh
arrpc --debug
```

Look for the listener lines and, when an application connects, a handshake:

```
[arRPC > bridge] listening on 1337
[arRPC > ipc] listening at /var/folders/…/T/discord-ipc-0
[arRPC > websocket] listening on 6463
[arRPC > ipc] new connection!
[arRPC > ipc] handshake: v=1 client_id=383226320970055681
[arRPC > rpc] sending: {"cmd":"SET_ACTIVITY", … }
```

If you see `message: … SET_ACTIVITY` and the matching `sending:` line, arrpc is fine
and the problem is on the Discord side (bridge consumer, port, CSP).

## Watch the bridge directly

Any WebSocket client works; this one needs nothing but Python 3:

```sh
python3 - <<'PY'
import base64, json, os, socket, struct

s = socket.create_connection(("127.0.0.1", 1337), timeout=5)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(f"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
          f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())

buf = b""
while b"\r\n\r\n" not in buf:
    buf += s.recv(4096)
buf = buf.split(b"\r\n\r\n", 1)[1]

while True:
    while len(buf) < 2:
        buf += s.recv(4096)
    n, off = buf[1] & 0x7F, 2
    if n == 126:
        n, off = struct.unpack(">H", buf[2:4])[0], 4
    elif n == 127:
        n, off = struct.unpack(">Q", buf[2:10])[0], 10
    while len(buf) < off + n:
        buf += s.recv(4096)
    print(json.dumps(json.loads(buf[off:off + n])))
    buf = buf[off + n:]
PY
```

If activities appear here but not in Discord, the bridge consumer is not running or
is listening on a different port.

## Nothing appears in Discord web

| Check | Fix |
| --- | --- |
| Is a bridge consumer enabled? | Vencord: enable `WebRichPresence (arRPC)`. No mods: run `examples/bridge_mod.js` in the console. |
| Is the consumer using the same port? | arrpc uses `1337` unless changed; the plugin expects `ws://127.0.0.1:1337`. |
| Webcord with a strict CSP? | Disable *Use built-in Content Security Policy* in Advanced settings, then run the bridge mod. |
| Does the application actually send RPC? | `--debug` must show a `SET_ACTIVITY`. Many apps only start RPC when they see a desktop client at launch — restart the app after arrpc is up. |
| Does the browser block localhost? | Allow `ws://127.0.0.1` for the Discord tab (some hardened profiles block it). |

## Discord desktop is running

Discord desktop owns `$TMPDIR/discord-ipc-0`, so arrpc takes `-1`. RPC applications
try `-0` first, find Discord and talk to it instead — the same limitation arRPC has.

```
[arRPC > ipc] not available, trying again (attempt 1)
[arRPC > ipc] listening at /var/folders/…/T/discord-ipc-1
```

Options:

- Quit Discord desktop (the normal setup for Discord web), or
- start arrpc with `--ipc-socket "$TMPDIR/discord-ipc-0"` while Discord is not
  running, to force index `0`.

arrpc refuses `--ipc-socket` if something still answers pings there, so this cannot
silently break a live Discord.

## The bridge or the ports are taken

```
[arRPC > bridge] 1337 in use! (Address already in use)
[arRPC > websocket] 6463 in use! (Address already in use)
```

Find and stop the other process, or move arrpc:

```sh
lsof -nP -iTCP:1337 -sTCP:LISTEN
arrpc --bridge-port 11337          # point the consumer at the new port too
arrpc --ws-ports 16463-16472
```

If no transport can bind at all, arrpc exits with status `1`.

## The application's origin is rejected

```
[arRPC > websocket] disallowed origin https://example.com
```

Only Discord's own origins are allowed, like arRPC. Add one with
`--allow-origin https://example.com` (repeatable), or `--allow-any-origin` for
experiments.

## Games are not detected

```sh
arrpc --scan-once --debug
arrpc --detect-path "/Applications/The Game.app/Contents/MacOS/The Game"
```

- The game must be in Discord's detectable database — unlisted titles are not
  detected, even if they support RPC natively.
- Processes of other users are invisible; run arrpc as the same user (LaunchAgent).
- Sandboxed builds (App Store) cannot read the process table.
- Wrappers (Wine, CrossOver, Parallels) and renamed bundles change the path.
- Detection publishes synthetic activities; a bridge consumer is still required to
  see them.

## Invites and deep links do nothing

That is expected without `--open-links`: arrpc answers the RPC call so the application
does not hang, but does not open a browser. With the flag, valid targets are opened
with `/usr/bin/open`.

## Logs, exit codes, cleanup

| Code | Meaning |
| --- | --- |
| `0` | normal shutdown or successful one-shot command |
| `1` | no transport could bind, unreadable `--detectable`, no `--detect-path` match |
| `2` | invalid command line |

Run with `--debug` for frames and handshakes. Running under `launchd`, logs go to
`/tmp/arrpc.log` by default (see [Install]({{ '/install/' | relative_url }})).

Check for stray instances:

```sh
pgrep -fl arrpc
pkill -f /usr/local/bin/arrpc
```

arrpc creates no configuration, cache or preference files. Its IPC socket file is
removed on shutdown; if a crash leaves one behind, the next start replaces it because
nothing answers pings on it.
