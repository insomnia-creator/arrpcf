#include "process_scan.hpp"

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "net.hpp"
#include "util.hpp"

namespace {

struct ProcessEntry {
  int pid = 0;
  std::string path;
};

std::vector<ProcessEntry> parse_spec(const std::string &spec) {
  std::vector<ProcessEntry> out;
  size_t start = 0;
  int fake_pid = 900000;
  while (start <= spec.size()) {
    size_t sep = spec.find(';', start);
    if (sep == std::string::npos) sep = spec.size();
    std::string item = spec.substr(start, sep - start);
    if (!item.empty()) {
      ProcessEntry entry;
      size_t bar = item.rfind('|');
      if (bar != std::string::npos) {
        entry.pid = std::atoi(item.c_str() + bar + 1);
        entry.path = item.substr(0, bar);
      } else {
        entry.pid = fake_pid++;
        entry.path = item;
      }
      out.push_back(std::move(entry));
    }
    if (sep == spec.size()) break;
    start = sep + 1;
  }
  return out;
}

// Debug/test hooks:
//   ARRPC_FAKE_PROCESSES="/path/to/Game.app/x;[path|pid];..."
//   ARRPC_FAKE_PROCESSES_FILE=<file with one "path" or "path|pid" per line>
// The file variant is re-read on every scan, which makes detection *and* loss
// of a game testable without launching real games.
bool fake_processes_enabled() {
  const char *inline_spec = std::getenv("ARRPC_FAKE_PROCESSES");
  const char *file = std::getenv("ARRPC_FAKE_PROCESSES_FILE");
  return (inline_spec && *inline_spec) || (file && *file);
}

std::vector<ProcessEntry> fake_processes() {
  const char *file = std::getenv("ARRPC_FAKE_PROCESSES_FILE");
  if (file && *file) {
    std::string spec;
    if (FILE *handle = std::fopen(file, "rb")) {
      char buffer[4096];
      size_t n = 0;
      while ((n = std::fread(buffer, 1, sizeof(buffer), handle)) > 0) {
        spec.append(buffer, n);
      }
      std::fclose(handle);
    }
    for (char &c : spec) {
      if (c == '\n' || c == '\r') c = ';';
    }
    return parse_spec(spec);
  }
  const char *env = std::getenv("ARRPC_FAKE_PROCESSES");
  return parse_spec(env ? env : "");
}

#if defined(__APPLE__)

std::vector<ProcessEntry> list_processes() {
  std::vector<ProcessEntry> out;

  int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
  if (bytes <= 0) return out;

  std::vector<pid_t> pids(static_cast<size_t>(bytes) / sizeof(pid_t) + 64, 0);
  int filled = proc_listpids(PROC_ALL_PIDS, 0, pids.data(),
                             static_cast<int>(pids.size() * sizeof(pid_t)));
  if (filled <= 0) return out;

  size_t count = static_cast<size_t>(filled) / sizeof(pid_t);
  if (count > pids.size()) count = pids.size();

  char path[PROC_PIDPATHINFO_MAXSIZE];
  const int self = static_cast<int>(::getpid());

  for (size_t i = 0; i < count; i++) {
    pid_t pid = pids[i];
    if (pid <= 0 || static_cast<int>(pid) == self) continue;
    int len = proc_pidpath(pid, path, sizeof(path));
    if (len <= 0) continue;
    ProcessEntry entry;
    entry.pid = static_cast<int>(pid);
    entry.path.assign(path, static_cast<size_t>(len));
    out.push_back(std::move(entry));
  }
  return out;
}

// Fetches argv[1..] for a pid (same-user only, like every other scanner).
std::string process_arguments(int pid) {
  int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
  size_t size = 0;
  if (::sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0 || size <= sizeof(int)) return {};

  std::vector<char> buffer(size, 0);
  if (::sysctl(mib, 3, buffer.data(), &size, nullptr, 0) != 0) return {};

  int argc = 0;
  std::memcpy(&argc, buffer.data(), sizeof(argc));
  if (argc <= 0) return {};

  size_t pos = sizeof(argc);
  while (pos < size && buffer[pos] != '\0') pos++;  // executable path
  while (pos < size && buffer[pos] == '\0') pos++;  // padding

  std::string joined;
  for (int i = 0; i < argc && pos < size; i++) {
    std::string arg(buffer.data() + pos);
    pos += arg.size() + 1;
    if (i == 0) continue;  // argv[0] is the binary itself
    if (!joined.empty()) joined.push_back(' ');
    joined += arg;
  }
  return joined;
}

#else

std::vector<ProcessEntry> list_processes() { return {}; }
std::string process_arguments(int) { return {}; }

#endif  // __APPLE__

}  // namespace

