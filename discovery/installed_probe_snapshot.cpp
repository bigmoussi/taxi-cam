// Standalone diagnostic for the exact archived 0.7.2 DLL. No target calls/writes.

#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: Windows types must precede the BCrypt/PSAPI declarations.
#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
// clang-format on

#include "../src/scene_handoff.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace {
constexpr wchar_t kInstalledPath[] = L"C:\\XboxGames\\Microsoft Flight Simulator 2024\\Content\\taxi-camera-native.addon64";
constexpr char kExpectedHash[] = "6A6AF4EF6848458268DF6535FC1C5F44E9040B2D3CBA8A89CB2CF0F01EE60080";
constexpr std::uint32_t kInstanceRva = 0x96768;
constexpr std::uint64_t kPreferredBase = 0x180000000;
constexpr unsigned kAttempts = 3;
constexpr std::uint32_t kObjectBudget = 1024;
using Publication = taxi_camera::SceneHandoff::Publication;
static_assert(std::is_standard_layout_v<Publication> && sizeof(Publication) == 128);
// SceneHandoff contains library types. Clang computes its exact layout using
// the same pinned compiler/libc++ as the hash-pinned DLL; no STL field is read.
constexpr std::size_t kPublicationOffset = offsetof(taxi_camera::SceneHandoff, publication_);
static_assert(kPublicationOffset == 312, "The exact 0.7.2 publication layout changed.");
static_assert(kPublicationOffset + sizeof(Publication) <= sizeof(taxi_camera::SceneHandoff));
static_assert(kAttempts * 2 * sizeof(Publication) <= kObjectBudget);
using PublicationBytes = std::array<std::uint8_t, sizeof(Publication)>;

