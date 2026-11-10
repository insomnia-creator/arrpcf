#pragma once

#include <chrono>
#include <functional>
#include <unordered_map>
#include <vector>

// Minimal single-threaded poll() reactor: the whole server runs on one thread,
// which is why the binary stays small and there are no locks anywhere.
class Reactor {
 public:
  using Cb = std::function<void(short)>;
  using Clock = std::chrono::steady_clock;

  void add(int fd, short events, Cb cb);
  void set_events(int fd, short events);
  void remove(int fd);

  // Run `cb` on the next loop iteration (used to defer work out of callbacks).
  void post(std::function<void()> cb);
  // One-shot timer.
  void call_later(int delay_ms, std::function<void()> cb);
  // Repeating timer.
  void every(int period_ms, std::function<void()> cb);

  void run();
  void stop() { running_ = false; }

 private:
  struct Entry {
    short events = 0;
    Cb cb;
  };
  struct Timer {
    Clock::time_point due;
    int period_ms = 0;
    std::function<void()> cb;
    bool dead = false;
  };

  int next_timeout_ms() const;
  void run_posted();
  void run_timers();

  std::unordered_map<int, Entry> entries_;
  std::vector<Timer> timers_;
  std::vector<std::function<void()>> posted_;
  bool running_ = false;
};
