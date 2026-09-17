#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>

namespace taxi_camera::standalone {
// Companion-only attach preference. Not part of the bridge IPC Settings layout.
inline constexpr UINT kDefaultAutoConnect = 1;

inline bool load_auto_connect(const std::wstring& directory) {
  if (directory.empty())
    return kDefaultAutoConnect != 0;
  const auto path = directory + L"\\settings.ini";
  const UINT value = GetPrivateProfileIntW(L"companion", L"auto_connect", kDefaultAutoConnect, path.c_str());
  return value != 0;
}

inline bool save_auto_connect(const std::wstring& directory, bool enabled) {
  if (directory.empty())
    return false;
  const auto path = directory + L"\\settings.ini";
  return WritePrivateProfileStringW(L"companion", L"auto_connect", enabled ? L"1" : L"0", path.c_str()) != FALSE;
}

enum class ConnectCommand : std::uint32_t { none = 0, connect = 1, reset = 2 };

class ConnectCommandQueue {
 public:
  void request(ConnectCommand command) noexcept {
    if (command == ConnectCommand::none)
      return;
    // Reset supersedes a pending Connect; a later Connect does not clear Reset.
    const auto previous = pending_.load(std::memory_order_relaxed);
    if (previous == static_cast<std::uint32_t>(ConnectCommand::reset) && command == ConnectCommand::connect)
      return;
    pending_.store(static_cast<std::uint32_t>(command), std::memory_order_release);
  }
  ConnectCommand take() noexcept {
    return static_cast<ConnectCommand>(pending_.exchange(static_cast<std::uint32_t>(ConnectCommand::none), std::memory_order_acq_rel));
  }
  bool pending() const noexcept { return pending_.load(std::memory_order_acquire) != 0; }

 private:
  std::atomic<std::uint32_t> pending_{};
};

// Decide whether the worker should attempt bridge load/start this tick.
// manual_session_armed latches an explicit Connect/Reset across scheduled
// preflight retries until success, exhaustion, or a new simulator session.
inline bool should_attempt_connect(bool auto_connect,
                                   bool already_attempted,
                                   ConnectCommand command,
                                   bool manual_session_armed = false) noexcept {
  if (command == ConnectCommand::reset || command == ConnectCommand::connect)
    return true;
  if (already_attempted)
    return false;
  return auto_connect || manual_session_armed;
}

// After stale-heartbeat recovery, ignore the same mapped beat until a newer one arrives.
inline bool heartbeat_confirms_bridge(std::uint64_t sample_heartbeat, std::uint64_t ignore_through) noexcept {
  return sample_heartbeat && sample_heartbeat > ignore_through;
}

inline bool fresh_load_allowed(bool load_started_this_session) noexcept { return !load_started_this_session; }
}  // namespace taxi_camera::standalone
