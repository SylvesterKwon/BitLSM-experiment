#pragma once
// Open-loop schedule for qui_player's update thread: update i is due
// i/rate seconds after t0. Pure arithmetic on nanoseconds so it is testable
// without a clock; the driver feeds it steady_clock readings relative to t0.
#include <cmath>
#include <cstdint>

namespace qui {

struct Pacer {
  double rate;  // updates per second, > 0

  // Nanoseconds after t0 at which update i is due.
  int64_t DueNs(uint64_t i) const {
    return static_cast<int64_t>(
        std::llround(static_cast<double>(i) * 1e9 / rate));
  }

  // Number of updates due strictly before `elapsed_ns`, i.e. those with
  // DueNs(i) < elapsed_ns. At the end of a run of T seconds this is the
  // scheduled total, ceil(T * rate).
  uint64_t ScheduledBy(int64_t elapsed_ns) const {
    if (elapsed_ns <= 0) return 0;
    return static_cast<uint64_t>(
        std::ceil(static_cast<double>(elapsed_ns) * rate / 1e9));
  }

  // Due but not yet issued.
  uint64_t Backlog(int64_t elapsed_ns, uint64_t issued) const {
    uint64_t due = ScheduledBy(elapsed_ns);
    return due > issued ? due - issued : 0;
  }
};

}  // namespace qui
