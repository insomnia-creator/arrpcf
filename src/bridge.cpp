#include "bridge.hpp"

Bridge::Bridge(Reactor &reactor) : server_(reactor, logging::Tag::bridge, "bridge") {
  server_.on_connect = [this](WsConnectionPtr conn, const HttpRequest &) {
    logging::line(logging::Tag::bridge, "web connected");

    // Catch up newly connected clients with the latest activity of every socket.
    for (const auto &kv : last_payload_) conn->send_json(kv.second);
  };
  server_.on_close = [](WsConnectionPtr conn) {
    // Only report real bridge clients, not failed HTTP/WebSocket handshakes.
    if (conn->handshaked()) logging::line(logging::Tag::bridge, "web disconnected");
  };
  server_.on_message = [](WsConnectionPtr, std::string_view) {
    // The bridge is one-way; Discord never sends anything meaningful back.
  };
}

bool Bridge::start(const std::string &host, int port) {
  return server_.listen(host, port, port, nullptr);
}

void Bridge::publish(const std::string &socket_id, bool has_activity, const std::string &payload) {
  if (has_activity) {
    last_payload_[socket_id] = payload;
  } else {
    // Cleared sockets are never re-sent on catch-up, so forget them entirely
    // (arRPC keeps a null entry in lastMsg forever; this just bounds memory).
    last_payload_.erase(socket_id);
  }
  server_.broadcast(payload);
}
