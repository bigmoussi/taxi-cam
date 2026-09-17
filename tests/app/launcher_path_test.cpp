#include <cstdio>
#include <stdexcept>
#include "../../src/app/launcher.hpp"
#include "../../src/app/launcher_log.hpp"

namespace {
void require(bool value, const char* label) {
  if (!value)
    throw std::runtime_error(label);
}
struct Files {
  std::wstring original, alias, copy;
  ~Files() {
    for (const auto* path : {&alias, &copy, &original})
      if (!path->empty())
        DeleteFileW(path->c_str());
  }
};
struct ModuleFixture {
  unsigned snapshots = 0, closed = 0, pauses = 0, fail_snapshots = 0;
  DWORD failure = ERROR_BAD_LENGTH, last_error = 0;
  bool fail_after_entry = false;
  HANDLE snapshot(DWORD) {
    ++snapshots;
    if (snapshots <= fail_snapshots) {
      last_error = failure;
      return INVALID_HANDLE_VALUE;
    }
    return reinterpret_cast<HANDLE>(std::uintptr_t(123));
  }
  bool first(HANDLE, MODULEENTRY32W& entry) {
    require(entry.dwSize == sizeof(entry), "Module entry size initialized");
    std::wcscpy(entry.szModule, snapshots == 1 && fail_after_entry ? L"partial.dll" : L"complete.dll");
    std::wcscpy(entry.szExePath, L"C:\\fixture\\module.dll");
    entry.modBaseAddr = reinterpret_cast<BYTE*>(std::uintptr_t(0x10000));
    entry.modBaseSize = 4096;
    return true;
  }
  bool next(HANDLE, MODULEENTRY32W&) {
    last_error = fail_after_entry && snapshots == 1 ? failure : ERROR_NO_MORE_FILES;
    return false;
  }
  DWORD error() { return last_error; }
  void close(HANDLE) { ++closed; }
  void pause() { ++pauses; }
};
void module_checks() {
  using taxi_camera::standalone::module_inventory;
  ModuleFixture racing;
  racing.fail_snapshots = 2;
  auto result = module_inventory(1, racing);
  require(!result.error && result.entries.size() == 1 && result.attempts == 3 && racing.closed == 1 && racing.pauses == 2,
          "Transient loader races retry the snapshot and keep the successful inventory");
  ModuleFixture partial;
  partial.fail_after_entry = true;
  result = module_inventory(1, partial);
  require(!result.error && result.attempts == 2 && result.entries.size() == 1 && result.entries[0].name == L"complete.dll" &&
              partial.closed == 2,
          "Partial inventories are discarded before a whole-snapshot retry");
  ModuleFixture perpetual;
  perpetual.fail_snapshots = 100;
  result = module_inventory(1, perpetual);
  require(result.error == ERROR_BAD_LENGTH && result.entries.empty() && result.attempts == 32 && perpetual.pauses == 31,
          "Changing loader inventory cannot cause an unbounded startup wait");
  ModuleFixture denied;
  denied.fail_snapshots = 1;
  denied.failure = ERROR_ACCESS_DENIED;
  result = module_inventory(1, denied);
  require(result.error == ERROR_ACCESS_DENIED && result.entries.empty() && result.attempts == 1 && denied.pauses == 0,
          "Access failure remains an error, not a missing bridge or retry");
  partial = {};
  partial.fail_after_entry = true;
  partial.failure = ERROR_PARTIAL_COPY;
  result = module_inventory(1, partial);
  require(result.error == ERROR_PARTIAL_COPY && result.entries.empty() && result.attempts == 1 && partial.closed == 1,
          "A failed enumeration must never publish an incomplete module list");
  partial.failure = 0;
  partial.snapshots = partial.closed = 0;
  result = module_inventory(1, partial);
  require(result.error == ERROR_GEN_FAILURE && result.entries.empty(), "Failed API without an error still refuses partial contents");
  DWORD error = 1;
  unsigned attempts = 0;
  const auto current = taxi_camera::standalone::modules(GetCurrentProcessId(), &error, &attempts);
  require(!error && !current.empty() && attempts > 0, "Real Windows module enumeration remains usable");
}
bool mkdir_p(const std::wstring& path) {
  if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
    return true;
  const auto slash = path.find_last_of(L"\\/");
  return slash != std::wstring::npos && mkdir_p(path.substr(0, slash)) &&
         (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS);
}
bool write_empty_file(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  CloseHandle(file);
  return true;
}
void dual_install_checks(const std::wstring& original, const std::wstring& alias, const std::wstring& copy) {
  using taxi_camera::standalone::accepted_simulator_image;
  using taxi_camera::standalone::add_steam_library_simulators;
  using taxi_camera::standalone::discover_msfs2024_executables;
  using taxi_camera::standalone::known_msfs2024_layout;
  using taxi_camera::standalone::select_simulator;
  using taxi_camera::standalone::steam_library_roots_from_vdf;

  require(known_msfs2024_layout(L"D:\\SteamLibrary\\steamapps\\common\\MSFS2024\\FlightSimulator2024.exe"), "Steam MSFS2024 layout");
  require(known_msfs2024_layout(L"C:/Program Files (x86)/Steam/steamapps/common/Limitless/FlightSimulator2024.exe"),
          "Steam Limitless layout");
  require(known_msfs2024_layout(L"C:\\XboxGames\\Microsoft Flight Simulator 2024\\Content\\FlightSimulator2024.exe"), "XboxGames layout");
  require(known_msfs2024_layout(L"C:\\Program Files\\WindowsApps\\Microsoft.Limitless_1.8.16.0_x64__8wekyb3d8bbwe\\FlightSimulator2024.exe"),
          "WindowsApps Limitless layout");
  require(!known_msfs2024_layout(L"C:\\not-a-simulator\\FlightSimulator2024.exe"), "Random FlightSimulator2024.exe is not a 2024 install");
  require(!known_msfs2024_layout(L"C:\\XboxGames\\Microsoft Flight Simulator 2024\\Content\\SimConnect.dll"), "Non-exe layout rejected");
  require(!known_msfs2024_layout(L"C:\\Program Files\\WindowsApps\\Other.App_1.0.0.0_x64__8wekyb3d8bbwe\\FlightSimulator2024.exe"),
          "Unrelated WindowsApps package rejected");

  const auto steam = L"D:\\SteamLibrary\\steamapps\\common\\MSFS2024\\FlightSimulator2024.exe";
  const auto store = L"C:\\XboxGames\\Microsoft Flight Simulator 2024\\Content\\FlightSimulator2024.exe";
  require(accepted_simulator_image(store, steam, {}), "Configured Steam missing must still accept a live Store 2024");
  require(accepted_simulator_image(steam, store, {}), "Configured Store missing must still accept a live Steam 2024");
  wchar_t named_root[MAX_PATH]{};
  require(GetTempPathW(MAX_PATH, named_root) != 0, "Temporary directory for named 2024 fixtures");
  const auto named = std::wstring(named_root) + L"taxi-dual-named-" + std::to_wstring(GetCurrentProcessId());
  const auto steam_file = named + L"\\steam\\FlightSimulator2024.exe";
  const auto store_file = named + L"\\store\\FlightSimulator2024.exe";
  const auto steam_alias = named + L"\\steam-alias\\FlightSimulator2024.exe";
  require(mkdir_p(named + L"\\steam") && mkdir_p(named + L"\\store") && mkdir_p(named + L"\\steam-alias"),
          "Create named 2024 fixture directories");
  require(CopyFileW(original.c_str(), steam_file.c_str(), TRUE) != FALSE, "Create named Steam fixture");
  require(CopyFileW(copy.c_str(), store_file.c_str(), TRUE) != FALSE, "Create named Store fixture");
  require(CreateHardLinkW(steam_alias.c_str(), steam_file.c_str(), nullptr) != FALSE, "Create named Steam alias");
  require(accepted_simulator_image(steam_file, store_file, {steam_file}), "Discovered configured file remains accepted");
  require(!accepted_simulator_image(store_file, steam_file, {steam_file}), "Separate non-2024 copy is not a fallback candidate");
  require(accepted_simulator_image(steam_alias, steam_file, {}), "Configured path aliases still match");
  require(accepted_simulator_image(L"C:\\temp\\FlightSimulator2024.exe", L"", {}), "Empty hint still accepts any 2024 process name");
  require(!accepted_simulator_image(L"C:\\temp\\NotTheSimulator.exe", L"", {}), "Wrong process name is never a candidate");
  DeleteFileW(steam_alias.c_str());
  DeleteFileW(store_file.c_str());
  DeleteFileW(steam_file.c_str());
  RemoveDirectoryW((named + L"\\steam-alias").c_str());
  RemoveDirectoryW((named + L"\\store").c_str());
  RemoveDirectoryW((named + L"\\steam").c_str());
  RemoveDirectoryW(named.c_str());

  require(select_simulator({{10, copy}}, original) == 10, "Configured file not running selects the other 2024");
  require(select_simulator({{5, original}, {10, copy}}, original) == 5, "Configured install wins when both are running");
  require(select_simulator({{10, original}, {5, copy}}, L"") == 5, "Two live 2024 installs without a hint pick the lowest PID");
  require(select_simulator({{5, original}, {10, alias}}, original) == 0, "Two processes of the same configured file stay ambiguous");
  require(select_simulator({{20, original}, {10, copy}}, L"C:\\missing\\FlightSimulator2024.exe") == 10,
          "Hint that is not running picks the lowest distinct 2024 PID");
  require(select_simulator({}, original) == 0, "No live 2024 remains unmatched");

  const auto vdf = steam_library_roots_from_vdf(
      L"\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t"
      L"\"D:\\\\SteamLibrary\"\n\t}\n}\n");
  require(vdf.size() == 2 && _wcsicmp(vdf[0].c_str(), L"C:\\Program Files (x86)\\Steam") == 0 &&
              _wcsicmp(vdf[1].c_str(), L"D:\\SteamLibrary") == 0,
          "Steam libraryfolders.vdf yields both library roots");

  wchar_t temporary[MAX_PATH]{};
  require(GetTempPathW(MAX_PATH, temporary) != 0, "Temporary directory for Steam library fixture");
  const auto library = std::wstring(temporary) + L"taxi-steam-lib-" + std::to_wstring(GetCurrentProcessId());
  const auto msfs = library + L"\\steamapps\\common\\MSFS2024\\FlightSimulator2024.exe";
  require(mkdir_p(library + L"\\steamapps\\common\\MSFS2024"), "Create Steam MSFS2024 fixture directory");
  require(write_empty_file(msfs), "Create Steam MSFS2024 fixture executable");
  std::vector<std::wstring> discovered;
  add_steam_library_simulators(discovered, library);
  require(discovered.size() == 1 && _wcsicmp(discovered[0].c_str(), msfs.c_str()) == 0, "Steam library discovery finds MSFS2024.exe");
  const auto known = discover_msfs2024_executables(original);
  bool saw_configured = false;
  for (const auto& path : known)
    saw_configured = saw_configured || taxi_camera::standalone::same_path(path, original);
  require(saw_configured, "Discovery includes the configured executable when it exists");
  DeleteFileW(msfs.c_str());
  RemoveDirectoryW((library + L"\\steamapps\\common\\MSFS2024").c_str());
  RemoveDirectoryW((library + L"\\steamapps\\common").c_str());
  RemoveDirectoryW((library + L"\\steamapps").c_str());
  RemoveDirectoryW(library.c_str());
}
}  // namespace

