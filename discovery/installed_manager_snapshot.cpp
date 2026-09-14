// Standalone diagnostic for the exact installed 0.7.4 DLL. No target calls/writes.

#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: Windows types must precede the BCrypt/PSAPI declarations.
#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
// clang-format on

#include "../src/scene_handoff.hpp"
#include "../src/scene_capture_manager.hpp"

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
constexpr char kExpectedHash[] = "93B1E37009E2C101ED065BDDD87383E308F4A60D7932F0757A3196907CBAF876";
constexpr std::uint32_t kInstanceRva = 0x9d778;
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
  if (!GetFileSizeEx(file, &size) || size.QuadPart != 1154560)
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

using Manager = taxi_camera::SceneCaptureManager;
using List = Manager::List;
using Device = Manager::Device;
using Tracker = taxi_camera::source_state::Tracker;
struct ManagerCounts {
  unsigned registered = 0, stable = 0, changed = 0, invalid = 0, touched_invalid = 0, touched_valid = 0;
  unsigned unknown_sources = 0, rt_sources = 0, other_sources = 0, selected_models[2]{99,99};
  std::uint64_t draws = 0, submissions = 0, tails = 0;
} counts;
bool manager_capture(HANDLE process, std::uintptr_t base, Result& result) {
  std::uint64_t instance = 0, again = 0;
  if (!exact_read(process, base+kInstanceRva, &instance, 8, MEM_IMAGE, reinterpret_cast<void*>(base), result) ||
      !instance || !bounded(instance,sizeof(Manager))) return fail(result,"manager_pointer");
  const auto field = [&](std::size_t offset, void* destination, std::size_t bytes) {
    if (offset > sizeof(Manager) || bytes > sizeof(Manager)-offset || result.object_bytes > 524288-bytes)
      return fail(result,"manager_field_budget");
    result.object_bytes += static_cast<unsigned>(bytes);
    return exact_read(process, instance+offset,destination,bytes,MEM_PRIVATE,nullptr,result);
  };
  for (std::size_t index=0;index<Manager::MaximumLists;++index) {
    const auto offset=offsetof(Manager,lists_)+index*sizeof(List);
    std::uint64_t native=0;
    if (!field(offset+offsetof(List,native),&native,8)) return false;
    if (!native) continue;
    ++counts.registered;
    struct Small { std::uint64_t generation,recording,count; bool invalid,touched; } a{},b{};
    const auto read_small=[&](Small& value) {
      return field(offset+offsetof(List,object_generation),&value.generation,8) &&
             field(offset+offsetof(List,recording),&value.recording,8) &&
             field(offset+offsetof(List,source_effects)+offsetof(taxi_camera::source_state::Recording,count),&value.count,8) &&
             field(offset+offsetof(List,source_effects)+offsetof(taxi_camera::source_state::Recording,invalid),&value.invalid,1) &&
             field(offset+offsetof(List,source_touched),&value.touched,1);
    };
    if (!read_small(a)||!read_small(b)) return false;
    if(a.generation!=b.generation||a.recording!=b.recording||a.count!=b.count||a.invalid!=b.invalid||a.touched!=b.touched) {++counts.changed;continue;}
    ++counts.stable;
    counts.invalid+=a.invalid;
    counts.touched_invalid+=a.invalid&&a.touched;
    counts.touched_valid+=!a.invalid&&a.touched;
  }
  for (std::size_t d=0;d<Manager::MaximumDevices;++d) {
    std::uint64_t key=0;
    const auto device=offsetof(Manager,devices_)+d*sizeof(Device);
    if(!field(device+offsetof(Device,key),&key,8))return false;
    if(!key)continue;
    for(std::size_t index=0;index<Tracker::capacity;++index){
      Tracker::Slot value{},check{};
      const auto offset=device+offsetof(Device,source_states)+offsetof(Tracker,sources_)+index*sizeof(value);
      if(!field(offset,&value,sizeof(value))||!field(offset,&check,sizeof(check)))return false;
      if(!value.key.handle||value.key!=check.key||value.state.model!=check.state.model)continue;
      auto model=static_cast<unsigned>(value.state.model);
      counts.unknown_sources+=model==0; counts.rt_sources+=model==1||model==2;counts.other_sources+=model==3;
      if(value.key.generation==29089)counts.selected_models[0]=model;
      if(value.key.generation==29090)counts.selected_models[1]=model;
    }
  }
  if(!field(offsetof(Manager,stats_)+offsetof(Manager::Statistics,source_draws),&counts.draws,8)||
     !field(offsetof(Manager,stats_)+offsetof(Manager::Statistics,submissions),&counts.submissions,8)||
     !field(offsetof(Manager,stats_)+offsetof(Manager::Statistics,tail_submissions),&counts.tails,8))return false;
  if(!exact_read(process,base+kInstanceRva,&again,8,MEM_IMAGE,reinterpret_cast<void*>(base),result)||instance!=again)
    return fail(result,"manager_changed");
  result.valid=true;result.stable=true;result.error="none";return true;
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
  manager_capture(process.value, reinterpret_cast<std::uintptr_t>(selected), result);
  return result;
}


} // namespace
int main(int argc,char**argv){
  DWORD pid=0;
  if(argc!=3||std::strcmp(argv[1],"--pid"))return 2;
  auto p=std::from_chars(argv[2],argv[2]+std::strlen(argv[2]),pid);
  if(p.ec!=std::errc{}||!pid)return 2;
  const auto result=inspect(pid);
  std::printf("{\"valid\":%s,\"verified\":%s,\"error\":\"%s\",\"windows_error\":%lu,\"object_bytes\":%u,\"registered\":%u,\"stable_lists\":%u,\"changed_lists\":%u,\"invalid_lists\":%u,\"touched_invalid_lists\":%u,\"touched_valid_lists\":%u,\"unknown_sources\":%u,\"rt_sources\":%u,\"other_sources\":%u,\"published_models\":[%u,%u],\"source_draws\":%llu,\"submissions\":%llu,\"tail_submissions\":%llu}\n",result.valid?"true":"false",result.verified?"true":"false",result.error,result.windows_error,result.object_bytes,counts.registered,counts.stable,counts.changed,counts.invalid,counts.touched_invalid,counts.touched_valid,counts.unknown_sources,counts.rt_sources,counts.other_sources,counts.selected_models[0],counts.selected_models[1],counts.draws,counts.submissions,counts.tails);
  return result.valid?0:1;
}