struct Handle {
  HANDLE value = nullptr;
  ~Handle() {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
};
struct Result {
  struct HeaderDifference {
    std::uint32_t offset = 0;
    std::uint8_t disk = 0, loaded = 0;
  };
  bool verified = false, stable = false, valid = false;
  std::uint32_t pid = 0, object_bytes = 0, pointer_bytes = 0, metadata_bytes = 0, read_failures = 0, attempts = 0;
  DWORD windows_error = 0;
  std::uint64_t scene_epoch = 0, capture_sequence = 0;
  std::array<std::uint64_t, 2> entry_ids{}, resource_ids{}, device_epochs{}, resource_generations{};
  std::array<HeaderDifference, 16> header_differences{};
  std::uint32_t header_difference_count = 0;
  bool image_base_validated = false;
  const char* error = "not_started";
};
bool fail(Result& result, const char* error, DWORD windows_error = 0) {
  result.error = error;
  result.windows_error = windows_error;
  return false;
}
bool readable(DWORD access) {
  if (access & (PAGE_GUARD | PAGE_NOACCESS))
    return false;
  const auto basic = access & 0xff;
  return basic == PAGE_READONLY || basic == PAGE_READWRITE || basic == PAGE_WRITECOPY || basic == PAGE_EXECUTE_READ ||
         basic == PAGE_EXECUTE_READWRITE || basic == PAGE_EXECUTE_WRITECOPY;
}
bool bounded(std::uintptr_t address, std::size_t size) {
  return address != 0 && size != 0 && size <= UINTPTR_MAX - address;
}
bool exact_read(HANDLE process, std::uintptr_t address, void* destination, std::size_t size, DWORD type, void* allocation, Result& result) {
  if (!bounded(address, size))
    return fail(result, "read_bounds");
  MEMORY_BASIC_INFORMATION region{};
  if (VirtualQueryEx(process, reinterpret_cast<const void*>(address), &region, sizeof(region)) != sizeof(region))
    return fail(result, "region_query_failed", GetLastError());
  const auto start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
  if (region.State != MEM_COMMIT || region.Type != type || (allocation && region.AllocationBase != allocation) ||
      !readable(region.Protect) || start > address || region.RegionSize > UINTPTR_MAX - start || address - start >= region.RegionSize ||
      size > region.RegionSize - (address - start))
    return fail(result, "region_refused");
  SIZE_T copied = 0;
  if (!ReadProcessMemory(process, reinterpret_cast<const void*>(address), destination, size, &copied) || copied != size) {
    ++result.read_failures;
    return fail(result, "exact_read_failed", GetLastError());
  }
  return true;
}
std::uint64_t number(const PublicationBytes& bytes, std::size_t offset) {
  std::uint64_t result = 0;
  std::memcpy(&result, bytes.data() + offset, sizeof(result));
  return result;
}
bool publication_values(const PublicationBytes& bytes, Result& result) {
  if (bytes[offsetof(Publication, valid)] != 1)
    return false;
  const auto scene = number(bytes, offsetof(Publication, scene_epoch));
  const auto sequence = number(bytes, offsetof(Publication, sequence));
  if (!scene || !sequence || !number(bytes, offsetof(Publication, device_key)) ||
      !number(bytes, offsetof(Publication, manager) + offsetof(taxi_camera::SceneManagerIdentity, identity)) ||
      !number(bytes, offsetof(Publication, manager) + offsetof(taxi_camera::SceneManagerIdentity, generation)))
    return false;
  Result candidate;
  std::array<std::uint64_t, 2> handles{};
  for (unsigned i = 0; i < 2; ++i) {
    candidate.entry_ids[i] = number(bytes, offsetof(Publication, entry_ids) + i * sizeof(std::uint64_t));
    handles[i] = number(bytes, offsetof(Publication, handles) + i * sizeof(std::uint64_t));
    const auto resource = offsetof(Publication, resources) + i * sizeof(taxi_camera::SceneResourceIdentity);
    candidate.device_epochs[i] = number(bytes, resource + offsetof(taxi_camera::SceneResourceIdentity, device_epoch));
    candidate.resource_ids[i] = number(bytes, resource + offsetof(taxi_camera::SceneResourceIdentity, resource_id));
    candidate.resource_generations[i] = number(bytes, resource + offsetof(taxi_camera::SceneResourceIdentity, generation));
    if (!candidate.entry_ids[i] || !handles[i] || !candidate.device_epochs[i] || !candidate.resource_ids[i] ||
        !candidate.resource_generations[i])
      return false;
  }
  if (candidate.entry_ids[0] == candidate.entry_ids[1] || handles[0] == handles[1] ||
      candidate.resource_ids[0] == candidate.resource_ids[1] || candidate.device_epochs[0] != candidate.device_epochs[1])
    return false;
  result.scene_epoch = scene;
  result.capture_sequence = sequence;
  result.entry_ids = candidate.entry_ids;
  result.resource_ids = candidate.resource_ids;
  result.device_epochs = candidate.device_epochs;
  result.resource_generations = candidate.resource_generations;
  return true;
}
struct SnapshotReader {
  virtual ~SnapshotReader() = default;
  virtual bool pointer(std::uint64_t&) = 0;
  virtual bool publication(std::uint64_t, PublicationBytes&) = 0;
};
void capture(SnapshotReader& reader, Result& result) {
  for (unsigned attempt = 0; attempt < kAttempts; ++attempt) {
    ++result.attempts;
    result.stable = false;
    std::uint64_t first = 0, last = 0;
    result.pointer_bytes += 8;
    if (!reader.pointer(first)) {
      fail(result, "instance_read_failed");
      return;
    }
    if (!first) {
      fail(result, "instance_unavailable");
      return;
    }
    if ((first & 7) || !bounded(first, kPublicationOffset + sizeof(Publication))) {
      fail(result, "instance_bounds");
      return;
    }
    PublicationBytes initial{}, repeated{};
    if (result.object_bytes > kObjectBudget - initial.size()) {
      fail(result, "object_budget");
      return;
    }
    result.object_bytes += initial.size();
    if (!reader.publication(first, initial)) {
      fail(result, "publication_read_failed");
      return;
    }
    result.object_bytes += repeated.size();
    if (!reader.publication(first, repeated)) {
      fail(result, "publication_recheck_failed");
      return;
    }
    result.pointer_bytes += 8;
    if (!reader.pointer(last)) {
      fail(result, "instance_recheck_failed");
      return;
    }
    if (first != last || initial != repeated) {
      fail(result, "publication_changed");
      continue;
    }
    result.stable = true;
    if (!publication_values(initial, result)) {
      fail(result, "publication_unavailable_or_invalid");
      continue;
    }
    result.valid = true;
    result.error = "";
    return;
  }
}
struct RemoteReader final : SnapshotReader {
  HANDLE process;
  std::uintptr_t base;
  Result& result;
  RemoteReader(HANDLE selected, std::uintptr_t module, Result& output) : process(selected), base(module), result(output) {}
  bool pointer(std::uint64_t& value) override {
    return exact_read(process, base + kInstanceRva, &value, sizeof(value), MEM_IMAGE, reinterpret_cast<void*>(base), result);
  }
  bool publication(std::uint64_t instance, PublicationBytes& bytes) override {
    return exact_read(process, instance + kPublicationOffset, bytes.data(), bytes.size(), MEM_PRIVATE, nullptr, result);
  }
};

bool user_token(HANDLE process, std::vector<std::uint8_t>& bytes) {
  Handle token;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token.value))
    return false;
  DWORD required = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &required);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !required || required > 65536)
    return false;
  bytes.resize(required);
  return GetTokenInformation(token.value, TokenUser, bytes.data(), required, &required) != FALSE;
}
std::wstring basename(const wchar_t* text) {
  const std::wstring path(text);
  const auto separator = path.find_last_of(L"/\\");
  return separator == std::wstring::npos ? path : path.substr(separator + 1);
}
bool equal(const wchar_t* left, const wchar_t* right) {
  return CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}
