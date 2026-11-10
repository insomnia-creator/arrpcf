---
title: Options
description: Complete reference for arrpcf's command line flags and environment variables.
permalink: /options/
---

## Synopsis

```
arrpcf [options]
```

Every option can also be written as `--option=value`. Options default to what arRPC
uses, so an unconfigured `arrpcf` behaves like an unconfigured upstream
`npx arrpc`.

## Transports

| Option | Default | Description |
| --- | --- | --- |
| `--ipc-base <path>` | `$XDG_RUNTIME_DIR` → `$TMPDIR` → `$TMP` → `$TEMP` → `/tmp`, plus `/discord-ipc` | Base path for the IPC socket. arrpcf probes `-0` … `-9` and binds the first free one, exactly like Discord. |
| `--ipc-socket <path>` | — | Bind exactly this path, skipping probing. Refuses a path that already answers pings, so it cannot steal Discord's live socket. |
| `--no-ipc` | off | Disable the IPC (unix socket) transport. |
| `--ws-ports <first-last>` | `6463-6472` | RPC WebSocket port range. The first bindable port is used; occupied ports are logged and skipped. |
| `--no-ws` | off | Disable the RPC WebSocket transport. |
| `--bind <host>` | `127.0.0.1` | Bind address for the RPC WebSocket listener. |
| `--bridge-port <port>` | `1337` | Bridge WebSocket port (`ARRPCF_BRIDGE_PORT`). |
| `--bridge-host <host>` | `127.0.0.1` | Bridge bind address. arRPC binds `0.0.0.0`; arrpcf defaults to loopback because bridge messages contain your activity. Use `--bridge-host 0.0.0.0` for the original behaviour. |
| `--no-bridge` | off | Disable the bridge WebSocket. |
| `--allow-origin <origin>` | — | Add an allowed `Origin` for the RPC WebSocket. Repeatable. |
| `--allow-any-origin` | off | Skip the `Origin` check entirely (not recommended). |

The RPC WebSocket always allows an empty origin plus `https://discord.com`,
`https://ptb.discord.com` and `https://canary.discord.com`, matching arRPC.

## Process detection

| Option | Default | Description |
| --- | --- | --- |
| `--no-process-scanning` | off | Disable game detection (`ARRPCF_NO_PROCESS_SCANNING`). |
| `--scan-interval <secs>` | `5` | Seconds between process scans, `1`–`3600`. |
| `--detectable <file>` | builtin | Use Discord's full `detectable.json` instead of the embedded darwin subset. Accepts the unmodified file from `https://discord.com/api/v9/applications/detectable`. |
| `--scan-once` | off | Scan the process table once, print what was detected, exit. |
| `--detect-path <path>` | — | Run the matcher against one executable path, print the matching app(s), exit. |

## Behaviour

| Option | Default | Description |
| --- | --- | --- |
| `--open-links` | off | Open `INVITE_BROWSER`, `GUILD_TEMPLATE_BROWSER` and `DEEP_LINK` targets in the default browser. arrpcf always answers these commands; with this flag it also acts on them. |
| `--debug` | off | Verbose logging: frames, handshakes, socket probes (`ARRPCF_DEBUG`). |
| `--no-color` | off | Disable ANSI colours. Also honoured: `NO_COLOR`, and auto-disabled when stdout is not a TTY. |
| `--version` | — | Print the version and exit. |
| `-h`, `--help` | — | Print the option summary and exit. |

## Environment variables

| Variable | Equivalent | Description |
| --- | --- | --- |
| `ARRPCF_BRIDGE_PORT` | `--bridge-port` | Bridge port. |
| `ARRPCF_NO_PROCESS_SCANNING` | `--no-process-scanning` | Disable game detection. Any value except `0`/`false`/`no`/`off` enables it. |
| `ARRPCF_DEBUG` | `--debug` | Verbose logging. |
| `NO_COLOR` | `--no-color` | Disable colours. |
| `ARRPCF_FAKE_PROCESSES` | — | Debug only: `path[|pid];path[|pid]` pretends those processes are running. |
| `ARRPCF_FAKE_PROCESSES_FILE` | — | Debug only: same format, one entry per line, re-read on every scan (lets you test detection *and* loss). |

<div class="note">
the original arRPC names are still accepted as aliases: <code>ARRPC_BRIDGE_PORT</code>,
<code>ARRPC_NO_PROCESS_SCANNING</code>, <code>ARRPC_DEBUG</code>,
<code>ARRPC_FAKE_PROCESSES</code> and <code>ARRPC_FAKE_PROCESSES_FILE</code>.
When both are set, the <code>ARRPCF_*</code> variable wins.
</div>

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Normal shutdown (`SIGINT`/`SIGTERM`) or a successful one-shot command. |
| `1` | Startup failure (no transport could bind, unreadable `--detectable`), or no match for `--detect-path`. |
| `2` | Invalid command line. |

## Examples

Default setup for Discord web:

```sh
arrpcf
```

Keep the bridge off the network and answer link requests:

```sh
arrpcf --bridge-host 127.0.0.1 --open-links
```

Run a second instance for testing on private ports and a private socket:

```sh
arrpcf --ipc-base /tmp/arrpcf-test/discord-ipc \
      --ws-ports 16463-16472 \
      --bridge-port 11337 \
      --no-process-scanning --debug
```

Take over `discord-ipc-0` explicitly (only when Discord desktop is not running —
arrpcf refuses a socket that still answers pings):

```sh
arrpcf --ipc-socket "$TMPDIR/discord-ipc-0"
```

Use Discord's current, full game database:

```sh
curl -o detectable.json https://discord.com/api/v9/applications/detectable
arrpcf --detectable ./detectable.json
```

Accept a custom web client origin:

```sh
arrpcf --allow-origin https://my-client.example
```

## Notes

- **Port probing.** Both the IPC index and the WebSocket port are chosen by probing,
  so arrpcf coexists with Discord desktop or another RPC server. Occupied ports are
  reported in the log (`6463 in use!`) and skipped.
- **Socket probe.** A candidate IPC socket is considered free when connecting fails
  or when nothing answers a `PING` within one second; a socket that answers is left
  untouched. This is the same heuristic arRPC uses.
- **No config file.** Everything is a flag or an environment variable; there is
  nothing to keep in sync between machines.
