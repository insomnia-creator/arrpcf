---
title: arRPC parity
description: What arrpc matches from arRPC v3.7.0, and the handful of deliberate differences.
permalink: /parity/
---

## Compatibility target

arrpc implements the protocols and behaviour of
[arRPC v3.7.0](https://github.com/OpenAsar/arrpc). The bridge protocol is unchanged,
so every consumer that works with arRPC works with arrpc — the Vencord
`WebRichPresence (arRPC)` plugin, `examples/bridge_mod.js`, and the integrations in
ArmCord and Vesktop.

## What is implemented

| Area | arRPC | arrpc |
| --- | --- | --- |
| IPC transport (unix socket) | yes | yes — same framing, same probing, same close codes |
| IPC socket index probing `-0…-9` | yes | yes — one-second `PING` probe per index |
| RPC WebSocket `6463-6472` | yes | yes — JSON, `Origin` allow-list, `v=1` |
| Erlpack (`encoding=etf`) | rejected | rejected |
| HTTP transport | not implemented | not implemented (`426` instead of a hang) |
| Bridge WebSocket | yes | yes — broadcast plus catch-up per socket |
| `DISPATCH` / `READY` | yes | yes — identical payload, mock `arRPC` user |
| `SET_ACTIVITY` translation | yes | yes — buttons, `instance`→`flags`, seconds→ms |
| Activity clear on `null` | yes | yes |
| Activity clear on disconnect | yes | yes — with the last reported pid |
| `CONNECTIONS_CALLBACK` | generic success | identical reply |
| `INVITE_BROWSER` / `GUILD_TEMPLATE_BROWSER` | emitted, never answered | always answered (see below) |
| `DEEP_LINK` | emitted, never answered | always answered |
| Process scanning | Windows + Linux | macOS (`libproc`) |
| Detectable database | full 11.6 MB file | embedded 7.3 KB darwin subset, `--detectable` for the full file |
| Env variables | `ARRPC_BRIDGE_PORT`, `ARRPC_NO_PROCESS_SCANNING`, `ARRPC_DEBUG` | same names |

## Intentional differences

| arRPC | arrpc | Why |
| --- | --- | --- |
| Bridge binds `0.0.0.0` | `127.0.0.1` by default (`--bridge-host 0.0.0.0` restores it) | bridge messages contain your activity; it should not be reachable from the LAN |
| Invites, guild templates and deep links are emitted to a callback with no listener, so the client never gets a reply | always answered; `--open-links` also opens the target | avoids RPC libraries hanging on those calls |
| Malformed WebSocket JSON crashes the process | logged and ignored | robustness |
| Scanner matches executables of every platform in the database | darwin entries only | no false positives on macOS |
| Database names are matched as suffixes only | additionally matches `/Foo.app/Contents/MacOS/Foo` bundles | macOS reports the inner binary, not the bundle |
| Every matching entry can emit an activity | one activity per application id per scan | no duplicate ghosts when processes share an engine |
| `--ipc-socket` equivalent does not exist | refuses a socket that answers pings | an exact path cannot silently kill a live Discord |
| Rejected connections still emit a close event | rejected connections publish nothing | a bad `Origin` cannot clear the card of an unrelated socket |
| Plain HTTP to `6463` hangs | `426 Upgrade Required` | debuggability |
| No `--version`, `--help` output, `--scan-once`, `--detect-path` | available | operability |

## How the parity was verified

`tools/compare_upstream.py` runs one identical RPC session against the original Node
arRPC and against arrpc, then compares:

- the `READY` packet,
- the response to every command in the session,
- every message published on the bridge (ignoring only `socketId`, which is a
  per-server counter).

```sh
git clone https://github.com/OpenAsar/arrpc /tmp/arrpc-ref
(cd /tmp/arrpc-ref && npm install)
python3 tools/compare_upstream.py /tmp/arrpc-ref build/arrpc
```

Result against arRPC v3.7.0:

| Comparison | Result |
| --- | --- |
| `READY` packet | identical |
| `SET_ACTIVITY` (buttons, timestamps, instance, assets, party) | identical |
| `SET_ACTIVITY` (second activity) | identical |
| `SET_ACTIVITY` (`activity: null`) | identical |
| `CONNECTIONS_CALLBACK` | identical |
| unknown command | identical (no reply) |
| `INVITE_BROWSER` / `GUILD_TEMPLATE_BROWSER` / `DEEP_LINK` | intentional difference: arRPC is silent, arrpc answers |
| bridge messages, including the disconnect clear | identical |

```
READY identical: True
response[0] SET_ACTIVITY: identical
response[1] SET_ACTIVITY: identical
response[2] SET_ACTIVITY: identical
response[3] CONNECTIONS_CALLBACK: identical
response[4] INVITE_BROWSER: intentional difference (upstream silent, we answer …)
response[5] GUILD_TEMPLATE_BROWSER: intentional difference (upstream silent, we answer …)
response[6] DEEP_LINK: intentional difference (upstream silent, we answer …)
response[7] BOGUS_COMMAND: identical
bridge payloads identical (4 messages): True

PARITY OK
```

The same session was also replayed with a real `discord-rpc@4` Node client
(`setActivity` with buttons, assets, timestamps and `instance`, then
`clearActivity`) — see [Testing]({{ '/testing/' | relative_url }}).