bool file_id(HANDLE file, FILE_ID_INFO& id) {
  return GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) != FALSE;
}
bool same_file(HANDLE first, const wchar_t* path) {
  Handle second;
  second.value = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  FILE_ID_INFO left{}, right{};
  return second.value != INVALID_HANDLE_VALUE && file_id(first, left) && file_id(second.value, right) &&
         left.VolumeSerialNumber == right.VolumeSerialNumber &&
         std::memcmp(left.FileId.Identifier, right.FileId.Identifier, sizeof(left.FileId.Identifier)) == 0;
}
bool hash_matches(HANDLE file) {
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart != 1115136)
    return false;
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
    return false;
  bool valid = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
  std::array<std::uint8_t, 16384> buffer{};
  std::uint64_t read = 0;
  while (valid && read < static_cast<std::uint64_t>(size.QuadPart)) {
    DWORD copied = 0;
    const auto requested = static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), size.QuadPart - read));
    valid = ReadFile(file, buffer.data(), requested, &copied, nullptr) && copied == requested &&
            BCryptHashData(hash, buffer.data(), copied, 0) >= 0;
    read += copied;
  }
  std::array<std::uint8_t, 32> digest{};
  valid = valid && BCryptFinishHash(hash, digest.data(), digest.size(), 0) >= 0;
  if (hash)
    BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned i = 0; valid && i < digest.size(); ++i)
    valid = kExpectedHash[i * 2] == hex[digest[i] >> 4] && kExpectedHash[i * 2 + 1] == hex[digest[i] & 15];
  return valid;
}

