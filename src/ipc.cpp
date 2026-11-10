#include "ipc.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "json.hpp"
#include "net.hpp"

namespace {

constexpr size_t kMaxPacketBytes = 8 * 1024 * 1024;

int32_t read_le32(const unsigned char *p) {
  return static_cast<int32_t>(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
                              (uint32_t(p[3]) << 24));
}

void append_le32(std::string &out, int32_t v) {
  uint32_t u = static_cast<uint32_t>(v);
  out.push_back(static_cast<char>(u & 0xFF));
  out.push_back(static_cast<char>((u >> 8) & 0xFF));
  out.push_back(static_cast<char>((u >> 16) & 0xFF));
  out.push_back(static_cast<char>((u >> 24) & 0xFF));
}

std::string encode_packet(int32_t type, std::string_view payload) {
  std::string out;
  out.reserve(payload.size() + 8);
  append_le32(out, type);
  append_le32(out, static_cast<int32_t>(payload.size()));
  out.append(payload.data(), payload.size());
  return out;
}

// Blocking availability probe: arRPC connects, sends PING and treats
// "no pong within 1s" (or an immediate connection failure) as available.
bool socket_is_available(const std::string &path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;

  sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return false;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size());

  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return true;  // nothing listening there -> free
  }

  std::string ping = encode_packet(IPC_PING, "1");
  (void)::send(fd, ping.data(), ping.size(), 0);

  bool free_slot = true;
  struct pollfd p;
  p.fd = fd;
  p.events = POLLIN;
  p.revents = 0;
  int rc = ::poll(&p, 1, 1000);
  if (rc > 0) {
    // Somebody answered (a pong) -> Discord or another arRPC owns this socket.
    free_slot = false;
  }

  ::close(fd);
  return free_slot;
}

}  // namespace

