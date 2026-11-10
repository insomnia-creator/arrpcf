#pragma once

#include <string>
#include <unordered_map>

#include "log.hpp"
#include "reactor.hpp"
#include "ws.hpp"

// The bridge WebSocket (default port 1337) forwards JSON exactly as the Discord
// web client's LOCAL_ACTIVITY_UPDATE dispatcher expects. Newly connected
// clients get the last activity of every still-active socket, like arRPC.
class Bridge {
 public:
  explicit Bridge(Reactor &reactor);

  bool start(const std::string &host, int port);
  bool is_listening() const { return server_.is_listening(); }
  int bound_port() const { return server_.bound_port(); }

  void publish(const std::string &socket_id, bool has_activity, const std::string &payload);

 private:
  WsServer server_;
  // Only sockets with a *live* activity are kept; clearing removes the entry.
  std::unordered_map<std::string, std::string> last_payload_;
};
