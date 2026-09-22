#include <cassert>
#include <cstdio>
#include "../../src/shared/waiting_page.hpp"

using taxi_camera::profiles::WaitingPageMinimumMs;
using taxi_camera::standalone::WaitingPageTimer;

int main() {
  WaitingPageTimer timer;
  assert(timer.observe(1000, 0) == 0);
  // Each side starts its own minimum time when it is first admitted.
  assert(timer.observe(1000, 1) == 1);
  assert(timer.observe(1500, 3) == 3);
  assert(timer.observe(1000 + WaitingPageMinimumMs - 1, 3) == 3);
  assert(timer.observe(1000 + WaitingPageMinimumMs, 3) == 2);
  assert(timer.observe(1500 + WaitingPageMinimumMs, 3) == 0);
  // OFF resets that side only; the next ON shows the page again.
  assert(timer.observe(5000, 2) == 0);
  assert(timer.observe(5100, 3) == 1);
  assert(timer.observe(5100 + WaitingPageMinimumMs, 3) == 0);
  // A zero or backwards clock never ends the page early.
  WaitingPageTimer zero;
  assert(zero.observe(0, 1) == 1);
  assert(zero.observe(0, 1) == 1);
  WaitingPageTimer backwards;
  assert(backwards.observe(9000, 2) == 2);
  assert(backwards.observe(8000, 2) == 2);
  // Bits outside the two sides are ignored.
  WaitingPageTimer wide;
  assert(wide.observe(100, 0xffu) == 3);
  std::puts("PASS waiting page timer: per-side minimum, OFF reset, clock guards and side bounds");
  return 0;
}
