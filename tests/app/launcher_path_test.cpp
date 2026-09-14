#include <cstdio>
#include <stdexcept>
#include "../../src/app/launcher.hpp"

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
}  // namespace

int main() {
  try {
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

    require(DeleteFileW(files.alias.c_str()) != FALSE, "Remove test alias");
    require(CopyFileW(files.copy.c_str(), files.alias.c_str(), TRUE) != FALSE, "Replace alias with a distinct file");
    require(!same_path(files.original, files.alias), "Replaced path must not retain stale identity");
    using taxi_camera::standalone::LaunchResult;
    using taxi_camera::standalone::LaunchRetry;
    const LaunchResult pending{false, ERROR_BAD_EXE_FORMAT, L"Headers not ready", true};
    LaunchRetry retry;
    require(retry.ready(1000), "First startup attempt immediate");
    require(retry.schedule(pending, 1000) && !retry.ready(1999) && retry.ready(2000), "Preflight retry waits one second");
    for (unsigned i = 1; i < 59; ++i)
      require(retry.schedule(pending, 1000 + i * 1000), "Bounded preflight retry available");
    require(!retry.schedule(pending, 61000), "No more than 60 total load attempts");
    for (const auto error : {WAIT_TIMEOUT, ERROR_ACCESS_DENIED, ERROR_MOD_NOT_FOUND, ERROR_INVALID_ADDRESS}) {
      LaunchRetry terminal;
      require(!terminal.schedule({false, static_cast<DWORD>(error), L"Load or start may have begun"}, 1000),
              "Never repeat remote load/start, timeout, identity or other terminal errors");
    }
    LaunchRetry complete;
    require(!complete.schedule({true, 0, L"Loaded"}, 1000), "Successful load never retried");
    LaunchRetry overflow;
    require(!overflow.schedule(pending, UINT64_MAX), "Retry deadline cannot overflow");
    std::puts("PASS launcher paths and startup: aliases, replaced files, bounded preflight retries and terminal remote-load failures.");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL launcher paths: %s\n", error.what());
    return 1;
  }
}
