#pragma once

#include <string>
#include <string_view>

// Logging mirrors arRPC's own output so existing log-scrapers/users feel at home:
//   [arRPC > ipc] listening at /var/folders/.../discord-ipc-0
namespace logging {

enum class Tag {
  none,     // [arRPC]
  ipc,      // [arRPC > ipc]
  ws,       // [arRPC > websocket]
  bridge,   // [arRPC > bridge]
  process,  // [arRPC > process]
  rpc,      // [arRPC > rpc]
};

void init(bool debug_enabled, bool color_enabled);
bool debug_enabled();

void line(Tag tag, std::string_view msg);
void error(Tag tag, std::string_view msg);
void debug(Tag tag, std::string_view msg);  // only prints with --debug

// printf-style formatting helper used throughout the codebase.
std::string fmt(const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

}  // namespace logging
