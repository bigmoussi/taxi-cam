#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "../src/write_budget.hpp"

int main() {
  using taxi_camera::WriteBudget;
  WriteBudget budget;
  assert(budget.limit() == 4096);
  for (std::uint32_t index = 0; index < 4096; ++index) {
    assert(budget.try_acquire(10, 1000));
  }
  // In particular, a legitimate frame with more than the old 512-write ceiling
  // remains enabled. Exactly 4096 writes fit before the first nonfatal skip.
  assert(budget.window_accepted == 4096 && budget.total_accepted == 4096);
  assert(!budget.limit_hit && budget.window_epoch == 1);
  for (std::uint32_t index = 0; index < 7000; ++index)
    assert(!budget.try_acquire(10, 1049));
  assert(budget.limit_hit && budget.window_accepted == 4096);
  assert(budget.window_skipped == 7000 && budget.total_skipped == 7000);
  assert(budget.peak_accepted == 4096 && budget.peak_skipped == 7000);
  assert(budget.window_started_ms == 1000 && budget.window_epoch == 1);

  // A frame transition restores capacity without discarding session evidence.
  assert(budget.try_acquire(11, 1049));
  assert(!budget.limit_hit && budget.window_accepted == 1 && budget.window_skipped == 0);
  assert(budget.total_accepted == 4097 && budget.total_skipped == 7000);
  assert(budget.window_epoch == 2 && budget.present_frame == 11);
  assert(budget.timed_window_resets == 0);

  budget.set_limit(64);
  budget.reset_session(20, 2000);
  for (unsigned index = 0; index < 64; ++index)
    assert(budget.try_acquire(20, 2000));
  assert(!budget.try_acquire(20, 2049));
  assert(budget.try_acquire(20, 2050));
  assert(budget.window_started_ms == 2050 && budget.window_accepted == 1 && !budget.limit_hit);
  assert(budget.total_accepted == 65 && budget.total_skipped == 1 && budget.window_epoch == 2);
  assert(budget.timed_window_resets == 1);
  for (unsigned index = 1; index < 64; ++index)
    assert(budget.try_acquire(20, 2099));
  assert(!budget.try_acquire(20, 2099));
  assert(budget.try_acquire(20, 2100));
  assert(budget.total_accepted == 129 && budget.total_skipped == 2 && budget.window_epoch == 3);
  assert(budget.timed_window_resets == 2);

  // A long pause grants one window, with no catch-up loop or accumulated credit.
  assert(budget.try_acquire(20, 1000000));
  assert(budget.window_epoch == 4 && budget.window_accepted == 1);
  for (unsigned index = 1; index < 64; ++index)
    assert(budget.try_acquire(20, 1000000));
  assert(!budget.try_acquire(20, 999999));
  assert(budget.window_epoch == 4 && budget.limit_hit);
  assert(budget.timed_window_resets == 3);
  // Frame and time expiry together create only one window, counted as a frame
  // transition rather than evidence of a stalled Present counter.
  assert(budget.try_acquire(21, 1000050));
  assert(budget.window_epoch == 5 && budget.window_accepted == 1 && budget.timed_window_resets == 3);

  budget.set_limit(0);
  assert(budget.limit() == 64);
  budget.set_limit(63);
  assert(budget.limit() == 64);
  budget.set_limit(4096);
  assert(budget.limit() == 4096);
  budget.set_limit(std::numeric_limits<std::uint32_t>::max());
  assert(budget.limit() == 16384);
  budget.reset_session(0, 0);
  assert(budget.limit() == 16384 && budget.window_epoch == 1);
  assert(budget.total_accepted == 0 && budget.total_skipped == 0 && budget.peak_accepted == 0 && budget.peak_skipped == 0);
  assert(budget.window_accepted == 0 && budget.window_skipped == 0 && !budget.limit_hit);
  assert(budget.timed_window_resets == 0);
  for (unsigned index = 0; index < 16384; ++index)
    assert(budget.try_acquire(0, 0));
  assert(!budget.try_acquire(0, 0));

  // Lowering a limit mid-window cannot refill it. Raising it permits only the
  // difference, and the flag retains the earlier skip until the window changes.
  budget.set_limit(64);
  assert(!budget.try_acquire(0, 1));
  budget.reset_session(1, 0);
  for (unsigned index = 0; index < 64; ++index)
    assert(budget.try_acquire(1, 0));
  assert(!budget.try_acquire(1, 0));
  budget.set_limit(65);
  assert(budget.try_acquire(1, 0));
  assert(!budget.try_acquire(1, 0));
  assert(budget.limit_hit && budget.window_accepted == 65 && budget.window_skipped == 2);
  assert(budget.try_acquire(2, 0));
  assert(!budget.limit_hit && budget.total_skipped == 2);

  // Long-lived observational counters saturate instead of wrapping or creating
  // extra write capacity. The timestamp check also avoids unsigned underflow.
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  budget.reset_session(maximum, maximum - 49);
  budget.total_accepted = maximum;
  budget.total_skipped = maximum;
  budget.window_epoch = maximum;
  for (unsigned index = 0; index < 65; ++index)
    assert(budget.try_acquire(maximum, maximum));
  budget.window_skipped = maximum;
  assert(!budget.try_acquire(maximum, maximum));
  assert(budget.total_accepted == maximum && budget.total_skipped == maximum && budget.window_skipped == maximum);
  assert(budget.peak_skipped == maximum && budget.window_epoch == maximum);
  assert(budget.try_acquire(0, maximum));
  assert(budget.window_epoch == maximum && budget.window_accepted == 1 && !budget.limit_hit);

  std::puts("PASS: bounded write capacity, exact limits, nonfatal frame/time recovery, session statistics, clamping and saturation.");
}