bool compare_loaded_headers(const std::array<std::uint8_t, 4096>& disk,
                            std::array<std::uint8_t, 4096> loaded,
                            std::size_t extent,
                            std::size_t image_base_offset,
                            std::uint64_t actual_base,
                            Result& result) {
  if (!actual_base || extent > disk.size() || image_base_offset > extent || sizeof(actual_base) > extent - image_base_offset)
    return fail(result, "image_base_field_bounds");
  std::uint64_t loaded_base = 0;
  std::memcpy(&loaded_base, loaded.data() + image_base_offset, sizeof(loaded_base));
  if (loaded_base != actual_base)
    return fail(result, "loaded_image_base_mismatch");
  result.image_base_validated = true;
  // The observed loader updates precisely OptionalHeader.ImageBase. Accept only
  // its verified actual module base, then restore those eight local bytes for
  // the otherwise exact disk/header comparison. No other byte is normalized.
  std::memcpy(loaded.data() + image_base_offset, disk.data() + image_base_offset, sizeof(loaded_base));
  for (std::size_t i = 0; i < extent; ++i) {
    if (loaded[i] == disk[i])
      continue;
    if (result.header_difference_count < result.header_differences.size())
      result.header_differences[result.header_difference_count] = {static_cast<std::uint32_t>(i), disk[i], loaded[i]};
    ++result.header_difference_count;
  }
  if (result.header_difference_count)
    return fail(result, "loaded_headers_mismatch");
  return true;
}

bool module_headers(HANDLE process, HMODULE module, HANDLE file, Result& result) {
  LARGE_INTEGER zero{};
  std::array<std::uint8_t, 4096> disk{}, loaded{};
  DWORD copied = 0;
  if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) || !ReadFile(file, disk.data(), disk.size(), &copied, nullptr) ||
      copied != disk.size())
    return fail(result, "file_headers_failed", GetLastError());
  IMAGE_DOS_HEADER dos{};
  std::memcpy(&dos, disk.data(), sizeof(dos));
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 || std::uint32_t(dos.e_lfanew) > disk.size() - sizeof(IMAGE_NT_HEADERS64))
    return fail(result, "file_dos_invalid");
  IMAGE_NT_HEADERS64 nt{};
  std::memcpy(&nt, disk.data() + dos.e_lfanew, sizeof(nt));
  const auto section_offset = static_cast<std::size_t>(dos.e_lfanew) + 24 + nt.FileHeader.SizeOfOptionalHeader;
  const auto section_bytes = nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
  if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt.OptionalHeader.ImageBase != kPreferredBase || !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 32 ||
      section_offset > disk.size() || section_bytes > disk.size() - section_offset)
    return fail(result, "file_nt_invalid");
  MODULEINFO info{};
  const auto base = reinterpret_cast<std::uintptr_t>(module);
  if (!K32GetModuleInformation(process, module, &info, sizeof(info)) || info.lpBaseOfDll != module ||
      info.SizeOfImage != nt.OptionalHeader.SizeOfImage || !bounded(base, info.SizeOfImage) || kInstanceRva > info.SizeOfImage ||
      8 > info.SizeOfImage - kInstanceRva)
    return fail(result, "module_metadata_mismatch", GetLastError());
  bool slot = false;
  for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
    IMAGE_SECTION_HEADER section{};
    std::memcpy(&section, disk.data() + section_offset + i * sizeof(section), sizeof(section));
    const auto size = section.Misc.VirtualSize;
    if (section.VirtualAddress > info.SizeOfImage || size > info.SizeOfImage - section.VirtualAddress)
      return fail(result, "section_bounds");
    if (kInstanceRva >= section.VirtualAddress && kInstanceRva - section.VirtualAddress < size &&
        8 <= size - (kInstanceRva - section.VirtualAddress))
      slot = (section.Characteristics & (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE)) == (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE) &&
             !(section.Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_DISCARDABLE));
  }
  if (!slot)
    return fail(result, "instance_section_refused");
  const auto extent = section_offset + section_bytes;
  result.metadata_bytes += extent;
  if (!exact_read(process, base, loaded.data(), extent, MEM_IMAGE, module, result))
    return fail(result, "loaded_headers_read_failed", result.windows_error);
  const auto image_base_offset =
      static_cast<std::size_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + offsetof(IMAGE_OPTIONAL_HEADER64, ImageBase);
  return compare_loaded_headers(disk, loaded, extent, image_base_offset, base, result);
}

