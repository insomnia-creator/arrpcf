#include "config.hpp"

#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

const char *const kVersion = "1.1.0";
const char *const kCompatVersion = "3.7.0";

namespace {

bool parse_int(const std::string &text, int *out) {
  if (text.empty()) return false;
  char *end = nullptr;
  long value = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || (end && *end != '\0')) return false;
  if (value < -2147483647L || value > 2147483647L) return false;
  *out = static_cast<int>(value);
  return true;
}

// Variables use the ARRPCF_ prefix; arRPC's ARRPC_ names are still honoured so
// existing setups and upstream tooling keep working.
const char *env_value(const char *name, const char *legacy) {
  const char *value = std::getenv(name);
  if (value && *value) return value;
  value = std::getenv(legacy);
  if (value && *value) return value;
  return nullptr;
}

bool parse_env_int(const char *name, const char *legacy, int *out) {
  const char *value = env_value(name, legacy);
  if (!value) return false;
  return parse_int(value, out);
}

bool env_truthy(const char *name, const char *legacy) {
  const char *value = env_value(name, legacy);
  if (!value) return false;
  std::string v(value);
  for (auto &c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return !(v == "0" || v == "false" || v == "no" || v == "off");
}

}  // namespace

void print_help() {
  std::printf(
      "arrpcf %s - native arRPC-compatible Discord RPC server for macOS\n"
      "(protocol compatible with arRPC v%s, https://github.com/insomnia-creator/arrpcf)\n"
      "\n"
      "Bridges Discord's local RPC servers (IPC socket, loopback WebSocket) to the\n"
      "arRPC bridge WebSocket consumed by Discord web / Vencord / ArmCord / Vesktop.\n"
      "\n"
      "Usage: arrpcf [options]\n"
      "\n"
      "Transports\n"
      "  --ipc-base <path>        Base path for the local RPC socket\n"
      "                           (default: $XDG_RUNTIME_DIR|$TMPDIR|$TMP|$TEMP|/tmp + /discord-ipc;\n"
      "                            the first free suffix -0..-9 is used, like Discord)\n"
      "  --ipc-socket <path>      Bind exactly this socket path (no probing)\n"
      "  --no-ipc                 Disable the IPC (unix socket) transport\n"
      "  --ws-ports <first-last>  RPC WebSocket port range (default: 6463-6472)\n"
      "  --no-ws                  Disable the RPC WebSocket transport\n"
      "  --bind <host>            Bind address for TCP listeners (default: 127.0.0.1)\n"
      "  --bridge-port <port>     Bridge WebSocket port (default: 1337, env ARRPCF_BRIDGE_PORT)\n"
      "  --bridge-host <host>     Bridge bind address (default: 127.0.0.1; arRPC binds 0.0.0.0)\n"
      "  --no-bridge              Disable the bridge WebSocket\n"
      "  --allow-origin <origin>  Extra allowed Origin for the RPC WebSocket (repeatable)\n"
      "  --allow-any-origin       Disable the Origin check (not recommended)\n"
      "\n"
      "Process detection (macOS)\n"
      "  --no-process-scanning    Disable game detection (env ARRPCF_NO_PROCESS_SCANNING)\n"
      "  --scan-interval <secs>   Seconds between scans (default: 5)\n"
      "  --detectable <file>      Use Discord's full detectable.json instead of the builtin\n"
      "                           darwin subset (https://discord.com/api/v9/applications/detectable)\n"
      "  --scan-once              Scan the process table once, print result, exit\n"
      "  --detect-path <path>     Test the executable matcher on one path, exit\n"
      "\n"
      "Behaviour\n"
      "  --open-links             Open INVITE_BROWSER / GUILD_TEMPLATE_BROWSER / DEEP_LINK\n"
      "                           with the default browser (arRPC never answers these, so\n"
      "                           clients that await a reply hang; we always reply)\n"
      "  --debug                  Verbose logging (env ARRPCF_DEBUG)\n"
      "  --no-color               Disable ANSI colours (also honours NO_COLOR)\n"
      "  --version                Print version and exit\n"
      "  -h, --help               Show this help\n"
      "\n"
      "Env: ARRPCF_BRIDGE_PORT, ARRPCF_NO_PROCESS_SCANNING, ARRPCF_DEBUG\n"
      "     (arRPC's ARRPC_* names are still accepted as aliases)\n"
      "\n"
      "Running without the Discord desktop app is the normal setup for Discord web:\n"
      "if Discord desktop owns discord-ipc-0, arrpcf takes the next free index and\n"
      "desktop-style RPC clients will keep talking to Discord instead.\n",
      kVersion, kCompatVersion);
}

