#pragma once

#include <cstdint>

namespace taxi_camera {

// Presentation watchdog logic. The host thread samples lock-free counters and
// applies the decision: on trip it closes the graphics gate and disarms the
// cameras; on recovery it re-arms. The class itself never waits or locks.
//
// frame_pulse counts hooked ExecuteCommandLists and Close calls on simulator
// threads; it advances whenever the simulator presents, including in menus.
// sim_frames counts SIMCONNECT_PERIOD_SIM_FRAME telemetry packets; it stops in
// menus and while paused, so on its own it can only substitute for the pulse
// while no direct queue has been observed yet.
class FreezeWatchdog {
 public:
  static constexpr std::uint64_t StallMs = 3000;
  static constexpr std::uint64_t RecoveryMs = 5000;
  struct Sample {
    std::uint64_t now_ms = 0;
    std::uint64_t frame_pulse = 0;
    bool pulse_available = false;  // A direct queue or Close has been observed.
    std::uint64_t sim_frames = 0;
    bool sim_frames_expected = false;  // Flight session ready: SIM_FRAME should flow.
    bool armed = false;                // Bridge connected with hooks installed.
    bool worker_alive = false;         // Bridge worker heartbeat is recent.
  };
  struct Decision {
    bool trip = false;
    bool recover = false;
    // SIM_FRAME stopped while presentation continued: pause or menu, logged once.
    bool telemetry_stall_noted = false;
    bool worker_alive = false;
    const char* reason = "";
    std::uint64_t stalled_ms = 0;
  };

  Decision observe(const Sample& sample) noexcept {
    Decision decision;
    decision.worker_alive = sample.worker_alive;
    if (!sample.armed) {
      // Nothing to guard; forget stall timers so re-arming starts a fresh window.
      pulse_since_ = sim_since_ = 0;
      recover_since_ = 0;
      telemetry_noted_ = false;
      return decision;
    }
    const bool pulse_advanced = sample.pulse_available && (pulse_since_ == 0 || sample.frame_pulse != last_pulse_);
    if (pulse_advanced || !sample.pulse_available) {
      last_pulse_ = sample.frame_pulse;
      pulse_since_ = sample.now_ms;
    }
    const bool sim_advanced = sim_since_ == 0 || sample.sim_frames != last_sim_;
    if (sim_advanced) {
      last_sim_ = sample.sim_frames;
      sim_since_ = sample.now_ms;
      telemetry_noted_ = false;
    }
    const auto pulse_stalled_ms = sample.pulse_available && sample.now_ms >= pulse_since_ ? sample.now_ms - pulse_since_ : 0;
    const auto sim_stalled_ms = sample.sim_frames_expected && sample.now_ms >= sim_since_ ? sample.now_ms - sim_since_ : 0;
    const bool pulse_stalled = sample.pulse_available && pulse_stalled_ms >= StallMs;
    const bool sim_stalled = sample.sim_frames_expected && sim_stalled_ms >= StallMs;
    if (tripped_) {
      const bool progressing = sample.pulse_available ? pulse_advanced : sim_advanced;
      if (!progressing) {
        recover_since_ = 0;
        return decision;
      }
      if (!recover_since_)
        recover_since_ = sample.now_ms;
      else if (sample.now_ms - recover_since_ >= RecoveryMs) {
        tripped_ = false;
        recover_since_ = 0;
        decision.recover = true;
        decision.reason = "presentation_resumed";
      }
      return decision;
    }
    if (pulse_stalled || (!sample.pulse_available && sim_stalled)) {
      tripped_ = true;
      ++trips_;
      recover_since_ = 0;
      decision.trip = true;
      decision.reason = pulse_stalled ? (sim_stalled ? "presentation_and_sim_frame_stalled" : "presentation_stalled")
                                      : "sim_frame_stalled_before_first_submit";
      decision.stalled_ms = pulse_stalled ? pulse_stalled_ms : sim_stalled_ms;
      return decision;
    }
    if (sim_stalled && !telemetry_noted_) {
      telemetry_noted_ = true;
      decision.telemetry_stall_noted = true;
      decision.reason = "sim_frame_stalled_presentation_alive";
      decision.stalled_ms = sim_stalled_ms;
    }
    return decision;
  }

  bool tripped() const noexcept { return tripped_; }
  unsigned trips() const noexcept { return trips_; }

 private:
  std::uint64_t last_pulse_ = 0, pulse_since_ = 0;
  std::uint64_t last_sim_ = 0, sim_since_ = 0;
  std::uint64_t recover_since_ = 0;
  unsigned trips_ = 0;
  bool tripped_ = false;
  bool telemetry_noted_ = false;
};

}  // namespace taxi_camera
