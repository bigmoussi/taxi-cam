#pragma once

#include <cstdint>

namespace taxi_camera {
inline constexpr float kDefaultNightBoostEv = 8.f;
// Increment only when a release deliberately replaces saved night-boost values.
// Each aircraft profile records the applied revision; later user edits persist.
inline constexpr std::uint32_t kNightBoostPreferenceRevision = 1;
}  // namespace taxi_camera