ConfigStatus parse_config(int argc, char **argv, Config *cfg, std::string *err) {
  // Environment defaults first (CLI wins).
  if (parse_env_int("ARRPCF_BRIDGE_PORT", "ARRPC_BRIDGE_PORT", &cfg->bridge_port)) {
    if (cfg->bridge_port < 1 || cfg->bridge_port > 65535) {
      *err = "ARRPCF_BRIDGE_PORT is not a valid port";
      return ConfigStatus::Error;
    }
  }
  if (env_truthy("ARRPCF_NO_PROCESS_SCANNING", "ARRPC_NO_PROCESS_SCANNING")) cfg->process_scan = false;
  if (env_truthy("ARRPCF_DEBUG", "ARRPC_DEBUG")) cfg->debug = true;
  if (std::getenv("NO_COLOR")) cfg->color = false;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    std::string inline_value;
    bool has_inline = false;

    if (arg.rfind("--", 0) == 0) {
      size_t eq = arg.find('=');
      if (eq != std::string::npos) {
        inline_value = arg.substr(eq + 1);
        arg = arg.substr(0, eq);
        has_inline = true;
      }
    }

    auto take_value = [&](std::string *out) -> bool {
      if (has_inline) {
        *out = inline_value;
        return true;
      }
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };

    if (arg == "-h" || arg == "--help") {
      print_help();
      return ConfigStatus::ExitSuccess;
    }
    if (arg == "--version") {
      std::printf("arrpcf %s (protocol compatible with arRPC v%s)\n", kVersion, kCompatVersion);
      return ConfigStatus::ExitSuccess;
    }
    if (arg == "--no-ipc") {
      cfg->ipc_enabled = false;
      continue;
    }
    if (arg == "--no-ws") {
      cfg->ws_enabled = false;
      continue;
    }
    if (arg == "--no-bridge") {
      cfg->bridge_enabled = false;
      continue;
    }
    if (arg == "--no-process-scanning") {
      cfg->process_scan = false;
      continue;
    }
    if (arg == "--scan-once") {
      cfg->scan_once = true;
      continue;
    }
    if (arg == "--open-links") {
      cfg->open_links = true;
      continue;
    }
    if (arg == "--debug") {
      cfg->debug = true;
      continue;
    }
    if (arg == "--no-color") {
      cfg->color = false;
      continue;
    }
    if (arg == "--allow-any-origin") {
      cfg->allow_any_origin = true;
      continue;
    }

    std::string value;
    if (arg == "--ipc-base") {
      if (!take_value(&value)) return (*err = "--ipc-base needs a path", ConfigStatus::Error);
      cfg->ipc_base = value;
    } else if (arg == "--ipc-socket") {
      if (!take_value(&value)) return (*err = "--ipc-socket needs a path", ConfigStatus::Error);
      cfg->ipc_socket = value;
    } else if (arg == "--ws-ports") {
      if (!take_value(&value)) return (*err = "--ws-ports needs <first-last>", ConfigStatus::Error);
      int first = 0, last = 0;
      char extra = 0;
      if (std::sscanf(value.c_str(), "%d-%d%c", &first, &last, &extra) != 2) {
        return (*err = "--ws-ports must look like 6463-6472", ConfigStatus::Error);
      }
      if (first < 1 || last > 65535 || first > last) {
        return (*err = "--ws-ports range is invalid", ConfigStatus::Error);
      }
      cfg->ws_port_first = first;
      cfg->ws_port_last = last;
    } else if (arg == "--bind") {
      if (!take_value(&value)) return (*err = "--bind needs a host", ConfigStatus::Error);
      cfg->bind_host = value;
    } else if (arg == "--bridge-host") {
      if (!take_value(&value)) return (*err = "--bridge-host needs a host", ConfigStatus::Error);
      cfg->bridge_host = value;
    } else if (arg == "--bridge-port") {
      if (!take_value(&value)) return (*err = "--bridge-port needs a port", ConfigStatus::Error);
      int port = 0;
      if (!parse_int(value, &port) || port < 1 || port > 65535) {
        return (*err = "--bridge-port is not a valid port", ConfigStatus::Error);
      }
      cfg->bridge_port = port;
    } else if (arg == "--allow-origin") {
      if (!take_value(&value)) return (*err = "--allow-origin needs an origin", ConfigStatus::Error);
      cfg->extra_origins.push_back(value);
    } else if (arg == "--scan-interval") {
      if (!take_value(&value)) return (*err = "--scan-interval needs seconds", ConfigStatus::Error);
      int seconds = 0;
      if (!parse_int(value, &seconds) || seconds < 1 || seconds > 3600) {
        return (*err = "--scan-interval must be 1..3600", ConfigStatus::Error);
      }
      cfg->scan_interval_ms = seconds * 1000;
    } else if (arg == "--detectable") {
      if (!take_value(&value)) return (*err = "--detectable needs a file", ConfigStatus::Error);
      cfg->detectable_path = value;
    } else if (arg == "--detect-path") {
      if (!take_value(&value)) return (*err = "--detect-path needs a path", ConfigStatus::Error);
      cfg->detect_path = value;
    } else {
      *err = "unknown option: " + std::string(argv[i]) + " (try --help)";
      return ConfigStatus::Error;
    }
  }

  if (cfg->color && !::isatty(STDOUT_FILENO)) cfg->color = false;
  return ConfigStatus::Ok;
}
