#pragma once

#include <string>
#include <vector>

extern const char *const kVersion;
extern const char *const kCompatVersion;

struct Config {
  // transports
  bool ipc_enabled = true;
  bool ws_enabled = true;
  bool bridge_enabled = true;

  std::string ipc_base;    // empty -> default_ipc_base_path()
  std::string ipc_socket;  // exact path, skips probing
  int ws_port_first = 6463;
  int ws_port_last = 6472;
  std::string bind_host = "127.0.0.1";
  std::string bridge_host = "127.0.0.1";
  int bridge_port = 1337;

  // origin policy for the RPC websocket
  bool allow_any_origin = false;
  std::vector<std::string> extra_origins;

  // process detection
  bool process_scan = true;
  int scan_interval_ms = 5000;
  std::string detectable_path;
  bool scan_once = false;
  std::string detect_path;

  // behaviour
  bool open_links = false;
  bool debug = false;
  bool color = true;
};

enum class ConfigStatus { Ok, ExitSuccess, Error };

ConfigStatus parse_config(int argc, char **argv, Config *cfg, std::string *err);
void print_help();
