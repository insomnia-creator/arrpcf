---
title: Overview
description: A small C++ reimplementation of arRPC for macOS.
permalink: /
---

<div class="hero">
  <span class="hero-kicker">C++17 · macOS · 316 KB</span>
  <h1>Discord RPC everywhere, natively.</h1>
  <p class="hero-lede">
    arrpc is a from-scratch reimplementation of <a href="https://github.com/OpenAsar/arrpc">arRPC</a>
    for macOS. It opens Discord's local RPC servers, lets ordinary RPC applications connect
    to them, and republishes what they report on the bridge WebSocket that Discord web,
    Vencord, ArmCord and Vesktop understand.
  </p>
  <p class="hero-actions">
    <a class="button button-primary" href="{{ '/install/' | relative_url }}">Build it</a>
    <a class="button" href="{{ '/options/' | relative_url }}">Options</a>
    <a class="button" href="{{ '/how-it-works/' | relative_url }}">How it works</a>
  </p>
</div>

## What it does

Rich Presence normally works because Discord's desktop client runs a few local servers
for applications to talk to. Without that client — Discord in a browser, or an
alternative client — those servers do not exist and games and apps have nothing to
talk to.

arrpc provides them:

<pre class="diagram">
┌─ application with Discord RPC ─────────────────────────┐
│ discord-rpc · @xhayper/discord-rpc · Rust SDK · games  │
└───────────┬─────────────────────────┬──────────────────┘
            │                         │
            ▼                         ▼
  $TMPDIR/discord-ipc-N   ws://127.0.0.1:6463-6472
┌─ arrpc ────────────────────────────────────────────────┐
│ IPC transport · WebSocket transport · macOS game scan  │
│ SET_ACTIVITY  →  LOCAL_ACTIVITY_UPDATE shape           │
└─────────────────────────┬──────────────────────────────┘
                          │
                          ▼
                 ws://127.0.0.1:1337
┌─ bridge consumer ──────────────────────────────────────┐
│ Discord web + bridge mod · Vencord · Vesktop           │
└────────────────────────────────────────────────────────┘
</pre>

The bridge protocol is unchanged from arRPC, so every existing consumer already works:
the Vencord `WebRichPresence (arRPC)` plugin, `examples/bridge_mod.js`, and the
integrations built into ArmCord and Vesktop.

## Why a rewrite

- **One small binary.** 316 KB of C++17 that links only `libSystem` and `libc++`.
  No Node runtime, no npm tree, no Electron helper process.
- **Protocol-faithful, not a shim.** The IPC packet framing, WebSocket query
  parameters, `READY` payload, `SET_ACTIVITY` translation, probes and close codes
  match arRPC — verified with a differential test against the original.
- **A real macOS server.** `launchd`-friendly, per-user IPC socket, game detection
  through `libproc` and `KERN_PROCARGS2` instead of shelling out to `ps`.
- **Single-threaded by design.** One `poll()` loop, no locks, no races; the binary
  is small because the problem is small.

## Quick start

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/arrpc
```

Then enable a bridge consumer in Discord web — for Vencord, turn on the
`WebRichPresence (arRPC)` plugin — and start any RPC-capable application.

## At a glance

| What | Where |
| --- | --- |
| RPC IPC socket | `$XDG_RUNTIME_DIR` / `$TMPDIR` / `$TMP` / `$TEMP` / `/tmp` + `discord-ipc-0` … `-9` |
| RPC WebSocket | `127.0.0.1:6463` – `6472` (first free port) |
| Bridge WebSocket | `127.0.0.1:1337` (`--bridge-port`, `ARRPC_BRIDGE_PORT`) |
| Game detection | macOS process table, every 5 s, builtin darwin subset of Discord's DB |
| Logs | `[arRPC]` / `[arRPC > ipc|websocket|bridge|process]` on stdout |

## Where to go next

- [Install]({{ '/install/' | relative_url }}) — build, run, launchd service.
- [Options]({{ '/options/' | relative_url }}) — every flag and environment variable.
- [How it works]({{ '/how-it-works/' | relative_url }}) — components and data flow.
- [Protocol]({{ '/protocol/' | relative_url }}) — the wire formats, with examples.
- [Game detection]({{ '/games/' | relative_url }}) — how macOS games are found.
- [arRPC parity]({{ '/parity/' | relative_url }}) — what matches, what intentionally differs.
- [Testing]({{ '/testing/' | relative_url }}) — the suite and the differential test.
- [Troubleshooting]({{ '/troubleshooting/' | relative_url }}) — when nothing shows up.
