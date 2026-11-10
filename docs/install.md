---
title: Install
description: Build, install and run arrpcf on macOS, including a launchd service.
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
git clone https://github.com/insomnia-creator/arrpcf.git
cd arrpcf
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The result is `build/arrpcf` — a single Mach-O executable of about 316 KB that links
only `libSystem` and `libc++`:

```sh
$ otool -L build/arrpcf
/usr/lib/libSystem.B.dylib
/usr/lib/libc++.1.dylib
```

`make` and `make test` are thin wrappers around the CMake commands if you prefer.

### Universal binary

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build -j
file build/arrpcf      # Mach-O universal binary with 2 architectures
```

## Run

```sh
./build/arrpcf
```

```
[arrpcf] arrpcf (native) v1.1.0 - protocol compatible with arRPC v3.7.0
[arrpcf > bridge] listening on 1337
[arrpcf > ipc] listening at /var/folders/…/T/discord-ipc-0
[arrpcf > websocket] listening on 6463
[arrpcf > process] started
[arrpcf] ready
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
sudo cmake --install build --prefix /usr/local   # → /usr/local/bin/arrpcf
```

or simply copy the binary:

```sh
install -m 0755 build/arrpcf /usr/local/bin/arrpcf
```

## Run as a login service

A LaunchAgent template lives in
[`scripts/dev.arrpcf.plist`](https://github.com/insomnia-creator/arrpcf/blob/main/scripts/dev.arrpcf.plist).
Adjust `ProgramArguments` if the binary is not at `/usr/local/bin/arrpcf`, then:

```sh
cp scripts/dev.arrpcf.plist ~/Library/LaunchAgents/
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/dev.arrpcf.plist
```

Check it and follow the log:

```sh
launchctl print gui/$(id -u)/dev.arrpcf | head -20
tail -f /tmp/arrpcf.log
```

Stop or remove it:

```sh
launchctl bootout gui/$(id -u)/dev.arrpcf
rm ~/Library/LaunchAgents/dev.arrpcf.plist
```

<div class="note">
LaunchAgents inherit the per-user <code>TMPDIR</code>, which is where Discord's IPC
socket lives (<code>$TMPDIR/discord-ipc-0</code>). That is why arrpcf should run as a
user agent and not as a root LaunchDaemon.
</div>

## Verify the install

```sh
arrpcf --version          # arrpcf 1.1.0 (protocol compatible with arRPC v3.7.0)
arrpcf --detect-path "/Applications/Brawlhalla.app/Contents/MacOS/Brawlhalla"
arrpcf --scan-once        # what game detection sees right now
```

Run the protocol test suite against the built binary:

```sh
python3 tools/test_server.py build/arrpcf
```

## Uninstall

```sh
sudo rm /usr/local/bin/arrpcf
launchctl bootout gui/$(id -u)/dev.arrpcf 2>/dev/null
rm -f ~/Library/LaunchAgents/dev.arrpcf.plist /tmp/arrpcf.log
```

arrpcf creates nothing else on disk: no caches, no config files, no preferences. The
IPC socket file is removed on shutdown.
