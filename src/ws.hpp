#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "log.hpp"
#include "reactor.hpp"
#include "transport.hpp"

struct HttpRequest {
  std::string method;
  std::string target;  // raw request target, e.g. /?v=1&client_id=123
  std::string path;
  std::string query;
  std::unordered_map<std::string, std::string> headers;  // lowercased names
  std::unordered_map<std::string, std::string> params;   // percent-decoded query params
};

class WsServer;
class WsConnection;
using WsConnectionPtr = std::shared_ptr<WsConnection>;

class WsConnection : public RpcConnection, public std::enable_shared_from_this<WsConnection> {
 public:
  WsConnection(WsServer *server, Reactor &reactor, int fd);
  ~WsConnection() override;

  std::string origin;
  std::string encoding = "json";

  void send_json(const std::string &payload) override;
  void close_conn() override;
  void close_with(uint16_t code, std::string_view reason = "");

  bool ready() const { return handshake_done_ && !dead_; }
  bool handshaked() const { return handshake_done_; }

  // Must be called right after the owning shared_ptr is created.
  void start() { self_weak_ = weak_from_this(); }

 private:
  friend class WsServer;

  WsConnectionPtr self() { return self_weak_.lock(); }

  void send_frame(uint8_t opcode, std::string_view payload);
  void send_raw(std::string_view bytes);
  void flush();
  void update_interest();

  WsServer *server_ = nullptr;
  Reactor &reactor_;
  int fd_ = -1;
  bool handshake_done_ = false;
  bool dead_ = false;
  bool close_sent_ = false;
  std::weak_ptr<WsConnection> self_weak_;

  std::string in_;
  std::string out_;

  // fragmentation state
  bool in_fragment_ = false;
  uint8_t fragment_opcode_ = 0;
  std::string fragment_;
};

class WsServer {
 public:
  using ConnectCb = std::function<void(WsConnectionPtr, const HttpRequest &)>;
  using MessageCb = std::function<void(WsConnectionPtr, std::string_view)>;
  using CloseCb = std::function<void(WsConnectionPtr)>;

  WsServer(Reactor &reactor, logging::Tag log_tag, std::string name);
  ~WsServer();

  // Tries ports first..last and binds the first available one.
  bool listen(const std::string &host, int port_first, int port_last, int *bound_port);
  void broadcast(std::string_view payload);

  size_t client_count() const { return clients_.size(); }
  int bound_port() const { return bound_port_; }
  bool is_listening() const { return listen_fd_ >= 0; }

  ConnectCb on_connect;
  MessageCb on_message;
  CloseCb on_close;

 private:
  friend class WsConnection;

  void handle_accept();
  void handle_events(const WsConnectionPtr &conn, short revents);
  void read_available(const WsConnectionPtr &conn);
  void process_input(const WsConnectionPtr &conn);
  bool parse_handshake(const WsConnectionPtr &conn, HttpRequest *req, bool *need_more);
  void process_frames(const WsConnectionPtr &conn);
  bool deliver_message(const WsConnectionPtr &conn, uint8_t opcode, std::string_view payload);
  void drop(WsConnectionPtr conn);

  Reactor &reactor_;
  logging::Tag tag_;
  std::string name_;
  int listen_fd_ = -1;
  int bound_port_ = 0;
  std::unordered_set<WsConnectionPtr> clients_;
};

// Query-string parsing shared by the RPC websocket transport.
std::unordered_map<std::string, std::string> parse_query(std::string_view query);
