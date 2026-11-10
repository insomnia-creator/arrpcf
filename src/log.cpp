#include "log.hpp"

#include <cstdarg>
#include <cstdio>

namespace logging {
namespace {

bool g_debug = false;
bool g_color = false;

struct Rgb {
  int r, g, b;
};

const Rgb kBrand{88, 101, 242};  // arRPC blurple

Rgb tag_color(Tag tag) {
  switch (tag) {
    case Tag::ipc: return {254, 231, 92};
    case Tag::ws: return {235, 69, 158};
    case Tag::bridge: return {87, 242, 135};
    case Tag::process: return {237, 66, 69};
    case Tag::rpc: return {148, 155, 164};
    case Tag::none:
    default: return kBrand;
  }
}

const char *tag_name(Tag tag) {
  switch (tag) {
    case Tag::ipc: return "ipc";
    case Tag::ws: return "websocket";
    case Tag::bridge: return "bridge";
    case Tag::process: return "process";
    case Tag::rpc: return "rpc";
    case Tag::none:
    default: return "";
  }
}

std::string rgb(const Rgb &c, std::string_view msg) {
  if (!g_color) return std::string(msg);
  char prefix[32];
  std::snprintf(prefix, sizeof(prefix), "\x1b[38;2;%d;%d;%dm", c.r, c.g, c.b);
  return std::string(prefix) + std::string(msg) + "\x1b[0m";
}

std::string make_prefix(Tag tag) {
  std::string name = tag_name(tag);
  if (name.empty()) return rgb(kBrand, "arRPC");
  return rgb(kBrand, "arRPC") + " > " + rgb(tag_color(tag), name);
}

}  // namespace

void init(bool debug_enabled, bool color_enabled) {
  g_debug = debug_enabled;
  g_color = color_enabled;
}

bool debug_enabled() { return g_debug; }

void line(Tag tag, std::string_view msg) {
  std::string out = "[" + make_prefix(tag) + "] " + std::string(msg) + "\n";
  std::fwrite(out.data(), 1, out.size(), stdout);
  std::fflush(stdout);
}

void error(Tag tag, std::string_view msg) {
  std::string prefix = "[" + make_prefix(tag) + "] ";
  if (g_color) prefix = "\x1b[38;2;237;66;69m" + prefix;  // red
  std::string out = prefix + std::string(msg);
  if (g_color) out += "\x1b[0m";
  out += "\n";
  std::fwrite(out.data(), 1, out.size(), stderr);
  std::fflush(stderr);
}

void debug(Tag tag, std::string_view msg) {
  if (!g_debug) return;
  line(tag, msg);
}

std::string fmt(const char *format, ...) {
  char stack_buf[512];
  va_list ap;
  va_start(ap, format);
  int needed = std::vsnprintf(stack_buf, sizeof(stack_buf), format, ap);
  va_end(ap);

  if (needed < 0) return {};
  if (static_cast<size_t>(needed) < sizeof(stack_buf)) return std::string(stack_buf, static_cast<size_t>(needed));

  std::string big(static_cast<size_t>(needed) + 1, '\0');
  va_start(ap, format);
  std::vsnprintf(big.data(), big.size(), format, ap);
  va_end(ap);
  big.resize(static_cast<size_t>(needed));
  return big;
}

}  // namespace logging
