#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace taxi_camera {

struct DisplayExposureState {
  float applied_ev = -8.8f;
  float target_ev = -8.8f;
  float night_boost_ev = 0;
  bool lighting_valid = false;
  const char* status = "manual";
};

// Display adaptation only: it cannot restore light contributions missing from
// the scene texture. No saturation, channel colouring or engine changes occur.
// Ambient1..4000 follows the official Asobo display-lighting template range;
// its mapping to the user's night EV boost is a bounded visual heuristic.
class DisplayExposureController {
 public:
  static constexpr float DayExposureEv = -8.8f;
  static constexpr float DefaultNightBoostEv = 4;
  static constexpr float MaximumNightBoostEv = 8;
  static constexpr std::uint64_t MaximumLightingAgeMs = 1500;
  static constexpr float SlewEvPerSecond = 1;

  const DisplayExposureState& snapshot() const noexcept { return state_; }
  void reset() noexcept {
    state_ = {};
    initialized_ = false;
    previous_ms_ = 0;
  }

  const DisplayExposureState& update(std::uint64_t now_ms,
                                     float manual_ev,
                                     bool automatic,
                                     float maximum_night_boost_ev,
                                     bool lighting_valid,
                                     double ambient,
                                     std::uint64_t lighting_sample_ms) noexcept {
    if (!std::isfinite(manual_ev) || !std::isfinite(maximum_night_boost_ev)) {
      state_.status = "invalid_exposure_setting";
      return state_;
    }
    manual_ev = std::clamp(manual_ev, -16.f, 4.f);
    maximum_night_boost_ev = std::clamp(maximum_night_boost_ev, 0.f, MaximumNightBoostEv);
    state_.lighting_valid = lighting_valid && lighting_sample_ms != 0 && now_ms >= lighting_sample_ms &&
                            now_ms - lighting_sample_ms <= MaximumLightingAgeMs && std::isfinite(ambient) && ambient >= 0 && ambient <= 1e7;
    state_.night_boost_ev = 0;
    state_.target_ev = manual_ev;
    if (automatic && state_.lighting_valid) {
      const auto darkness = std::clamp(std::log2(4000.0 / std::max(ambient, 1.0)) / std::log2(4000.0), 0.0, 1.0);
      state_.night_boost_ev = maximum_night_boost_ev * static_cast<float>(darkness);
      state_.target_ev = std::clamp(manual_ev + state_.night_boost_ev, -16.f, 4.f);
    }
    state_.status = !automatic                  ? "manual"
                    : !state_.lighting_valid    ? "lighting_unavailable"
                    : state_.night_boost_ev > 0 ? "automatic_night"
                                                : "automatic_day";
    if (!initialized_ || !automatic) {
      state_.applied_ev = manual_ev;
    } else {
      // No accumulated catch-up jump after a stopped Present loop. A backwards
      // caller clock grants no adaptation step and starts a new elapsed window.
      const auto elapsed = now_ms >= previous_ms_ ? std::min<std::uint64_t>(now_ms - previous_ms_, 1000) : 0;
      const auto step = static_cast<float>(elapsed) * (SlewEvPerSecond / 1000);
      state_.applied_ev += std::clamp(state_.target_ev - state_.applied_ev, -step, step);
      state_.applied_ev = std::clamp(state_.applied_ev, -16.f, 4.f);
    }
    previous_ms_ = now_ms;
    initialized_ = true;
    return state_;
  }

 private:
  DisplayExposureState state_{};
  std::uint64_t previous_ms_ = 0;
  bool initialized_ = false;
};

}  // namespace taxi_camera
