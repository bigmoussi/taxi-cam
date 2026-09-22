#pragma once
#include <array>
#include <cstdint>
#include "../profiles/catalog.hpp"

namespace taxi_camera::standalone {
// Per-side minimum PLEASE WAIT time. The clock starts when a side is first
// admitted for display writes and resets when that side stops drawing, so each
// OFF to TAXI change shows the page for at least WaitingPageMinimumMs.
class WaitingPageTimer {
 public:
  unsigned observe(std::uint64_t now, unsigned active_mask) noexcept {
    unsigned waiting = 0;
    for (unsigned side = 0; side < since_.size(); ++side) {
      const unsigned bit = 1u << side;
      if (!(active_mask & bit)) {
        since_[side] = 0;
        continue;
      }
      if (!since_[side])
        since_[side] = now ? now : 1;
      if (now < since_[side] || now - since_[side] < profiles::WaitingPageMinimumMs)
        waiting |= bit;
    }
    return waiting;
  }

 private:
  std::array<std::uint64_t, MaxDisplaySides> since_{};
};
}  // namespace taxi_camera::standalone
