#pragma once

#include <memory>
#include <string>

// Transport-agnostic view of a connected RPC client (IPC socket or WebSocket).
// arRPC calls this object `socket`; it carries the client id and the "last pid"
// used to clear an activity when the client disappears.
struct RpcConnection {
  int socket_id = 0;            // arRPC's numeric socket id (stringified in bridge messages)
  std::string client_id;        // ?client_id= / handshake client_id
  std::string transport;        // "ipc" or "websocket"
  long long last_pid = 0;
  bool has_last_pid = false;
  bool closed = false;
  // True once the RPC layer accepted this connection (socket id + READY sent).
  // Rejected handshakes/origins must never publish activity updates.
  bool attached = false;

  virtual ~RpcConnection() = default;

  // Sends one protocol message (already-serialized JSON object).
  virtual void send_json(const std::string &payload) = 0;
  // Closes the transport (CLOSE frame / WS close handshake).
  virtual void close_conn() = 0;
};

using RpcConnectionPtr = std::shared_ptr<RpcConnection>;
