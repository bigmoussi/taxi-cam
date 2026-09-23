#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace taxi_camera {

// Events a pilot should learn about inside the simulator. The text is sent
// through the bridge's existing SimConnect worker (never from a render thread)
// when the companion setting "Show messages in simulator" is on.
enum class SimEvent : unsigned {
  bridge_connected,
  cameras_ready,
  connection_stopped,
  simulator_unsupported,
  presentation_stalled,  // Watchdog trip, posted by the watchdog thread itself.
  cameras_disarmed,      // Degraded path applied by the bridge worker.
  presentation_resumed,
  speed_cutoff,
  aircraft_mismatch,
  camera_startup_failed,
  capture_paused,
  hook_storm,  // Registration failures exceeded the safe rate; disarmed for the session.
  count
};

struct SimMessage {
  const char* text = "";
  float seconds = 6.0f;
  bool alert = false;  // Coloured PRINT fallback and longer display.
};

inline constexpr SimMessage sim_message_for(SimEvent event) noexcept {
  switch (event) {
    case SimEvent::bridge_connected:
      return {"Taxi Cam connected.", 5.0f, false};
    case SimEvent::cameras_ready:
      return {"Taxi Cam: cameras ready.", 5.0f, false};
    case SimEvent::connection_stopped:
      return {"Taxi Cam disconnected; camera output closed.", 6.0f, false};
    case SimEvent::simulator_unsupported:
      return {"Taxi Cam: this simulator build is not supported yet; cameras stay off.", 10.0f, true};
    case SimEvent::presentation_stalled:
      return {"Taxi Cam: simulator frames stalled; releasing Taxi Cam hooks so the simulator can continue.", 12.0f, true};
    case SimEvent::cameras_disarmed:
      return {"Taxi Cam degraded: cameras disarmed and hooks released; they re-arm when frames resume.", 12.0f, true};
    case SimEvent::presentation_resumed:
      return {"Taxi Cam: simulator frames resumed; cameras re-armed.", 6.0f, false};
    case SimEvent::speed_cutoff:
      return {"Taxi Cam: above 60 knots, taxi cameras off.", 5.0f, false};
    case SimEvent::aircraft_mismatch:
      return {"Taxi Cam: the loaded aircraft does not match the selected profile.", 8.0f, false};
    case SimEvent::camera_startup_failed:
      return {"Taxi Cam: camera startup stopped; see the companion Diagnostics.", 8.0f, true};
    case SimEvent::capture_paused:
      return {"Taxi Cam: camera capture paused; waiting for verified GPU state.", 6.0f, false};
    case SimEvent::hook_storm:
      return {"Taxi Cam: native hook failures exceeded the safe rate; cameras disarmed for this session.", 12.0f, true};
    default:
      return {};
  }
}

inline constexpr const char* sim_event_name(SimEvent event) noexcept {
  constexpr const char* names[]{"bridge_connected",     "cameras_ready",         "connection_stopped",   "simulator_unsupported",
                                "presentation_stalled", "cameras_disarmed",      "presentation_resumed", "speed_cutoff",
                                "aircraft_mismatch",    "camera_startup_failed", "capture_paused",       "hook_storm"};
  return static_cast<unsigned>(event) < static_cast<unsigned>(SimEvent::count) ? names[static_cast<unsigned>(event)] : "invalid_event";
}

