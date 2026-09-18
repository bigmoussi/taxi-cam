#pragma once
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <new>

namespace taxi_camera::standalone::crash_evidence {
// Small local diagnostic record for the renderer instruction seen in the A350
// reports. No stack/heap pages or image data are copied. Never sent over IPC.
struct Record {
  std::uint32_t magic = 0x54434352, version = 1, bytes = sizeof(Record), process = 0;
  volatile LONG state = 0;  // 0 empty, 1 writing, 2 complete
  DWORD code = 0, parameters = 0, thread = 0;
  std::uint64_t module = 0, fault_rva = 0;
  ULONG_PTR information[EXCEPTION_MAXIMUM_PARAMETERS]{};
  CONTEXT context{};
};
static_assert(sizeof(Record) <= 4096);
inline Record* record = nullptr;
inline HANDLE record_file = INVALID_HANDLE_VALUE;
inline constexpr std::uint64_t renderer_fault_rva = 64028644;
inline constexpr unsigned retained_records = 16;
inline constexpr unsigned maintenance_entries = 4096, maintenance_passes = 32;
inline constexpr std::uint64_t maintenance_milliseconds = 250;

struct Mapping {
  HANDLE file = INVALID_HANDLE_VALUE;
  Record* memory = nullptr;
  Mapping() = default;
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  void close() noexcept {
    if (memory)
      UnmapViewOfFile(memory);
    if (file != INVALID_HANDLE_VALUE)
      CloseHandle(file);
    memory = nullptr;
    file = INVALID_HANDLE_VALUE;
  }
  ~Mapping() { close(); }
};

inline bool record_filename(const wchar_t* name) noexcept {
  constexpr wchar_t prefix[] = L"renderer-fault-";
  if (!name || std::wcsncmp(name, prefix, std::size(prefix) - 1) != 0)
    return false;
  const wchar_t* cursor = name + std::size(prefix) - 1;
  const auto number = [&cursor](std::uint64_t maximum, wchar_t separator, bool positive) noexcept {
    if (*cursor < L'0' || *cursor > L'9' || (*cursor == L'0' && cursor[1] >= L'0' && cursor[1] <= L'9'))
      return false;
    std::uint64_t value = 0;
    do {
      const auto digit = static_cast<unsigned>(*cursor - L'0');
      if (value > (maximum - digit) / 10)
        return false;
      value = value * 10 + digit;
      ++cursor;
    } while (*cursor >= L'0' && *cursor <= L'9');
    return (!positive || value != 0) && *cursor++ == separator;
  };
  return number((std::numeric_limits<DWORD>::max)(), L'-', true) && number((std::numeric_limits<std::uint64_t>::max)(), L'.', false) &&
         std::wcscmp(cursor, L"bin") == 0;
}

struct Candidate {
  FILETIME created{};
  DWORD volume{}, index_high{}, index_low{};
  LONG state{};
  wchar_t name[64]{};
};
inline bool older(const Candidate& a, const Candidate& b) noexcept {
  if (a.state != b.state)
    return a.state < b.state;  // Empty, incomplete, then completed evidence.
  const auto comparison = CompareFileTime(&a.created, &b.created);
  return comparison < 0 || (comparison == 0 && std::wcscmp(a.name, b.name) < 0);
}
inline bool inspect_record(HANDLE file, Candidate& candidate) noexcept {
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(file, &information) ||
      (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || information.nFileSizeHigh ||
      (information.nFileSizeLow != 0 && information.nFileSizeLow != 4096))
    return false;
  LONG state = 0;
  if (information.nFileSizeLow) {
    struct Prefix {
      std::uint32_t magic, version, bytes, process;
      LONG state;
    } prefix{};
    static_assert(sizeof(Prefix) == offsetof(Record, state) + sizeof(LONG));
    DWORD read{};
    if (!ReadFile(file, &prefix, sizeof(prefix), &read, nullptr) || read != sizeof(prefix) || prefix.magic != 0x54434352 ||
        prefix.version != 1 || prefix.bytes != sizeof(Record) || prefix.state < 0 || prefix.state > 2)
      return false;
    state = prefix.state;
  }
  candidate.created = information.ftCreationTime;
  candidate.volume = information.dwVolumeSerialNumber;
  candidate.index_high = information.nFileIndexHigh;
  candidate.index_low = information.nFileIndexLow;
  candidate.state = state;
  return true;
}

// Hold the directory's exclusive retention lock while calling this. Each pass
// stores only the oldest sixteen remaining names, even for a large old archive.
// Failed deletions advance the cursor, so locked/mapped records cannot prevent
// trying newer inactive files. No record is ever opened for writing or resized.
inline bool make_room(const wchar_t* directory) noexcept {
  Candidate cursor{};
  bool after_cursor = false;
  wchar_t path[32768]{};
  const auto started = GetTickCount64();
  unsigned inspected = 0;
  for (unsigned pass = 0; pass < maintenance_passes; ++pass) {
    if (GetTickCount64() - started >= maintenance_milliseconds)
      return false;
    std::array<Candidate, retained_records> candidates{};
    unsigned selected = 0;
    std::uint64_t count = 0;
    if (std::swprintf(path, std::size(path), L"%ls\\renderer-fault-*.bin", directory) < 0)
      return false;
    WIN32_FIND_DATAW entry{};
    const auto search = FindFirstFileW(path, &entry);
    if (search == INVALID_HANDLE_VALUE)
      return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
      if (++inspected > maintenance_entries || GetTickCount64() - started >= maintenance_milliseconds) {
        FindClose(search);
        return false;
      }
      if ((entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !record_filename(entry.cFileName))
        continue;
      ++count;
      Candidate candidate;
      std::wcscpy(candidate.name, entry.cFileName);  // Canonical numeric name is at most 50 characters.
      if (std::swprintf(path, std::size(path), L"%ls\\%ls", directory, candidate.name) < 0) {
        FindClose(search);
        return false;
      }
      const auto inspection = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (inspection == INVALID_HANDLE_VALUE)
        continue;
      const bool valid = inspect_record(inspection, candidate);
      CloseHandle(inspection);
      if (!valid)
        continue;
      if (after_cursor && !older(cursor, candidate))
        continue;
      unsigned position = 0;
      while (position < selected && !older(candidate, candidates[position]))
        ++position;
      if (position == candidates.size())
        continue;
      if (selected < candidates.size())
        ++selected;
      for (unsigned i = selected - 1; i > position; --i)
        candidates[i] = candidates[i - 1];
      candidates[position] = candidate;
    } while (FindNextFileW(search, &entry));
    const auto enumeration_error = GetLastError();
    FindClose(search);
    if (enumeration_error != ERROR_NO_MORE_FILES)
      return false;
    if (count < retained_records)
      return true;
    if (!selected)
      return false;
    for (unsigned i = 0; i < selected && count >= retained_records; ++i) {
      if (GetTickCount64() - started >= maintenance_milliseconds)
        return false;
      const auto& candidate = candidates[i];
      if (std::swprintf(path, std::size(path), L"%ls\\%ls", directory, candidate.name) < 0)
        return false;
      const auto file = CreateFileW(path, DELETE | GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (file == INVALID_HANDLE_VALUE)
        continue;
      Candidate verified;
      FILE_DISPOSITION_INFO disposition{TRUE};
      if (inspect_record(file, verified) && verified.volume == candidate.volume && verified.index_high == candidate.index_high &&
          verified.index_low == candidate.index_low && verified.state == candidate.state &&
          SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)))
        --count;
      CloseHandle(file);
    }
    if (count < retained_records)
      return true;
    cursor = candidates[selected - 1];
    after_cursor = true;
  }
  return false;
}

