#include "../../src/camera/mount_config.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace nc = taxi_camera::native_camera;
namespace {
unsigned checks = 0;
SRWLOCK test_lock = SRWLOCK_INIT;
nc::MountPair latest = nc::default_mounts();
std::atomic<unsigned> requests{0};
void require(bool value, const char* label) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", label);
    std::exit(1);
  }
}
bool equal(const nc::MountPair& a, const nc::MountPair& b) {
  for (unsigned i = 0; i < a.size(); ++i)
    if (a[i].position_m != b[i].position_m || a[i].pitch_degrees != b[i].pitch_degrees || a[i].yaw_degrees != b[i].yaw_degrees ||
        a[i].fov_radians != b[i].fov_radians)
      return false;
  return true;
}
nc::MountPair current() {
  AcquireSRWLockShared(&test_lock);
  const auto result = latest;
  ReleaseSRWLockShared(&test_lock);
  return result;
}
constexpr const char* valid = "nose = 0,-1.75,26.950668984,-17.5,0,1.24\ntail = 0,18,-25,-32,0,1.02\n";
void parser_tests() {
  nc::MountPair output{};
  const char* error = nullptr;
  require(nc::parse_mount_config(valid, output, error) && error[0] == '\0', "Complete config parses");
  require(equal(output, nc::default_mounts()), "Config maps all twelve fields exactly");
  require(nc::parse_mount_config("\xef\xbb\xbf# units\r\n tail = 1,2,3,4,5,0.05\r\n\n nose=5e2,-5e2,0,89,-180,1.55\n", output, error),
          "BOM whitespace comments reversed fields exponents and limits");
  require(output[0].position_m[0] == 500 && output[0].pitch_degrees == 89 && output[1].yaw_degrees == 5, "Reordered fields preserved");
  const auto seed = output;
  for (const char* text :
       {"", "nose=0,0,0,0,0,1", "nose=0,0,0,0,0,1\nnose=0,0,0,0,0,1", "wing=0,0,0,0,0,1\ntail=0,0,0,0,0,1",
        "nose=0,0,0,0,0,1,2\ntail=0,0,0,0,0,1", "nose=0,0,0,0,0\ntail=0,0,0,0,0,1", "nose=0,0,0,0,,1\ntail=0,0,0,0,0,1",
        "nose=nan,0,0,0,0,1\ntail=0,0,0,0,0,1", "nose=0,0,0,0,0,inf\ntail=0,0,0,0,0,1", "nose=0,0,0,0,0,1e999\ntail=0,0,0,0,0,1",
        "nose=501,0,0,0,0,1\ntail=0,0,0,0,0,1", "nose=0,0,0,90,0,1\ntail=0,0,0,0,0,1", "nose=0,0,0,0,181,1\ntail=0,0,0,0,0,1",
        "nose=0,0,0,0,0,0.049\ntail=0,0,0,0,0,1", "nose=0,0,0,0,0,1.551\ntail=0,0,0,0,0,1", "nose=0,0,0,0,0,1junk\ntail=0,0,0,0,0,1",
        "nose=0,0,0,0,0,1\ntail=0,0,0,0,0,-1"}) {
    require(!nc::parse_mount_config(text, output, error), "Malformed or out-of-range config refused");
    require(equal(output, seed) && error[0], "Failure leaves entire prior pair unchanged");
  }
  for (const auto& text : {std::string(4097, ' '), std::string(valid) + std::string(1, '\0')}) {
    require(!nc::parse_mount_config(text, output, error) && equal(output, seed), "Size and NUL refusal");
  }
}
void write_config(const std::wstring& path, const std::string& contents) {
  // The mount worker may already hold a shared read on this path. Open with the
  // same share mask and retry brief sharing/lock conflicts so the test can
  // replace contents without discarding a readable two-camera config.
  HANDLE file = INVALID_HANDLE_VALUE;
  for (unsigned attempt = 0; attempt < 100 && file == INVALID_HANDLE_VALUE; ++attempt) {
    file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      const auto error = GetLastError();
      if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION && error != ERROR_ACCESS_DENIED)
        break;
      Sleep(10);
    }
  }
  require(file != INVALID_HANDLE_VALUE, "Open own test config");
  DWORD bytes = 0;
  require(WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &bytes, nullptr) && bytes == contents.size(),
          "Write own test config");
  require(CloseHandle(file), "Close own test config");
}
template <typename Predicate>
void wait_for(Predicate predicate, const char* label) {
  const auto deadline = GetTickCount64() + 3500;
  while (!predicate() && GetTickCount64() < deadline)
    Sleep(10);
  require(predicate(), label);
}
void worker_tests() {
  require(!nc::initialize_mount_config(nullptr) && !nc::initialize_mount_config(L"relative.dll"), "Relative/null paths refused");
  std::array<wchar_t, 1024> executable{};
  require(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())) != 0, "Own executable path");
  auto directory = std::wstring(executable.data());
  directory.resize(directory.find_last_of(L'\\') + 1);
  directory += L"mount-config-fixture-" + std::to_wstring(GetCurrentProcessId());
  require(CreateDirectoryW(directory.c_str(), nullptr), "Create own unique fixture directory");
  const auto addon = directory + L"\\fixture.addon64", file = directory + L"\\taxi-camera-mounts.cfg";
  require(nc::initialize_mount_config(addon.c_str()), "Start bounded config worker");
  wait_for([] { return nc::mount_config_status().checks > 0; }, "Initial absent-file check");
  require(requests == 0, "Absent config preserves defaults");
  write_config(file, valid);
  wait_for([] { return nc::mount_config_status().applications == 1; }, "Changed valid config applied");
  require(equal(current(), nc::default_mounts()), "Worker sends complete numeric pair");
  auto ui = current();
  ui[0].fov_radians = 1.3f;
  AcquireSRWLockExclusive(&test_lock);
  latest = ui;
  ReleaseSRWLockExclusive(&test_lock);
  const auto checked = nc::mount_config_status().checks;
  wait_for([&] { return nc::mount_config_status().checks >= checked + 2; }, "Unchanged file rechecked");
  require(equal(current(), ui) && requests == 1, "Unchanged file cannot overwrite UI edit");
  write_config(file, "nose=0,0,0,0,0,1\ntail=0,0,0,0,0,nan");
  wait_for([] { return std::string_view(nc::mount_config_status().state) == "invalid_config_current_mounts_retained"; },
           "Invalid file status");
  require(equal(current(), ui) && requests == 1, "Invalid tail cannot partially apply nose");
  write_config(file, valid);
  wait_for([] { return nc::mount_config_status().applications == 2; }, "Valid file replacement applies again");
  const auto began = GetTickCount64();
  require(nc::shutdown_mount_config(), "Event-based stop completes");
  require(GetTickCount64() - began < 1900 && !nc::mount_config_status().running, "Stop interrupts one-second wait");
  require(DeleteFileW(file.c_str()) && RemoveDirectoryW(directory.c_str()), "Remove only own exact fixture paths");
}
}  // namespace
namespace taxi_camera::native_camera {
// Link the production worker to a numeric mailbox fixture, never the engine.
bool request_scene_mounts(const MountPair& mounts) noexcept {
  // Accept a fully filled MountPair (legacy nose/tail files duplicate tail into
  // the third slot in parse_mount_config). Do not require a third file line.
  if (!valid_mounts(mounts, kMaxCameraFeeds))
    return false;
  AcquireSRWLockExclusive(&test_lock);
  latest = mounts;
  ReleaseSRWLockExclusive(&test_lock);
  ++requests;
  return true;
}
}  // namespace taxi_camera::native_camera
int main() {
  parser_tests();
  worker_tests();
  std::printf("PASS mount config: %u checks; own files and numeric mailbox only.\n", checks);
}
