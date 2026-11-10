---
title: Protocol
description: The wire formats arrpcf speaks — IPC framing, WebSocket parameters, RPC commands and bridge messages.
permalink: /protocol/
---

arrpcf implements two of Discord's local RPC transports and one output protocol.
Everything on this page is byte-for-byte compatible with arRPC v3.7.0 except where
[arRPC parity]({{ '/parity/' | relative_url }}) lists an intentional difference.

## Transport discovery

| Transport | Address |
| --- | --- |
| IPC | `$XDG_RUNTIME_DIR` / `$TMPDIR` / `$TMP` / `$TEMP` / `/tmp` + `discord-ipc-0` … `-9` |
| WebSocket | `127.0.0.1:6463` … `6472`, first free port |

The IPC index is chosen by probing: arrpcf connects to `-N`, sends a `PING` and waits up
to one second. A connection failure or silence means the slot is free; a reply means
somebody (Discord desktop, another arrpcf) owns it and the next index is tried.

## IPC framing

Every packet is an 8-byte header followed by a UTF-8 JSON body:

| Offset | Size | Type | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | int32, little-endian | packet type |
| 4 | 4 | int32, little-endian | body length in bytes |
| 8 | n | bytes | JSON |

| Type | Name | Direction | Body |
| --- | --- | --- | --- |
| `0` | `HANDSHAKE` | app → arrpcf | `{"v": 1, "client_id": "…"}` |
| `1` | `FRAME` | both | an RPC command or response |
| `2` | `CLOSE` | both | `{"code": 1000, "message": "…"}` |
| `3` | `PING` | both | any JSON value, echoed back |
| `4` | `PONG` | both | the same value |

Packets are independent: a connection may coalesce or split them across reads, and
arrpcf buffers until a whole packet is available. Bodies larger than 8 MB are rejected.

The first packet must be a `HANDSHAKE` with `v: 1` and a non-empty `client_id`.
Violations close the connection with:

| Code | Meaning |
| --- | --- |
| `4004` | unsupported version |
| `4000` | client id required |
| `1003` | unsupported: bad framing, frame before handshake, not JSON |

## WebSocket transport

Connect to `ws://127.0.0.1:6463/?v=1&encoding=json&client_id=<id>`.

| Query parameter | Default | Accepted |
| --- | --- | --- |
| `v` | `1` | anything parsing to `1` (JS `parseInt` semantics) |
| `encoding` | `json` | `json` only — `etf`/Erlpack is not implemented, like arRPC |
| `client_id` | empty | optional; used as the activity's `application_id` |

| Header | Rule |
| --- | --- |
| `Upgrade` / `Connection` | must request a WebSocket |
| `Origin` | absent, or one of `https://discord.com`, `https://ptb.discord.com`, `https://canary.discord.com`, or a `--allow-origin` value |

Rejected connections receive a normal close frame and publish nothing. A plain HTTP
request gets `426 Upgrade Required` instead of hanging. Text and binary frames,
fragmentation and ping/pong are supported; messages are capped at 8 MB.

## READY

Immediately after a valid handshake, the client receives:

```json
{
  "cmd": "DISPATCH",
  "data": {
    "v": 1,
    "config": {
      "cdn_host": "cdn.discordapp.com",
      "api_endpoint": "//discord.com/api",
      "environment": "production"
    },
    "user": {
      "id": "1045800378228281345",
      "username": "arrpcf",
      "discriminator": "0",
      "global_name": "arRPC",
      "avatar": "cfefa4d9839fb4bdf030f91c2a13e95c",
      "avatar_decoration_data": null,
      "bot": false,
      "flags": 0,
      "premium_type": 0
    }
  },
  "evt": "READY",
  "nonce": null
}
```

The user is a mock account; `arrpcf` is reported as the connected user, exactly as
arRPC does.

## SET_ACTIVITY

Request from the application:

```json
{
  "cmd": "SET_ACTIVITY",
  "nonce": "nonce-1",
  "args": {
    "pid": 999,
    "activity": {
      "details": "In a menu",
      "state": "Waiting",
      "timestamps": { "start": 1700000000 },
      "buttons": [{ "label": "Website", "url": "https://example.com" }],
      "instance": true
    }
  }
}
```

### Translation rules

