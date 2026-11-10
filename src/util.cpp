#include "util.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

extern char **environ;

long long now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool open_url(const std::string &url) {
  if (url.empty()) return false;
  // Only ever hand http(s)/discord URLs to `open`; never shell out.
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0 && url.rfind("discord://", 0) != 0) {
    return false;
  }

  pid_t pid = 0;
  char *argv[] = {const_cast<char *>("/usr/bin/open"), const_cast<char *>(url.c_str()), nullptr};
  int rc = ::posix_spawn(&pid, "/usr/bin/open", nullptr, nullptr, argv, environ);
  if (rc != 0) return false;
  return true;  // do not wait; `open` returns quickly and we must not block the loop
}
