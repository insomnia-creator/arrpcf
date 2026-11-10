---
title: Testing
description: The protocol test suite, sanitizer builds, the differential test against arRPC and a real RPC client.
permalink: /testing/
---

## Protocol test suite

```sh
make test
# or
python3 tools/test_server.py build/arrpcf
```

76 checks, Python standard library only, no fixtures. The suite boots the real binary
on private ports and socket paths in a temporary directory, then drives it with raw
IPC packets and hand-written WebSocket frames.

| Group | Covered |
| --- | --- |
| CLI helpers | `--detect-path` (bundle, nested suffix, no match), `--scan-once`, `--version`, unknown option |
| IPC transport | framing, `READY` payload, `PING`/`PONG`, `SET_ACTIVITY` translation, large payload, clearing, disconnect clear, last pid |
| IPC handshake | missing `client_id` (`4000`), bad version (`4004`), frame before handshake (`1003`) |
| Control commands | `CONNECTIONS_CALLBACK`, valid/invalid invites (`4011`), guild templates (`4017`), deep links, unknown command silence |
| WebSocket | handshake and `Sec-WebSocket-Accept`, `READY`, `SET_ACTIVITY`, bridge output, metadata/flags |
| WebSocket policy | disallowed origin, canary origin, empty origin, bad version, `encoding=etf`, plain HTTP `426` |
| Rejected connections | a bad `Origin` or handshake does not clear another socket's activity |
| Game detection | fake process detected, activity shape, pid, app id as `socketId`, loss on disappearance, re-detection with a fresh start, `--no-process-scanning` |
| Bridge | broadcast, catch-up for a client connecting late |
| Probing | an occupied IPC socket is skipped, handshake works on `-1`, `--ipc-socket` refuses an active socket |
| Options | exact socket path, `--allow-origin`, occupied WebSocket port fall-through |
| Environment | `ARRPC_BRIDGE_PORT` alias still works, `ARRPCF_BRIDGE_PORT` wins when both are set |
| Shutdown | IPC socket file is unlinked |

Each run ends with `76 passed, 0 failed`.

## Sanitizers

```sh
make asan      # builds with -fsanitize=address,undefined and runs the suite
```

The full suite is clean under AddressSanitizer and UndefinedBehaviorSanitizer
(`detect_stack_use_after_return` enabled). macOS does not support LeakSanitizer, so
memory growth was additionally checked by a soak run: 12 000 activity round-trips
moved RSS by about 0.4 MB — allocator arenas, not per-message growth.

## Differential test against arRPC

```sh
git clone https://github.com/OpenAsar/arrpc /tmp/arrpc-ref
(cd /tmp/arrpc-ref && npm install)
python3 tools/compare_upstream.py /tmp/arrpc-ref build/arrpcf    # PARITY OK
```

The tool starts both servers on private ports and different `TMPDIR`s, performs the
same session against each, and compares the `READY` packet, every command response
and every bridge message. See [arRPC parity]({{ '/parity/' | relative_url }}) for the
result table and the three intentional differences.

## Real client

A `discord-rpc@4` Node client was used to validate the practical path:

```js
const RPC = require('discord-rpc');
const client = new RPC.Client({ transport: 'ipc' });

client.on('ready', async () => {
  await client.setActivity({
    details: 'Native C++ arRPC',
    state: 'validating real client',
    startTimestamp: new Date(),
    largeImageKey: 'logo',
    buttons: [{ label: 'GitHub', url: 'https://github.com/OpenAsar/arrpc' }],
    instance: false,
  });
  await client.clearActivity();
  client.destroy();
});

client.login({ clientId: '123456789012345678' });
```

Observed on the bridge: `application_id`, `details`, `state`, `assets`, button labels
and URLs, `flags: 0`, a millisecond `start` timestamp, the client pid, and the
subsequent `activity: null` — exactly what the bridge mods expect.

## Manual checks worth knowing

```sh
arrpcf --scan-once                                   # what game detection sees
arrpcf --detect-path "/Applications/Brawlhalla.app/Contents/MacOS/Brawlhalla"
arrpcf --debug                                       # frames, handshakes, probes
ARRPCF_FAKE_PROCESSES="/Applications/Brawlhalla.app/Contents/MacOS/Brawlhalla|4242" \
  arrpcf --scan-once                                 # detection without the game
```

## Continuous integration

`.github/workflows/ci.yml` builds the binary on a macOS runner, runs the protocol
suite, and checks that the documentation site builds.