// Per-event repeat interval plus a global budget so a flapping state cannot
// spam the simulator. Watchdog events keep a shorter interval: each trip matters.
class SimMessageLimiter {
 public:
  static constexpr std::uint64_t WindowMs = 20000;
  static constexpr unsigned WindowBudget = 4;
  static constexpr std::uint64_t interval_ms(SimEvent event) noexcept {
    switch (event) {
      case SimEvent::presentation_stalled:
      case SimEvent::cameras_disarmed:
      case SimEvent::presentation_resumed:
        return 15000;
      case SimEvent::speed_cutoff:
      case SimEvent::aircraft_mismatch:
      case SimEvent::capture_paused:
        return 60000;
      default:
        return 30000;
    }
  }
  bool admit(SimEvent event, std::uint64_t now_ms) noexcept {
    const auto index = static_cast<unsigned>(event);
    if (index >= static_cast<unsigned>(SimEvent::count))
      return false;
    if (now_ms < window_start_ || now_ms - window_start_ >= WindowMs) {
      window_start_ = now_ms;
      window_count_ = 0;
    }
    auto& last = last_ms_[index];
    if (last && now_ms >= last && now_ms - last < interval_ms(event)) {
      ++suppressed_;
      return false;
    }
    if (window_count_ >= WindowBudget) {
      ++suppressed_;
      return false;
    }
    last = now_ms ? now_ms : 1;
    ++window_count_;
    ++admitted_;
    return true;
  }
  std::uint64_t admitted() const noexcept { return admitted_; }
  std::uint64_t suppressed() const noexcept { return suppressed_; }

 private:
  std::array<std::uint64_t, static_cast<unsigned>(SimEvent::count)> last_ms_{};
  std::uint64_t window_start_ = 0;
  unsigned window_count_ = 0;
  std::uint64_t admitted_ = 0, suppressed_ = 0;
};

// Edge detection from bridge state to events. Level inputs only raise their
// event when they become true; a new connection resets the per-connection ones.
struct SimEventInputs {
  bool connected = false;
  bool cameras_ready = false;
  bool connection_stopped = false;  // Pulse from the connection session.
  bool simulator_unsupported = false;
  bool degraded = false;  // Watchdog gate closed.
  bool speed_cutoff = false;
  bool aircraft_mismatch = false;
  bool camera_startup_failed = false;
  bool capture_paused = false;
  bool hook_storm = false;
};
class SimEventTracker {
 public:
  // Writes up to count events; returns how many were written.
  std::size_t observe(const SimEventInputs& in, SimEvent* events, std::size_t capacity) noexcept {
    std::size_t count = 0;
    const auto emit = [&](SimEvent event) {
      if (count < capacity)
        events[count++] = event;
    };
    if (in.connection_stopped)
      emit(SimEvent::connection_stopped);
    if (in.connected && !previous_.connected)
      emit(SimEvent::bridge_connected);
    // Independent of the connection, announced once for the process.
    if (in.hook_storm && !storm_announced_) {
      storm_announced_ = true;
      emit(SimEvent::hook_storm);
    }
    // The degraded gate is independent of the connection: it is applied and
    // lifted by the watchdog and must be announced exactly once per transition.
    if (in.degraded && !degraded_announced_) {
      degraded_announced_ = true;
      emit(SimEvent::cameras_disarmed);
    }
    if (!in.degraded && degraded_announced_) {
      degraded_announced_ = false;
      emit(SimEvent::presentation_resumed);
    }
    if (!in.connected) {
      // Per-connection notices repeat on the next connection.
      previous_ = {};
      previous_.degraded = in.degraded;
      return count;
    }
    if (in.cameras_ready && !previous_.cameras_ready)
      emit(SimEvent::cameras_ready);
    if (in.simulator_unsupported && !previous_.simulator_unsupported)
      emit(SimEvent::simulator_unsupported);
    if (in.speed_cutoff && !previous_.speed_cutoff)
      emit(SimEvent::speed_cutoff);
    if (in.aircraft_mismatch && !previous_.aircraft_mismatch)
      emit(SimEvent::aircraft_mismatch);
    if (in.camera_startup_failed && !previous_.camera_startup_failed)
      emit(SimEvent::camera_startup_failed);
    if (in.capture_paused && !previous_.capture_paused)
      emit(SimEvent::capture_paused);
    previous_ = in;
    return count;
  }

 private:
  SimEventInputs previous_{};
  bool degraded_announced_ = false;
  bool storm_announced_ = false;
};

}  // namespace taxi_camera
