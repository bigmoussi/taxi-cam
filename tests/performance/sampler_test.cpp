#define TAXI_PERFORMANCE_TESTING
#include "../../tools/performance/sampler.cpp"

namespace {
using namespace taxi_performance;
unsigned checks{};
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
struct Fixture {
  ipc::Mailbox writer;
  Fixture() {
    require(writer.open(GetCurrentProcessId(), true), "Create process-owned isolated IPC fixture");
    reset();
  }
  void reset() {
    require(writer.lock(1000), "Lock isolated fixture");
    auto& value = *writer.data();
    value = {};
    value.magic = ipc::ProtocolMagic;
    value.version = ipc::ProtocolVersion;
    value.bytes = sizeof(value);
    value.owner_pid = GetCurrentProcessId();
    value.owner_heartbeat = GetTickCount64();
    value.settings.profile = 4;
    value.settings.aircraft_session_epoch = 7;
    value.status.heartbeat = value.status.identity_sample_ms = GetTickCount64();
    value.status.graphics_ready = value.status.scene_ready = 1;
    value.status.active_profile = value.status.detected_profile = 4;
    value.status.aircraft_session_epoch = 7;
    std::strcpy(value.status.aircraft_type, "Airbus");
    std::strcpy(value.status.aircraft_path, "fixture-aircraft");
    writer.unlock();
  }
};
struct LockContext {
  HANDLE mutex{}, ready{}, release{};
  bool abandon{};
};
DWORD WINAPI hold_mutex(void* opaque) {
  const auto& context = *static_cast<LockContext*>(opaque);
  if (WaitForSingleObject(context.mutex, 5000) != WAIT_OBJECT_0)
    return 1;
  SetEvent(context.ready);
  if (context.abandon)
    return 0;
  if (WaitForSingleObject(context.release, 5000) != WAIT_OBJECT_0) {
    ReleaseMutex(context.mutex);
    return 2;
  }
  return ReleaseMutex(context.mutex) ? 0 : 3;
}
void ipc_checks() {
  Fixture fixture;
  ReadOnlyIpc reader(GetCurrentProcessId());
  const auto initial = *fixture.writer.data();
  const auto snapshot = reader.read();
  require(snapshot.valid() && snapshot.value.owner_pid == GetCurrentProcessId(), "Read coherent process-owned fixture");
  require(std::memcmp(&initial, fixture.writer.data(), sizeof(initial)) == 0, "Read-only sampling must not alter IPC contents");
  Options options;
  options.pid = GetCurrentProcessId();
  options.profile = 4;
  options.mask = 0;
  require(validate_capture(snapshot, options).empty(), "Fresh ready fixture accepted");
  auto changed = snapshot;
  changed.value.settings.manual_mask = 1;
  require(validate_capture(changed, options) == "off_foreground_render_demand_present", "Unassigned foreground rendering is not OFF");
  changed = snapshot;
  changed.value.settings.profile = changed.value.status.active_profile = changed.value.status.detected_profile = 1;
  options.profile = 1;
  require(validate_capture(changed, options) == "off_button_state_unready", "OFF requires a fresh followed button state");
  options.profile = 4;
  changed = snapshot;
  changed.value.status.taxi_mask = 1;
  require(validate_capture(changed, options) == "unexpected_taxi_mask", "Unexpected mask refused");
  changed = snapshot;
  changed.value.settings.aircraft_session_epoch = 8;
  require(validate_capture(changed, options) == "session_mismatch", "Changing session refused");
  changed = snapshot;
  changed.value.status.detected_profile = 0;
  require(validate_capture(changed, options) == "aircraft_identity_unready_or_changed", "Waiting aircraft identity refused");
  changed = snapshot;
  changed.value.settings.scene_test = 1;
  require(validate_capture(changed, options) == "diagnostic_mode_enabled", "Diagnostic mode is not a normal OFF/ON comparison");
  changed = snapshot;
  changed.value.status.scene_ready = 0;
  require(validate_capture(changed, options) == "scene_not_ready", "Unready native pair refused");
  fixture.writer.data()->owner_heartbeat = 0;
  require(reader.read().error == "ipc_stale_or_future", "Disconnected owner refused");
  fixture.reset();
  fixture.writer.data()->status.heartbeat = GetTickCount64() + 6000;
  require(reader.read().error == "ipc_stale_or_future", "Future heartbeat refused");
  fixture.reset();
  fixture.writer.data()->status.heartbeat = GetTickCount64() - 5001;
  require(reader.read().error == "ipc_stale_or_future", "Stale status refused");
  fixture.reset();
  fixture.writer.data()->bytes--;
  require(reader.read().error == "ipc_header_invalid", "Incompatible mapping layout refused");
  fixture.reset();
  fixture.writer.data()->settings.camera_rate = 4;
  require(reader.read().error == "ipc_settings_invalid", "Invalid settings refused");
  fixture.reset();
  std::memset(fixture.writer.data()->status.aircraft_path, 'x', sizeof(fixture.writer.data()->status.aircraft_path));
  require(reader.read().error == "ipc_status_invalid", "Unterminated strings refused");
  fixture.reset();
  fixture.writer.data()->status.stage_ms[2] = std::numeric_limits<double>::infinity();
  require(reader.read().error == "ipc_status_invalid", "Nonfinite metrics refused before JSON serialization");
  fixture.reset();
  wchar_t name[128]{};
  std::swprintf(name, 128, L"Local\\380TaxiCamera.Control.%lu", GetCurrentProcessId());
  Handle mutex(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name));
  Handle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr)), release(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  require(mutex && ready && release, "Create own mutex contention fixture");
  LockContext context{mutex.value, ready.value, release.value, false};
  {
    Handle worker(CreateThread(nullptr, 0, hold_mutex, &context, 0, nullptr));
    require(worker && WaitForSingleObject(ready.value, 5000) == WAIT_OBJECT_0, "Worker holds fixture mutex");
    const auto before = clock_ms();
    const auto busy = reader.read();
    require(busy.error == "ipc_busy" && !busy.value.owner_pid, "Busy read never reuses the last good snapshot");
    require(clock_ms() - before < 100, "Busy sampling must not wait for the producer");
    SetEvent(release.value);
    require(WaitForSingleObject(worker.value, 5000) == WAIT_OBJECT_0, "Release fixture writer");
  }
  require(reader.read().valid(), "Fresh read recovers after contention");
  ResetEvent(ready.value);
  context.abandon = true;
  const auto before_abandoned = *fixture.writer.data();
  {
    Handle worker(CreateThread(nullptr, 0, hold_mutex, &context, 0, nullptr));
    require(worker && WaitForSingleObject(worker.value, 5000) == WAIT_OBJECT_0, "Abandon own fixture mutex");
  }
  require(reader.read().error == "ipc_abandoned", "Abandoned writer invalidates its snapshot");
  require(std::memcmp(&before_abandoned, fixture.writer.data(), sizeof(before_abandoned)) == 0,
          "Abandoned-mutex handling never writes service/heartbeat fields");
  ReadOnlyIpc missing(0);
  require(missing.read().error == "ipc_unavailable", "Wrong PID never opens another mapping");
}
void focus_checks() {
  const auto first = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1));
  const auto second = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(2));
  Focus focus{first, 23};
  focus.observe(first, 23);
  require(!focus.changed, "Stable foreground accepted");
  focus.event(second, 42);
  focus.event(first, 23);
  require(focus.changed && focus.events == 2, "Focus departure and return between polls remains invalid");
  focus = {first, 23};
  focus.event(second, 23);
  focus.event(first, 23);
  require(focus.changed && focus.events == 2, "Changing and restoring foreground window within the simulator between polls stays invalid");
  focus = {first, 23};
  focus.observe(first, 42);
  require(focus.changed, "Initial background process invalidates capture");
}
void counter_checks() {
  ipc::Status previous{}, next{};
  previous.captures = previous.composed = previous.stamps = 20;
  next = previous;
  require(counter_issue(previous, next, 0).empty(), "Settled OFF accepts unchanged completion counts");
  next.captures++;
  require(counter_issue(previous, next, 0) == "off_background_or_unsettled_work",
          "OFF prewarm or delayed captures invalidate the baseline");
  require(counter_issue(previous, next, 1).empty(), "ON permits forward capture progress");
  next = previous;
  next.composed--;
  require(counter_issue(previous, next, 1) == "delivery_counter_reversal", "Counter reset invalidates comparison identity");
}
void cpu_and_refusal(const std::wstring& directory) {
  Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, GetCurrentProcessId()));
  require(static_cast<bool>(process), "Open own counters read-only");
  const auto first = process_cpu(process.value), last = process_cpu(process.value);
  require(first.created && first.created == last.created && last.user >= first.user && last.kernel >= first.kernel && last.at >= first.at,
          "Process counters preserve identity and monotonic raw units");
  unsigned failures = 0;
  const auto observed = threads(GetCurrentProcessId(), failures);
  require(std::any_of(observed.begin(), observed.end(),
                      [](const auto& thread) { return thread.id == GetCurrentThreadId() && thread.cpu.created; }),
          "Thread counters include current thread under its own PID");
  Options options;
  options.pid = GetCurrentProcessId();
  options.profile = 4;
  options.output = directory;
  options.phase = L"fixture-refusal";
  bool refused = false;
  try {
    capture(options);
  } catch (const std::exception& error) {
    refused = std::string(error.what()).find("not FlightSimulator2024.exe") != std::string::npos;
  }
  require(refused, "Production capture refuses a non-simulator PID before opening fixture IPC");
  require(!std::filesystem::exists(std::filesystem::path(directory + L"/samples.jsonl")), "Refused capture creates no misleading samples");
  const auto hash_file = directory + L"/hash-fixture.txt";
  {
    std::ofstream file(std::filesystem::path(hash_file), std::ios::binary | std::ios::trunc);
    file << "abc";
  }
  require(sha256(hash_file) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 matches known fixture bytes");
  Fixture fixture;
  std::ofstream json_file(std::filesystem::path(directory + L"/ipc-fixture.json"), std::ios::binary | std::ios::trunc);
  json_file << "{\"settings\":" << settings_json(fixture.writer.data()->settings)
            << ",\"status\":" << status_json(fixture.writer.data()->status) << '}';
  require(json(std::string("quote\"\n\\")) == "\"quote\\\"\\u000a\\\\\"", "JSON escaping preserves control and path characters");
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 2, "Pass isolated output directory");
    ipc_checks();
    focus_checks();
    counter_checks();
    cpu_and_refusal(argv[1]);
    std::printf(
        "PASS: %u performance sampler checks; own fixture IPC, stale/busy/abandoned rejection, focus invalidation, CPU identity and "
        "wrong-PID refusal.\n",
        checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
