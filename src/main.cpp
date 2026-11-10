#include <poll.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bridge.hpp"
#include "config.hpp"
#include "detect.hpp"
#include "ipc.hpp"
#include "log.hpp"
#include "net.hpp"
#include "process_scan.hpp"
#include "reactor.hpp"
#include "rpc.hpp"
#include "util.hpp"
#include "ws.hpp"

namespace {

int g_signal_pipe[2] = {-1, -1};

void on_signal(int) {
  if (g_signal_pipe[1] >= 0) {
    const char byte = 1;
    ssize_t ignored = ::write(g_signal_pipe[1], &byte, 1);
    (void)ignored;
  }
}

bool origin_allowed(const std::string &origin, const Config &cfg) {
  static const char *kAllowed[] = {
      "https://discord.com",
      "https://ptb.discord.com",
      "https://canary.discord.com",
  };
  for (const char *allowed : kAllowed) {
    if (origin == allowed) return true;
  }
  for (const auto &extra : cfg.extra_origins) {
    if (origin == extra) return true;
  }
  return false;
}

// JS parseInt()-like prefix parsing: "1abc" -> 1.
bool parse_int_prefix(const std::string &text, int *out) {
  if (text.empty()) return false;
  char *end = nullptr;
  long value = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str()) return false;
  *out = static_cast<int>(value);
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  std::signal(SIGPIPE, SIG_IGN);
  // --open-links spawns /usr/bin/open; auto-reap it (no zombies, no waiting).
  std::signal(SIGCHLD, SIG_IGN);

  Config cfg;
  std::string err;
  const ConfigStatus status = parse_config(argc, argv, &cfg, &err);
  if (status == ConfigStatus::ExitSuccess) return 0;
  if (status == ConfigStatus::Error) {
    std::fprintf(stderr, "arrpcf: %s\n", err.c_str());
    return 2;
  }

  logging::init(cfg.debug, cfg.color);
  logging::line(logging::Tag::none, logging::fmt("arrpcf (native) v%s - protocol compatible with arRPC v%s",
                                                 kVersion, kCompatVersion));

  // --- detectable game database ---------------------------------------------
  const bool needs_db = cfg.process_scan || cfg.scan_once || !cfg.detect_path.empty();
  DetectableDb db;
  if (needs_db) {
    if (!cfg.detectable_path.empty()) {
      if (!db.load_file(cfg.detectable_path, &err)) {
        logging::error(logging::Tag::process, err);
        return 1;
      }
      logging::line(logging::Tag::process,
                    logging::fmt("loaded %zu detectable apps from %s", db.size(),
                                 cfg.detectable_path.c_str()));
    } else {
      if (!db.load_builtin()) {
        logging::error(logging::Tag::process, "failed to load the embedded detectable database");
        return 1;
      }
      logging::debug(logging::Tag::process,
                     logging::fmt("builtin detectable DB: %zu darwin apps", db.size()));
    }
  }

  if (!cfg.detect_path.empty()) {
    const auto matches = db.match_path(net::to_lower(cfg.detect_path));
    if (matches.empty()) {
      std::printf("no match: %s\n", cfg.detect_path.c_str());
      return 1;
    }
    for (const auto &match : matches) {
      std::printf("match: %s (%s)", match.app->name.c_str(), match.app->id.c_str());
      if (!match.exe->arguments.empty()) std::printf(" args~\"%s\"", match.exe->arguments.c_str());
      std::printf("\n");
    }
    return 0;
  }

  if (cfg.scan_once) {
    const auto detections = detect_processes(db);
    for (const auto &detection : detections) {
      std::printf("%s (%s) pid=%d path=%s\n", detection.name.c_str(), detection.app_id.c_str(),
                  detection.pid, detection.path.c_str());
    }
    if (detections.empty()) logging::line(logging::Tag::process, "no detectable games running");
    return 0;
  }

