# arrpcf (native)

[![CI](https://github.com/insomnia-creator/arrpcf/actions/workflows/ci.yml/badge.svg)](https://github.com/insomnia-creator/arrpcf/actions/workflows/ci.yml) [![docs](https://img.shields.io/badge/docs-insomnia--creator.github.io%2Farrpcf-5865f2)](https://insomnia-creator.github.io/arrpcf/)

A small, dependency-free **C++17** reimplementation of
[OpenAsar/arRPC](https://github.com/OpenAsar/arrpc) for **macOS**.

It speaks Discord's local RPC protocols on the ports/paths that RPC applications
already know, and re-publishes what it learns on the arRPC **bridge** WebSocket
that Discord web / Vencord / ArmCord / Vesktop consume.
No Node, no npm, no Electron: one static binary of ~316 KB that links only
`libSystem` and `libc++`.

```
app with Discord RPC ──IPC unix socket──┐
app with Discord RPC ──ws://127.0.0.1──▶ arrpcf ──ws://127.0.0.1:1337──▶ Discord web / Vencord
macOS process table ──libproc scan──────┘         ({"activity", "pid", "socketId"})
```

**Documentation:** <https://insomnia-creator.github.io/arrpcf/>

Status: protocol-compatible with **arRPC v3.7.0**; all of arRPC's implemented
transports, commands and behaviours are covered (see
[parity](https://insomnia-creator.github.io/arrpcf/parity/)), plus a few deliberate
macOS fixes.

---

## Build

Requirements: macOS 11+, Xcode command line tools (clang + CMake ≥ 3.16). No third
party libraries — `yyjson` (MIT, single file) is vendored.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/arrpcf --help

# optional: universal binary + install
cmake -B build-arm64 -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build-arm64 -j
sudo cmake --install build-arm64 --prefix /usr/local
```

`make` / `make test` wrap the two commands above.

## Run

```sh
./build/arrpcf                 # foreground
./build/arrpcf --debug         # verbose protocol logging
```

Then, in Discord web: enable a bridge consumer —

* **Vencord**: enable the `WebRichPresence (arRPC)` plugin.
* **No mods**: run [`examples/bridge_mod.js`](https://github.com/OpenAsar/arrpc/blob/main/examples/bridge_mod.js)
  from the browser console.
* **ArmCord / Vesktop**: built in, no server needed.

and start any RPC-capable application (VS Code, Spotify, an MP game, …).

### Run as a login service

```sh
sudo cmake --install build --prefix /usr/local     # installs /usr/local/bin/arrpcf
cp scripts/dev.arrpcf.plist ~/Library/LaunchAgents/
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/dev.arrpcf.plist
launchctl print    gui/$(id -u)/dev.arrpcf     # status
tail -f /tmp/arrpcf.log                              # logs
```

launchd agents inherit the per-user `TMPDIR`, which is exactly where Discord's
IPC socket lives (`$TMPDIR/discord-ipc-0`), so no configuration is needed.

---

## Options

```
$ arrpcf --help

Transports
  --ipc-base <path>        Base path for the local RPC socket
                           (default: $XDG_RUNTIME_DIR|$TMPDIR|$TMP|$TEMP|/tmp + /discord-ipc;
                            the first free suffix -0..-9 is used, like Discord)
  --ipc-socket <path>      Bind exactly this socket path (no probing)
  --no-ipc                 Disable the IPC (unix socket) transport
  --ws-ports <first-last>  RPC WebSocket port range (default: 6463-6472)
  --no-ws                  Disable the RPC WebSocket transport
  --bind <host>            Bind address for TCP listeners (default: 127.0.0.1)
  --bridge-port <port>     Bridge WebSocket port (default: 1337, env ARRPCF_BRIDGE_PORT)
  --bridge-host <host>     Bridge bind address (default: 127.0.0.1; arRPC binds 0.0.0.0)
  --no-bridge              Disable the bridge WebSocket
  --allow-origin <origin>  Extra allowed Origin for the RPC WebSocket (repeatable)
  --allow-any-origin       Disable the Origin check (not recommended)

Process detection (macOS)
  --no-process-scanning    Disable game detection (env ARRPCF_NO_PROCESS_SCANNING)
  --scan-interval <secs>   Seconds between scans (default: 5)
  --detectable <file>      Use Discord's full detectable.json instead of the builtin
                           darwin subset (https://discord.com/api/v9/applications/detectable)
  --scan-once              Scan the process table once, print result, exit
  --detect-path <path>     Test the executable matcher on one path, exit

Behaviour
  --open-links             Open INVITE_BROWSER / GUILD_TEMPLATE_BROWSER / DEEP_LINK
                           with the default browser (arRPC never answers these, so
                           clients that await a reply hang; we always reply)
  --debug                  Verbose logging (env ARRPCF_DEBUG)
  --no-color               Disable ANSI colours (also honours NO_COLOR)
  --version                Print version and exit
  -h, --help               Show this help
```

Environment variables (same names as arRPC where arRPC has one):

| Variable | Meaning |
| --- | --- |
| `ARRPCF_BRIDGE_PORT` | Bridge port (default `1337`) |
| `ARRPCF_NO_PROCESS_SCANNING` | Same as `--no-process-scanning` |
| `ARRPCF_DEBUG` | Same as `--debug` |
| `NO_COLOR` | Same as `--no-color` |
| `ARRPCF_FAKE_PROCESSES` | Debug: `path[|pid];path[|pid]` pretends these processes run |
| `ARRPCF_FAKE_PROCESSES_FILE` | Debug: same, re-read from a file on every scan |

arRPC's original names (`ARRPC_BRIDGE_PORT`, `ARRPC_NO_PROCESS_SCANNING`, `ARRPC_DEBUG`,
`ARRPC_FAKE_PROCESSES`, `ARRPC_FAKE_PROCESSES_FILE`) are still honoured as aliases; the
`ARRPCF_*` variable wins when both are set.

---

## What it implements

### Transports

| Transport | State | Notes |
| --- | --- | --- |
| Discord IPC (Unix socket) | ✅ | binary framing, `HANDSHAKE`/`FRAME`/`CLOSE`/`PING`/`PONG`, socket probing `-0..-9` |
| Discord RPC WebSocket `127.0.0.1:6463-6472` | ✅ | JSON encoding, `Origin` allow-list, `v=1` check, `client_id` |
| Erlpack/ETF encoding | ❌ | closed with a clean close frame, like arRPC |
| HTTP transport | ❌ | arRPC doesn't implement it either; plain HTTP gets `426 Upgrade Required` |
| arRPC bridge WebSocket `127.0.0.1:1337` | ✅ | broadcast + catch-up of the last activity per socket |
| Process scanning (macOS) | ✅ | `libproc` + `KERN_PROCARGS2`, every 5 s |

### Commands

`DISPATCH/READY`, `SET_ACTIVITY` (buttons → `metadata.button_urls` + `buttons`
labels, `instance` → `flags`, seconds → milliseconds timestamps),
`CONNECTIONS_CALLBACK`, `INVITE_BROWSER`, `GUILD_TEMPLATE_BROWSER`, `DEEP_LINK`.

### Parity with arRPC

Everything arRPC does *for the bridge* is reproduced byte-for-byte in shape:

* READY: same mock `arRPC` user (`1045800378228281345`), same `config`.
* `SET_ACTIVITY`: `{application_id, type: 0, metadata, flags, ...activity, buttons}`.
* Activity clear on `activity: null` and on socket close (with the last `pid`).
* Process detection re-emits on every scan so activities that happen before a
  Discord client connects are still picked up (the bridge caches per socket id).
* Bridge catch-up on connect for every socket with a live activity.
* IPC socket probing: an occupied/ponging `discord-ipc-N` is skipped.
* Close codes `4000` / `4004` / `1003` on bad handshakes.

### Intentional differences

| arRPC | here | why |
| --- | --- | --- |
| Bridge listens on `0.0.0.0` | `127.0.0.1` by default (`--bridge-host 0.0.0.0` to restore) | the bridge carries your activity; it should not be on the LAN |
| `INVITE_BROWSER` / `GUILD_TEMPLATE_BROWSER` / `DEEP_LINK` are emitted to a callback nothing listens to, so clients never get a reply | always answered; `--open-links` additionally opens the URL with `/usr/bin/open` | avoids RPC libs hanging on these calls |
| Malformed WS JSON crashes the process | logged and ignored | robustness |
| Full 11 MB detectable DB | 7 KB builtin darwin subset; `--detectable` loads the full file | a 316 KB binary shouldn't carry 11 MB of other platforms' games |
| Scanner matches executables of *all* OSes in the DB | darwin entries only | no false positives |
| DB never contains `.app` bundle names for the inner binary | also matches `/Foo.app/Contents/MacOS/Foo` | macOS launches the binary, not the bundle |
| One process can produce several activities (e.g. shared engine binaries) | first match per app id per scan | avoids duplicate ghosts |
| Plain HTTP to 6463 hangs | `426 Upgrade Required` | debuggability |

---

## macOS notes

* **Socket path.** Discord desktop and every RPC client use
  `$TMPDIR/discord-ipc-0` (`TMPDIR` is per-user, e.g.
  `/var/folders/xx/…/T/`). arrpcf binds the first *free* index exactly like
  arRPC (and Discord): a socket that answers a PING is left alone.
* **Discord desktop coexistence.** If Discord desktop is running it owns
  `discord-ipc-0`, so arrpcf takes `-1`, but RPC clients try `-0` first and end
  up talking to Discord. arRPC has the same limitation — its use case is
  Discord **web** (where desktop isn't running). Quit Discord desktop, or start
  arrpcf with `--ipc-socket "$TMPDIR/discord-ipc-0"` if Discord is not running
  (arrpcf refuses that path if something still answers pings there).
* **Process detection** uses `proc_pidpath`, which returns paths of processes of
  the same user (and of other users when run as root). Bundle executables
  (`/Applications/X.app/Contents/MacOS/X`) and the App Store's `…/Contents/…`
  layout both work; games started through Steam/Rosetta/CrossOver are seen if
  their executable path matches Discord's database.
* **Arguments** (`-game tf`, `net.minecraft.client.main.Main`) come from
  `KERN_PROCARGS2`, which only works for your own processes — true for games you
  launch yourself.
* **No App-Sandbox access**: an App Store build of a helper cannot read other
  processes; run the CLI/launchd agent instead (as above).

## Debugging

```sh
arrpcf --debug                     # protocol frames, handshakes, probes
arrpcf --scan-once                 # what game detection sees right now
arrpcf --detect-path "/Applications/Brawlhalla.app/Contents/MacOS/Brawlhalla"
arrpcf --detectable ~/Downloads/detectable.json --scan-once
ARRPCF_FAKE_PROCESSES="/Applications/Brawlhalla.app/Contents/MacOS/Brawlhalla|4242" arrpcf --scan-once
```

## Tests

```sh
python3 tools/test_server.py          # 76 protocol/e2e checks, stdlib only
python3 tools/test_server.py build-asan/arrpcf   # same suite under ASan/UBSan
```

The suite boots the real binary on private ports/paths and exercises IPC
framing, READY, `SET_ACTIVITY` translation, clearing, socket close, probing,
WebSocket handshake/origin/version/encoding rejection, control commands
(invites, guild templates, deep links, `CONNECTIONS_CALLBACK`), bridge broadcast
and catch-up, process detect → lose → re-detect, `--ipc-socket`,
`--allow-origin`, port fall-through and the CLI helpers.

Two extra layers of validation were used while porting:

```sh
# Differential test against the original Node implementation: same session,
# compare READY, every response and every bridge payload.
git clone https://github.com/OpenAsar/arrpc /tmp/arrpc-ref
(cd /tmp/arrpc-ref && npm install)
python3 tools/compare_upstream.py /tmp/arrpc-ref build/arrpcf    # PARITY OK
```

plus a real `discord-rpc@4` Node client (`setActivity` with buttons/assets/
timestamps, then `clearActivity`) against the IPC socket.

## Layout

```
src/main.cpp          wiring, signal handling
src/ipc.*             Discord IPC transport (framing, probing, handshake)
src/ws.*              minimal RFC 6455 server (no libwebsockets)
src/rpc.*             protocol: READY, SET_ACTIVITY, invites, deep links
src/bridge.*          bridge WebSocket + per-socket cache
src/detect.*          Discord detectable-DB matching
src/process_scan.*    libproc/KERN_PROCARGS2 scanning
src/net.*             sockets, SHA-1, base64, query parsing
src/reactor.*         poll() event loop + timers
src/json.*            thin yyjson wrapper
src/config.*          CLI/env parsing
src/detectable_darwin.hpp   generated DB subset (tools/gen_detectable.py)
```

## Documentation site

The documentation in [`docs/`](docs/) is a small Jekyll site published with GitHub
Pages at <https://insomnia-creator.github.io/arrpcf/>. Preview it locally with the same
Jekyll version GitHub uses:

```sh
gem install 'jekyll:3.9.5' kramdown-parser-gfm --no-document
jekyll build --source docs --destination _site
python3 tools/check_links.py _site /arrpcf      # verifies links and anchors
```

## License

MIT. Protocol, behaviour and the original arRPC are by
[CanadaHonk / OpenAsar](https://github.com/OpenAsar/arrpc) (MIT); JSON parsing by
[yyjson](https://github.com/ibireme/yyjson) (MIT).