// Initialization-only I/O. The held file handle deliberately omits delete
// sharing, protecting the mapped record until this session releases it.
inline bool create_record(const wchar_t* directory,
                          DWORD process,
                          std::uint64_t ticks,
                          std::uint64_t module,
                          Mapping& destination) noexcept {
  if (!directory || !process || destination.memory || destination.file != INVALID_HANDLE_VALUE)
    return false;
  wchar_t path[32768]{};
  if (std::swprintf(path, std::size(path), L"%ls\\renderer-fault-retention.lock", directory) < 0)
    return false;
  const auto retention_lock = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (retention_lock == INVALID_HANDLE_VALUE)
    return false;
  struct Unlock {
    HANDLE handle;
    ~Unlock() { CloseHandle(handle); }
  } unlock{retention_lock};
  BY_HANDLE_FILE_INFORMATION lock_information{};
  if (!GetFileInformationByHandle(retention_lock, &lock_information) ||
      (lock_information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !make_room(directory) ||
      std::swprintf(path, std::size(path), L"%ls\\renderer-fault-%lu-%llu.bin", directory, process,
                    static_cast<unsigned long long>(ticks)) < 0)
    return false;
  const auto file =
      CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  const auto mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 4096, nullptr);
  auto* memory = mapping ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 4096) : nullptr;
  if (mapping)
    CloseHandle(mapping);
  if (!memory) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
    CloseHandle(file);
    return false;
  }
  destination.file = file;
  destination.memory = new (memory) Record{};
  destination.memory->process = process;
  destination.memory->module = module;
  return true;
}