std::string default_ipc_base_path() {
  const char *env_dirs[] = {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"};
  for (const char *name : env_dirs) {
    const char *value = std::getenv(name);
    if (!value || !*value) continue;
    std::string dir(value);
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();  // TMPDIR ends in '/'
    return dir + "/discord-ipc";
  }
  return "/tmp/discord-ipc";
}

// ---------------------------------------------------------------------------
// IpcConnection
// ---------------------------------------------------------------------------

IpcConnection::IpcConnection(IpcServer *server, Reactor &reactor, int fd)
    : server_(server), reactor_(reactor), fd_(fd) {
  transport = "ipc";
}

IpcConnection::~IpcConnection() {
  if (!dead_ && fd_ >= 0) ::close(fd_);
}

void IpcConnection::send_json(const std::string &payload) { send_frame(IPC_FRAME, payload); }

void IpcConnection::close_conn() { close_with(IPC_CLOSE_NORMAL, ""); }

void IpcConnection::close_with(int32_t code, std::string_view message) {
  if (close_sent_ || dead_) return;
  close_sent_ = true;

  std::string payload;
  {
    js::D *doc = js::new_doc();
    js::V *o = js::obj(doc);
    js::put(doc, o, "code", js::sint(doc, code));
    js::put(doc, o, "message", js::str(doc, message));
    payload = js::dump(doc, o);
    js::free_doc(doc);
  }
  send_frame(IPC_CLOSE, payload);
  // flush() schedules the drop once the close packet has been written out.
  flush();
}

void IpcConnection::send_frame(int32_t type, std::string_view payload) {
  if (dead_) return;
  out_ += encode_packet(type, payload);
  flush();
}

void IpcConnection::flush() {
  while (!out_.empty()) {
    ssize_t n = ::send(fd_, out_.data(), out_.size(), 0);
    if (n > 0) {
      out_.erase(0, static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (n < 0 && errno == EINTR) continue;
    auto self = this->self();
    if (self) server_->drop(self);
    return;
  }
  update_interest();
  if (close_sent_ && out_.empty() && !dead_) {
    auto self = this->self();
    if (self) server_->reactor_.post([srv = server_, self]() { srv->drop(self); });
  }
}

void IpcConnection::update_interest() {
  short events = POLLIN;
  if (!out_.empty()) events |= POLLOUT;
  reactor_.set_events(fd_, events);
}

// ---------------------------------------------------------------------------
// IpcServer
// ---------------------------------------------------------------------------

IpcServer::IpcServer(Reactor &reactor) : reactor_(reactor) {}

IpcServer::~IpcServer() {
  if (listen_fd_ >= 0) {
    reactor_.remove(listen_fd_);
    ::close(listen_fd_);
  }
  if (!socket_path_.empty()) ::unlink(socket_path_.c_str());
  for (auto &c : clients_)
    if (!c->dead_) ::close(c->fd_);
}

namespace {

// TMPDIR usually ends with '/': avoid ".../T//discord-ipc-0" in logs and keep
// the probed path byte-identical to what Discord's own clients compute.
std::string normalize_base(std::string base) {
  while (base.size() > 1 && base.back() == '/') base.pop_back();
  return base;
}

}  // namespace

bool IpcServer::listen(const std::string &raw_base, const std::string &exact_socket,
                       std::string *err) {
  const std::string base = normalize_base(raw_base);
  std::string path;
  if (!exact_socket.empty()) {
    // Refuse to steal a socket that is answering pings (i.e. Discord desktop or
    // another arRPC); otherwise an exact path could silently kill real RPC.
    if (!socket_is_available(exact_socket)) {
      if (err) *err = exact_socket + " is already in use (Discord running?)";
      return false;
    }
    path = exact_socket;
  } else {
    int attempt = 0;
    for (; attempt <= 9; attempt++) {
      std::string candidate = base + "-" + std::to_string(attempt);
      logging::debug(logging::Tag::ipc, logging::fmt("checking %s", candidate.c_str()));
      if (socket_is_available(candidate)) {
        path = candidate;
        break;
      }
      logging::line(logging::Tag::ipc,
                    logging::fmt("not available, trying again (attempt %d)", attempt + 1));
    }
    if (path.empty()) {
      if (err) *err = "ran out of tries to find a free discord-ipc socket (0-9)";
      return false;
    }
  }

  int fd = net::listen_unix(path, err);
  if (fd < 0) return false;

  listen_fd_ = fd;
  socket_path_ = path;
  reactor_.add(fd, POLLIN, [this](short) { handle_accept(); });
  logging::line(logging::Tag::ipc, logging::fmt("listening at %s", path.c_str()));
  return true;
}

void IpcServer::handle_accept() {
  for (;;) {
    int fd = net::accept_conn(listen_fd_);
    if (fd < 0) return;
    logging::line(logging::Tag::ipc, "new connection!");
    auto conn = std::make_shared<IpcConnection>(this, reactor_, fd);
    conn->start();
    clients_.insert(conn);
    reactor_.add(fd, POLLIN, [this, conn](short revents) { handle_events(conn, revents); });
  }
}

void IpcServer::handle_events(const IpcConnectionPtr &conn, short revents) {
  if (conn->dead_) return;
  if (revents & POLLIN) {
    read_available(conn);
    if (conn->dead_) return;
  }
  if (revents & POLLOUT) {
    conn->flush();
    if (conn->dead_) return;
  }
  if (revents & (POLLERR | POLLHUP | POLLNVAL)) drop(conn);
}

void IpcServer::read_available(const IpcConnectionPtr &conn) {
  char buf[16 * 1024];
  for (;;) {
    ssize_t n = ::recv(conn->fd_, buf, sizeof(buf), 0);
    if (n > 0) {
      conn->in_.append(buf, static_cast<size_t>(n));
      process_input(conn);
      if (conn->dead_) return;
      continue;
    }
    if (n == 0) {
      drop(conn);
      return;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    drop(conn);
    return;
  }
}

void IpcServer::process_input(const IpcConnectionPtr &conn) {
  for (;;) {
    if (conn->dead_) return;
    if (conn->in_.size() < 8) return;

    const auto *p = reinterpret_cast<const unsigned char *>(conn->in_.data());
    const int32_t type = read_le32(p);
    const int32_t len = read_le32(p + 4);

    if (len < 0 || static_cast<size_t>(len) > kMaxPacketBytes) {
      conn->close_with(IPC_CLOSE_UNSUPPORTED, "invalid packet length");
      return;
    }
    if (conn->in_.size() < 8 + static_cast<size_t>(len)) return;

    std::string payload = conn->in_.substr(8, static_cast<size_t>(len));
    conn->in_.erase(0, 8 + static_cast<size_t>(len));

    switch (type) {
      case IPC_HANDSHAKE:
        if (conn->handshaked_) {
          conn->close_with(IPC_CLOSE_UNSUPPORTED, "already handshook");
          return;
        }
        handle_handshake(conn, payload);
        if (conn->dead_) return;
        break;

      case IPC_FRAME:
        if (!conn->handshaked_) {
          conn->close_with(IPC_CLOSE_UNSUPPORTED, "need to handshake first");
          return;
        }
        if (on_message) on_message(conn, payload);
        if (conn->dead_) return;
        break;

      case IPC_PING:
        conn->send_frame(IPC_PONG, payload);
        break;

      case IPC_PONG:
        break;

      case IPC_CLOSE:
        drop(conn);
        return;

      default:
        conn->close_with(IPC_CLOSE_UNSUPPORTED, "invalid type");
        return;
    }
  }
}

void IpcServer::handle_handshake(const IpcConnectionPtr &conn, std::string_view payload) {
  js::D *doc = js::parse(payload);
  if (!doc || !js::is_obj(yyjson_mut_doc_get_root(doc))) {
    js::free_doc(doc);
    conn->close_with(IPC_CLOSE_UNSUPPORTED, "invalid handshake");
    return;
  }

  int64_t version = 1;
  js::get_int(yyjson_mut_doc_get_root(doc), "v", &version);
  const char *client_id = js::get_str(yyjson_mut_doc_get_root(doc), "client_id");
  std::string client = client_id ? client_id : "";

  logging::debug(logging::Tag::ipc,
                 logging::fmt("handshake: v=%lld client_id=%s", static_cast<long long>(version), client.c_str()));
  js::free_doc(doc);

  if (version != 1) {
    logging::line(logging::Tag::ipc, logging::fmt("unsupported version requested %lld", static_cast<long long>(version)));
    conn->close_with(IPC_INVALID_VERSION, "unsupported version");
    return;
  }
  if (client.empty()) {
    logging::line(logging::Tag::ipc, "client id required");
    conn->close_with(IPC_INVALID_CLIENTID, "client id required");
    return;
  }

  conn->client_id = client;
  conn->handshaked_ = true;
  if (on_connection) on_connection(conn);
}

void IpcServer::drop(IpcConnectionPtr conn) {
  if (!conn || conn->dead_) return;
  conn->dead_ = true;
  reactor_.remove(conn->fd_);
  ::close(conn->fd_);
  conn->fd_ = -1;
  clients_.erase(conn);
  if (on_close) on_close(conn);
}
