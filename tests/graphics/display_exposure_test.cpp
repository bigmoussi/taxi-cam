#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../../src/graphics/display_exposure.hpp"

int main() {
  using taxi_camera::DisplayExposureController;
  DisplayExposureController controller;
  constexpr auto day = DisplayExposureController::DayExposureEv;
  assert(controller.snapshot().applied_ev == day);
  auto value = controller.update(1000, day, true, 4, true, 4000, 1000);
  assert(value.applied_ev == day && value.target_ev == day && value.lighting_valid && value.night_boost_ev == 0);
  value = controller.update(1500, day, true, 4, true, 0.536, 1500);
  assert(value.target_ev == day + 4 && value.applied_ev == day + .5f && value.night_boost_ev == 4);
  for (unsigned i = 2; i <= 8; ++i)
    value = controller.update(1000 + i * 500, day, true, 4, true, 0.536, 1000 + i * 500);
  assert(value.applied_ev == day + 4);
  value = controller.update(5001, -7, false, 4, true, 0.5, 5001);
  assert(value.applied_ev == -7 && value.target_ev == -7 && value.night_boost_ev == 0 && std::strcmp(value.status, "manual") == 0);
  // Manual mode does not need lighting and must not slew the user's choice.
  value = controller.update(5001, -4.2f, false, 4, false, 0, 0);
  assert(value.applied_ev == -4.2f && !value.lighting_valid);
  controller.reset();
  controller.update(1000, day, true, 4, true, 0, 1000);
  for (unsigned i = 1; i <= 4; ++i)
    value = controller.update(1000 + i * 1000, day, true, 4, true, 0, 1000 + i * 1000);
  assert(value.applied_ev == day + 4);  // Zero is a valid darkest reading.
  value = controller.update(6500, day, true, 4, true, 0, 5000);
  assert(value.lighting_valid && value.target_ev == day + 4);
  value = controller.update(6501, day, true, 4, true, 0, 5000);
  assert(!value.lighting_valid && value.target_ev == day && value.applied_ev < day + 4 && value.applied_ev > day + 3.99f);
  value = controller.update(1000000, day, true, 4, false, 0, 0);
  assert(value.applied_ev > day + 2.99f);  // A long gap advances at most1EV.
  const auto before_reverse = value.applied_ev;
  value = controller.update(999999, day, true, 4, false, 0, 0);
  assert(value.applied_ev == before_reverse);

  float previous_boost = 4;
  for (double ambient : {0., .5, 1., 2., 10., 100., 1000., 4000., 50000.}) {
    controller.reset();
    value = controller.update(100, day, true, 4, true, ambient, 100);
    assert(value.lighting_valid && value.night_boost_ev <= previous_boost);
    previous_boost = value.night_boost_ev;
  }
  assert(previous_boost == 0);
  for (double invalid : {-1., 1e7 + 1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    value = controller.update(200, day, true, 4, true, invalid, 200);
    assert(!value.lighting_valid && value.target_ev == day && std::isfinite(value.applied_ev));
  }
  value = controller.update(200, day, true, 4, true, 1, 201);
  assert(!value.lighting_valid);
  value = controller.update(300, 3, true, 99, true, 0, 300);
  assert(value.target_ev == 4 && value.night_boost_ev == 8);
  value = controller.update(300, -99, false, -1, false, 0, 0);
  assert(value.applied_ev == -16 && value.target_ev == -16);
  value = controller.update(300, 99, false, 4, false, 0, 0);
  assert(value.applied_ev == 4 && value.target_ev == 4);
  value = controller.update(300, std::numeric_limits<float>::quiet_NaN(), true, 4, true, 1, 300);
  assert(value.applied_ev == 4 && std::strcmp(value.status, "invalid_exposure_setting") == 0);
  value = controller.update(300, day, true, std::numeric_limits<float>::infinity(), true, 1, 300);
  assert(value.applied_ev == 4);
  controller.reset();
  assert(controller.snapshot().applied_ev == day && controller.snapshot().night_boost_ev == 0);
  std::puts("Display exposure: PASS");
}
