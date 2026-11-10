#include "rpc.hpp"

#include <cctype>
#include <cstdint>
#include <string>

#include "util.hpp"

namespace {

int digit_count_u(uint64_t n) {
  int digits = 1;
  while (n >= 10) {
    n /= 10;
    digits++;
  }
  return digits;
}

bool is_valid_code(const std::string &code) {
  if (code.size() < 2 || code.size() > 64) return false;
  for (char raw : code) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (!(std::isalnum(c) || c == '-' || c == '_')) return false;
  }
  return true;
}

}  // namespace

bool js_truthy(js::V *value) {
  if (!value) return false;
  if (yyjson_mut_is_null(value)) return false;
  if (yyjson_mut_is_bool(value)) return yyjson_mut_get_bool(value);
  if (yyjson_mut_is_uint(value)) return yyjson_mut_get_uint(value) != 0;
  if (yyjson_mut_is_sint(value)) return yyjson_mut_get_sint(value) != 0;
  if (yyjson_mut_is_real(value)) return yyjson_mut_get_real(value) != 0.0;
  if (yyjson_mut_is_str(value)) return yyjson_mut_get_len(value) != 0;
  return true;  // arrays and objects are truthy in JS
}

void copy_activity_into(js::D *doc, js::V *dst, js::V *activity) {
  if (!js::is_obj(activity)) return;
  js::for_each(activity, [&](js::V *key, js::V *val) {
    const char *name = yyjson_mut_get_str(key);
    if (name) js::put(doc, dst, name, js::copy(doc, val));
  });
}

void build_bridge_activity(js::D *doc, js::V *out, const std::string &client_id, js::V *activity) {
  if (!client_id.empty()) js::put(doc, out, "application_id", js::str(doc, client_id));
  js::put(doc, out, "type", js::uint(doc, 0));

  js::V *buttons = js::is_obj(activity) ? js::get(activity, "buttons") : nullptr;

  js::V *metadata = js::obj(doc);
  if (js::is_arr(buttons)) {
    js::V *urls = js::arr(doc);
    js::for_each_arr(buttons, [&](js::V *button) {
      const char *url = js::is_obj(button) ? js::get_str(button, "url") : nullptr;
      js::push(doc, urls, url ? js::str(doc, url) : js::null(doc));
    });
    js::put(doc, metadata, "button_urls", urls);
  }
  js::put(doc, out, "metadata", metadata);

  const bool instance = js::is_obj(activity) && js_truthy(js::get(activity, "instance"));
  js::put(doc, out, "flags", js::uint(doc, instance ? 1u : 0u));

  // Activity fields come last so they win over the synthesized ones, exactly
  // like arRPC's `{...defaults, ...activity, ...extra}`.
  copy_activity_into(doc, out, activity);

  if (js::is_arr(buttons)) {
    js::V *labels = js::arr(doc);
    js::for_each_arr(buttons, [&](js::V *button) {
      const char *label = js::is_obj(button) ? js::get_str(button, "label") : nullptr;
      js::push(doc, labels, label ? js::str(doc, label) : js::null(doc));
    });
    js::put(doc, out, "buttons", labels);
  }
}

void translate_timestamps(js::V *activity) {
  js::V *timestamps = js::get(activity, "timestamps");
  if (!js::is_obj(timestamps)) return;

  const int now_digits = digit_count_u(static_cast<uint64_t>(now_ms()));
  js::for_each(timestamps, [&](js::V *, js::V *value) {
    if (!js::is_num(value)) return;
    if (yyjson_mut_is_uint(value)) {
      uint64_t n = yyjson_mut_get_uint(value);
      if (now_digits - digit_count_u(n) > 2) yyjson_mut_set_uint(value, n * 1000);
    } else if (yyjson_mut_is_sint(value)) {
      int64_t n = yyjson_mut_get_sint(value);
      uint64_t magnitude = static_cast<uint64_t>(n < 0 ? -n : n);
      if (now_digits - digit_count_u(magnitude) > 2) yyjson_mut_set_sint(value, n * 1000);
    } else {
      double n = yyjson_mut_get_real(value);
      double magnitude = n < 0 ? -n : n;
      if (magnitude >= 1.0 && now_digits - digit_count_u(static_cast<uint64_t>(magnitude)) > 2) {
        yyjson_mut_set_real(value, n * 1000.0);
      }
    }
  });
}

RpcService::RpcService(Bridge &bridge, bool open_links)
    : bridge_(bridge), open_links_(open_links) {}

