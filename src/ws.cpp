#include "ws.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "net.hpp"

namespace {

constexpr const char *kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr size_t kMaxHeaderBytes = 32 * 1024;
constexpr size_t kMaxMessageBytes = 8 * 1024 * 1024;

std::string trim(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) b++;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) e--;
  return std::string(s.substr(b, e - b));
}

std::string http_response(const char *status, const std::string &body) {
  return std::string("HTTP/1.1 ") + status +
         "\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + body;
}

void append_be16(std::string &out, uint16_t v) {
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>(v & 0xFF));
}

void append_be64(std::string &out, uint64_t v) {
  for (int i = 7; i >= 0; i--) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

}  // namespace

std::unordered_map<std::string, std::string> parse_query(std::string_view query) {
  std::unordered_map<std::string, std::string> params;
  size_t start = 0;
  while (start <= query.size()) {
    size_t amp = query.find('&', start);
    if (amp == std::string_view::npos) amp = query.size();
    std::string_view pair = query.substr(start, amp - start);
    if (!pair.empty()) {
      size_t eq = pair.find('=');
      if (eq == std::string_view::npos) {
        params[net::percent_decode(pair)] = "";
      } else {
        params[net::percent_decode(pair.substr(0, eq))] = net::percent_decode(pair.substr(eq + 1));
      }
    }
    if (amp == query.size()) break;
    start = amp + 1;
  }
  return params;
}

// ---------------------------------------------------------------------------
// WsConnection
// ---------------------------------------------------------------------------

WsConnection::WsConnection(WsServer *server, Reactor &reactor, int fd)
    : server_(server), reactor_(reactor), fd_(fd) {
  transport = "websocket";
}

WsConnection::~WsConnection() {
  if (!dead_ && fd_ >= 0) ::close(fd_);
}

void WsConnection::send_json(const std::string &payload) { send_frame(0x1, payload); }

void WsConnection::close_conn() { close_with(1000); }

void WsConnection::close_with(uint16_t code, std::string_view reason) {
  if (close_sent_ || dead_) return;
  close_sent_ = true;
  std::string payload;
  append_be16(payload, code);
  payload.append(reason);
  send_frame(0x8, payload);
  // flush() schedules the drop once the close frame has been written out.
  flush();
}

void WsConnection::send_raw(std::string_view bytes) {
  if (dead_) return;
  out_.append(bytes.data(), bytes.size());
  flush();
}

void WsConnection::send_frame(uint8_t opcode, std::string_view payload) {
  if (dead_) return;
  std::string frame;
  frame.reserve(payload.size() + 10);
  frame.push_back(static_cast<char>(0x80 | opcode));  // FIN + opcode

  const uint64_t len = payload.size();
  if (len < 126) {
    frame.push_back(static_cast<char>(len));
  } else if (len <= 0xFFFF) {
    frame.push_back(static_cast<char>(126));
    append_be16(frame, static_cast<uint16_t>(len));
  } else {
    frame.push_back(static_cast<char>(127));
    append_be64(frame, len);
  }
  frame.append(payload.data(), payload.size());
  send_raw(frame);
}

