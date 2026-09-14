#include <cstdio>
#include <limits>
#include <stdexcept>
#include "launcher.hpp"
#include "protocol.hpp"
#include "settings_store.hpp"

namespace {
unsigned checks{};
void require(bool value, const char* label) {
  ++checks;
  if (!value)
    throw std::runtime_error(label);
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    using namespace taxi_camera::standalone;
    Settings settings;
    require(valid_settings(settings), "Default settings");
    for (const auto value : {14u, 61u, 0u, UINT32_MAX}) {
      auto bad = settings;
      bad.camera_rate = value;
      require(!valid_settings(bad), "Invalid rate admitted");
    }
    for (const auto value : {-17.f, 5.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
      auto bad = settings;
      bad.exposure = value;
      require(!valid_settings(bad), "Invalid exposure admitted");
    }
    auto bad = settings;
    bad.profile = 999;
    require(!valid_settings(bad), "Unknown aircraft admitted");
    bad = settings;
    bad.mounts[0][0] = 501;
    require(!valid_settings(bad), "Unbounded position admitted");
    bad = settings;
    bad.mounts[1][5] = 0;
    require(!valid_settings(bad), "Zero lens admitted");
    bad = settings;
    bad.calibration_budget = 0;
    require(!valid_settings(bad), "Invalid calibration budget admitted");
    Mailbox owner, reader;
    require(owner.open(GetCurrentProcessId(), true), "Create mailbox");
    require(reader.open(GetCurrentProcessId(), false), "Open mailbox");
    require(owner.lock(100), "Lock mailbox");
    owner.data()->settings.camera_rate = 60;
    owner.unlock();
    require(reader.lock(100), "Read mailbox lock");
    require(reader.data()->settings.camera_rate == 60, "Settings exchange");
    reader.unlock();
    require(owner.lock(100), "Mutate test version");
    owner.data()->version = 999;
    owner.unlock();
    Mailbox refused;
    require(!refused.open(GetCurrentProcessId(), false), "Incompatible mailbox accepted");
    owner.close();
    reader.close();
    require(find_simulator(L"C:\\not-a-simulator\\FlightSimulator2024.exe") == 0, "Wrong simulator path accepted");
    require(argc == 2, "Provide exact bridge DLL");
    std::wstring install = argv[1];
    install.resize(install.find_last_of(L"\\/"));
    settings_override = install + L"\\smoke-settings-" + std::to_wstring(GetCurrentProcessId());
    Settings saved = settings;
    saved.camera_rate = 60;
    saved.mounts[0][2] = 27.25;
    saved.exposure = -7.5f;
    saved.calibration_budget = 1024;
    saved.manual_mask = 3;
    saved.left_id = 999;
    saved.right_id = 888;
    saved.route_request = 12;
    require(save_settings(saved), "Atomic profile save");
    Settings loaded;
    require(load_settings(loaded, install), "Profile reload");
    require(loaded.camera_rate == 60 && loaded.mounts[0][2] == 27.25 && loaded.exposure == -7.5f && loaded.calibration_budget == 1024,
            "Camera, exposure, rate and calibration persistence");
    require(!loaded.manual_mask && !loaded.left_id && !loaded.right_id && !loaded.route_request,
            "Session texture IDs and manual tests must not persist");
    saved.camera_rate = 999;
    require(!save_settings(saved), "Reject invalid save");
    require(load_settings(loaded, install) && loaded.camera_rate == 60, "Invalid save must preserve previous settings");
    HMODULE dll = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    require(dll != nullptr, "Load exact standalone bridge DLL");
    auto start = reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(dll, "TaxiCameraStart"));
    require(start != nullptr, "Native bridge entry point");
    require(start(nullptr) == ERROR_BAD_ENVIRONMENT, "Private camera started in wrong host");
    require(start(nullptr) == ERROR_BAD_ENVIRONMENT, "Repeated wrong-host call admitted");
    require(FreeLibrary(dll) != FALSE, "Unused wrong-host bridge unload");
    std::printf("PASS native smoke: %u settings, IPC, startup path and exact-DLL wrong-host checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL native smoke: %s\n", error.what());
    return 1;
  }
}
