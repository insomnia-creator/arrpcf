#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "bridge.hpp"
#include "json.hpp"
#include "log.hpp"
#include "transport.hpp"

// Protocol implementation: exactly the commands arRPC answers, plus the
// translation of SET_ACTIVITY into the shape Discord's LOCAL_ACTIVITY_UPDATE
// expects on the bridge.
class RpcService {
 public:
  RpcService(Bridge &bridge, bool open_links);

  // Called for every accepted (and handshaked) client; sends the READY packet.
  void attach_connection(RpcConnectionPtr conn);
  void handle_message(RpcConnectionPtr conn, std::string_view payload);
  void handle_close(RpcConnectionPtr conn);

  // Process-scanning entry points (equivalent to synthetic SET_ACTIVITY calls).
  void emit_detected_activity(const std::string &app_id, const std::string &name, long long pid,
                              long long start_ms);
  void clear_detected_activity(const std::string &app_id, long long pid, bool has_pid);

 private:
  using DataFn = std::function<void(js::D *, js::V *)>;

  void dispatch(RpcConnection *conn, js::V *msg);
  void on_set_activity(RpcConnection *conn, js::V *msg);
  void send_response(RpcConnection *conn, const std::string &cmd, const DataFn &fill_data,
                     const char *evt, js::V *nonce);
  void publish_activity(const std::string &socket_id, bool has_activity, js::V *activity,
                        bool has_pid, long long pid);

  Bridge &bridge_;
  bool open_links_ = false;
  int next_socket_id_ = 0;
};

// Helpers shared with the process scanner.
void build_bridge_activity(js::D *doc, js::V *out, const std::string &client_id, js::V *activity);
void copy_activity_into(js::D *doc, js::V *dst, js::V *activity);
void translate_timestamps(js::V *activity);
bool js_truthy(js::V *value);
