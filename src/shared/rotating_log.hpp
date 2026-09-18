#pragma once
#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <string>
#include <string_view>

namespace taxi_camera::standalone {
inline constexpr std::uint64_t BridgeLogBytes = 8 * 1024 * 1024;
inline constexpr std::uint64_t LauncherLogBytes = 4 * 1024 * 1024;
inline constexpr std::size_t MaxLogRecordBytes = 8192;

namespace log_detail {
struct Handle {
  HANDLE value{INVALID_HANDLE_VALUE};
  explicit Handle(HANDLE handle) noexcept : value(handle) {}
  ~Handle() {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};
struct Lock {
  HANDLE mutex;
  ~Lock() { ReleaseMutex(mutex); }
};
inline std::uint64_t path_key(const std::wstring& path) noexcept {
  std::uint64_t hash = 14695981039346656037ull;
  for (auto c : path) {
    if (c >= L'A' && c <= L'Z')
      c += L'a' - L'A';
    if (c == L'/')
      c = L'\\';
    hash = (hash ^ static_cast<std::uint16_t>(c)) * 1099511628211ull;
  }
  return hash;
}
inline bool write(HANDLE file, std::string_view bytes) noexcept {
  DWORD written{};
  return bytes.empty() || (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size());
}
inline bool rotate(HANDLE file, const std::wstring& path, std::uint64_t length, std::uint64_t limit) {
  // One extra byte identifies a complete line at an exact tail boundary. An
  // oversized legacy log is reduced to recent complete lines, never archived
  // whole and left unbounded. Memory and the temporary file are also bounded.
  const auto take = length > limit ? limit + 1 : length;
  std::string tail(static_cast<std::size_t>(take), '\0');
  LARGE_INTEGER offset{};
  offset.QuadPart = static_cast<LONGLONG>(length - take);
  if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN))
    return false;
  DWORD read{};
  if (take && (!ReadFile(file, tail.data(), static_cast<DWORD>(take), &read, nullptr) || read != take))
    return false;
  if (length > limit) {
    const auto newline = tail.find('\n');
    tail.erase(0, newline == std::string::npos ? tail.size() : newline + 1);
    const auto last = tail.rfind('\n');
    tail.resize(last == std::string::npos ? 0 : last + 1);
  }
  const auto temporary = path + L".rotating";
  const auto previous = path + L".1";
  {
    Handle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (output.value == INVALID_HANDLE_VALUE)
      return false;
    if (!write(output.value, tail) || !FlushFileBuffers(output.value)) {
      // Close before cleanup; the active log remains intact on every failure.
      CloseHandle(output.value);
      output.value = INVALID_HANDLE_VALUE;
      DeleteFileW(temporary.c_str());
      return false;
    }
  }
  if (!MoveFileExW(temporary.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    return false;
  }
  // Preserve the bounded previous log before resetting this file. Holding an
  // exclusive writer handle plus the process-shared lock covers the whole
  // size-check/archive/truncate/append transaction.
  offset.QuadPart = 0;
  return SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) && SetEndOfFile(file);
}
}  // namespace log_detail

// The caller supplies a full path and complete UTF-8 log records. Best effort:
// contention or an I/O error drops this record instead of blocking startup or
// allowing a full log to grow. Logging resumes on the next successful append.
inline bool append_rotating_log(const std::wstring& path, std::string_view record, std::uint64_t limit) noexcept {
  try {
    if (path.empty() || path.size() > 32700 || record.empty() || record.size() > MaxLogRecordBytes || !limit || limit > BridgeLogBytes ||
        record.size() > limit)
      return false;
    wchar_t mutex_name[80]{};
    std::swprintf(mutex_name, 80, L"Local\\TaxiCam.Log.%016llx", static_cast<unsigned long long>(log_detail::path_key(path)));
    log_detail::Handle mutex(CreateMutexW(nullptr, FALSE, mutex_name));
    if (!mutex.value)
      return false;
    const auto wait = WaitForSingleObject(mutex.value, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
      return false;
    const log_detail::Lock lock{mutex.value};
    log_detail::Handle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE)
      return false;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.value, &length) || length.QuadPart < 0)
      return false;
    if (static_cast<std::uint64_t>(length.QuadPart) > limit - record.size() &&
        !log_detail::rotate(file.value, path, static_cast<std::uint64_t>(length.QuadPart), limit))
      return false;
    LARGE_INTEGER end{};
    if (!SetFilePointerEx(file.value, {}, &end, FILE_END))
      return false;
    if (log_detail::write(file.value, record))
      return true;
    // A partial write must not leave half a diagnostic record for the next
    // writer. Failure to roll back still cannot exceed the checked byte limit.
    if (SetFilePointerEx(file.value, end, nullptr, FILE_BEGIN))
      SetEndOfFile(file.value);
    return false;
  } catch (...) {
    return false;
  }
}
}  // namespace taxi_camera::standalone
