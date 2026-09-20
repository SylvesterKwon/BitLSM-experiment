// Arithmetic gate for qui::Pacer. Prints "PACER OK" and exits 0.
#include <cstdlib>
#include <iostream>

#include "honk_player/qui_pacer.h"

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::cout << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond   \
                << "\n";                                                  \
      std::exit(1);                                                       \
    }                                                                     \
  } while (0)

int main() {
  qui::Pacer p{1000.0};  // one update per millisecond
  CHECK(p.DueNs(0) == 0);
  CHECK(p.DueNs(1) == 1'000'000);
  CHECK(p.DueNs(1500) == 1'500'000'000);

  // Updates due strictly before `elapsed`: at t=0 nothing is due yet.
  CHECK(p.ScheduledBy(-5) == 0);
  CHECK(p.ScheduledBy(0) == 0);
  CHECK(p.ScheduledBy(1) == 1);            // update 0 (due at 0) is now due
  CHECK(p.ScheduledBy(5'000'000) == 5);    // due at 0..4 ms
  CHECK(p.ScheduledBy(5'500'000) == 6);    // due at 0..5 ms
  CHECK(p.ScheduledBy(1'800'000'000'000LL) == 1'800'000);  // T=1800 s

  CHECK(p.Backlog(5'000'000, 5) == 0);
  CHECK(p.Backlog(5'000'000, 2) == 3);
  CHECK(p.Backlog(5'000'000, 9) == 0);     // issued ahead is impossible, clamp

  qui::Pacer slow{0.5};  // one update every 2 s
  CHECK(slow.DueNs(3) == 6'000'000'000LL);
  CHECK(slow.ScheduledBy(6'000'000'000LL) == 3);
  CHECK(slow.ScheduledBy(6'000'000'001LL) == 4);

  std::cout << "PACER OK\n";
  return 0;
}