void WsConnection::flush() {
  while (!out_.empty()) {
    ssize_t n = ::send(fd_, out_.data(), out_.size(), 0);
    if (n > 0) {
      out_.erase(0, static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (n < 0 && errno == EINTR) continue;
    // Socket is gone.
    auto self = this->self();
    if (!self) return;
    server_->drop(self);
    return;
  }
  update_interest();
  if (close_sent_ && out_.empty() && !dead_) {
    auto self = this->self();
    if (!self) return;
    server_->reactor_.post([srv = server_, self]() { srv->drop(self); });
  }
}

void WsConnection::update_interest() {
  short events = POLLIN;
  if (!out_.empty()) events |= POLLOUT;
  reactor_.set_events(fd_, events);
}

// ---------------------------------------------------------------------------
// WsServer
// ---------------------------------------------------------------------------

WsServer::WsServer(Reactor &reactor, logging::Tag log_tag, std::string name)
    : reactor_(reactor), tag_(log_tag), name_(std::move(name)) {}

WsServer::~WsServer() {
  if (listen_fd_ >= 0) {
    reactor_.remove(listen_fd_);
    ::close(listen_fd_);
  }
  for (auto &c : clients_)
    if (!c->dead_) ::close(c->fd_);
}

bool WsServer::listen(const std::string &host, int port_first, int port_last, int *bound_port) {
  for (int port = port_first; port <= port_last; port++) {
    if (port < 1 || port > 65535) continue;
    std::string err;
    int fd = net::listen_tcp(host, port, &err);
    if (fd < 0) {
      logging::line(tag_, logging::fmt("%d in use! (%s)", port, err.c_str()));
      continue;
    }
    listen_fd_ = fd;
    bound_port_ = port;
    reactor_.add(fd, POLLIN, [this](short) { handle_accept(); });
    logging::line(tag_, logging::fmt("listening on %d", port));
    if (bound_port) *bound_port = port;
    return true;
  }
  logging::error(tag_, logging::fmt("could not bind any port in %d-%d", port_first, port_last));
  return false;
}

void WsServer::handle_accept() {
  for (;;) {
    int fd = net::accept_conn(listen_fd_);
    if (fd < 0) return;
    auto conn = std::make_shared<WsConnection>(this, reactor_, fd);
    conn->start();
    clients_.insert(conn);
    reactor_.add(fd, POLLIN, [this, conn](short revents) { handle_events(conn, revents); });
  }
}

void WsServer::handle_events(const WsConnectionPtr &conn, short revents) {
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

void WsServer::read_available(const WsConnectionPtr &conn) {
  char buf[16 * 1024];
  for (;;) {
    ssize_t n = ::recv(conn->fd_, buf, sizeof(buf), 0);
    if (n > 0) {
      conn->in_.append(buf, static_cast<size_t>(n));
      process_input(conn);
      if (conn->dead_) return;
      continue;
    }
    if (n == 0) {  // peer closed
      drop(conn);
      return;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    drop(conn);
    return;
  }
}

bool WsServer::parse_handshake(const WsConnectionPtr &conn, HttpRequest *req, bool *need_more) {
  *need_more = false;
  size_t header_end = conn->in_.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    if (conn->in_.size() > kMaxHeaderBytes) {
      conn->send_raw(http_response("431 Request Header Fields Too Large", "header too large"));
      conn->close_sent_ = true;
      conn->flush();
      return false;
    }
    *need_more = true;
    return false;
  }

  std::string_view head(conn->in_.data(), header_end);
  conn->in_.erase(0, header_end + 4);

  size_t line_end = head.find("\r\n");
  std::string_view request_line = head.substr(0, line_end == std::string_view::npos ? head.size() : line_end);
  size_t sp1 = request_line.find(' ');
  size_t sp2 = sp1 == std::string_view::npos ? std::string_view::npos : request_line.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
    conn->send_raw(http_response("400 Bad Request", "malformed request line"));
    conn->close_sent_ = true;
    conn->flush();
    return false;
  }
  req->method = std::string(request_line.substr(0, sp1));
  req->target = std::string(request_line.substr(sp1 + 1, sp2 - sp1 - 1));

  size_t qmark = req->target.find('?');
  if (qmark == std::string::npos) {
    req->path = req->target;
  } else {
    req->path = req->target.substr(0, qmark);
    req->query = req->target.substr(qmark + 1);
    req->params = parse_query(req->query);
  }

  size_t pos = (line_end == std::string_view::npos) ? head.size() : line_end + 2;
  while (pos < head.size()) {
    size_t next = head.find("\r\n", pos);
    if (next == std::string_view::npos) next = head.size();
    std::string_view line = head.substr(pos, next - pos);
    pos = next + 2;
    if (line.empty()) continue;
    size_t colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    std::string name = net::to_lower(trim(line.substr(0, colon)));
    std::string value = trim(line.substr(colon + 1));
    if (!name.empty()) req->headers[name] = value;
  }

  auto upgrade = req->headers.find("upgrade");
  auto connection = req->headers.find("connection");
  auto key = req->headers.find("sec-websocket-key");

  bool is_ws = upgrade != req->headers.end() &&
               net::to_lower(upgrade->second).find("websocket") != std::string::npos &&
               key != req->headers.end() && !key->second.empty() &&
               (connection == req->headers.end() ||
                net::to_lower(connection->second).find("upgrade") != std::string::npos);

  if (!is_ws) {
    conn->send_raw(http_response("426 Upgrade Required",
                                 "This is an arRPC WebSocket endpoint, not an HTTP server.\n"));
    conn->close_sent_ = true;
    conn->flush();
    return false;
  }

  std::string accept = net::sha1_base64(key->second + kGuid);
  std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
  response += "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
  conn->send_raw(response);
  return true;
}

void WsServer::process_input(const WsConnectionPtr &conn) {
  if (!conn->handshake_done_) {
    HttpRequest req;
    bool need_more = false;
    if (!parse_handshake(conn, &req, &need_more)) {
      if (need_more) return;
      // Handshake failed; close once the rejection has been flushed.
      if (!conn->close_sent_) {
        conn->close_sent_ = true;
        conn->flush();
      }
      if (conn->out_.empty()) reactor_.post([this, conn]() { drop(conn); });
      return;
    }
    conn->handshake_done_ = true;
    if (on_connect) on_connect(conn, req);
    if (conn->dead_) return;
  }
  process_frames(conn);
}

bool WsServer::deliver_message(const WsConnectionPtr &conn, uint8_t opcode, std::string_view payload) {
  if (opcode == 0x1 || opcode == 0x2) {
    if (on_message) on_message(conn, payload);
  }
  return !conn->dead_;
}

void WsServer::process_frames(const WsConnectionPtr &conn) {
  for (;;) {
    if (conn->dead_) return;
    if (conn->in_.size() < 2) return;

    const auto *p = reinterpret_cast<const unsigned char *>(conn->in_.data());
    const bool fin = (p[0] & 0x80) != 0;
    const uint8_t opcode = p[0] & 0x0F;
    const bool masked = (p[1] & 0x80) != 0;
    uint64_t len = p[1] & 0x7F;
    size_t pos = 2;

    if (len == 126) {
      if (conn->in_.size() < pos + 2) return;
      len = (uint64_t(p[2]) << 8) | uint64_t(p[3]);
      pos = 4;
    } else if (len == 127) {
      if (conn->in_.size() < pos + 8) return;
      len = 0;
      for (size_t i = 0; i < 8; i++) len = (len << 8) | uint64_t(p[pos + i]);
      pos = 8 + 2;
    }

    if (!masked) {  // client-to-server frames must be masked
      conn->close_with(1002, "unmasked frame");
      return;
    }
    if (len > kMaxMessageBytes) {
      conn->close_with(1009, "message too big");
      return;
    }
    if (opcode >= 0x8 && (len > 125 || !fin)) {
      conn->close_with(1002, "invalid control frame");
      return;
    }
    if (conn->in_.size() < pos + 4 + len) return;  // incomplete

    unsigned char mask[4] = {p[pos], p[pos + 1], p[pos + 2], p[pos + 3]};
    pos += 4;

    std::string payload(static_cast<size_t>(len), '\0');
    for (size_t i = 0; i < payload.size(); i++) {
      payload[i] = static_cast<char>(p[pos + i] ^ mask[i % 4]);
    }
    conn->in_.erase(0, pos + static_cast<size_t>(len));

    switch (opcode) {
      case 0x0: {  // continuation
        if (!conn->in_fragment_) {
          conn->close_with(1002, "unexpected continuation");
          return;
        }
        conn->fragment_.append(payload);
        if (conn->fragment_.size() > kMaxMessageBytes) {
          conn->close_with(1009, "message too big");
          return;
        }
        if (fin) {
          std::string message;
          message.swap(conn->fragment_);
          conn->in_fragment_ = false;
          uint8_t op = conn->fragment_opcode_;
          if (!deliver_message(conn, op, message)) return;
        }
        break;
      }
      case 0x1:
      case 0x2: {
        if (conn->in_fragment_) {
          conn->close_with(1002, "nested fragment");
          return;
        }
        if (fin) {
          if (!deliver_message(conn, opcode, payload)) return;
        } else {
          conn->in_fragment_ = true;
          conn->fragment_opcode_ = opcode;
          conn->fragment_ = std::move(payload);
        }
        break;
      }
      case 0x8:  // close
        conn->close_with(1000);
        return;
      case 0x9:  // ping
        conn->send_frame(0xA, payload);
        break;
      case 0xA:  // pong
        break;
      default:
        conn->close_with(1002, "unknown opcode");
        return;
    }
  }
}

void WsServer::drop(WsConnectionPtr conn) {
  if (!conn || conn->dead_) return;
  conn->dead_ = true;
  reactor_.remove(conn->fd_);
  ::close(conn->fd_);
  conn->fd_ = -1;
  clients_.erase(conn);
  if (on_close) on_close(conn);
}

void WsServer::broadcast(std::string_view payload) {
  std::vector<WsConnectionPtr> targets(clients_.begin(), clients_.end());
  for (auto &conn : targets) {
    if (!conn->ready()) continue;
    conn->send_json(std::string(payload));
  }
}