  // --- servers ---------------------------------------------------------------
  Reactor reactor;
  Bridge bridge(reactor);
  RpcService rpc(bridge, cfg.open_links);

  bool any_listener = false;

  if (cfg.bridge_enabled) {
    if (bridge.start(cfg.bridge_host, cfg.bridge_port)) any_listener = true;
  }

  IpcServer ipc(reactor);
  ipc.on_connection = [&rpc](IpcConnectionPtr conn) { rpc.attach_connection(conn); };
  ipc.on_message = [&rpc](IpcConnectionPtr conn, std::string_view payload) {
    rpc.handle_message(conn, payload);
  };
  ipc.on_close = [&rpc](IpcConnectionPtr conn) { rpc.handle_close(conn); };

  if (cfg.ipc_enabled) {
    const std::string base = cfg.ipc_base.empty() ? default_ipc_base_path() : cfg.ipc_base;
    if (ipc.listen(base, cfg.ipc_socket, &err)) {
      any_listener = true;
    } else {
      logging::error(logging::Tag::ipc, "failed to listen: " + err);
    }
  }

  WsServer ws(reactor, logging::Tag::ws, "rpc");
  ws.on_connect = [&cfg, &rpc](WsConnectionPtr conn, const HttpRequest &req) {
    const auto origin_it = req.headers.find("origin");
    const std::string origin = origin_it != req.headers.end() ? origin_it->second : "";

    logging::line(logging::Tag::ws, logging::fmt("new connection! origin: %s", origin.c_str()));

    if (!origin.empty() && !cfg.allow_any_origin && !origin_allowed(origin, cfg)) {
      logging::line(logging::Tag::ws, "disallowed origin " + origin);
      conn->close_conn();
      return;
    }

    int version = 1;
    const auto v_it = req.params.find("v");
    if (v_it != req.params.end() && !parse_int_prefix(v_it->second, &version)) version = -1;
    if (version != 1) {
      logging::line(logging::Tag::ws, logging::fmt("unsupported version requested %d", version));
      conn->close_conn();
      return;
    }

    std::string encoding = "json";
    const auto enc_it = req.params.find("encoding");
    if (enc_it != req.params.end() && !enc_it->second.empty()) encoding = enc_it->second;
    if (encoding != "json") {
      logging::line(logging::Tag::ws, "unsupported encoding requested " + encoding);
      conn->close_conn();
      return;
    }

    const auto id_it = req.params.find("client_id");
    if (id_it != req.params.end()) conn->client_id = id_it->second;
    conn->encoding = encoding;
    conn->origin = origin;

    rpc.attach_connection(conn);
  };
  ws.on_message = [&rpc](WsConnectionPtr conn, std::string_view payload) {
    rpc.handle_message(conn, payload);
  };
  ws.on_close = [&rpc](WsConnectionPtr conn) { rpc.handle_close(conn); };

  if (cfg.ws_enabled) {
    if (ws.listen(cfg.bind_host, cfg.ws_port_first, cfg.ws_port_last, nullptr)) any_listener = true;
  }

  if (!any_listener) {
    logging::error(logging::Tag::none, "no transport could be started, exiting");
    return 1;
  }

  // --- process scanning ------------------------------------------------------
  ProcessScanner scanner(reactor, rpc, db);
  if (cfg.process_scan) scanner.start(cfg.scan_interval_ms);

  // --- signals ---------------------------------------------------------------
  if (::pipe(g_signal_pipe) == 0) {
    net::set_nonblocking(g_signal_pipe[0]);
    net::set_nonblocking(g_signal_pipe[1]);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    reactor.add(g_signal_pipe[0], POLLIN, [&reactor](short) {
      char buf[64];
      while (::read(g_signal_pipe[0], buf, sizeof(buf)) > 0) {
      }
      logging::line(logging::Tag::none, "shutting down");
      reactor.stop();
    });
  }

  logging::line(logging::Tag::none, "ready");
  reactor.run();

  logging::line(logging::Tag::none, "bye");
  return 0;
}
