#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "detect.hpp"
#include "log.hpp"
#include "reactor.hpp"
#include "rpc.hpp"

struct Detection {
  std::string app_id;
  std::string name;
  int pid = 0;
  std::string path;
};

// One-shot process-table scan: which detectable games are running right now?
std::vector<Detection> detect_processes(const DetectableDb &db);

// Periodically walks the process table and turns detectable games into
// synthetic SET_ACTIVITY events (arRPC's process scanning).
class ProcessScanner {
 public:
  ProcessScanner(Reactor &reactor, RpcService &rpc, const DetectableDb &db);

  void start(int interval_ms);
  void scan();

 private:
  Reactor &reactor_;
  RpcService &rpc_;
  const DetectableDb &db_;

  std::unordered_map<std::string, long long> timestamps_;
  std::unordered_map<std::string, std::string> names_;
  std::unordered_map<std::string, long long> pids_;
};