std::vector<Detection> detect_processes(const DetectableDb &db) {
  std::vector<Detection> out;

#if !defined(__APPLE__)
  if (!fake_processes_enabled()) {
    static bool warned = false;
    if (!warned) {
      logging::line(logging::Tag::process, "process scanning is implemented for macOS only");
      warned = true;
    }
    return out;
  }
#endif

  const auto processes = fake_processes_enabled() ? fake_processes() : list_processes();

  std::unordered_map<int, std::string> args_cache;
  std::vector<std::string> seen_ids;

  for (const auto &process : processes) {
    const std::string path_lower = net::to_lower(process.path);
    const auto matches = db.match_path(path_lower);

    for (const auto &match : matches) {
      if (!match.exe->arguments.empty()) {
        auto it = args_cache.find(process.pid);
        if (it == args_cache.end()) {
          it = args_cache.emplace(process.pid, process_arguments(process.pid)).first;
        }
        if (it->second.find(match.exe->arguments) == std::string::npos) continue;
      }

      // One detection per application id per scan.
      if (std::find(seen_ids.begin(), seen_ids.end(), match.app->id) != seen_ids.end()) break;
      seen_ids.push_back(match.app->id);

      Detection detection;
      detection.app_id = match.app->id;
      detection.name = match.app->name;
      detection.pid = process.pid;
      detection.path = process.path;
      out.push_back(std::move(detection));
      break;
    }
  }

  return out;
}

ProcessScanner::ProcessScanner(Reactor &reactor, RpcService &rpc, const DetectableDb &db)
    : reactor_(reactor), rpc_(rpc), db_(db) {}

void ProcessScanner::start(int interval_ms) {
  logging::line(logging::Tag::process, "started");
  scan();
  reactor_.every(interval_ms, [this]() { scan(); });
}

void ProcessScanner::scan() {
  const auto detections = detect_processes(db_);

  std::vector<std::string> seen;
  seen.reserve(detections.size());
  for (const auto &detection : detections) {
    seen.push_back(detection.app_id);
    names_[detection.app_id] = detection.name;
    pids_[detection.app_id] = detection.pid;

    auto ts = timestamps_.find(detection.app_id);
    if (ts == timestamps_.end()) {
      timestamps_.emplace(detection.app_id, now_ms());
      logging::line(logging::Tag::process, "detected game! " + detection.name);
    }
    // Re-sent on every scan on purpose: activities that arrived before the
    // Discord client connected are picked up by the bridge's catch-up logic.
    rpc_.emit_detected_activity(detection.app_id, detection.name, detection.pid,
                                timestamps_[detection.app_id]);
  }

  for (auto it = timestamps_.begin(); it != timestamps_.end();) {
    if (std::find(seen.begin(), seen.end(), it->first) != seen.end()) {
      ++it;
      continue;
    }
    auto name = names_.find(it->first);
    auto pid = pids_.find(it->first);
    logging::line(logging::Tag::process, "lost game! " + (name != names_.end() ? name->second : it->first));
    rpc_.clear_detected_activity(it->first, pid != pids_.end() ? pid->second : 0,
                                 pid != pids_.end());
    if (name != names_.end()) names_.erase(name);
    if (pid != pids_.end()) pids_.erase(pid);
    it = timestamps_.erase(it);
  }
}
