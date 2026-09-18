#include "updater.hpp"
#include "../shared/version.hpp"
#include <bcrypt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <array>
#include <fstream>
#include <tuple>
#include <vector>

namespace taxi_camera::standalone {
namespace {
constexpr LONGLONG DownloadLimit = 256LL * 1024 * 1024;
std::wstring wide(const char* s) {
  std::wstring result;
  while (*s)
    result += static_cast<unsigned char>(*s++);
  return result;
}
bool hex_hash(const std::wstring& hash) {
  if (hash.size() != 64)
    return false;
  for (wchar_t c : hash)
    if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F')))
      return false;
  return true;
}
void clean_cache(const std::wstring& path) {
  if (path.empty())
    return;
  // Delete only the fixed files in the directory this instance created.
  for (auto name : {L"\\setup.exe", L"\\setup.exe.partial", L"\\result.txt", L"\\check.ps1"})
    DeleteFileW((path + name).c_str());
  RemoveDirectoryW(path.c_str());
}
bool extract_helper(const std::wstring& cache) {
  const auto module = GetModuleHandleW(nullptr);
  const auto resource = FindResourceW(module, MAKEINTRESOURCEW(201), MAKEINTRESOURCEW(10));
  const auto bytes = resource ? SizeofResource(module, resource) : 0;
  const auto loaded = resource ? LoadResource(module, resource) : nullptr;
  const auto data = loaded ? LockResource(loaded) : nullptr;
  if (!data || !bytes || bytes > 1024 * 1024)
    return false;
  const HANDLE file = CreateFileW((cache + L"\\check.ps1").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  DWORD written{};
  const bool ok = WriteFile(file, data, bytes, &written, nullptr) && written == bytes;
  CloseHandle(file);
  return ok;
}
std::wstring make_cache() {
  wchar_t local[MAX_PATH]{};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local)))
    return {};
  const std::wstring root = std::wstring(local) + L"\\Taxi Cam";
  CreateDirectoryW(root.c_str(), nullptr);
  const std::wstring updates = root + L"\\Updates";
  CreateDirectoryW(updates.c_str(), nullptr);
  std::array<unsigned char, 16> random{};
  if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    return {};
  std::wstring path = updates + L"\\";
  for (auto c : random) {
    path += L"0123456789abcdef"[c >> 4];
    path += L"0123456789abcdef"[c & 15];
  }
  return CreateDirectoryW(path.c_str(), nullptr) ? path : L"";
}
}  // namespace
bool parse_update_version(const std::wstring& tag, UpdateVersion& value) {
  if (tag.size() > 64 || tag.empty() || tag[0] != L'v')
    return false;
  size_t pos = 1;
  auto number = [&](std::uint32_t& out) {
    const auto start = pos;
    std::uint64_t n = 0;
    while (pos < tag.size() && tag[pos] >= L'0' && tag[pos] <= L'9') {
      n = n * 10 + tag[pos++] - L'0';
      if (n > UINT32_MAX)
        return false;
    }
    if (pos == start || (pos - start > 1 && tag[start] == L'0'))
      return false;
    out = static_cast<std::uint32_t>(n);
    return true;
  };
  UpdateVersion parsed;
  if (!number(parsed.major) || pos >= tag.size() || tag[pos++] != L'.' || !number(parsed.minor) ||
      pos >= tag.size() || tag[pos++] != L'.' || !number(parsed.patch) || tag.compare(pos, 7, L"-build.") != 0)
    return false;
  pos += 7;
  if (!number(parsed.build) || pos != tag.size())
    return false;
  value = parsed;
  return true;
}
bool newer_update(const UpdateVersion& candidate, const UpdateVersion& current) {
  return std::tie(candidate.major, candidate.minor, candidate.patch, candidate.build) >
         std::tie(current.major, current.minor, current.patch, current.build);
}
bool simulator_blocks_update() {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return true;  // Cannot establish that the DLL is unlocked.
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool blocked = !Process32FirstW(snapshot, &entry);
  if (!blocked)
    do {
      if (!_wcsicmp(entry.szExeFile, L"FlightSimulator2024.exe") || !_wcsicmp(entry.szExeFile, L"FlightSimulator.exe")) {
        blocked = true;
        break;
      }
    } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return blocked;
}
HANDLE verified_update_file(const std::wstring& path, const std::wstring& sha256) {
  if (!hex_hash(sha256))
    return INVALID_HANDLE_VALUE;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return file;
  LARGE_INTEGER size{};
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  bool ok = GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
            !(attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
            GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= DownloadLimit;
  BCRYPT_ALG_HANDLE algorithm{};
  BCRYPT_HASH_HANDLE hash{};
  std::array<unsigned char, 32> digest{};
  if (ok)
    ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
         BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
  std::array<unsigned char, 65536> buffer{};
  LONGLONG total{};
  while (ok) {
    DWORD read{};
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
      ok = false;
      break;
    }
    if (!read)
      break;
    total += read;
    ok = total <= DownloadLimit && BCryptHashData(hash, buffer.data(), read, 0) >= 0;
  }
  if (ok)
    ok = total == size.QuadPart && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
  if (hash)
    BCryptDestroyHash(hash);
  if (algorithm)
    BCryptCloseAlgorithmProvider(algorithm, 0);
  std::wstring actual;
  for (auto c : digest) {
    actual += L"0123456789abcdef"[c >> 4];
    actual += L"0123456789abcdef"[c & 15];
  }
  if (!ok || _wcsicmp(actual.c_str(), sha256.c_str())) {
    CloseHandle(file);
    return INVALID_HANDLE_VALUE;
  }
  return file;
}
std::wstring update_installer_arguments(const std::wstring& directory, DWORD pid) {
  if (!pid || directory.empty() || directory.find_first_of(L"\"\r\n") != std::wstring::npos ||
      directory.back() == L'\\' || directory.back() == L'/')
    return {};
  return L"/DIR=\"" + directory + L"\" /UPDATEFROMPID=" + std::to_wstring(pid);
}
Updater::~Updater() {
  stop();
  clean_cache(cache_);
}
void Updater::stop() {
  cancelled_ = true;
  if (worker_.joinable())
    worker_.join();
}
bool Updater::begin(const std::wstring& installation, bool manual) {
  if (busy_)
    return false;
  if (worker_.joinable())
    worker_.join();
  {
    std::lock_guard lock(mutex_);
    if (manual && result_.available && !cache_.empty() && result_.installer == cache_ + L"\\setup.exe") {
      result_.manual = true;
      ready_ = true;
      return true;  // Reuse a declined/blocked download; launch always verifies its bytes again.
    }
    ready_ = false;
  }
  clean_cache(cache_);
  cache_ = make_cache();
  cancelled_ = false;
  busy_ = true;
  worker_ = std::thread([this, installation, manual] {
    UpdateResult result;
    result.manual = manual;
    result.error = L"Could not check for updates. Please try again later.";
    wchar_t system[MAX_PATH]{};
    const auto cache = cache_;
    if (!cache.empty() && extract_helper(cache) && GetSystemDirectoryW(system, MAX_PATH) &&
        installation.find(L'\"') == std::wstring::npos) {
      const std::wstring executable = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
      std::wstring command = L"\"" + executable + L"\" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
                             cache + L"\\check.ps1\" -OutputDirectory \"" + cache + L"\" -CurrentVersion \"" +
                             wide(TAXI_CAM_VERSION) + L"\" -CurrentBuild " + std::to_wstring(TAXI_CAM_BUILD_NUMBER);
      STARTUPINFOW startup{};
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION process{};
      if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         installation.c_str(), &startup, &process)) {
        CloseHandle(process.hThread);
        const ULONGLONG start = GetTickCount64();
        DWORD wait{};
        while ((wait = WaitForSingleObject(process.hProcess, 100)) == WAIT_TIMEOUT && !cancelled_ &&
               GetTickCount64() - start < 180000) {}
        DWORD code = 1;
        if (wait != WAIT_OBJECT_0) {
          // Cancellation must not hold companion shutdown indefinitely if the
          // helper cannot finish terminating (for example, pending OS I/O).
          if (TerminateProcess(process.hProcess, ERROR_CANCELLED))
            WaitForSingleObject(process.hProcess, 2000);
        } else
          GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess);
        if (!code && !cancelled_) {
          std::ifstream stream((cache + L"\\result.txt").c_str(), std::ios::binary);
          std::string data(256, '\0');
          stream.read(data.data(), static_cast<std::streamsize>(data.size()));
          const auto length = stream.gcount();
          data.resize(static_cast<size_t>(length));
          if (data == "current\n")
            result.error.clear();
          else if (data.size() < 256 && data.rfind("ready\n", 0) == 0) {
            const auto split = data.find('\n', 6);
            if (split != std::string::npos && data.back() == '\n') {
              result.tag = wide(data.substr(6, split - 6).c_str());
              result.sha256 = wide(data.substr(split + 1, data.size() - split - 2).c_str());
              UpdateVersion latest{}, current{};
              result.installer = cache + L"\\setup.exe";
              if (parse_update_version(result.tag, latest) &&
                  parse_update_version(L"v" + wide(TAXI_CAM_VERSION) + L"-build." + std::to_wstring(TAXI_CAM_BUILD_NUMBER), current) &&
                  newer_update(latest, current)) {
                const HANDLE verified = verified_update_file(result.installer, result.sha256);
                if (verified != INVALID_HANDLE_VALUE) {
                  CloseHandle(verified);
                  result.available = true;
                  result.error.clear();
                }
              }
            }
          }
        }
      }
    }
    DeleteFileW((cache + L"\\check.ps1").c_str());
    {
      std::lock_guard lock(mutex_);
      result_ = std::move(result);
      ready_ = !cancelled_;
    }
    busy_ = false;
  });
  return true;
}
bool Updater::take(UpdateResult& result) {
  std::lock_guard lock(mutex_);
  if (!ready_)
    return false;
  result = result_;
  ready_ = false;
  return true;
}
bool Updater::launch(const UpdateResult& result, const std::wstring& installation, std::wstring& error) {
  {
    std::lock_guard lock(mutex_);
    result_.available = false;  // A failed handoff must permit a fresh check/download.
  }
  error = L"The installer could not be verified or started. Please check for updates again.";
  if (!result.available || cache_.empty() || result.installer != cache_ + L"\\setup.exe")
    return false;
  if (simulator_blocks_update()) {
    error = L"Close Microsoft Flight Simulator before installing this update, then check for updates again.";
    return false;
  }
  const auto arguments = update_installer_arguments(installation, GetCurrentProcessId());
  if (arguments.empty())
    return false;
  HANDLE verified = verified_update_file(result.installer, result.sha256);
  if (verified == INVALID_HANDLE_VALUE)
    return false;
  SHELLEXECUTEINFOW execute{};
  execute.cbSize = sizeof(execute);
  execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  execute.lpVerb = L"open";
  execute.lpFile = result.installer.c_str();
  execute.lpParameters = arguments.c_str();
  execute.nShow = SW_SHOWNORMAL;
  const bool launched = ShellExecuteExW(&execute) && execute.hProcess;
  CloseHandle(verified);
  if (execute.hProcess)
    CloseHandle(execute.hProcess);
  if (launched)
    cache_.clear();  // The installer still needs its source; retain this unique directory.
  return launched;
}
}  // namespace taxi_camera::standalone
