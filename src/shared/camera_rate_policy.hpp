#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "camera_rate.hpp"

namespace taxi_camera {

// Why the schedule runs below the saved camera_rate. The saved value is never
// rewritten; these only describe the rate actually requested from the schedule.
enum CameraRateLimit : unsigned {
  kRateLimitNone = 0,
  kRateLimitParked = 1,      // Ground speed held at zero: parked floor applied.
  kRateLimitPfdRefresh = 2,  // Aircraft PFD redraw rate cannot show more pairs.
  kRateLimitManager = 4,     // Camera-manager cadence saturates at this rate.
};

struct EffectiveCameraRate {
  unsigned rate = kDefaultCameraRate;            // Requested from the schedule now.
  unsigned useful_maximum = kMaximumCameraRate;  // Cap while moving, per aircraft.
  unsigned reasons = kRateLimitNone;             // CameraRateLimit bits.
};

// Useful maximum for one aircraft: the lower of its PFD refresh (0 = not
// measured) and the manager ceiling, never below the schedule minimum.
constexpr unsigned useful_camera_rate(unsigned pfd_refresh_hz) noexcept {
  const unsigned pfd = pfd_refresh_hz ? pfd_refresh_hz : kMaximumCameraRate;
  return std::clamp(std::min(pfd, kManagerCeilingCameraRate), kMinimumCameraRate, kMaximumCameraRate);
}

// parked_rate 0 disables the floor. The floor only lowers the rate; a parked
// floor above the moving rate leaves the moving rate in place.
constexpr EffectiveCameraRate effective_camera_rate(unsigned user_rate,
                                                    unsigned pfd_refresh_hz,
                                                    bool parked,
                                                    unsigned parked_rate = kDefaultParkedCameraRate) noexcept {
  EffectiveCameraRate result;
  const unsigned requested = std::clamp(user_rate, kMinimumCameraRate, kMaximumCameraRate);
  result.useful_maximum = useful_camera_rate(pfd_refresh_hz);
  result.rate = requested;
  if (requested > result.useful_maximum) {
    result.rate = result.useful_maximum;
    const unsigned pfd = pfd_refresh_hz ? pfd_refresh_hz : kMaximumCameraRate;
    if (pfd < kManagerCeilingCameraRate)
      result.reasons |= kRateLimitPfdRefresh;
    else
      result.reasons |= kRateLimitManager;
  }
  if (parked && parked_rate) {
    const unsigned floor = std::clamp(parked_rate, kMinimumCameraRate, kMaximumCameraRate);
    if (floor < result.rate) {
      result.rate = floor;
      result.reasons |= kRateLimitParked;
    }
  }
  return result;
}

constexpr const char* camera_rate_limit_name(unsigned reasons) noexcept {
  if (reasons & kRateLimitParked)
    return "parked";
  if (reasons & kRateLimitPfdRefresh)
    return "pfd_refresh";
  if (reasons & kRateLimitManager)
    return "manager_ceiling";
  return "user";
}

// Companion status suffix for the rate in use; empty when the saved rate runs.
constexpr const wchar_t* camera_rate_limit_text(unsigned reasons) noexcept {
  if (reasons & kRateLimitParked)
    return L" (parked floor)";
  if (reasons & kRateLimitPfdRefresh)
    return L" (capped at the PFD refresh rate)";
  if (reasons & kRateLimitManager)
    return L" (capped at the camera-manager ceiling)";
  return L"";
}

// Ground-speed hysteresis for the parked floor. Parked needs the public GROUND
// VELOCITY below kParkedBelowKnots continuously for kParkedSettleMs; any sample
// at or above kMovingAboveKnots restores the moving rate at once. Missing or
// stale telemetry is treated as moving so a telemetry gap never lowers the rate.
// The band between the two thresholds holds the current state, so creeping
// speed at taxi start cannot flap the schedule.
class ParkedRatePolicy {
 public:
  static constexpr double kParkedBelowKnots = 0.5;
  static constexpr double kMovingAboveKnots = 1.0;
  static constexpr std::uint64_t kParkedSettleMs = 3000;

  bool update(std::uint64_t now_ms, bool speed_valid, double knots) noexcept {
    if (!speed_valid || !std::isfinite(knots) || knots < 0 || knots >= kMovingAboveKnots) {
      reset();
      return parked_;
    }
    if (knots >= kParkedBelowKnots) {
      if (!parked_)
        still_ = false;
      return parked_;
    }
    if (!still_ || now_ms < still_since_) {
      still_ = true;
      still_since_ = now_ms;
    }
    if (!parked_ && now_ms - still_since_ >= kParkedSettleMs)
      parked_ = true;
    return parked_;
  }

  bool parked() const noexcept { return parked_; }
  void reset() noexcept {
    parked_ = false;
    still_ = false;
    still_since_ = 0;
  }

 private:
  bool parked_ = false;
  bool still_ = false;
  std::uint64_t still_since_ = 0;
};

}  // namespace taxi_camera
