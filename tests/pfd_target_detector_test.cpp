#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../src/pfd_target_detector.hpp"

using taxi_camera::PfdTargetConfidence;
using taxi_camera::PfdTargetDetector;
using taxi_camera::PfdTargetObservation;

int main() {
  static PfdTargetDetector detector;
  std::array<PfdTargetObservation, 3> values{
      {{10, 100000, 768, 1024, 5, 28}, {20, 200000, 768, 1024, 5, 28}, {30, 9000000, 768, 1024, 5, 28}}};
  auto observe = [&](std::uint64_t now) -> const auto& { return detector.observe(values.data(), values.size(), now); };
  auto add = [&](std::uint64_t left, std::uint64_t right, std::uint64_t other) {
    for (auto& value : values)
      value.draws += value.id == 20 ? left : value.id == 10 ? right : other;
  };
  assert(!observe(0).valid && detector.snapshot().stable_windows == 0);
  add(100, 90, 10);
  assert(!observe(999).valid && detector.snapshot().stable_windows == 0);
  assert(!observe(1000).valid && detector.snapshot().stable_windows == 1);
  assert(detector.snapshot().targets[0] == 0 && detector.snapshot().confidence == PfdTargetConfidence::stabilizing);
  // Reverse both enumeration order and busiest-PFD rank. Side mapping is by ID.
  std::swap(values[0], values[2]);
  add(90, 100, 10);
  assert(!observe(2000).valid && detector.snapshot().stable_windows == 2);
  add(100, 100, 10);
  assert(observe(3000).valid);
  assert(detector.snapshot().targets[0] == 20 && detector.snapshot().targets[1] == 10);
  assert(detector.snapshot().confidence == PfdTargetConfidence::confirmed);
  assert(!observe(4000).valid && std::strcmp(detector.snapshot().status, "no_activity") == 0);
  assert(detector.snapshot().targets[0] == 0 && detector.snapshot().stable_windows == 0);

  add(90, 100, 60);  // Exactly 1.5 times third place is not a clear lead.
  assert(!observe(5000).valid && std::strcmp(detector.snapshot().status, "ambiguous_activity") == 0);
  add(91, 100, 60);
  assert(!observe(6000).valid && detector.snapshot().stable_windows == 1);
  add(30, 91, 1);  // Just over the 3:1 comparability limit.
  assert(!observe(7000).valid && std::strcmp(detector.snapshot().status, "incomparable_rates") == 0);
  add(30, 90, 1);
  assert(!observe(8000).valid && detector.snapshot().stable_windows == 1);

  // Counter rollback is detected even before the next complete time window.
  values[0].draws = 0;
  assert(!observe(8100).valid && std::strcmp(detector.snapshot().status, "counter_reset") == 0);
  assert(detector.snapshot().stable_windows == 0);
  add(100, 90, 10);
  assert(!observe(9100).valid && detector.snapshot().stable_windows == 1);
  assert(!observe(9000).valid && std::strcmp(detector.snapshot().status, "clock_reset") == 0);
  add(100, 90, 10);
  assert(!observe(15001).valid && std::strcmp(detector.snapshot().status, "stale_window") == 0);
  assert(detector.snapshot().stable_windows == 0);

  detector.reset();
  observe(0);
  for (unsigned window = 1; window <= 3; ++window) {
    add(100, 90, 10);
    observe(window * 1000);
  }
  assert(detector.snapshot().valid);
  for (auto& value : values)
    if (value.id == 20)
      value.width = 767;
  assert(!observe(3100).valid && std::strcmp(detector.snapshot().status, "candidate_disappeared") == 0);
  assert(detector.snapshot().targets[0] == 0);
  assert(!detector.observe(nullptr, 0, 3200).valid && std::strcmp(detector.snapshot().status, "no_candidates") == 0);
  assert(!detector.observe(nullptr, 1, 3300).valid && std::strcmp(detector.snapshot().status, "invalid_input") == 0);
  assert(!detector.observe(values.data(), PfdTargetDetector::capacity + 1, 3400).valid);
  assert(std::strcmp(detector.snapshot().status, "capacity_exceeded") == 0);

  values = {{{10, 0, 768, 1024, 5, 28}, {20, 0, 768, 1024, 5, 28}, {30, 0, 768, 1024, 5, 28}}};
  values[1].id = 10;
  assert(!observe(0).valid && std::strcmp(detector.snapshot().status, "duplicate_id") == 0);
  values[1].id = 0;
  assert(!observe(0).valid && std::strcmp(detector.snapshot().status, "invalid_input") == 0);
  values[1].id = 20;
  for (unsigned field = 0; field < 4; ++field) {
    detector.reset();
    auto wrong = values;
    for (auto& value : wrong) {
      if (field == 0)
        value.width = 1024;
      if (field == 1)
        value.height = 768;
      if (field == 2)
        value.levels = 1;
      if (field == 3)
        value.format = 26;
    }
    assert(!detector.observe(wrong.data(), wrong.size(), 0).valid);
    assert(std::strcmp(detector.snapshot().status, "no_candidates") == 0);
  }

  // UINT64_MAX deltas exercise ratio checks without multiplication overflow.
  detector.reset();
  observe(0);
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  add(maximum, maximum / 2, maximum / 4);
  assert(!observe(1000).valid && detector.snapshot().stable_windows == 1);
  values[1].draws = 0;
  assert(!observe(2000).valid && std::strcmp(detector.snapshot().status, "counter_reset") == 0);

  static std::array<PfdTargetObservation, PfdTargetDetector::capacity> full;
  for (std::size_t index = 0; index < full.size(); ++index)
    full[index] = {index + 1, 0, 768, 1024, 5, 28};
  detector.reset();
  assert(!detector.observe(full.data(), full.size(), 0).valid);
  for (unsigned window = 1; window <= 3; ++window) {
    full[100].draws += 100;
    full.back().draws += 90;
    detector.observe(full.data(), full.size(), window * 1000);
  }
  assert(detector.snapshot().valid && detector.snapshot().targets[0] == full.size() && detector.snapshot().targets[1] == 101);
  detector.reset();
  assert(!detector.snapshot().valid && detector.snapshot().targets[0] == 0 && detector.snapshot().stable_windows == 0);

  std::puts("PFD target detector: PASS");
}
