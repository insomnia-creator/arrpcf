#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

#include "log.hpp"
#include "reactor.hpp"
#include "transport.hpp"

class IpcServer;
class IpcConnection;
using IpcConnectionPtr = std::shared_ptr<IpcConnection>;

// Discord IPC packet types.
enum IpcType : int32_t {
  IPC_HANDSHAKE = 0,
  IPC_FRAME = 1,
  IPC_CLOSE = 2,
  IPC_PING = 3,
  IPC_PONG = 4,
};

// Discord RPC close/error codes we care about.
enum IpcCode : int32_t {
  IPC_CLOSE_NORMAL = 1000,
  IPC_CLOSE_UNSUPPORTED = 1003,
  IPC_INVALID_CLIENTID = 4000,
  IPC_INVALID_VERSION = 4004,
};

class IpcConnection : public RpcConnection, public std::enable_shared_from_this<IpcConnection> {
 public:
  IpcConnection(IpcServer *server, Reactor &reactor, int fd);
  ~IpcConnection() override;

  void send_json(const std::string &payload) override;
  void close_conn() override;
  void close_with(int32_t code, std::string_view message);

  bool handshaked() const { return handshaked_; }
  void start() { self_weak_ = weak_from_this(); }

 private:
  friend class IpcServer;

  IpcConnectionPtr self() { return self_weak_.lock(); }
  void send_frame(int32_t type, std::string_view payload);
  void flush();
  void update_interest();

  IpcServer *server_ = nullptr;
  Reactor &reactor_;
  int fd_ = -1;
  bool handshaked_ = false;
  bool dead_ = false;
  bool close_sent_ = false;
  std::weak_ptr<IpcConnection> self_weak_;
  std::string in_;
  std::string out_;
};

class IpcServer {
 public:
  using ConnectCb = std::function<void(IpcConnectionPtr)>;
  using MessageCb = std::function<void(IpcConnectionPtr, std::string_view)>;
  using CloseCb = std::function<void(IpcConnectionPtr)>;

  IpcServer(Reactor &reactor);
  ~IpcServer();

  // `exact_socket` wins when non-empty; otherwise `base` is probed with -0..-9.
  bool listen(const std::string &base, const std::string &exact_socket, std::string *err);

  const std::string &socket_path() const { return socket_path_; }
  bool is_listening() const { return listen_fd_ >= 0; }

  ConnectCb on_connection;
  MessageCb on_message;
  CloseCb on_close;

 private:
  friend class IpcConnection;

  void handle_accept();
  void handle_events(const IpcConnectionPtr &conn, short revents);
  void read_available(const IpcConnectionPtr &conn);
  void process_input(const IpcConnectionPtr &conn);
  void handle_handshake(const IpcConnectionPtr &conn, std::string_view payload);
  void drop(IpcConnectionPtr conn);

  Reactor &reactor_;
  int listen_fd_ = -1;
  std::string socket_path_;
  std::unordered_set<IpcConnectionPtr> clients_;
};

// Default Discord IPC socket base path, matching arRPC (and Discord itself).
std::string default_ipc_base_path();