| Input | Output |
| --- | --- |
| `application_id` | the connection's `client_id` |
| `type` | always `0` (playing) |
| `buttons[].url` | `metadata.button_urls` |
| `buttons[].label` | `buttons` (labels only) |
| `instance` | `flags` bit 0 (`1` when truthy, else `0`) |
| `timestamps.*` in seconds | multiplied by 1000 when the value has at least three fewer digits than the current millisecond clock (i.e. `1700000000` → `1700000000000`) |
| every other activity field | passed through unchanged |

The activity is merged **over** the generated fields, so an activity that already
contains `type`, `flags` or `metadata` keeps its own values — the same spread order
arRPC uses. `pid` updates the connection's remembered pid.

### Response to the application

```json
{
  "cmd": "SET_ACTIVITY",
  "data": {
    "details": "In a menu",
    "state": "Waiting",
    "timestamps": { "start": 1700000000000 },
    "buttons": [{ "label": "Website", "url": "https://example.com" }],
    "instance": true,
    "name": "",
    "application_id": "1234567890",
    "type": 0
  },
  "evt": null,
  "nonce": "nonce-1"
}
```

### Bridge message

```json
{
  "activity": {
    "application_id": "1234567890",
    "type": 0,
    "metadata": { "button_urls": ["https://example.com"] },
    "flags": 1,
    "details": "In a menu",
    "state": "Waiting",
    "timestamps": { "start": 1700000000000 },
    "buttons": ["Website"],
    "instance": true
  },
  "pid": 999,
  "socketId": "0"
}
```

`pid` is omitted when the application did not send one. `socketId` is the numeric
connection id as a string — or the Discord application id for process-detected games.

### Clearing an activity

`activity: null` (or a missing activity) clears the card:

```json
{ "cmd": "SET_ACTIVITY", "data": null, "evt": null, "nonce": 5 }
```

and publishes `{"activity": null, "pid": 999, "socketId": "0"}` on the bridge.
Disconnecting also clears the card, using the last pid the connection reported.

## Control commands

| Command | Request arguments | Reply |
| --- | --- | --- |
| `CONNECTIONS_CALLBACK` | any | `data: {"code": 1000}`, `evt: "ERROR"` |
| `INVITE_BROWSER` | `{"code": "abcdef"}` | `data: {"code": "abcdef"}`, `evt: null` |
| `INVITE_BROWSER` (bad code) | empty or malformed | `data: {"code": 4011, "message": "Invalid invite id: …"}`, `evt: "ERROR"` |
| `GUILD_TEMPLATE_BROWSER` | `{"code": "abcdef"}` | `data: {"code": "abcdef"}`, `evt: null` |
| `GUILD_TEMPLATE_BROWSER` (bad code) | empty or malformed | `data: {"code": 4017, "message": "Invalid guild template id: …"}`, `evt: "ERROR"` |
| `DEEP_LINK` | `{"url": "https://…"}` | `data: null`, `evt: null` |
| anything else | — | ignored, no reply (same as arRPC) |

Invite and template codes are considered valid when they are 2–64 characters of
`[A-Za-z0-9_-]`. With `--open-links`, valid targets are also opened with
`/usr/bin/open`.

<div class="note">
arRPC emits these commands to a callback that has no listener and therefore never
replies, which makes some RPC libraries hang. arrpcf always replies; this is the one
deliberate behavioural change, listed in <a href="{{ '/parity/' | relative_url }}">parity</a>.
</div>

## Bridge protocol

The bridge listens on `ws://127.0.0.1:1337` and only ever sends messages:

```json
{ "activity": { … }, "pid": 999, "socketId": "0" }
{ "activity": null,  "pid": 999, "socketId": "0" }
```

A minimal consumer, which is all the bridge mods do:

```js
const ws = new WebSocket('ws://127.0.0.1:1337');

ws.onmessage = ({ data }) => {
  const { activity, pid, socketId } = JSON.parse(data);

  if (activity === null) {
    dispatcher.dispatch({ type: 'LOCAL_ACTIVITY_UPDATE', activity: null, pid, socketId });
    return;
  }

  // Give the activity a name the client can display.
  if (!activity.name) activity.name = appNameFor(activity.application_id);

  dispatcher.dispatch({ type: 'LOCAL_ACTIVITY_UPDATE', activity, pid, socketId });
};
```

New bridge clients immediately receive the latest message of every socket that still
has a live activity, so opening Discord after a game starts works. Cleared sockets are
forgotten, so nothing stale is replayed.

The bridge is one-way: messages sent by a bridge client are read and discarded.
