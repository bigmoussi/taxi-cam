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
//
// Neither counter sees the GPU. When a simulator queue is held on one of our
// timeline Waits, the simulator keeps recording and submitting, so both keep
// advancing while the image is frozen (Frame Generation switched on in flight,
// issue 69: more than eight minutes without a trip). bridge_wait_stalled
// reports that case directly and trips on its own. It recovers like any trip;
// a second one in the same armed period latches the cameras off.
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
    bool bridge_wait_stalled = false;  // A simulator queue is held on our Wait (QueuedWaitStall).
  };
  struct Decision {
    bool trip = false;
    bool recover = false;
    // SIM_FRAME stopped while presentation continued: pause or menu, logged once.
    bool telemetry_stall_noted = false;
    // Hooked lists and queues went quiet while SIM_FRAME continued: flight
    // reload or a renderer path we do not hook, logged once, never a trip.
    bool presentation_stall_noted = false;
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
      telemetry_noted_ = presentation_noted_ = false;
      wait_trips_ = 0;
      latched_ = false;
      return decision;
    }
    const bool pulse_advanced = sample.pulse_available && (pulse_since_ == 0 || sample.frame_pulse != last_pulse_);
    if (pulse_advanced || !sample.pulse_available) {
      last_pulse_ = sample.frame_pulse;
      pulse_since_ = sample.now_ms;
      presentation_noted_ = false;
    }
    const bool sim_advanced = sim_since_ == 0 || sample.sim_frames != last_sim_;
    if (sim_advanced) {
      last_sim_ = sample.sim_frames;
      sim_since_ = sample.now_ms;
      telemetry_noted_ = false;
    }
    const auto pulse_stalled_ms = sample.pulse_available && sample.now_ms >= pulse_since_ ? sample.now_ms - pulse_since_ : 0;
    const auto sim_stalled_ms = sample.now_ms >= sim_since_ ? sample.now_ms - sim_since_ : 0;
    const bool pulse_stalled = sample.pulse_available && pulse_stalled_ms >= StallMs;
    // SIM_FRAME advancing proves the simulator is alive even when our hooked
    // lists and queues see nothing (flight reload). A soft freeze stops both
    // within half a second (issue 53), so requiring both loses no real trip.
    // Telemetry that has never flowed cannot veto a presentation stall.
    const bool sim_known = sample.sim_frames != 0;
    const bool sim_stalled = sim_stalled_ms >= StallMs;
    const bool telemetry_stalled = sample.sim_frames_expected && sim_stalled;
    if (tripped_) {
      // Re-arming after a second held Wait would freeze the simulator again.
      if (latched_)
        return decision;
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
    if (sample.bridge_wait_stalled) {
      tripped_ = true;
      ++trips_;
      latched_ = ++wait_trips_ >= 2;
      recover_since_ = 0;
      decision.trip = true;
      decision.reason = latched_ ? "bridge_queue_wait_stalled_latched" : "bridge_queue_wait_stalled";
      decision.stalled_ms = StallMs;
      return decision;
    }
    if ((pulse_stalled && (sim_stalled || !sim_known)) || (!sample.pulse_available && telemetry_stalled)) {
      tripped_ = true;
      ++trips_;
      recover_since_ = 0;
      decision.trip = true;
      decision.reason = pulse_stalled ? (sim_known ? "presentation_and_sim_frame_stalled" : "presentation_stalled_no_telemetry")
                                      : "sim_frame_stalled_before_first_submit";
      decision.stalled_ms = pulse_stalled ? pulse_stalled_ms : sim_stalled_ms;
      return decision;
    }
    if (pulse_stalled && !presentation_noted_) {
      presentation_noted_ = true;
      decision.presentation_stall_noted = true;
      decision.reason = "presentation_stalled_sim_frame_alive";
      decision.stalled_ms = pulse_stalled_ms;
    }
    if (telemetry_stalled && !telemetry_noted_) {
      telemetry_noted_ = true;
      decision.telemetry_stall_noted = true;
      decision.reason = "sim_frame_stalled_presentation_alive";
      decision.stalled_ms = sim_stalled_ms;
    }
    return decision;
  }

  bool tripped() const noexcept { return tripped_; }
  bool latched() const noexcept { return latched_; }
  unsigned trips() const noexcept { return trips_; }

 private:
  std::uint64_t last_pulse_ = 0, pulse_since_ = 0;
  std::uint64_t last_sim_ = 0, sim_since_ = 0;
  std::uint64_t recover_since_ = 0;
  unsigned trips_ = 0;
  unsigned wait_trips_ = 0;
  bool tripped_ = false;
  bool latched_ = false;
  bool telemetry_noted_ = false;
  bool presentation_noted_ = false;
};

}  // namespace taxi_camera
