#pragma once

namespace taxi_camera {

inline constexpr unsigned kMinimumCameraRate = 5;
inline constexpr unsigned kMaximumCameraRate = 60;
inline constexpr unsigned kDefaultCameraRate = 10;
// Schedule floor used while the aircraft is parked; 0 in settings disables it.
inline constexpr unsigned kDefaultParkedCameraRate = kMinimumCameraRate;
// Live camera-manager cadence was 42–47 updates/s (0.9.35 rate sweep). From
// this setting on, nearly every manager update is already a gate transition,
// so a higher request cannot add useful pulses and is reported as capped.
inline constexpr unsigned kManagerCeilingCameraRate = 15;

}  // namespace taxi_camera
