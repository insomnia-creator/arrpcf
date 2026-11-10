#include "net.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

namespace net {
namespace {

// --- SHA-1 (RFC 3174), only used for the WebSocket accept key -----------------
struct Sha1 {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  uint64_t total = 0;
  unsigned char buf[64];
  size_t buflen = 0;

  static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

  void block(const unsigned char *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
      w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) |
             uint32_t(p[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      uint32_t tmp = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = tmp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  void update(const unsigned char *data, size_t len) {
    total += len;
    while (len > 0) {
      size_t take = 64 - buflen;
      if (take > len) take = len;
      std::memcpy(buf + buflen, data, take);
      buflen += take;
      data += take;
      len -= take;
      if (buflen == 64) {
        block(buf);
        buflen = 0;
      }
    }
  }

  void finish(unsigned char out[20]) {
    uint64_t bits = total * 8;
    unsigned char pad = 0x80;
    update(&pad, 1);
    unsigned char zero = 0;
    while (buflen != 56) update(&zero, 1);
    unsigned char len[8];
    for (int i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - i * 8));
    update(len, 8);
    for (int i = 0; i < 5; i++) {
      out[i * 4] = (unsigned char)(h[i] >> 24);
      out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
      out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
      out[i * 4 + 3] = (unsigned char)(h[i]);
    }
  }
};

}  // namespace

bool set_nonblocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void set_nosigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void)fd;
#endif
}

int listen_tcp(const std::string &host, int port, std::string *err) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (err) *err = std::strerror(errno);
    return -1;
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  set_nosigpipe(fd);

  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  std::string h = host.empty() ? "127.0.0.1" : host;
  if (h == "localhost") h = "127.0.0.1";
  if (::inet_pton(AF_INET, h.c_str(), &addr.sin_addr) != 1) {
    if (err) *err = "invalid host: " + h;
    ::close(fd);
    return -1;
  }

  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    if (err) *err = std::strerror(errno);
    ::close(fd);
    return -1;
  }
  if (::listen(fd, 128) < 0) {
    if (err) *err = std::strerror(errno);
    ::close(fd);
    return -1;
  }
  set_nonblocking(fd);
  return fd;
}

int listen_unix(const std::string &path, std::string *err) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    if (err) *err = std::strerror(errno);
    return -1;
  }

  sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    if (err) *err = "socket path too long: " + path;
    ::close(fd);
    return -1;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size());

  ::unlink(path.c_str());  // remove stale socket file
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), static_cast<socklen_t>(sizeof(addr))) < 0) {
    if (err) *err = std::strerror(errno);
    ::close(fd);
    return -1;
  }
  ::chmod(path.c_str(), 0600);
  if (::listen(fd, 128) < 0) {
    if (err) *err = std::strerror(errno);
    ::close(fd);
    return -1;
  }
  set_nonblocking(fd);
  return fd;
}

int accept_conn(int listen_fd) {
  for (;;) {
    int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR) continue;
      return -1;  // EAGAIN or a real error
    }
    set_nonblocking(fd);
    set_nosigpipe(fd);
    return fd;
  }
}

std::string base64_encode(const unsigned char *data, size_t len) {
  static const char *kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  size_t i = 0;
  while (i + 2 < len) {
    uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back(kAlphabet[(n >> 6) & 63]);
    out.push_back(kAlphabet[n & 63]);
    i += 3;
  }
  if (i + 1 == len) {
    uint32_t n = uint32_t(data[i]) << 16;
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out += "==";
  } else if (i + 2 == len) {
    uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back(kAlphabet[(n >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::string sha1_base64(std::string_view data) {
  Sha1 sha;
  sha.update(reinterpret_cast<const unsigned char *>(data.data()), data.size());
  unsigned char digest[20];
  sha.finish(digest);
  return base64_encode(digest, sizeof(digest));
}

std::string percent_decode(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    char c = in[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < in.size()) {
      auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
      };
      int hi = hex(in[i + 1]);
      int lo = hex(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
      } else {
        out.push_back(c);
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string to_lower(std::string_view in) {
  std::string out(in);
  for (char &c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace net
