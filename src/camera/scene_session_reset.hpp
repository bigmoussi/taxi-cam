#pragma once

#include "entry_pair.hpp"

namespace taxi_camera::native_camera {
// Reset authorization is distinct from public aircraft identity. A telemetry
// sample can become valid again before old native camera ownership has drained.
// This policy owns no pointers; the observer still verifies each native erase.
class SceneSessionReset {
 public:
  void begin(std::uint32_t profile) noexcept {
    profile_ = profile;
    retired_ = false;
  }
  void observe_empty(engine_camera::EmptyPairCancel cancelled, const engine_camera::Snapshot& pair) noexcept {
    // An empty published snapshot alone is not sufficient: creation may be in
    // progress before PairController publishes its first owned ID.
    if (holding() && cancelled == engine_camera::EmptyPairCancel::cancelled && empty(pair))
      retired_ = true;
  }
  bool ready(bool public_ready, const engine_camera::Snapshot& pair) const noexcept {
    return holding() && retired_ && public_ready && empty(pair);
  }
  bool consume(bool public_ready, const engine_camera::Snapshot& pair) noexcept {
    if (!ready(public_ready, pair))
      return false;
    profile_ = 0;
    retired_ = false;
    return true;
  }
  bool holding() const noexcept { return profile_ != 0; }
  std::uint32_t profile() const noexcept { return profile_; }
  static bool empty(const engine_camera::Snapshot& pair) noexcept {
    return !pair.owner.valid() && !pair.owned_ids[0] && !pair.owned_ids[1] && !pair.request_pending && !pair.creation_pending;
  }

 private:
  std::uint32_t profile_{};
  bool retired_{};
};
}  // namespace taxi_camera::native_camera
