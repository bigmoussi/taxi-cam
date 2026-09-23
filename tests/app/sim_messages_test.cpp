#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "../../src/shared/sim_messages.hpp"

namespace {
using taxi_camera::SimEvent;
using taxi_camera::SimEventInputs;
using taxi_camera::SimEventTracker;
using taxi_camera::SimMessageLimiter;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

void mapping() {
  for (unsigned i = 0; i < static_cast<unsigned>(SimEvent::count); ++i) {
    const auto event = static_cast<SimEvent>(i);
    const auto message = taxi_camera::sim_message_for(event);
    require(message.text && std::strlen(message.text) >= 12 && std::strlen(message.text) < 200, "Event without display text");
    require(std::strncmp(message.text, "Taxi Cam", 8) == 0, "Message does not identify Taxi Cam");
    require(message.seconds >= 3 && message.seconds <= 15, "Display duration out of range");
    require(std::strcmp(taxi_camera::sim_event_name(event), "invalid_event") != 0, "Event without a log name");
  }
  require(taxi_camera::sim_message_for(SimEvent::count).text[0] == 0, "Invalid event produced text");
  require(taxi_camera::sim_message_for(SimEvent::presentation_stalled).alert &&
              taxi_camera::sim_message_for(SimEvent::cameras_disarmed).alert &&
              taxi_camera::sim_message_for(SimEvent::simulator_unsupported).alert,
          "Hang, degraded and unsupported notices are not alerts");
  require(!taxi_camera::sim_message_for(SimEvent::bridge_connected).alert, "Routine connect notice is an alert");
  require(std::strstr(taxi_camera::sim_message_for(SimEvent::cameras_disarmed).text, "disarmed") &&
              std::strstr(taxi_camera::sim_message_for(SimEvent::cameras_disarmed).text, "hooks"),
          "Degraded notice does not say what happened");
}

void limiter() {
  SimMessageLimiter limit;
  std::uint64_t now = 5000;
  require(limit.admit(SimEvent::bridge_connected, now), "First notice refused");
  require(!limit.admit(SimEvent::bridge_connected, now + 1000), "Repeated notice admitted inside its interval");
  require(limit.admit(SimEvent::cameras_ready, now + 1000), "Different event blocked by another event's interval");
  require(limit.admit(SimEvent::bridge_connected, now + SimMessageLimiter::interval_ms(SimEvent::bridge_connected)),
          "Notice refused after its interval");
  // Flapping: connect/stop every second must be capped by the window budget.
  SimMessageLimiter flap;
  unsigned admitted = 0;
  for (unsigned second = 0; second < 20; ++second) {
    admitted += flap.admit(second % 2 ? SimEvent::connection_stopped : SimEvent::bridge_connected, 100000 + second * 1000);
    admitted += flap.admit(SimEvent::cameras_ready, 100000 + second * 1000);
    admitted += flap.admit(SimEvent::capture_paused, 100000 + second * 1000);
  }
  require(admitted <= SimMessageLimiter::WindowBudget, "Flapping state exceeded the window budget");
  require(flap.suppressed() > 0 && flap.admitted() == admitted, "Limiter statistics disagree with its decisions");
  // The next window admits again.
  require(flap.admit(SimEvent::speed_cutoff, 100000 + SimMessageLimiter::WindowMs + 61000), "Fresh window refused a notice");
  // Watchdog events keep a shorter interval than routine ones and are never starved by them.
  SimMessageLimiter dog;
  require(dog.admit(SimEvent::presentation_stalled, 1000) && !dog.admit(SimEvent::presentation_stalled, 5000) &&
              dog.admit(SimEvent::presentation_stalled, 1000 + SimMessageLimiter::interval_ms(SimEvent::presentation_stalled)),
          "Watchdog notice interval not enforced");
  require(SimMessageLimiter::interval_ms(SimEvent::presentation_stalled) < SimMessageLimiter::interval_ms(SimEvent::capture_paused),
          "Watchdog notices are rate-limited harder than routine ones");
  require(!dog.admit(SimEvent::count, 1), "Invalid event admitted");
}

std::array<SimEvent, 8> events{};
std::size_t observe(SimEventTracker& tracker, const SimEventInputs& in) {
  events = {};
  return tracker.observe(in, events.data(), events.size());
}
bool has(std::size_t count, SimEvent event) {
  for (std::size_t i = 0; i < count; ++i)
    if (events[i] == event)
      return true;
  return false;
}

void tracker() {
  SimEventTracker track;
  SimEventInputs in;
  require(observe(track, in) == 0, "Idle bridge produced events");
  in.connected = true;
  auto count = observe(track, in);
  require(count == 1 && has(count, SimEvent::bridge_connected), "Connect edge not announced once");
  require(observe(track, in) == 0, "Steady connection repeated its notice");
  in.cameras_ready = true;
  in.speed_cutoff = true;
  count = observe(track, in);
  require(count == 2 && has(count, SimEvent::cameras_ready) && has(count, SimEvent::speed_cutoff), "Level edges not announced");
  require(observe(track, in) == 0, "Level inputs repeated their notices");
  in.speed_cutoff = false;
  require(observe(track, in) == 0, "Clearing a level produced a notice");
  in.speed_cutoff = true;
  count = observe(track, in);
  require(count == 1 && has(count, SimEvent::speed_cutoff), "Re-asserted level not announced");
  // Watchdog degraded path: announced once while connected, resumed once when lifted.
  in.degraded = true;
  count = observe(track, in);
  require(count == 1 && has(count, SimEvent::cameras_disarmed), "Degraded edge not announced");
  require(observe(track, in) == 0, "Degraded level repeated");
  in.degraded = false;
  count = observe(track, in);
  require(count == 1 && has(count, SimEvent::presentation_resumed), "Recovery not announced");
  // Disconnect: stop pulse announced; per-connection notices repeat on reconnect.
  in.connected = false;
  in.connection_stopped = true;
  count = observe(track, in);
  require(count == 1 && has(count, SimEvent::connection_stopped), "Connection stop not announced");
  in.connection_stopped = false;
  require(observe(track, in) == 0, "Disconnected idle produced events");
  // Degraded while disconnected still announces: the gate is independent of the companion.
  in.degraded = true;
  count = observe(track, in);
  require(count == 1 && has(count, SimEvent::cameras_disarmed), "Degraded while disconnected not announced");
  in.degraded = false;
  count = observe(track, in);
  require(count == 1 && has(count, SimEvent::presentation_resumed), "Recovery while disconnected not announced");
  in.connected = true;
  count = observe(track, in);
  require(count == 3 && has(count, SimEvent::bridge_connected) && has(count, SimEvent::cameras_ready) && has(count, SimEvent::speed_cutoff),
          "Reconnect did not repeat per-connection notices");
  in.simulator_unsupported = true;
  in.camera_startup_failed = true;
  in.aircraft_mismatch = true;
  in.capture_paused = true;
  count = observe(track, in);
  require(count == 4 && has(count, SimEvent::simulator_unsupported) && has(count, SimEvent::camera_startup_failed) &&
              has(count, SimEvent::aircraft_mismatch) && has(count, SimEvent::capture_paused),
          "Fault levels not announced");
  // A failure storm is announced once for the process, even across reconnects.
  in.hook_storm = true;
  count = observe(track, in);
  require(count == 1 && has(count, SimEvent::hook_storm), "Hook storm not announced");
  require(observe(track, in) == 0, "Hook storm repeated");
  in.connected = false;
  observe(track, in);
  in.connected = true;
  count = observe(track, in);
  require(!has(count, SimEvent::hook_storm) && has(count, SimEvent::bridge_connected), "Hook storm repeated after reconnect");
  // Bounded output: a small buffer truncates instead of overflowing.
  SimEventTracker small;
  SimEventInputs burst;
  burst.connected = burst.cameras_ready = burst.speed_cutoff = burst.aircraft_mismatch = true;
  std::array<SimEvent, 2> two{};
  require(small.observe(burst, two.data(), two.size()) == 2, "Tracker overflowed its output buffer");
}
}  // namespace

int main() {
  try {
    mapping();
    limiter();
    tracker();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "sim messages: %s\n", error.what());
    return 1;
  }
  std::printf("sim messages: %u checks passed\n", checks);
  return 0;
}
