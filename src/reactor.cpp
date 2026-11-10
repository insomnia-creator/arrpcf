#include "reactor.hpp"

#include <poll.h>

#include <cerrno>
#include <utility>

void Reactor::add(int fd, short events, Cb cb) {
  entries_[fd] = Entry{events, std::move(cb)};
}

void Reactor::set_events(int fd, short events) {
  auto it = entries_.find(fd);
  if (it != entries_.end()) it->second.events = events;
}

void Reactor::remove(int fd) { entries_.erase(fd); }

void Reactor::post(std::function<void()> cb) { posted_.push_back(std::move(cb)); }

void Reactor::call_later(int delay_ms, std::function<void()> cb) {
  Timer t;
  t.due = Clock::now() + std::chrono::milliseconds(delay_ms);
  t.period_ms = 0;
  t.cb = std::move(cb);
  timers_.push_back(std::move(t));
}

void Reactor::every(int period_ms, std::function<void()> cb) {
  Timer t;
  t.due = Clock::now() + std::chrono::milliseconds(period_ms);
  t.period_ms = period_ms;
  t.cb = std::move(cb);
  timers_.push_back(std::move(t));
}

void Reactor::run_posted() {
  if (posted_.empty()) return;
  std::vector<std::function<void()>> batch;
  batch.swap(posted_);
  for (auto &cb : batch) cb();
}

void Reactor::run_timers() {
  if (timers_.empty()) return;
  auto now = Clock::now();
  std::vector<std::function<void()>> due;
  for (auto &t : timers_) {
    if (t.dead || t.due > now) continue;
    due.push_back(t.cb);
    if (t.period_ms > 0) {
      t.due = now + std::chrono::milliseconds(t.period_ms);
    } else {
      t.dead = true;
    }
  }
  // Compact once a few one-shots have accumulated.
  if (timers_.size() > 16) {
    std::vector<Timer> live;
    live.reserve(timers_.size());
    for (auto &t : timers_)
      if (!t.dead) live.push_back(std::move(t));
    timers_.swap(live);
  }
  for (auto &cb : due) cb();
}

int Reactor::next_timeout_ms() const {
  if (!posted_.empty()) return 0;
  if (timers_.empty()) return -1;
  auto now = Clock::now();
  long long best = -1;
  for (const auto &t : timers_) {
    if (t.dead) continue;
    auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(t.due - now).count();
    if (delta < 0) delta = 0;
    if (best < 0 || delta < best) best = delta;
  }
  if (best < 0) return -1;
  if (best > 60000) best = 60000;
  return static_cast<int>(best);
}

void Reactor::run() {
  running_ = true;
  std::vector<struct pollfd> pfds;
  std::vector<int> fds;

  while (running_) {
    run_posted();
    run_timers();
    if (!running_) break;

    pfds.clear();
    fds.clear();
    pfds.reserve(entries_.size());
    fds.reserve(entries_.size());
    for (const auto &kv : entries_) {
      struct pollfd p;
      p.fd = kv.first;
      p.events = kv.second.events;
      p.revents = 0;
      pfds.push_back(p);
      fds.push_back(kv.first);
    }

    int timeout = next_timeout_ms();
    if (pfds.empty() && timeout < 0) break;  // nothing left to do

    int rc = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (rc == 0) continue;

    // Snapshot ready fds: callbacks may add/remove entries while we dispatch.
    std::vector<std::pair<int, short>> ready;
    ready.reserve(static_cast<size_t>(rc));
    for (size_t i = 0; i < pfds.size(); i++) {
      if (pfds[i].revents == 0) continue;
      ready.emplace_back(fds[i], pfds[i].revents);
    }
    for (auto &r : ready) {
      auto it = entries_.find(r.first);
      if (it == entries_.end()) continue;
      // Copy: the callback may remove its own entry (e.g. a socket closing),
      // which would destroy the std::function we are about to invoke.
      Cb cb = it->second.cb;
      cb(r.second);
      if (!running_) break;
    }
  }
}
