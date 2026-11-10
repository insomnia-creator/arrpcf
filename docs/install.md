---
title: Install
description: Build, install and run arrpc on macOS, including a launchd service.
permalink: /install/
---

## Requirements

- macOS 11 or newer (Apple silicon or Intel).
- Xcode command line tools — `xcode-select --install`.
- CMake 3.16 or newer (`brew install cmake`).
- Python 3 only for the test suite, not for running the server.

There are no third-party libraries. The JSON parser
([yyjson](https://github.com/ibireme/yyjson)) is vendored as a single C file.

## Build

```sh
git clone https://github.com/insomnia-creator/arrpc.git
cd arrpc
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The result is `build/arrpc` — a single Mach-O executable of about 316 KB that links
only `libSystem` and `libc++`:

```sh
$ otool -L build/arrpc
/usr/lib/libSystem.B.dylib
/usr/lib/libc++.1.dylib
```

`make` and `make test` are thin wrappers around the CMake commands if you prefer.

### Universal binary

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build -j
file build/arrpc      # Mach-O universal binary with 2 architectures
```

## Run

```sh
./build/arrpc
```

```
[arRPC] arRPC (native) v1.0.0 - protocol compatible with arRPC v3.7.0
[arRPC > bridge] listening on 1337
[arRPC > ipc] listening at /var/folders/…/T/discord-ipc-0
[arRPC > websocket] listening on 6463
[arRPC > process] started
[arRPC] ready
```

Enable a bridge consumer in Discord web:

- **Vencord** — enable the `WebRichPresence (arRPC)` plugin.
- **No mods** — run
  [`examples/bridge_mod.js`](https://github.com/OpenAsar/arrpc/blob/main/examples/bridge_mod.js)
  in the browser console.
- **ArmCord / Vesktop** — built in; no external server required.

Then start an RPC application and watch the log (add `--debug` for frames).

## Install system-wide

```sh
sudo cmake --install build --prefix /usr/local   # → /usr/local/bin/arrpc
```

or simply copy the binary:

```sh
install -m 0755 build/arrpc /usr/local/bin/arrpc
```

## Run as a login service

A LaunchAgent template lives in
[`scripts/dev.arrpc.arrpc.plist`](https://github.com/insomnia-creator/arrpc/blob/main/scripts/dev.arrpc.arrpc.plist).
Adjust `ProgramArguments` if the binary is not at `/usr/local/bin/arrpc`, then:

```sh
cp scripts/dev.arrpc.arrpc.plist ~/Library/LaunchAgents/
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/dev.arrpc.arrpc.plist
```

Check it and follow the log:

```sh
launchctl print gui/$(id -u)/dev.arrpc.arrpc | head -20
tail -f /tmp/arrpc.log
```

Stop or remove it:

```sh
launchctl bootout gui/$(id -u)/dev.arrpc.arrpc
rm ~/Library/LaunchAgents/dev.arrpc.arrpc.plist
```

<div class="note">
LaunchAgents inherit the per-user <code>TMPDIR</code>, which is where Discord's IPC
socket lives (<code>$TMPDIR/discord-ipc-0</code>). That is why arrpc should run as a
user agent and not as a root LaunchDaemon.
</div>

## Verify the install

```sh
arrpc --version          # arrpc (C++) 1.0.0 (protocol compatible with arRPC v3.7.0)
arrpc --detect-path "/Applications/Brawlhalla.app/Contents/MacOS/Brawlhalla"
arrpc --scan-once        # what game detection sees right now
```

Run the protocol test suite against the built binary:

```sh
python3 tools/test_server.py build/arrpc
```

## Uninstall

```sh
sudo rm /usr/local/bin/arrpc
launchctl bootout gui/$(id -u)/dev.arrpc.arrpc 2>/dev/null
rm -f ~/Library/LaunchAgents/dev.arrpc.arrpc.plist /tmp/arrpc.log
```

arrpc creates nothing else on disk: no caches, no config files, no preferences. The
IPC socket file is removed on shutdown.