int main() {
  try {
    module_checks();
    using taxi_camera::standalone::same_path;
    wchar_t temporary[MAX_PATH]{}, original[MAX_PATH]{};
    require(GetTempPathW(MAX_PATH, temporary) != 0, "Temporary directory");
    require(GetTempFileNameW(temporary, L"tax", 0, original) != 0, "Unique fixture file");
    Files files{original, std::wstring(original) + L".alias", std::wstring(original) + L".copy"};
    require(CreateHardLinkW(files.alias.c_str(), files.original.c_str(), nullptr) != FALSE, "Create alternate path to the same file");
    require(CopyFileW(files.original.c_str(), files.copy.c_str(), TRUE) != FALSE, "Create separate file with identical contents");

    require(same_path(files.original, files.original), "Identical path");
    require(same_path(files.original, files.alias), "Hard-linked simulator paths must match");
    require(same_path(files.alias, files.original), "Path identity must be symmetric");
    require(!same_path(files.original, files.copy), "A separate copy must not match");
    require(!same_path(files.original, files.original + L".missing"), "Missing alias must not match");
    require(!same_path(L"", files.original) && !same_path(files.original, L""), "Empty path must not match");
    dual_install_checks(files.original, files.alias, files.copy);

    require(DeleteFileW(files.alias.c_str()) != FALSE, "Remove test alias");
    require(CopyFileW(files.copy.c_str(), files.alias.c_str(), TRUE) != FALSE, "Replace alias with a distinct file");
    require(!same_path(files.original, files.alias), "Replaced path must not retain stale identity");
    using taxi_camera::standalone::LaunchResult;
    using taxi_camera::standalone::LaunchRetry;
    const LaunchResult pending{false, ERROR_BAD_EXE_FORMAT, L"Headers not ready", true};
    LaunchRetry retry;
    require(retry.ready(1000), "First startup attempt immediate");
    require(retry.schedule(pending, 1000) && !retry.ready(1999) && retry.ready(2000), "Preflight retry waits one second");
    for (unsigned i = 1; i < LaunchRetry::MaxPreflightRetries; ++i)
      require(retry.schedule(pending, 1000 + i * 1000), "Bounded preflight retry available");
    require(!retry.schedule(pending, 61000), "No more than 60 total load attempts");
    require(retry.schedule_recovery(61000) && !retry.ready(61000 + LaunchRetry::RecoveryDelayMs - 1) &&
                retry.ready(61000 + LaunchRetry::RecoveryDelayMs),
            "Exhausted preflight schedules a recovery wave");
    require(retry.retries() == 0 && retry.recoveries() == 1, "Recovery clears the preflight counter");
    for (unsigned i = 1; i < LaunchRetry::MaxRecoveryWaves; ++i)
      require(retry.schedule_recovery(100000 + i * LaunchRetry::RecoveryDelayMs), "Bounded recovery waves available");
    require(!retry.schedule_recovery(UINT64_MAX / 2), "Recovery wave budget is finite");
    retry.reset();
    require(retry.ready(0) && retry.retries() == 0 && retry.recoveries() == 0, "Reset clears retry state for Reconnect");
    for (const auto error : {WAIT_TIMEOUT, ERROR_ACCESS_DENIED, ERROR_MOD_NOT_FOUND, ERROR_INVALID_ADDRESS}) {
      LaunchRetry terminal;
      require(!terminal.schedule({false, static_cast<DWORD>(error), L"Load or start may have begun"}, 1000),
              "Never repeat remote load/start, timeout, identity or other terminal errors");
    }
    LaunchRetry complete;
    require(!complete.schedule({true, 0, L"Loaded"}, 1000), "Successful load never retried");
    LaunchRetry overflow;
    require(!overflow.schedule(pending, UINT64_MAX), "Retry deadline cannot overflow");
    std::puts("PASS launcher paths and startup: aliases, dual-install attach, bounded preflight retries, recovery waves and terminal remote-load failures.");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL launcher paths: %s\n", error.what());
    return 1;
  }
}