void RpcService::attach_connection(RpcConnectionPtr conn) {
  conn->attached = true;
  conn->socket_id = next_socket_id_++;

  js::D *doc = js::new_doc();
  js::V *msg = js::obj(doc);
  js::put(doc, msg, "cmd", js::str(doc, "DISPATCH"));

  js::V *data = js::obj(doc);
  js::put(doc, data, "v", js::uint(doc, 1));

  js::V *config = js::obj(doc);
  js::put(doc, config, "cdn_host", js::str(doc, "cdn.discordapp.com"));
  js::put(doc, config, "api_endpoint", js::str(doc, "//discord.com/api"));
  js::put(doc, config, "environment", js::str(doc, "production"));
  js::put(doc, data, "config", config);

  // Mock user data using the arRPC app/bot, same as arRPC itself.
  js::V *user = js::obj(doc);
  js::put(doc, user, "id", js::str(doc, "1045800378228281345"));
  js::put(doc, user, "username", js::str(doc, "arrpc"));
  js::put(doc, user, "discriminator", js::str(doc, "0"));
  js::put(doc, user, "global_name", js::str(doc, "arRPC"));
  js::put(doc, user, "avatar", js::str(doc, "cfefa4d9839fb4bdf030f91c2a13e95c"));
  js::put(doc, user, "avatar_decoration_data", js::null(doc));
  js::put(doc, user, "bot", js::boolean(doc, false));
  js::put(doc, user, "flags", js::uint(doc, 0));
  js::put(doc, user, "premium_type", js::uint(doc, 0));
  js::put(doc, data, "user", user);

  js::put(doc, msg, "data", data);
  js::put(doc, msg, "evt", js::str(doc, "READY"));
  js::put(doc, msg, "nonce", js::null(doc));

  std::string payload = js::dump(doc, msg);
  js::free_doc(doc);

  logging::debug(logging::Tag::rpc, "sending READY to socket " + std::to_string(conn->socket_id));
  conn->send_json(payload);
}

void RpcService::handle_message(RpcConnectionPtr conn, std::string_view payload) {
  logging::debug(logging::Tag::rpc, std::string("message: ") + std::string(payload));

  js::D *doc = js::parse(payload);
  if (!doc) {
    logging::line(logging::Tag::rpc, "ignoring malformed JSON message");
    return;
  }
  js::V *root = yyjson_mut_doc_get_root(doc);
  if (js::is_obj(root)) dispatch(conn.get(), root);
  js::free_doc(doc);
}

void RpcService::dispatch(RpcConnection *conn, js::V *msg) {
  const char *cmd_c = js::get_str(msg, "cmd");
  if (!cmd_c) return;

  const std::string cmd(cmd_c);
  js::V *args = js::get(msg, "args");
  js::V *nonce = js::get(msg, "nonce");

  if (cmd == "SET_ACTIVITY") {
    on_set_activity(conn, msg);
    return;
  }

  if (cmd == "CONNECTIONS_CALLBACK") {
    // arRPC answers this with a generic "success" error; some clients need it.
    send_response(conn, cmd,
                  [](js::D *d, js::V *data) { js::put(d, data, "code", js::uint(d, 1000)); },
                  "ERROR", nonce);
    return;
  }

  if (cmd == "INVITE_BROWSER" || cmd == "GUILD_TEMPLATE_BROWSER") {
    const bool is_invite = cmd == "INVITE_BROWSER";
    const char *code_c = js::is_obj(args) ? js::get_str(args, "code") : nullptr;
    const std::string code = code_c ? code_c : "";
    const bool valid = is_valid_code(code);
    const int error_code = is_invite ? 4011 : 4017;

    if (valid && open_links_) {
      open_url(is_invite ? "https://discord.com/invite/" + code
                         : "https://discord.com/template/" + code);
    }

    send_response(
        conn, cmd,
        [&](js::D *d, js::V *data) {
          if (valid) {
            js::put(d, data, "code", js::str(d, code));
          } else {
            js::put(d, data, "code", js::uint(d, static_cast<uint64_t>(error_code)));
            js::put(d, data, "message",
                    js::str(d, std::string("Invalid ") + (is_invite ? "invite" : "guild template") +
                                   " id: " + code));
          }
        },
        valid ? nullptr : "ERROR", nonce);
    return;
  }

  if (cmd == "DEEP_LINK") {
    std::string url;
    if (js::is_obj(args)) {
      for (const char *field : {"url", "deep_link", "link"}) {
        const char *value = js::get_str(args, field);
        if (value && *value) {
          url = value;
          break;
        }
      }
    }
    if (open_links_ && !url.empty()) open_url(url);
    send_response(conn, cmd, nullptr, nullptr, nonce);
    return;
  }

  logging::debug(logging::Tag::rpc, "unhandled command: " + cmd);
}

