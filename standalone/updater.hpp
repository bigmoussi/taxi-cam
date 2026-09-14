#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace taxi_camera::standalone {
struct UpdateVersion {
  std::uint32_t major{}, minor{}, patch{}, build{};
};
bool parse_update_version(const std::wstring& tag, UpdateVersion& value);
bool newer_update(const UpdateVersion& candidate, const UpdateVersion& current);
bool simulator_blocks_update();
// Returns an open, verified handle denying file writes/replacement until closed.
HANDLE verified_update_file(const std::wstring& path, const std::wstring& sha256);
std::wstring update_installer_arguments(const std::wstring& directory, DWORD pid);
struct UpdateResult {
  bool available{}, manual{};
  std::wstring tag, sha256, installer, error;
};
class Updater {
 public:
  ~Updater();
  bool begin(const std::wstring& installation, bool manual);
  bool take(UpdateResult& result);
  void stop();
  bool busy() const { return busy_; }
  bool launch(const UpdateResult& result, const std::wstring& installation, std::wstring& error);

 private:
  std::atomic<bool> cancelled_{false}, busy_{false};
  std::thread worker_;
  std::mutex mutex_;
  UpdateResult result_;
  bool ready_{};
  std::wstring cache_;
};
}  // namespace taxi_camera::standalone
