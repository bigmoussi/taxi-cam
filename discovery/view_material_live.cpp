// Explicit-PID, same-user read-only diagnostic. No engine or COM invocation.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off: Win32 types must precede PSAPI declarations.
#include <windows.h>
#include <psapi.h>
// clang-format on
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../native-camera/profile.hpp"
#include "aircraft_inventory.hpp"
#include "image_inventory.hpp"
#include "view_material_inventory.hpp"
namespace d = taxi_camera::discovery;
namespace n = taxi_camera::native_camera;
namespace {
struct Handle {
  HANDLE value = nullptr;
  ~Handle() {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
};
bool readable(DWORD p) {
  if (p & (PAGE_GUARD | PAGE_NOACCESS))
    return false;
  p &= 255;
  return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
         p == PAGE_EXECUTE_WRITECOPY;
}
bool same_user(HANDLE process) {
  const auto user = [](HANDLE p, std::vector<unsigned char>& out) {
    Handle token;
    if (!OpenProcessToken(p, TOKEN_QUERY, &token.value))
      return false;
    DWORD bytes = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
    if (!bytes || bytes > 65536 || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
      return false;
    out.resize(bytes);
    return GetTokenInformation(token.value, TokenUser, out.data(), bytes, &bytes) != FALSE;
  };
  std::vector<unsigned char> a, b;
  return user(process, a) && user(GetCurrentProcess(), b) &&
         EqualSid(reinterpret_cast<TOKEN_USER*>(a.data())->User.Sid, reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid);
}
class Image final : public d::ImageReader {
 public:
  Image(HANDLE p, HMODULE b, std::uint32_t size) : process(p), base(reinterpret_cast<std::uintptr_t>(b)), limit(size) {}
  d::ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    if (!maximum || rva >= limit || maximum > limit - rva || limit > UINTPTR_MAX - base)
      return {};
    MEMORY_BASIC_INFORMATION m{};
    const auto address = base + rva;
    if (VirtualQueryEx(process, reinterpret_cast<void*>(address), &m, sizeof(m)) != sizeof(m))
      return {};
    const auto start = reinterpret_cast<std::uintptr_t>(m.BaseAddress);
    if (start > address || m.RegionSize > UINTPTR_MAX - start || address - start >= m.RegionSize)
      return {};
    const auto amount = static_cast<std::uint32_t>(std::min<std::size_t>(maximum, m.RegionSize - (address - start)));
    return {amount, m.State == MEM_COMMIT && m.Type == MEM_IMAGE && reinterpret_cast<std::uintptr_t>(m.AllocationBase) == base &&
                        readable(m.Protect)};
  }
  bool read(std::uint32_t rva, void* out, std::size_t size) override {
    if (!out || !size || size > UINT32_MAX || size > 4 * 1024 * 1024 - attempted)
      return false;
    attempted += size;
    std::size_t done = 0;
    while (done < size) {
      if (done > UINT32_MAX - rva)
        return false;
      const auto w = query(rva + static_cast<std::uint32_t>(done), static_cast<std::uint32_t>(size - done));
      if (!w.readable || !w.size)
        return false;
      SIZE_T actual = 0;
      if (!ReadProcessMemory(process, reinterpret_cast<void*>(base + rva + done), static_cast<unsigned char*>(out) + done, w.size,
                             &actual) ||
          actual != w.size)
        return false;
      done += w.size;
    }
    return true;
  }
  HANDLE process;
  std::uintptr_t base;
  std::uint32_t limit;
  std::size_t attempted = 0;
};
class Objects final : public taxi_camera::engine_camera::MemoryReader, public d::AircraftObjectReader {
 public:
  explicit Objects(HANDLE p) : process(p) {}
  bool read(std::uint64_t address, void* out, std::size_t size) override {
    if (!out || !address || !size || size > 16 || size > UINTPTR_MAX - address || size > 65536 - attempted)
      return false;
    attempted += size;
    MEMORY_BASIC_INFORMATION m{};
    if (VirtualQueryEx(process, reinterpret_cast<void*>(address), &m, sizeof(m)) != sizeof(m))
      return false;
    const auto start = reinterpret_cast<std::uintptr_t>(m.BaseAddress);
    if (m.State != MEM_COMMIT || m.Type != MEM_PRIVATE || !readable(m.Protect) || start > address || m.RegionSize > UINTPTR_MAX - start ||
        address - start >= m.RegionSize || size > m.RegionSize - (address - start))
      return false;
    SIZE_T actual = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(address), out, size, &actual) || actual != size) {
      ++failures;
      return false;
    }
    return true;
  }
  HANDLE process;
  std::size_t attempted = 0;
  unsigned failures = 0;
};
bool section(const d::Inventory& image, std::uint32_t rva, std::uint32_t size, bool writable) {
  for (const auto& s : image.sections) {
    if (rva < s.rva || std::uint64_t(s.rva) + s.size > image.image_size || rva - s.rva >= s.size || size > s.size - (rva - s.rva))
      continue;
    return (s.flags & 0x40000000u) && !(s.flags & 0x22000000u) && (((s.flags & 0x80000000u) != 0) == writable);
  }
  return false;
}
int failure(const char* text) {
  std::printf("{\"complete\":false,\"error\":\"%s\",\"windows_error\":%lu}\n", text, GetLastError());
  return 1;
}
int inspect(DWORD pid) {
  Handle process;
  process.value = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!process.value)
    return failure("open_process");
  if (!same_user(process.value))
    return failure("same_user_required");
  std::array<wchar_t, 32768> path{}, module_path{};
  DWORD length = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process.value, 0, path.data(), &length) || !length || length >= path.size())
    return failure("process_path");
  const auto slash = std::wcsrchr(path.data(), L'\\');
  if (_wcsicmp(slash ? slash + 1 : path.data(), L"FlightSimulator2024.exe"))
    return failure("wrong_process");
  HMODULE module = nullptr;
  DWORD required = 0;
  if (!K32EnumProcessModulesEx(process.value, &module, sizeof(module), &required, LIST_MODULES_64BIT) || required < sizeof(module) ||
      !module)
    return failure("main_module");
  const auto module_length = K32GetModuleFileNameExW(process.value, module, module_path.data(), static_cast<DWORD>(module_path.size()));
  if (!module_length || module_length >= module_path.size())
    return failure("module_path");
  // The first enumerated module is not trusted by position alone.
  Handle path_file, module_file;
  path_file.value = CreateFileW(path.data(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
  module_file.value = CreateFileW(module_path.data(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
  FILE_ID_INFO a{}, b{};
  if (path_file.value == INVALID_HANDLE_VALUE || module_file.value == INVALID_HANDLE_VALUE ||
      !GetFileInformationByHandleEx(path_file.value, FileIdInfo, &a, sizeof(a)) ||
      !GetFileInformationByHandleEx(module_file.value, FileIdInfo, &b, sizeof(b)) || a.VolumeSerialNumber != b.VolumeSerialNumber ||
      std::memcmp(&a.FileId, &b.FileId, sizeof(a.FileId)))
    return failure("main_module_identity");
  MODULEINFO info{};
  if (!K32GetModuleInformation(process.value, module, &info, sizeof(info)) || info.lpBaseOfDll != module || info.SizeOfImage != 235963904)
    return failure("mapped_image_size");
  Image image_reader(process.value, module, info.SizeOfImage);
  d::Limits limits;
  limits.scan_bytes = 0;
  limits.records = 0;
  limits.export_names = 0;
  const auto image = d::inspect_image(image_reader, limits);
  if (!image.valid_image || image.machine != 0x8664 || image.timestamp != 1787653788 || image.image_size != 235963904 ||
      image.section_count != 14)
    return failure("image_build");
  const auto contract = n::verify_code_contract(image_reader, image, n::verified_profile());
  if (!contract.valid)
    return failure(contract.error.c_str());
  constexpr std::uint32_t global = 173790384;
  if (!section(image, global, 8, true))
    return failure("renderer_global_section");
  std::uint64_t renderer = 0, renderer_after = 0, vptr = 0, vptr_after = 0;
  if (!image_reader.read(global, &renderer, 8) || !renderer || renderer % 8)
    return failure("renderer_absent");
  Objects objects(process.value);
  if (!objects.read(renderer, &vptr, 8) || vptr != image_reader.base + 134482536 || !section(image, 134482536, 8, false))
    return failure("renderer_identity");
  const auto aircraft = d::inspect_aircraft_metadata(image_reader, objects, image, image_reader.base);
  const auto inventory = d::inspect_view_materials(objects, renderer);
  if (!inventory.complete)
    return failure(inventory.error);
  if (!image_reader.read(global, &renderer_after, 8) || renderer_after != renderer || !objects.read(renderer, &vptr_after, 8) ||
      vptr_after != vptr)
    return failure("renderer_changed");
  std::printf(
      "{\"complete\":true,\"pid\":%lu,\"code_ranges\":%zu,\"primary_view_valid\":%s,\"primary_view_index\":%d,"
      "\"object_bytes\":%zu,\"image_bytes\":%zu,\"read_failures\":%u,\"views\":[",
      pid, contract.records.size(), aircraft.valid && aircraft.available ? "true" : "false", aircraft.viewport_id, objects.attempted,
      image_reader.attempted, objects.failures);
  for (unsigned i = 0; i < inventory.views.size(); ++i) {
    const auto& v = inventory.views[i];
    std::printf(
        "%s{\"index\":%u,\"material_present\":%s,\"dimensions\":[%u,%u,%u,%u,%u,%u],\"flags_stable\":%s,"
        "\"flags_before\":[\"0x%016llx\",\"0x%016llx\"],\"flags_after\":[\"0x%016llx\",\"0x%016llx\"],\"bitmaps\":[",
        i ? "," : "", v.index, v.material_present ? "true" : "false", v.dimensions[0], v.dimensions[1], v.dimensions[2], v.dimensions[3],
        v.dimensions[4], v.dimensions[5], v.flags_stable ? "true" : "false", v.flags_before[0], v.flags_before[1], v.flags_after[0],
        v.flags_after[1]);
    for (unsigned s = 0; s < v.bitmaps.size(); ++s) {
      const auto& bmap = v.bitmaps[s];
      std::printf(
          "%s{\"slot\":%u,\"present\":%s,\"stale\":%s,\"handle_generation\":%u,\"current_generation\":%u,"
          "\"bitmap_width\":%u,\"bitmap_height\":%u,\"resource_present\":%s,\"resource_ordinal\":%u,"
          "\"dimension\":%u,\"width\":%llu,\"height\":%u,\"layers\":%u,\"mips\":%u,\"format\":%u,\"samples\":%u,\"native_flags\":%u}",
          s ? "," : "", bmap.slot, bmap.present ? "true" : "false", bmap.stale ? "true" : "false", bmap.handle_generation,
          bmap.current_generation, bmap.bitmap_width, bmap.bitmap_height, bmap.resource_present ? "true" : "false", bmap.resource_ordinal,
          bmap.dimension, bmap.width, bmap.height, bmap.layers, bmap.mips, bmap.format, bmap.samples, bmap.flags);
    }
    std::printf("]}");
  }
  std::puts("]}");
  return 0;
}
}  // namespace
int main(int argc, char** argv) {
  if (argc != 3 || std::strcmp(argv[1], "--pid"))
    return 2;
  DWORD pid = 0;
  const auto end = argv[2] + std::strlen(argv[2]);
  const auto parsed = std::from_chars(argv[2], end, pid);
  if (parsed.ec != std::errc{} || parsed.ptr != end || !pid)
    return 2;
  return inspect(pid);
}
