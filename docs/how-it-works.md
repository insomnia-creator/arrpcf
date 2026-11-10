---
title: How it works
description: Components, startup order and data flow inside arrpc.
permalink: /how-it-works/
---

## Components

arrpc is one process with one thread and one `poll()` loop. Each transport is an
object that owns a listening socket, a set of connections, and three callbacks
(`connect`, `message`, `close`) that the protocol layer subscribes to.

| Source file | Responsibility |
| --- | --- |
| `src/main.cpp` | wiring, signal handling, startup order |
| `src/ipc.cpp` | Discord IPC transport: binary framing, socket probing, handshake validation |
| `src/ws.cpp` | minimal RFC 6455 server: handshake, frames, fragmentation, ping/pong/close |
| `src/rpc.cpp` | protocol: `READY`, `SET_ACTIVITY` translation, invites, deep links |
| `src/bridge.cpp` | bridge WebSocket, per-socket cache, catch-up on connect |
| `src/detect.cpp` | matching executables against Discord's detectable database |
| `src/process_scan.cpp` | `libproc` scan, `KERN_PROCARGS2` arguments, detect/lose state |
| `src/net.cpp` | sockets, SHA-1, base64, query parsing |
| `src/reactor.cpp` | `poll()` loop plus one-shot and repeating timers |
| `src/json.cpp` | thin wrapper around yyjson's mutable DOM |
| `src/config.cpp` | flag and environment parsing |

<pre class="diagram">
main
 ├─ Bridge ─────────── WsServer(127.0.0.1:1337)      → bridge clients
 ├─ RpcService
 ├─ IpcServer ──────── unix $TMPDIR/discord-ipc-N    → RPC applications
 ├─ WsServer ───────── 127.0.0.1:6463-6472           → RPC applications
 ├─ ProcessScanner ─── libproc, every N seconds
 └─ Reactor ────────── poll() + timers + signal pipe
</pre>

## Startup order

1. Parse flags and environment variables; configure logging.
2. Load the detectable database — the embedded darwin subset, or `--detectable`
   (Discord's full 11 MB file parses in about 50 ms).
3. Start the bridge listener, then the IPC listener, then the RPC WebSocket listener.
   If none of them can bind, exit with status `1`.
4. Start the process scanner (unless disabled): one scan immediately, then on a timer.
5. Install `SIGINT`/`SIGTERM` handlers through a self-pipe and run the loop.

Shutdown is the reverse: the loop stops, listeners close, and the IPC socket file is
unlinked.

## Flow 1 — an RPC application reports something

```
app → IPC/WS transport → RpcService
```

1. The transport validates the handshake (`v: 1`, non-empty `client_id`, allowed
   `Origin`, `encoding=json`) and calls `RpcService::attach_connection`.
2. The connection receives a socket id and a `READY` packet shaped exactly like
   Discord's, including the mock `arRPC` user.
3. `SET_ACTIVITY` is translated once into two different shapes:
   * **response to the application** — the activity echoed back with `name: ""`,
     `application_id` and `type: 0`, so libraries see what they expect;
   * **bridge message** — Discord's internal `LOCAL_ACTIVITY_UPDATE` shape:
     `{activity, pid, socketId}` with `metadata.button_urls`, label-only `buttons`
     and `flags` from `instance`.
4. The bridge message is cached per socket id and broadcast to every connected
   bridge client.

## Flow 2 — a game is detected

```
ProcessScanner → synthetic SET_ACTIVITY → bridge
```

The scanner walks the process table, matches executables against Discord's database
and emits the equivalent of an `SET_ACTIVITY` call whose `application_id` is the
Discord application id. It re-emits on every scan — that is deliberately the same as
arRPC, and it means a Discord client that connects late still receives current
activities through the bridge cache. When a process disappears, an `activity: null`
message with the last pid is published.

## Flow 3 — a bridge client connects late

The bridge keeps the last bridge message per socket id, dropping entries as soon as
they are cleared. On connect, every cached live activity is replayed, so a browser
tab opened after a game started still shows the game.

## Flow 4 — an RPC client disconnects

On close, arrpc publishes `{activity: null, pid: <last pid>, socketId: <id>}` so the
web client clears the card instead of showing a ghost. Connections that were rejected
before they got a socket id (bad origin, unsupported version, bad handshake) never
publish anything.

## The loop

`Reactor` is a small `poll()` dispatcher:

- file descriptor callbacks for listeners and connections (`POLLIN`, `POLLOUT` when
  output is buffered);
- one-shot and repeating timers (the scan interval, deferred socket drops);
- a self-pipe for signals, because `poll()` has to be interrupted to notice them.

Deferred work matters: closing a connection from inside its own callback would free
the `std::function` currently executing, so drops are posted and callbacks are copied
before being invoked. The whole design is single-threaded — no locks appear anywhere
in the codebase.

## Robustness choices

| Situation | Behaviour |
| --- | --- |
| Client sends unmasked or nested frames | WebSocket protocol error, close `1002` |
| Message larger than 8 MB | close `1009` |
| Malformed JSON | logged and ignored (arRPC crashes here) |
| `SIGPIPE` on a dead socket | ignored globally, writes fail normally |
| `--open-links` spawns `/usr/bin/open` | `SIGCHLD` ignored so children are reaped |
| Rejected connection closes | no activity update for anyone |

## Ownership at a glance

Connections are `shared_ptr`s owned by their transport; reactor callbacks hold a
reference so a connection cannot vanish mid-callback, and each connection keeps a
`weak_ptr` to itself for deferred drops. JSON documents are allocated per message and
freed at the end of the handler, so memory does not grow with traffic (12 000
activities moved RSS by about 0.4 MB, all allocator arenas).
