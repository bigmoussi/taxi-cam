#include "../../src/app/updater.hpp"
#include <cstdio>
#include <fstream>
#include <string>

using namespace taxi_camera::standalone;
int main() {
  int failures = 0;
  const auto check = [&](bool ok, const char* name) {
    if (!ok) {
      std::fprintf(stderr, "FAIL: %s\n", name);
      ++failures;
    }
  };
  UpdateVersion current{}, latest{};
  check(parse_update_version(L"v0.8.0-build.7", current), "current version");
  check(parse_update_version(L"v0.8.0-build.10", latest) && newer_update(latest, current), "numeric build ordering");
  check(parse_update_version(L"v0.8.1-build.1", latest) && newer_update(latest, current), "patch version takes priority over build");
  check(parse_update_version(L"v0.9.0-build.1", latest) && newer_update(latest, current), "version takes priority over build");
  check(!newer_update(current, current), "no reinstall same release");
  check(!newer_update(current, latest), "no downgrade");
  check(parse_update_version(L"v0.8.0-build.6", latest) && !newer_update(latest, current), "lower published build ignored");
  check(parse_update_version(L"v0.7.9-build.999", latest) && !newer_update(latest, current),
        "older version ignored despite larger build");
  check(parse_update_version(L"v0.8.0-build.0", latest) && newer_update(current, latest),
        "published build is newer than local or PR build 0");
  for (const auto* tag : {L"v0.8.0-build.01", L"v0.8.0-build.-1", L"v0.8.0-build.4294967296", L"v0.8.0-build.9;calc",
                          L"0.8.0-build.9", L"v0.8.0-build.9-preview", L"v0.8.0-build.", L"v0.8.0-build.9\n",
                          L"v0.8.0-pr.12", L"v0.8.0-test.1", L"v0.8.0", L"pr-12"})
    check(!parse_update_version(tag, latest), "malformed tag rejected");
  check(update_installer_arguments(L"C:\\Users\\A B\\Taxi Cam", 123) ==
            L"/DIR=\"C:\\Users\\A B\\Taxi Cam\" /UPDATEFROMPID=123", "installer args preserve path spaces");
  check(update_installer_arguments(L"C:\\Bad\" /SILENT", 123).empty(), "reject argument injection");
  check(update_installer_arguments(L"C:\\App\\", 123).empty(), "reject trailing slash quote ambiguity");
  check(update_installer_arguments(L"C:\\App", 0).empty(), "require parent pid");
  // Test files live beside this test executable in the ignored build directory.
  wchar_t module[32768]{};
  GetModuleFileNameW(nullptr, module, 32768);
  std::wstring path(module);
  path.resize(path.find_last_of(L"\\/"));
  path += L"\\updater-integrity-test-" + std::to_wstring(GetCurrentProcessId()) + L".bin";
  { std::ofstream file(path.c_str(), std::ios::binary); file << "abc"; }
  const std::wstring hash = L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  HANDLE verified = verified_update_file(path, hash);
  check(verified != INVALID_HANDLE_VALUE, "known SHA256 digest");
  if (verified != INVALID_HANDLE_VALUE) {
    HANDLE write = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(write == INVALID_HANDLE_VALUE, "verified file cannot change before launch");
    if (write != INVALID_HANDLE_VALUE) CloseHandle(write);
    check(!DeleteFileW(path.c_str()), "verified file cannot be replaced before launch");
    CloseHandle(verified);
  }
  { std::ofstream file(path.c_str(), std::ios::binary); file << "tampered"; }
  verified = verified_update_file(path, hash);
  check(verified == INVALID_HANDLE_VALUE, "tampered installer rejected");
  if (verified != INVALID_HANDLE_VALUE) CloseHandle(verified);
  check(verified_update_file(path, L"not-a-sha256") == INVALID_HANDLE_VALUE, "malformed checksum rejected");
  Updater updater;
  UpdateResult result;
  result.available = true;
  result.installer = path;
  result.sha256 = hash;
  std::wstring error;
  check(!updater.launch(result, L"C:\\App", error), "arbitrary installer path cannot launch");
  DeleteFileW(path.c_str());
  if (!failures) std::puts("Updater version, integrity and handoff guards passed.");
  return failures ? 1 : 0;
}
