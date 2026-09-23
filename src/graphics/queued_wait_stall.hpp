#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace taxi_camera {

// Tracks the GPU progress of the timeline values this bridge made simulator
// queues Wait on. A value that stays incomplete while its fence does not
// advance means the simulator's queue is held on our Wait: the image freezes
// even though the simulator keeps recording and submitting, so the hooked
// presentation pulse and SIM_FRAME telemetry both stay alive. Seen when Frame
// Generation is switched on in flight (resolution change, issue 69).
//
// Watchdog thread only; the caller reads the fence values without any lock.
template <std::size_t Slots>
class QueuedWaitStall {
 public:
  static constexpr std::uint64_t DeviceRemoved = std::numeric_limits<std::uint64_t>::max();

  // One slot per published timeline, sampled every watchdog tick. Returns true
  // once this slot's pending Wait has made no GPU progress for stall_ms.
  bool observe(std::size_t slot,
               std::uint64_t waited,
               std::uint64_t released,
               std::uint64_t completed,
               std::uint64_t now_ms,
               std::uint64_t stall_ms) noexcept {
    if (slot >= Slots)
      return false;
    auto& progress = progress_[slot];
    // Nothing pending: no Wait yet, already satisfied, released from the CPU,
    // or the device is gone (its waits no longer hold anything).
    if (!waited || waited <= released || completed == DeviceRemoved || completed >= waited) {
      progress = {};
      return false;
    }
    if (!progress.since_ms || completed != progress.completed) {
      progress.completed = completed;
      progress.since_ms = now_ms;
      return false;
    }
    return now_ms >= progress.since_ms && now_ms - progress.since_ms >= stall_ms;
  }

  void forget() noexcept { progress_ = {}; }

 private:
  struct Progress {
    std::uint64_t completed = 0;
    std::uint64_t since_ms = 0;
  };
  std::array<Progress, Slots> progress_{};
};

}  // namespace taxi_camera
