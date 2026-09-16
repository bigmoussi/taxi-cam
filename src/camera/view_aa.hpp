#pragma once

#include "camera_layout.hpp"
#include "image_inventory.hpp"
#include "owned_view.hpp"

namespace taxi_camera::native_camera {

inline constexpr std::uint64_t kViewAaFlag = std::uint64_t{1} << 31;
inline constexpr std::uint32_t kViewFlagClearOverride = observed_store_layout().view_flag_clear_override;
inline constexpr std::uint32_t kViewFlagSetOverride = observed_store_layout().view_flag_set_override;

struct ViewAaResult {
  bool complete = false;
  bool write_attempted = false;
  const char* error = "not_attempted";
};

// Only a freshly verified owned mode2 view with its gate closed, in the current
// validated engine observer. The caller validates the current code contract
// proving ToggleVpEffectAA's P+48 bit31 and the two global override locations.
// This clears that one per-view bit, preserving every other bit and P+56.
// No engine command, global write, allocation, protection change or AA SDK call.
// Rereads both flag words and overrides; any failure leaves the gate closed.
// A write_attempted result requires a fresh full owned-view inspection before
// activation, including on success. This helper cannot establish ownership or
// freeze engine lifetime independently of its caller's observer contract.
ViewAaResult disable_owned_view_aa(const engine_camera::OwnedViewSnapshot& view,
                                   discovery::ImageReader& image,
                                   const CameraImageLayout& layout = observed_store_layout()) noexcept;

}  // namespace taxi_camera::native_camera