Result inspect(DWORD pid) {
  Result result;
  result.pid = pid;
  Handle process;
  process.value = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!process.value) {
    fail(result, "process_open_failed", GetLastError());
    return result;
  }
  std::array<wchar_t, 32768> path{};
  DWORD length = path.size();
  if (!QueryFullProcessImageNameW(process.value, 0, path.data(), &length) || !length || length >= path.size() ||
      !equal(basename(path.data()).c_str(), L"FlightSimulator2024.exe")) {
    fail(result, "wrong_process", GetLastError());
    return result;
  }
  std::vector<std::uint8_t> self_user, target_user;
  if (!user_token(GetCurrentProcess(), self_user) || !user_token(process.value, target_user) ||
      !EqualSid(reinterpret_cast<const TOKEN_USER*>(self_user.data())->User.Sid,
                reinterpret_cast<const TOKEN_USER*>(target_user.data())->User.Sid)) {
    fail(result, "same_user_check_failed", GetLastError());
    return result;
  }
  // Keep the exact expected file open without write/delete sharing throughout
  // the hash/header/read checks. Xbox path aliases must match its Windows ID.
  Handle expected;
  expected.value = CreateFileW(kInstalledPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (expected.value == INVALID_HANDLE_VALUE || !hash_matches(expected.value)) {
    fail(result, "installed_hash_mismatch", GetLastError());
    return result;
  }
  std::array<HMODULE, 512> modules{};
  DWORD needed = 0;
  if (!K32EnumProcessModulesEx(process.value, modules.data(), sizeof(modules), &needed, LIST_MODULES_64BIT) || !needed ||
      needed > sizeof(modules) || needed % sizeof(HMODULE)) {
    fail(result, "module_inventory_bounds", GetLastError());
    return result;
  }
  HMODULE selected = nullptr;
  for (unsigned i = 0; i < needed / sizeof(HMODULE); ++i) {
    std::array<wchar_t, 256> name{};
    if (!K32GetModuleBaseNameW(process.value, modules[i], name.data(), name.size())) {
      fail(result, "module_name_failed", GetLastError());
      return result;
    }
    if (!equal(name.data(), L"taxi-camera-native.addon64"))
      continue;
    if (selected) {
      fail(result, "ambiguous_addon_module");
      return result;
    }
    const auto size = K32GetModuleFileNameExW(process.value, modules[i], path.data(), path.size());
    if (!size || size >= path.size() || !same_file(expected.value, path.data())) {
      fail(result, "addon_file_identity_mismatch", GetLastError());
      return result;
    }
    selected = modules[i];
  }
  if (!selected || !module_headers(process.value, selected, expected.value, result)) {
    if (!selected)
      fail(result, "exact_addon_not_loaded");
    return result;
  }
  result.verified = true;
  RemoteReader reader(process.value, reinterpret_cast<std::uintptr_t>(selected), result);
  capture(reader, result);
  return result;
}

