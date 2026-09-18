#include "../../src/shared/rotating_log.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace taxi_camera::standalone;
unsigned checks{};
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE handle) : value(handle) {}
  ~Handle() {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};
struct Directory {
  std::wstring path;
  Directory() {
    wchar_t temporary[MAX_PATH]{}, unique[MAX_PATH]{};
    const auto size = GetTempPathW(MAX_PATH, temporary);
    require(size && size < MAX_PATH, "Get own temporary directory");
    require(GetTempFileNameW(temporary, L"tcl", 0, unique) != 0, "Reserve unique fixture name");
    require(DeleteFileW(unique) && CreateDirectoryW(unique, nullptr), "Create isolated log fixture directory");
    path = unique;
  }
  ~Directory() {
    // Delete only files directly inside the unique directory created above.
    WIN32_FIND_DATAW entry{};
    const auto search = FindFirstFileW((path + L"\\*").c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
      do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
          DeleteFileW((path + L"\\" + entry.cFileName).c_str());
      } while (FindNextFileW(search, &entry));
      FindClose(search);
    }
    RemoveDirectoryW(path.c_str());
  }
  std::wstring file(const wchar_t* name) const { return path + L"\\" + name; }
};
bool exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
void write(const std::wstring& path, std::string_view bytes) {
  Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  require(file.value != INVALID_HANDLE_VALUE, "Create fixture bytes");
  DWORD written = 0;
  require(WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size(),
          "Write all fixture bytes");
}
std::string read(const std::wstring& path) {
  Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr));
  require(file.value != INVALID_HANDLE_VALUE, "Read own log fixture");
  LARGE_INTEGER length{};
  require(GetFileSizeEx(file.value, &length) && length.QuadPart >= 0 && length.QuadPart <= 16 * 1024 * 1024, "Bound fixture read");
  std::string bytes(static_cast<std::size_t>(length.QuadPart), '\0');
  DWORD actual = 0;
  require(ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &actual, nullptr) && actual == bytes.size(),
          "Read complete fixture bytes");
  return bytes;
}
void bounded(const std::wstring& path, std::uint64_t limit) {
  for (const auto& name : {path, path + L".1"})
    if (exists(name))
      require(read(name).size() <= limit, "Active log and archive each obey their configured byte limit");
}
std::string numbered(unsigned sequence) {
  char text[20]{};
  std::snprintf(text, sizeof(text), "row%04u\n", sequence);
  return text;
}
void boundary_and_rotation(const Directory& directory) {
  const auto path = directory.file(L"bound.log");
  constexpr unsigned limit = 16;
  require(append_rotating_log(path, numbered(0), limit), "First complete record");
  require(append_rotating_log(path, numbered(1), limit), "Append exactly to byte limit");
  require(read(path) == numbered(0) + numbered(1) && !exists(path + L".1"), "Exact limit does not rotate prematurely");
  for (unsigned i = 2; i < 24; ++i) {
    require(append_rotating_log(path, numbered(i), limit), "Logging continues after repeated cap crossings");
    bounded(path, limit);
    const auto active = read(path), archive = read(path + L".1");
    require(active == (i % 2 ? numbered(i - 1) + numbered(i) : numbered(i)), "Newest records remain in the active log");
    const unsigned previous = i % 2 ? i - 3 : i - 2;
    require(archive == numbered(previous) + numbered(previous + 1), "Rotation replaces the archive with the newest complete generation");
  }
}
void legacy_tail(const Directory& directory) {
  // Each line has ten bytes and includes a two-byte UTF-8 character. A retained
  // window that cuts inside that character must discard the entire partial line.
  const std::array<std::string, 3> tail{"old-\xc3\xa9-00\n", "old-\xc3\xa9-01\n", "old-\xc3\xa9-02\n"};
  require(tail[0].size() == 10, "Known UTF-8 fixture line width");
  const std::string prefix(BridgeLogBytes + 1024, 'x');
  const auto legacy = prefix + "\n" + tail[0] + tail[1] + tail[2];
  for (const unsigned limit : {30u, 25u}) {
    const auto path = directory.file(limit == 30 ? L"legacy-boundary.log" : L"legacy-cut.log");
    write(path, legacy);
    write(path + L".1", std::string(100, 'z') + "\n");
    require(append_rotating_log(path, "fresh\r\n", limit), "Oversized legacy logs keep accepting current diagnostics");
    require(read(path) == "fresh\r\n", "Rotation writes the new record to an empty current log");
    require(read(path + L".1") == (limit == 30 ? tail[0] + tail[1] + tail[2] : tail[1] + tail[2]),
            "Legacy archive retains newest complete UTF-8 lines without truncating a character or dropping an exact-boundary line");
    bounded(path, limit);
    for (unsigned i = 0; i < 20; ++i)
      require(append_rotating_log(path, "still logging\n", limit), "No permanent logging freeze after the old file-size cap");
    bounded(path, limit);
    require(read(path).ends_with("still logging\n"), "Latest diagnostics remain present after legacy recovery");
  }
  const auto oversized_line = directory.file(L"long-line.log");
  write(oversized_line, std::string(128, 'a') + "\n");
  require(append_rotating_log(oversized_line, "new\n", 16), "An oversized single legacy line does not block logging");
  require(read(oversized_line) == "new\n" && read(oversized_line + L".1").empty(), "No partial legacy line is archived");
  const auto unfinished = directory.file(L"unfinished-line.log");
  write(unfinished, std::string(128, 'x') + "\nkept1\nkept2\nbroken-\xc3");
  require(append_rotating_log(unfinished, "fresh\n", 25), "Incomplete final legacy record does not block logging");
  require(read(unfinished + L".1") == "kept1\nkept2\n" && read(unfinished) == "fresh\n",
          "Legacy recovery drops an incomplete final UTF-8 record and preserves complete preceding lines");
  bounded(unfinished, 25);
}
void invalid_and_failure(const Directory& directory) {
  const auto path = directory.file(L"invalid.log");
  write(path, "unchanged\n");
  const auto original = read(path);
  for (const auto limit : {std::uint64_t{0}, BridgeLogBytes + 1})
    require(!append_rotating_log(path, "record\n", limit), "Invalid size limit is rejected");
  require(!append_rotating_log(path, {}, 16), "Empty record rejected");
  require(!append_rotating_log(path, "record\n", 3), "Record larger than file limit rejected");
  require(!append_rotating_log(path, std::string(8193, 'x'), BridgeLogBytes), "Record larger than 8192 bytes rejected");
  require(!append_rotating_log({}, "record\n", 16), "Empty path rejected");
  require(!append_rotating_log(std::wstring(40000, L'x'), "record\n", 16), "Oversized path rejected");
  require(read(path) == original && !exists(path + L".1"), "Invalid input leaves existing diagnostics unchanged");
  const auto maximum = directory.file(L"maximum.log");
  const std::string maximum_record = std::string(8191, 'a') + "\n";
  require(append_rotating_log(maximum, maximum_record, 8192) && read(maximum) == maximum_record, "Exactly 8192 record bytes accepted");
  const auto opaque = directory.file(L"opaque.log");
  const std::string opaque_record("a\0\xff\r\n", 5);
  require(append_rotating_log(opaque, opaque_record, 16) && read(opaque) == opaque_record,
          "Logger preserves caller bytes without inserting newlines or imposing an encoding");

  const auto blocked = directory.file(L"blocked.log");
  write(blocked, numbered(0) + numbered(1));
  write(blocked + L".1", "archive\n");
  {
    Handle archive(
        CreateFileW((blocked + L".1").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(archive.value != INVALID_HANDLE_VALUE, "Hold an archive handle which denies replacement");
    const auto started = GetTickCount64();
    require(!append_rotating_log(blocked, numbered(2), 16), "Archive replacement failure is nonfatal");
    require(GetTickCount64() - started < 2000, "Unwritable diagnostics do not freeze their caller");
    require(read(blocked) == numbered(0) + numbered(1) && read(blocked + L".1") == "archive\n",
            "Failed archive replacement leaves current and archived diagnostics unchanged");
  }
  require(append_rotating_log(blocked, numbered(2), 16), "Logging recovers when archive replacement becomes possible");
  bounded(blocked, 16);
  const auto absent_parent = directory.file(L"missing\\no.log");
  require(!append_rotating_log(absent_parent, "record\n", 16), "An unwritable path is a nonfatal failed append");
}

std::string writer_record(unsigned writer, unsigned sequence) {
  char line[32]{};
  std::snprintf(line, sizeof(line), "W%u-%04u|abcdefg\n", writer, sequence);
  return line;
}
int child_writer(const wchar_t* path, unsigned writer, unsigned count, unsigned limit, const wchar_t* event_name) {
  Handle start(OpenEventW(SYNCHRONIZE, FALSE, event_name));
  require(start.value && start.value != INVALID_HANDLE_VALUE && WaitForSingleObject(start.value, 10000) == WAIT_OBJECT_0,
          "Wait for concurrent writer start");
  const auto deadline = GetTickCount64() + 10000;
  for (unsigned i = 0; i < count; ++i) {
    const auto record = writer_record(writer, i);
    while (!append_rotating_log(path, record, limit)) {
      require(GetTickCount64() < deadline, "Concurrent nonblocking writer eventually makes progress");
      Sleep(1);
    }
  }
  return 0;
}
struct Children {
  std::vector<HANDLE> processes;
  ~Children() {
    for (auto process : processes) {
      if (WaitForSingleObject(process, 1000) == WAIT_TIMEOUT) {
        TerminateProcess(process, 1);  // Only a fixture child created by this process.
        WaitForSingleObject(process, 5000);
      }
      CloseHandle(process);
    }
  }
};
void concurrency(const Directory& directory, bool rotate) {
  constexpr unsigned writers = 4;
  const unsigned count = rotate ? 40 : 64, limit = rotate ? 80 : 4096;
  const auto path = directory.file(rotate ? L"concurrent-rotating.log" : L"concurrent-all.log");
  const auto event_name = L"Local\\TaxiCamLogFixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(rotate);
  Handle event(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
  require(event.value != nullptr && GetLastError() != ERROR_ALREADY_EXISTS, "Create private writer-start event");
  std::wstring executable(32768, L'\0');
  const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  require(length && length < executable.size(), "Locate own fixture executable");
  executable.resize(length);
  Children children;
  for (unsigned writer = 0; writer < writers; ++writer) {
    auto command = L"\"" + executable + L"\" --writer \"" + path + L"\" " + std::to_wstring(writer) + L" " + std::to_wstring(count) + L" " +
                   std::to_wstring(limit) + L" \"" + event_name + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                           &process) != FALSE,
            "Start an isolated concurrent fixture writer");
    CloseHandle(process.hThread);
    children.processes.push_back(process.hProcess);
  }
  require(SetEvent(event.value) != FALSE, "Release concurrent writers together");
  for (auto process : children.processes) {
    require(WaitForSingleObject(process, 15000) == WAIT_OBJECT_0, "Concurrent writer finishes without blocking indefinitely");
    DWORD result = 1;
    require(GetExitCodeProcess(process, &result) && result == 0, "Every child completes all accepted records");
  }
  bounded(path, limit);
  std::set<std::string> expected, actual;
  for (unsigned writer = 0; writer < writers; ++writer)
    for (unsigned i = 0; i < count; ++i)
      expected.insert(writer_record(writer, i));
  const auto inspect = [&](const std::wstring& name) {
    const auto bytes = read(name);
    require(bytes.size() % 16 == 0, "Concurrent writes retain whole fixed-width records");
    for (std::size_t offset = 0; offset < bytes.size(); offset += 16) {
      const auto record = bytes.substr(offset, 16);
      require(expected.contains(record), "No interleaved, truncated or corrupt concurrent record");
      require(actual.insert(record).second, "No duplicate accepted record across active and archived logs");
    }
  };
  inspect(path);
  if (rotate) {
    inspect(path + L".1");
    require(actual.size() >= 6 && actual.size() <= 10, "Concurrent rotation retains bounded newest generations");
    require(append_rotating_log(path, "latest record\r\n", limit) && read(path).ends_with("latest record\r\n"),
            "Diagnostics continue after concurrent cap crossings");
    bounded(path, limit);
  } else
    require(actual == expected && !exists(path + L".1"), "No accepted concurrent records are lost at the exact file limit");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc == 7 && std::wcscmp(argv[1], L"--writer") == 0)
      return child_writer(argv[2], static_cast<unsigned>(std::wcstoul(argv[3], nullptr, 10)),
                          static_cast<unsigned>(std::wcstoul(argv[4], nullptr, 10)),
                          static_cast<unsigned>(std::wcstoul(argv[5], nullptr, 10)), argv[6]);
    require(argc == 1, "Only the internal writer mode accepts arguments");
    require(BridgeLogBytes == 8 * 1024 * 1024 && LauncherLogBytes == 4 * 1024 * 1024, "Production log byte limits");
    Directory directory;
    boundary_and_rotation(directory);
    legacy_tail(directory);
    invalid_and_failure(directory);
    concurrency(directory, false);
    concurrency(directory, true);
    std::printf(
        "PASS rotating logs: %u checks; bounds, current diagnostics, complete UTF-8 legacy tails, failure safety and concurrent "
        "processes\n",
        checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL rotating logs: %s (Windows error %lu)\n", error.what(), GetLastError());
    return 1;
  }
}