void RpcService::on_set_activity(RpcConnection *conn, js::V *msg) {
  js::V *args = js::get(msg, "args");
  js::V *activity = js::is_obj(args) ? js::get(args, "activity") : nullptr;
  js::V *nonce = js::get(msg, "nonce");

  bool has_pid = false;
  long long pid = 0;
  if (js::is_obj(args)) has_pid = js::get_int(args, "pid", &pid);

  const std::string socket_id = conn ? std::to_string(conn->socket_id) : "";
  const std::string client_id = conn ? conn->client_id : "";

  if (!js::is_obj(activity)) {
    // Activity clear.
    send_response(conn, "SET_ACTIVITY", nullptr, nullptr, nonce);
    publish_activity(socket_id, false, nullptr, has_pid, pid);
    return;
  }

  if (conn && has_pid) {
    conn->last_pid = pid;
    conn->has_last_pid = true;
  }

  translate_timestamps(activity);

  // What the web client receives.
  js::D *bridge_doc = js::new_doc();
  js::V *bridge_activity = js::obj(bridge_doc);
  build_bridge_activity(bridge_doc, bridge_activity, client_id, activity);
  publish_activity(socket_id, true, bridge_activity, has_pid, pid);
  js::free_doc(bridge_doc);

  // What the RPC client receives.
  if (conn) {
    send_response(
        conn, "SET_ACTIVITY",
        [&](js::D *d, js::V *data) {
          copy_activity_into(d, data, activity);
          js::put(d, data, "name", js::str(d, ""));
          js::put(d, data, "application_id", js::str(d, client_id));
          js::put(d, data, "type", js::uint(d, 0));
        },
        nullptr, nonce);
  }
}

void RpcService::send_response(RpcConnection *conn, const std::string &cmd, const DataFn &fill_data,
                               const char *evt, js::V *nonce) {
  if (!conn) return;

  js::D *doc = js::new_doc();
  js::V *msg = js::obj(doc);
  js::put(doc, msg, "cmd", js::str(doc, cmd));

  js::V *data = js::null(doc);
  if (fill_data) {
    data = js::obj(doc);
    fill_data(doc, data);
  }
  js::put(doc, msg, "data", data);

  js::put(doc, msg, "evt", evt ? js::str(doc, evt) : js::null(doc));
  if (nonce) js::put(doc, msg, "nonce", js::copy(doc, nonce));

  std::string payload = js::dump(doc, msg);
  js::free_doc(doc);

  logging::debug(logging::Tag::rpc, std::string("sending: ") + payload);
  conn->send_json(payload);
}

void RpcService::publish_activity(const std::string &socket_id, bool has_activity, js::V *activity,
                                  bool has_pid, long long pid) {
  js::D *doc = js::new_doc();
  js::V *msg = js::obj(doc);
  js::put(doc, msg, "activity", activity ? js::copy(doc, activity) : js::null(doc));
  if (has_pid) js::put(doc, msg, "pid", js::sint(doc, pid));
  js::put(doc, msg, "socketId", js::str(doc, socket_id));

  std::string payload = js::dump(doc, msg);
  js::free_doc(doc);

  bridge_.publish(socket_id, has_activity, payload);
}

void RpcService::handle_close(RpcConnectionPtr conn) {
  conn->closed = true;
  // Rejected transports (bad Origin/version/encoding/handshake) are closed
  // before they get a socket id; clearing here would wipe an unrelated socket's
  // activity in the web client.
  if (!conn->attached) return;
  logging::line(logging::Tag::rpc, "socket closed, clearing activity for socket " +
                                        std::to_string(conn->socket_id));
  publish_activity(std::to_string(conn->socket_id), false, nullptr, conn->has_last_pid,
                   conn->last_pid);
}

void RpcService::emit_detected_activity(const std::string &app_id, const std::string &name,
                                        long long pid, long long start_ms) {
  js::D *src = js::new_doc();
  js::V *activity = js::obj(src);
  js::put(src, activity, "application_id", js::str(src, app_id));
  js::put(src, activity, "name", js::str(src, name));
  js::V *timestamps = js::obj(src);
  js::put(src, timestamps, "start", js::sint(src, start_ms));
  js::put(src, activity, "timestamps", timestamps);

  js::D *doc = js::new_doc();
  js::V *out = js::obj(doc);
  build_bridge_activity(doc, out, "", activity);
  publish_activity(app_id, true, out, true, pid);

  js::free_doc(doc);
  js::free_doc(src);
}

void RpcService::clear_detected_activity(const std::string &app_id, long long pid, bool has_pid) {
  publish_activity(app_id, false, nullptr, has_pid, pid);
}