unsigned checks = 0;
void require(bool value) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "fixture failure at check %u\n", checks);
    std::exit(1);
  }
}
struct Fixture final : SnapshotReader {
  Publication value{};
  unsigned reads = 0, fail_at = 0, change_at = 0;
  bool change_pointer = false, unavailable = false;
  Fixture() {
    value.valid = true;
    value.scene_epoch = 3;
    value.sequence = 5;
    value.device_key = 0x20000;
    value.manager = {0x30000, 7};
    value.entry_ids = {1001, 1002};
    value.handles = {0x40000, 0x50000};
    value.resources = {{{11, 11801, 21}, {11, 11802, 22}}};
  }
  bool pointer(std::uint64_t& output) override {
    ++reads;
    output = unavailable ? 0 : (change_pointer && reads % 4 == 0 ? 0x2000 : 0x1000);
    return reads != fail_at;
  }
  bool publication(std::uint64_t instance, PublicationBytes& output) override {
    require(instance == 0x1000);
    ++reads;
    std::memcpy(output.data(), &value, sizeof(value));
    if (reads == change_at || (change_at == UINT_MAX && reads % 4 == 3))
      output[offsetof(Publication, sequence)] ^= 1;
    if (reads == fail_at) {
      // A short reader may already have copied bytes; no result can expose them.
      std::fill(output.begin() + 8, output.end(), 0);
      return false;
    }
    return true;
  }
  Result run() {
    Result result;
    capture(*this, result);
    require(result.object_bytes <= 768 && result.pointer_bytes <= 48 && result.attempts <= 3);
    if (!result.valid)
      require(result.entry_ids == std::array<std::uint64_t, 2>{} && result.resource_ids == std::array<std::uint64_t, 2>{} &&
              result.scene_epoch == 0);
    return result;
  }
};
void self_test() {
  {
    std::array<std::uint8_t, 4096> disk{}, loaded{};
    constexpr auto offset = 120 + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + offsetof(IMAGE_OPTIONAL_HEADER64, ImageBase);
    static_assert(offset == 168);
    constexpr std::uint64_t actual = 0x7ffeb8cc0000;
    std::memcpy(disk.data() + offset, &kPreferredBase, sizeof(kPreferredBase));
    loaded = disk;
    std::memcpy(loaded.data() + offset, &actual, sizeof(actual));
    Result result;
    require(compare_loaded_headers(disk, loaded, 944, offset, actual, result) && result.image_base_validated &&
            result.header_difference_count == 0);
    result = {};
    require(!compare_loaded_headers(disk, loaded, 944, offset, actual + 65536, result) && !result.image_base_validated);
    result = {};
    require(!compare_loaded_headers(disk, disk, 944, offset, actual, result) && !result.image_base_validated);
    result = {};
    require(compare_loaded_headers(disk, disk, 944, offset, kPreferredBase, result) && result.image_base_validated);
    for (const auto changed : {0u, 120u, 167u, 176u, 943u}) {
      auto modified = loaded;
      modified[changed] ^= 1;
      result = {};
      require(!compare_loaded_headers(disk, modified, 944, offset, actual, result) && result.image_base_validated &&
              result.header_difference_count == 1 && result.header_differences[0].offset == changed);
    }
    result = {};
    require(!compare_loaded_headers(disk, loaded, offset + 7, offset, actual, result));
    result = {};
    require(!compare_loaded_headers(disk, loaded, 4097, offset, actual, result));
  }
  {
    Fixture fixture;
    const auto result = fixture.run();
    require(result.valid && result.stable && result.entry_ids == fixture.value.entry_ids && result.resource_ids[0] == 11801 &&
            result.resource_ids[1] == 11802 && result.object_bytes == 256 && result.pointer_bytes == 16 && result.attempts == 1);
  }
  for (unsigned failed = 1; failed <= 4; ++failed) {
    Fixture fixture;
    fixture.fail_at = failed;
    require(!fixture.run().valid);
  }
  for (unsigned scenario = 0; scenario < 15; ++scenario) {
    Fixture fixture;
    switch (scenario) {
      case 0:
        fixture.value.valid = false;
        break;
      case 1:
        fixture.value.scene_epoch = 0;
        break;
      case 2:
        fixture.value.sequence = 0;
        break;
      case 3:
        fixture.value.device_key = 0;
        break;
      case 4:
        fixture.value.manager.identity = 0;
        break;
      case 5:
        fixture.value.manager.generation = 0;
        break;
      case 6:
        fixture.value.entry_ids[0] = 0;
        break;
      case 7:
        fixture.value.entry_ids[1] = fixture.value.entry_ids[0];
        break;
      case 8:
        fixture.value.handles[1] = fixture.value.handles[0];
        break;
      case 9:
        fixture.value.resources[0].resource_id = 0;
        break;
      case 10:
        fixture.value.resources[1].resource_id = fixture.value.resources[0].resource_id;
        break;
      case 11:
        fixture.value.resources[1].device_epoch = 12;
        break;
      case 12:
        fixture.value.resources[0].generation = 0;
        break;
      case 13:
        fixture.change_pointer = true;
        break;
      case 14:
        fixture.change_at = UINT_MAX;
        break;
    }
    const auto result = fixture.run();
    require(!result.valid && result.attempts == 3 && result.object_bytes == 768 && result.pointer_bytes == 48);
  }
  {
    Fixture fixture;
    fixture.change_at = 3;
    const auto result = fixture.run();
    require(result.valid && result.attempts == 2 && result.object_bytes == 512);
  }
  {
    Fixture fixture;
    fixture.unavailable = true;
    const auto result = fixture.run();
    require(!result.valid && result.object_bytes == 0 && result.pointer_bytes == 8);
  }
  std::printf("{\"self_test_passed\":true,\"checks\":%u,\"publication_offset\":%zu,\"publication_size\":%zu,\"object_budget\":%u}\n",
              checks, kPublicationOffset, sizeof(Publication), kObjectBudget);
}
void print(const Result& result) {
  // Every string is a fixed diagnostic label; no paths or opaque addresses.
  std::printf(
      "{\"pid\":%u,\"addon_version\":\"0.7.2\",\"verified\":%s,\"stable\":%s,\"valid\":%s,"
      "\"attempts\":%u,\"object_bytes\":%u,\"pointer_bytes\":%u,\"metadata_bytes\":%u,\"read_failures\":%u,"
      "\"scene_epoch\":%llu,\"capture_sequence\":%llu,\"entry_ids\":[%llu,%llu],\"resource_ids\":[%llu,%llu],"
      "\"device_epochs\":[%llu,%llu],\"resource_generations\":[%llu,%llu],\"error\":\"%s\",\"windows_error\":%lu,"
      "\"image_base_validated\":%s,\"header_difference_count\":%u,\"header_differences\":[",
      result.pid, result.verified ? "true" : "false", result.stable ? "true" : "false", result.valid ? "true" : "false", result.attempts,
      result.object_bytes, result.pointer_bytes, result.metadata_bytes, result.read_failures, result.scene_epoch, result.capture_sequence,
      result.entry_ids[0], result.entry_ids[1], result.resource_ids[0], result.resource_ids[1], result.device_epochs[0],
      result.device_epochs[1], result.resource_generations[0], result.resource_generations[1], result.error, result.windows_error,
      result.image_base_validated ? "true" : "false", result.header_difference_count);
  for (std::size_t i = 0; i < std::min<std::size_t>(result.header_difference_count, result.header_differences.size()); ++i) {
    const auto& difference = result.header_differences[i];
    std::printf("%s{\"offset\":%u,\"disk\":%u,\"loaded\":%u}", i ? "," : "", difference.offset, static_cast<unsigned>(difference.disk),
                static_cast<unsigned>(difference.loaded));
  }
  std::puts("]}");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) {
    self_test();
    return 0;
  }
  DWORD pid = 0;
  if (argc == 3 && std::strcmp(argv[1], "--pid") == 0) {
    const auto end = argv[2] + std::strlen(argv[2]);
    const auto parsed = std::from_chars(argv[2], end, pid);
    if (parsed.ec == std::errc{} && parsed.ptr == end && pid != 0) {
      const auto result = inspect(pid);
      print(result);
      return result.valid ? 0 : 1;
    }
  }
  std::puts("{\"valid\":false,\"error\":\"usage: --self-test or --pid <explicit-nonzero-pid>\"}");
  return 2;
}
