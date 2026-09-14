#include <cstdio>
#include <stdexcept>
#include "launcher.hpp"

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
    std::puts("PASS launcher paths: aliases accepted; copies, missing and replaced files rejected.");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL launcher paths: %s\n", error.what());
    return 1;
  }
}
