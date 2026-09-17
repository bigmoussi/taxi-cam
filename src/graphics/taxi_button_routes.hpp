#pragma once

#include <array>
#include <cstdint>

namespace taxi_camera {

struct TaxiButtonIntentSnapshot {
  unsigned buttons = 0;
  bool held = false;
  bool timed_out = false;
};

// A missing response is not an OFF event. Hold the last accepted intent for a
// bounded interval after the first observed gap; pose freshness is enforced
// independently before any render gate opens. Fresh OFF is always immediate.
class TaxiButtonIntent {
 public:
  static constexpr std::uint64_t gap_grace_ms = 2000;
  void reset() noexcept { *this = {}; }
  TaxiButtonIntentSnapshot observe(std::uint64_t now, bool valid, bool left, bool right) noexcept {
    if (valid) {
      known_ = true;
      gap_ = false;
      expired_ = false;
      buttons_ = (left ? 1u : 0u) | (right ? 2u : 0u);
      return {buttons_, false, false};
    }
    if (!known_)
      return {};
    if (!gap_) {
      gap_ = true;
      gap_started_ = now;
    }
    if (expired_ || now < gap_started_ || now - gap_started_ >= gap_grace_ms) {
      expired_ = true;
      return {0, false, true};
    }
    return {buttons_, true, false};
  }

 private:
  bool known_ = false;
  bool gap_ = false;
  bool expired_ = false;
  unsigned buttons_ = 0;
  std::uint64_t gap_started_ = 0;
};

// Session-only resource identities. The caller serializes access and forgets
// destroyed resources; no simulator state or resource lifetime is owned here.
class TaxiButtonRoutes {
 public:
  std::array<std::uint64_t, 2> targets{};

  // Explicit device/profile session change only. Resource destruction uses forget().
  void reset() noexcept { *this = {}; }

  // The caller first validates supplied nonzero resource IDs. Zero requests
  // automatic detection for that side; both zero starts a fresh explicit request.
  bool select_explicit(const std::array<std::uint64_t, 2>& selection) noexcept {
    if (selection[0] && selection[0] == selection[1])
      return false;
    targets = selection;
    detected_targets_ = {};
    // Any nonzero manual ID anchors side order for the rest of the session,
    // even after that GPU identity is later destroyed. Both-Auto clears it.
    manual_session_ = selection[0] != 0 || selection[1] != 0;
    assigned_ = manual_session_;
    return true;
  }
  bool assign(unsigned side, std::uint64_t id) noexcept {
    if (side >= targets.size() || id == 0 || targets[1 - side] == id)
      return false;
    targets[side] = id;
    detected_targets_[side] = 0;
    manual_session_ = true;
    assigned_ = true;
    return true;
  }

  // Detector ordering is only an initial-session hint. Once a side is known,
  // retain every surviving identity through replacement. When both auto-owned
  // identities are destroyed, a freshly confirmed pair may be adopted again;
  // explicit manual anchors still require both-Auto or a profile reload.
  bool adopt_detected(const std::array<std::uint64_t, 2>& detected, bool exact_names = false) noexcept {
    if (!detected[0] || !detected[1] || detected[0] == detected[1])
      return false;
    if (exact_names || (!assigned_ && !targets[0] && !targets[1])) {
      targets = detected;
      detected_targets_ = exact_names ? std::array<std::uint64_t, 2>{} : detected;
      if (exact_names)
        manual_session_ = true;
      assigned_ = true;
      return true;
    }
    if (targets[0] && targets[1])
      return (targets == detected) || (targets[0] == detected[1] && targets[1] == detected[0]);
    if (!targets[0] && !targets[1])
      return false;
    const unsigned survivor = targets[0] ? 0u : 1u;
    if (targets[survivor] != detected[0] && targets[survivor] != detected[1])
      return false;
    targets[1 - survivor] = detected[0] == targets[survivor] ? detected[1] : detected[0];
    detected_targets_[1 - survivor] = targets[1 - survivor];
    assigned_ = true;
    return true;
  }

  void forget(std::uint64_t id) noexcept {
    for (unsigned side = 0; side < targets.size(); ++side)
      if (targets[side] == id) {
        targets[side] = 0;
        detected_targets_[side] = 0;
      }
    if (targets[0] || targets[1])
      assigned_ = true;
    else
      // Auto-only both-lost reopens discovery. A sticky manual session still
      // refuses until both-Auto or reset(), matching prior side-order guards.
      assigned_ = manual_session_;
  }

  // An allocation-rank policy must be withdrawn when its complete-group
  // evidence changes. Explicit live sides and sticky manual sessions survive;
  // auto-only both-lost reopens discovery on the next confirmed group.
  void forget_detected() noexcept {
    const auto previous = detected_targets_;
    for (auto id : previous)
      if (id)
        forget(id);
  }

  unsigned active_mask(bool valid, bool left, bool right) const noexcept {
    if (!valid)
      return 0;
    return (left && targets[0] != 0 ? 1u : 0u) | (right && targets[1] != 0 ? 2u : 0u);
  }

  bool matches(std::uint64_t id, unsigned mask) const noexcept {
    return id != 0 && (((mask & 1u) != 0 && targets[0] == id) || ((mask & 2u) != 0 && targets[1] == id));
  }

 private:
  bool assigned_ = false;
  // True after any nonzero select_explicit/assign or exact-name adopt. Sticky
  // across forget() so destroyed manual IDs cannot be replaced by a heuristic.
  bool manual_session_ = false;
  std::array<std::uint64_t, 2> detected_targets_{};
};

}  // namespace taxi_camera
