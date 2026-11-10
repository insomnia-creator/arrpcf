#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Small self-contained networking / encoding helpers (no external deps).
namespace net {

bool set_nonblocking(int fd);
void set_nosigpipe(int fd);

// Creates a listening TCP socket bound to `host`:`port` (IPv4 loopback by default).
// Returns fd or -1 (errno preserved, `err` filled when non-null).
int listen_tcp(const std::string &host, int port, std::string *err);

// Creates a listening AF_UNIX socket at `path` (unlinking a stale file first).
int listen_unix(const std::string &path, std::string *err);

// Accept helper; returns -1 when there is nothing left to accept.
int accept_conn(int listen_fd);

std::string base64_encode(const unsigned char *data, size_t len);
std::string sha1_base64(std::string_view data);  // base64(sha1(data)), for WS handshakes

std::string percent_decode(std::string_view in);
std::string to_lower(std::string_view in);
bool ends_with(std::string_view s, std::string_view suffix);

}  // namespace net