inline LONG capture(Record* destination, const EXCEPTION_POINTERS* exception) noexcept {
  if (!destination || !exception || !exception->ExceptionRecord || !exception->ContextRecord)
    return EXCEPTION_CONTINUE_SEARCH;
  const auto& e = *exception->ExceptionRecord;
  const auto& c = *exception->ContextRecord;
  const auto address = reinterpret_cast<std::uint64_t>(e.ExceptionAddress);
  if (e.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !destination->module || address < destination->module ||
      address - destination->module != renderer_fault_rva || c.Rip != address || e.NumberParameters > EXCEPTION_MAXIMUM_PARAMETERS ||
      InterlockedCompareExchange(&destination->state, 1, 0) != 0)
    return EXCEPTION_CONTINUE_SEARCH;
  // Only bounded copies into an already mapped, initialized record. No file
  // operations, allocation, locks, engine calls or exception suppression here.
  destination->code = e.ExceptionCode;
  destination->parameters = e.NumberParameters;
  destination->thread = GetCurrentThreadId();
  destination->fault_rva = renderer_fault_rva;
  for (DWORD i = 0; i < e.NumberParameters; ++i)
    destination->information[i] = e.ExceptionInformation[i];
  destination->context = c;
  InterlockedExchange(&destination->state, 2);
  return EXCEPTION_CONTINUE_SEARCH;
}
inline LONG CALLBACK handler(EXCEPTION_POINTERS* exception) noexcept {
  return capture(record, exception);
}

// Called outside DllMain, after the bridge verifies its host and pins itself.
// The process owns this mapping/handler until exit; Windows retains the dirty
// mapped-file pages after a process crash. No debugger is attached.
inline bool initialize() noexcept {
  if (record)
    return true;
  wchar_t directory[32768]{};
  const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, 32768);
  if (!length || length > 32000)
    return false;
  wchar_t folder[32768]{};
  std::swprintf(folder, 32768, L"%ls\\Taxi Cam", directory);
  CreateDirectoryW(folder, nullptr);
  Mapping mapping;
  if (!create_record(folder, GetCurrentProcessId(), GetTickCount64(), reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr)), mapping))
    return false;
  record = mapping.memory;
  if (!AddVectoredExceptionHandler(1, handler)) {
    record = nullptr;
    return false;
  }
  record_file = mapping.file;
  mapping.file = INVALID_HANDLE_VALUE;
  mapping.memory = nullptr;
  return true;
}
}  // namespace taxi_camera::standalone::crash_evidence
