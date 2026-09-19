// Read-only Steam SU7 camera-contract capture. Opens one explicit PID with
// query and read rights, proves it is the loaded Steam FlightSimulator2024
// module, and does not write, inject, or call simulator code.
#include "../../src/camera/activation_mask.hpp"
#include "../../src/camera/camera_release_contract.hpp"
#include "../../src/camera/code_contract.hpp"
#include "../../src/camera/rtti_vtables.hpp"

// clang-format off
#include <windows.h>
#include <appmodel.h>
#include <psapi.h>
// clang-format on

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
constexpr wchar_t kSteamSuffix[] = L"\\steamapps\\common\\MSFS2024\\FlightSimulator2024.exe";
constexpr wchar_t kStoreMarker[] = L"\\WindowsApps\\Microsoft.Limitless_";
constexpr wchar_t kExecutableSuffix[] = L"\\FlightSimulator2024.exe";
constexpr wchar_t kStoreVersion[] = L"_1.9.12.0_";

class Handle {
 public:
  Handle() = default;
  explicit Handle(HANDLE value) : value_(value) {}
  ~Handle() {
    if (value_)
      CloseHandle(value_);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE get() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }

 private:
  HANDLE value_ = nullptr;
};

class MainImageReader final : public taxi_camera::discovery::ImageReader {
 public:
  MainImageReader(HANDLE process, HMODULE module, std::uint32_t image_size)
      : process_(process), base_(reinterpret_cast<std::uintptr_t>(module)), module_(module), limit_(image_size) {}
  taxi_camera::discovery::ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    if (!maximum || rva >= limit_)
      return {};
    maximum = std::min(maximum, limit_ - rva);
    MEMORY_BASIC_INFORMATION region{};
    const auto address = base_ + rva;
    if (VirtualQueryEx(process_, reinterpret_cast<const void*>(address), &region, sizeof(region)) != sizeof(region))
      return {};
    const auto region_base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (region_base > address || !region.RegionSize)
      return {};
    const auto length = static_cast<std::uint32_t>(std::min<std::uintptr_t>(maximum, region_base + region.RegionSize - address));
    const auto protect = region.Protect & 0xff;
    const bool readable = region.State == MEM_COMMIT && region.Type == MEM_IMAGE && region.AllocationBase == module_ &&
                          (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
                          (protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                           protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY);
    return {length, readable};
  }
  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    if (!size || rva >= limit_ || size > limit_ - rva)
      return false;
    auto* output = static_cast<std::uint8_t*>(destination);
    std::size_t done = 0;
    while (done < size) {
      const auto window = query(rva + static_cast<std::uint32_t>(done), static_cast<std::uint32_t>(size - done));
      if (!window.readable || !window.size || window.size > size - done)
        return false;
      SIZE_T copied = 0;
      if (!ReadProcessMemory(process_, reinterpret_cast<const void*>(base_ + rva + done), output + done, window.size, &copied) ||
          copied != window.size)
        return false;
      done += window.size;
    }
    return true;
  }
  std::uintptr_t base() const { return base_; }

 private:
  HANDLE process_;
  std::uintptr_t base_;
  HMODULE module_;
  std::uint32_t limit_;
};

bool same_user(HANDLE process) {
  auto token_bytes = [](HANDLE source, std::vector<std::uint8_t>& data) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(source, TOKEN_QUERY, &raw))
      return false;
    Handle token(raw);
    DWORD required = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !required || required > 65536)
      return false;
    data.resize(required);
    return GetTokenInformation(token.get(), TokenUser, data.data(), required, &required) != FALSE;
  };
  std::vector<std::uint8_t> current, selected;
  if (!token_bytes(GetCurrentProcess(), current) || !token_bytes(process, selected))
    return false;
  return EqualSid(reinterpret_cast<const TOKEN_USER*>(current.data())->User.Sid,
                  reinterpret_cast<const TOKEN_USER*>(selected.data())->User.Sid) != FALSE;
}

bool ends_with(const std::wstring& value, const wchar_t* suffix) {
  const auto length = std::wcslen(suffix);
  return value.size() >= length && CompareStringOrdinal(value.c_str() + (value.size() - length), -1, suffix, -1, TRUE) == CSTR_EQUAL;
}

bool contains_text(const std::wstring& value, const wchar_t* needle) {
  return FindStringOrdinal(FIND_FROMSTART, value.c_str(), static_cast<int>(value.size()), needle, -1, TRUE) >= 0;
}

bool file_ids_match(const std::wstring& left, const std::wstring& right) {
  if (left.empty() || right.empty())
    return false;
  if (CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL)
    return true;
  const auto identity = [](const std::wstring& path, FILE_ID_INFO& info) {
    HANDLE raw = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE)
      return false;
    Handle file(raw);
    return GetFileInformationByHandleEx(file.get(), FileIdInfo, &info, sizeof(info)) != FALSE;
  };
  FILE_ID_INFO first{}, second{};
  return identity(left, first) && identity(right, second) && first.VolumeSerialNumber == second.VolumeSerialNumber &&
         std::memcmp(&first.FileId, &second.FileId, sizeof(first.FileId)) == 0;
}

// Attributes only. The package name comes from the process, not from a directory listing.
std::wstring store_package_executable(HANDLE process, std::wstring& package_name) {
  package_name.clear();
  UINT32 length = 0;
  if (GetPackageFullName(process, &length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length < 2 || length > 2048)
    return {};
  package_name.assign(length, L'\0');
  if (GetPackageFullName(process, &length, package_name.data()) != ERROR_SUCCESS)
    return {};
  while (!package_name.empty() && package_name.back() == L'\0')
    package_name.pop_back();
  constexpr wchar_t kPrefix[] = L"Microsoft.Limitless_1.9.12.0_";
  const auto prefix = static_cast<int>(std::wcslen(kPrefix));
  if (static_cast<int>(package_name.size()) < prefix ||
      CompareStringOrdinal(package_name.c_str(), prefix, kPrefix, prefix, TRUE) != CSTR_EQUAL ||
      !contains_text(package_name, L"8wekyb3d8bbwe")) {
    package_name.clear();
    return {};
  }
  wchar_t program_files[MAX_PATH]{};
  const auto root = GetEnvironmentVariableW(L"ProgramFiles", program_files, MAX_PATH);
  if (!root || root >= MAX_PATH)
    return {};
  return std::wstring(program_files) + L"\\WindowsApps\\" + package_name + L"\\FlightSimulator2024.exe";
}

bool file_version_1_9_12_0(const std::wstring& path) {
  DWORD handle = 0;
  const auto bytes = GetFileVersionInfoSizeW(path.c_str(), &handle);
  if (!bytes || bytes > 1024 * 1024)
    return false;
  std::vector<std::uint8_t> data(bytes);
  if (!GetFileVersionInfoW(path.c_str(), 0, bytes, data.data()))
    return false;
  VS_FIXEDFILEINFO* info = nullptr;
  UINT size = 0;
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &size) || !info || size < sizeof(*info))
    return false;
  return HIWORD(info->dwFileVersionMS) == 1 && LOWORD(info->dwFileVersionMS) == 9 && HIWORD(info->dwFileVersionLS) == 12 &&
         LOWORD(info->dwFileVersionLS) == 0;
}

std::uint16_t u16(const std::uint8_t* p);
std::uint32_t u32(const std::uint8_t* p);

bool fixed_is_1_9_12_0(const std::uint8_t* info) {
  if (u32(info) != 0xFEEF04BD)
    return false;
  const auto ms = u32(info + 8);
  const auto ls = u32(info + 12);
  return (ms >> 16) == 1 && (ms & 0xffff) == 9 && (ls >> 16) == 12 && (ls & 0xffff) == 0;
}

// The Store package file can refuse version.dll. The loaded resource is the
// same VS_FIXEDFILEINFO, read from the process image only.
bool loaded_version_1_9_12_0(taxi_camera::discovery::ImageReader& reader) {
  std::uint8_t dos[64]{};
  if (!reader.read(0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z')
    return false;
  const auto pe = u32(dos + 60);
  std::uint8_t opt[280]{};
  if (!reader.read(pe, opt, sizeof(opt)) || u32(opt) != 0x4550 || u16(opt + 24) != 0x20b || u32(opt + 24 + 108) < 3)
    return false;
  const auto root = u32(opt + 24 + 112 + 16);
  const auto root_size = u32(opt + 24 + 112 + 20);
  if (!root || root_size < 16 || root_size > 8 * 1024 * 1024)
    return false;
  const auto child = [&](std::uint32_t directory, std::uint32_t index, std::uint32_t& next, bool& data) {
    std::uint8_t entry[8]{};
    if (!reader.read(directory + 16 + index * 8, entry, sizeof(entry)))
      return false;
    const auto offset = u32(entry + 4);
    data = (offset & 0x80000000u) == 0;
    next = root + (offset & 0x7fffffffu);
    return next >= root && next - root < root_size;
  };
  std::uint8_t header[16]{};
  if (!reader.read(root, header, sizeof(header)))
    return false;
  const auto named = u16(header + 12);
  const auto ids = u16(header + 14);
  std::uint32_t version_dir = 0;
  bool data = false;
  bool found = false;
  for (std::uint32_t index = named; index < std::uint32_t(named) + ids; ++index) {
    std::uint8_t entry[8]{};
    if (!reader.read(root + 16 + index * 8, entry, sizeof(entry)))
      return false;
    if (u32(entry) != 16)
      continue;
    found = child(root, index, version_dir, data);
    break;
  }
  if (!found || data)
    return false;
  if (!reader.read(version_dir, header, sizeof(header)))
    return false;
  if (u16(header + 12) + u16(header + 14) == 0 || !child(version_dir, 0, version_dir, data) || data)
    return false;
  if (!child(version_dir, 0, version_dir, data) || !data)
    return false;
  std::uint8_t data_entry[16]{};
  if (!reader.read(version_dir, data_entry, sizeof(data_entry)))
    return false;
  const auto blob = u32(data_entry);
  const auto size = u32(data_entry + 4);
  if (size < 92 || size > 65536)
    return false;
  std::vector<std::uint8_t> version(size);
  if (!reader.read(blob, version.data(), version.size()))
    return false;
  std::uint32_t at = 6;
  while (at + 1 < version.size() && (version[at] || version[at + 1]))
    at += 2;
  at = (at + 2u + 3u) & ~3u;
  return at + 52 <= version.size() && fixed_is_1_9_12_0(version.data() + at);
}

std::uint16_t u16(const std::uint8_t* p) {
  return std::uint16_t(p[0] | (std::uint16_t(p[1]) << 8));
}
std::uint32_t u32(const std::uint8_t* p) {
  return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

struct RelocSlot {
  std::uint32_t rva = 0;
  std::uint8_t type = 0;
};

bool read_relocs(taxi_camera::discovery::ImageReader& reader,
                 std::vector<RelocSlot>& slots,
                 std::uint32_t& directory_rva,
                 std::uint32_t& directory_size) {
  std::uint8_t dos[64]{};
  if (!reader.read(0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z')
    return false;
  const auto pe = u32(dos + 60);
  std::uint8_t opt[256]{};
  if (!reader.read(pe, opt, sizeof(opt)) || u32(opt) != 0x4550 || u16(opt + 24) != 0x20b)
    return false;
  const auto directories = u32(opt + 24 + 108);
  if (directories < 6)
    return false;
  directory_rva = u32(opt + 24 + 112 + 5 * 8);
  directory_size = u32(opt + 24 + 112 + 5 * 8 + 4);
  if (!directory_size || directory_size > 8 * 1024 * 1024)
    return false;
  std::vector<std::uint8_t> bytes(directory_size);
  if (!reader.read(directory_rva, bytes.data(), bytes.size()))
    return false;
  std::uint32_t offset = 0;
  while (offset + 8 <= bytes.size()) {
    const auto page = u32(bytes.data() + offset);
    const auto size = u32(bytes.data() + offset + 4);
    if (size < 8 || size > bytes.size() - offset || (size % 2))
      return false;
    for (std::uint32_t at = 8; at < size; at += 2) {
      const auto word = std::uint16_t(bytes[offset + at] | (bytes[offset + at + 1] << 8));
      slots.push_back({page + (word & 0x0fff), static_cast<std::uint8_t>(word >> 12)});
    }
    offset += size;
  }
  return offset == bytes.size();
}

struct BodyHit {
  std::uint32_t rva = 0;
  std::vector<std::uint32_t> diffs;
};

std::uint32_t scan_templates(taxi_camera::discovery::ImageReader& reader,
                             const taxi_camera::discovery::Inventory& image,
                             const taxi_camera::native_camera::relocatable::ContractModel& model,
                             std::vector<std::uint32_t>& unique) {
  struct Seed {
    std::size_t index = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::vector<std::uint8_t> invariant;
  };
  std::vector<Seed> seeds(model.code.size());
  std::unordered_map<std::uint32_t, std::vector<std::size_t>> keys;
  for (std::size_t i = 0; i < model.code.size(); ++i) {
    auto& seed = seeds[i];
    seed.index = i;
    seed.invariant.assign(model.code[i].bytes.size(), 1);
    for (const auto& operand : model.code[i].operands)
      for (std::uint32_t n = 0; n < operand.width && operand.offset + n < seed.invariant.size(); ++n)
        seed.invariant[operand.offset + n] = 0;
    std::uint32_t run = 0;
    for (std::uint32_t at = 0; at < seed.invariant.size(); ++at) {
      run = seed.invariant[at] ? run + 1 : 0;
      if (run > seed.size) {
        seed.size = run;
        seed.offset = at + 1 - run;
      }
    }
    if (seed.size >= 4) {
      const auto* bytes = model.code[i].bytes.data() + seed.offset;
      const auto key =
          std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) | (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
      keys[key].push_back(i);
    }
  }
  std::vector<std::vector<BodyHit>> hits(model.code.size());
  std::vector<std::uint32_t> overflow(model.code.size());
  unique.assign(model.code.size(), 0);
  std::uint32_t setup_rva = 0;
  std::vector<std::uint8_t> chunk(32768 + 16384);
  for (const auto& section : image.sections) {
    if ((section.flags & 0xe2000000u) != 0x60000000u)
      continue;
    for (std::uint32_t offset = 0; offset < section.size;) {
      const auto count = std::min<std::uint32_t>(32768, section.size - offset);
      const auto size = std::min<std::uint32_t>(static_cast<std::uint32_t>(chunk.size()), section.size - offset);
      if (!reader.read(section.rva + offset, chunk.data(), size)) {
        std::cout << "code_read_failed rva=" << (section.rva + offset) << "\n";
        return 0;
      }
      for (std::uint32_t at = 0; at + 4 <= size && at < count; ++at) {
        const auto key = std::uint32_t(chunk[at]) | (std::uint32_t(chunk[at + 1]) << 8) | (std::uint32_t(chunk[at + 2]) << 16) |
                         (std::uint32_t(chunk[at + 3]) << 24);
        const auto found = keys.find(key);
        if (found == keys.end())
          continue;
        for (const auto index : found->second) {
          const auto& seed = seeds[index];
          const auto& code = model.code[index];
          if (std::uint64_t(at) + seed.size > size || std::memcmp(chunk.data() + at, code.bytes.data() + seed.offset, seed.size))
            continue;
          const auto address = std::uint64_t(section.rva) + offset + at;
          if (address < seed.offset)
            continue;
          const auto begin = static_cast<std::uint32_t>(address - seed.offset);
          if (std::uint64_t(begin) + code.bytes.size() > std::uint64_t(section.rva) + section.size)
            continue;
          std::vector<std::uint8_t> body(code.bytes.size());
          if (!reader.read(begin, body.data(), body.size()))
            continue;
          BodyHit hit;
          hit.rva = begin;
          for (std::uint32_t n = 0; n < body.size(); ++n)
            if (seed.invariant[n] && body[n] != code.bytes[n])
              hit.diffs.push_back(n);
          if (hit.diffs.size() <= 8)
            hits[index].push_back(std::move(hit));
          else
            ++overflow[index];
        }
      }
      offset += count;
    }
  }
  for (std::size_t i = 0; i < model.code.size(); ++i) {
    const auto name = model.symbols[model.code[i].symbol].name;
    std::uint32_t exact = 0;
    for (const auto& hit : hits[i])
      exact += hit.diffs.empty();
    std::cout << "template " << name << " exact=" << exact << " near=" << (hits[i].size() - exact) << " overflow=" << overflow[i]
              << " bytes=" << model.code[i].bytes.size() << " operands=" << model.code[i].operands.size() << "\n";
    if (exact == 1) {
      for (const auto& hit : hits[i])
        if (hit.diffs.empty()) {
          unique[i] = hit.rva;
          std::cout << "  unique rva=" << hit.rva << "\n";
        }
    } else if (exact > 1 && exact <= 16) {
      for (const auto& hit : hits[i])
        if (hit.diffs.empty())
          std::cout << "  candidate rva=" << hit.rva << "\n";
    }
    if (exact != 1) {
      std::uint32_t shown = 0;
      for (const auto& hit : hits[i]) {
        if (hit.diffs.empty() || shown >= 3)
          continue;
        std::vector<std::uint8_t> body(model.code[i].bytes.size());
        if (!reader.read(hit.rva, body.data(), body.size()))
          continue;
        std::cout << "  mismatch rva=" << hit.rva << " diffs=" << hit.diffs.size();
        const auto count = std::min<std::size_t>(hit.diffs.size(), 6);
        for (std::size_t n = 0; n < count; ++n) {
          const auto at = hit.diffs[n];
          std::cout << " [" << at << "]=0x" << std::hex << int(body[at]) << "/0x" << int(model.code[i].bytes[at]) << std::dec;
        }
        std::cout << "\n";
        ++shown;
      }
    }
    if (exact == 0 && name == "setup_entry" && hits[i].size() == 1 && hits[i][0].diffs.size() == 1) {
      setup_rva = hits[i][0].rva;
      std::cout << "  near rva=" << setup_rva << " diff=" << hits[i][0].diffs[0] << "\n";
    }
    if (name == "setup_entry" && exact == 1)
      setup_rva = unique[i];
  }
  return setup_rva;
}

void classify_relocs(const std::vector<RelocSlot>& slots) {
  std::uint32_t dir64 = 0, highlow = 0, pad = 0, other = 0, unaligned = 0;
  for (const auto& slot : slots) {
    if (slot.type == 10) {
      ++dir64;
      if (slot.rva % 8)
        ++unaligned;
    } else if (slot.type == 3) {
      ++highlow;
    } else if (slot.type == 0) {
      ++pad;
    } else {
      ++other;
    }
  }
  std::cout << "relocs total=" << slots.size() << " dir64=" << dir64 << " unaligned_dir64=" << unaligned << " highlow=" << highlow
            << " pad=" << pad << " other=" << other << "\n";
}

void live_rtti(taxi_camera::discovery::ImageReader& reader, const taxi_camera::discovery::Inventory& image) {
  std::uint32_t primary = 0, named = 0, gxz_a = 0, gxz_b = 0, msvc = 0, camera_text = 0, unreadable = 0;
  const char* interesting[] = {"Camera", "Node", "Manager", "Bitmap", "Material", "Scene", "Aircraft", "View", "Lod"};
  for (const auto& section : image.sections) {
    if ((section.flags & 0xe2000000u) != 0x40000000u)
      continue;
    std::vector<std::uint8_t> bytes(section.size);
    if (!reader.read(section.rva, bytes.data(), bytes.size())) {
      std::cout << "rtti_section_unreadable rva=" << section.rva << "\n";
      continue;
    }
    for (std::uint32_t at = 0; at + 24 <= bytes.size(); at += 4) {
      if (u32(bytes.data() + at) != 1 || u32(bytes.data() + at + 4) || u32(bytes.data() + at + 8))
        continue;
      if (u32(bytes.data() + at + 20) != section.rva + at)
        continue;
      ++primary;
      const auto type = u32(bytes.data() + at + 12);
      if (type + 16 >= image.image_size) {
        ++unreadable;
        continue;
      }
      char name[121]{};
      if (!reader.read(type + 16, name, 120)) {
        ++unreadable;
        continue;
      }
      name[120] = 0;
      const auto length = std::strlen(name);
      if (!length || length == 120)
        continue;
      bool printable = true;
      for (std::size_t n = 0; n < length; ++n)
        printable = printable && name[n] >= 32 && name[n] < 127;
      if (!printable)
        continue;
      ++named;
      if (std::strncmp(name, "gxzA", 4) == 0)
        ++gxz_a;
      else if (std::strncmp(name, "gxzB", 4) == 0)
        ++gxz_b;
      else if (std::strncmp(name, ".?A", 3) == 0)
        ++msvc;
      for (const auto* word : interesting)
        if (std::strstr(name, word))
          ++camera_text;
    }
  }
  std::cout << "live_rtti primary=" << primary << " named=" << named << " gxzA=" << gxz_a << " gxzB=" << gxz_b << " msvc=" << msvc
            << " role_text=" << camera_text << " unreadable_names=" << unreadable << "\n";
}

void classify_operands(const taxi_camera::native_camera::relocatable::ContractModel& model,
                       const std::vector<std::uint32_t>& unique,
                       const std::vector<RelocSlot>& slots) {
  std::unordered_map<std::uint32_t, std::uint8_t> at;
  at.reserve(slots.size());
  for (const auto& slot : slots)
    at.emplace(slot.rva, slot.type);
  std::uint32_t rip = 0, image_rva = 0, reloc_overlap = 0, other = 0;
  for (std::size_t i = 0; i < model.code.size(); ++i) {
    if (!unique[i])
      continue;
    for (const auto& operand : model.code[i].operands) {
      const auto field = unique[i] + operand.offset;
      bool overlap = false;
      for (std::uint32_t back = 0; back < 8 && field >= back; ++back) {
        const auto found = at.find(field - back);
        if (found == at.end())
          continue;
        const auto span = found->second == 10 ? 8u : found->second == 3 ? 4u : 2u;
        if (back < span && back + operand.width > 0 && field - back < field + operand.width) {
          overlap = true;
          break;
        }
      }
      if (overlap) {
        ++reloc_overlap;
        if (reloc_overlap <= 8)
          std::cout << "  reloc_overlap symbol=" << model.symbols[model.code[i].symbol].name << " offset=" << operand.offset
                    << " rva=" << field << "\n";
      } else if (operand.kind == taxi_camera::native_camera::relocatable::AddressKind::pc_relative)
        ++rip;
      else if (operand.kind == taxi_camera::native_camera::relocatable::AddressKind::image_rva)
        ++image_rva;
      else
        ++other;
    }
  }
  std::cout << "operand_class unique_bodies rip=" << rip << " image_rva=" << image_rva << " reloc_overlap=" << reloc_overlap
            << " other=" << other << "\n";
}

void activation_constants(taxi_camera::discovery::ImageReader& reader, const taxi_camera::discovery::Inventory& image) {
  const std::uint8_t needle[16] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  std::uint32_t hits = 0;
  for (const auto& section : image.sections) {
    if ((section.flags & 0xe2000000u) != 0x40000000u || section.size < 16)
      continue;
    std::vector<std::uint8_t> bytes(section.size);
    if (!reader.read(section.rva, bytes.data(), bytes.size()))
      continue;
    for (std::uint32_t at = 0; at + 16 <= bytes.size(); at += 8) {
      if (std::memcmp(bytes.data() + at, needle, 16) == 0) {
        ++hits;
        if (hits <= 8)
          std::cout << "activation_constant rva=" << (section.rva + at) << "\n";
      }
    }
  }
  std::cout << "activation_constant_hits=" << hits << "\n";
}
}  // namespace

int main(int argc, char** argv) {
  const char* pid_text = nullptr;
  bool store_edition = false;
  if (argc == 2) {
    pid_text = argv[1];
  } else if (argc == 3 && std::strcmp(argv[1], "store") == 0) {
    store_edition = true;
    pid_text = argv[2];
  } else if (argc == 3 && std::strcmp(argv[1], "steam") == 0) {
    pid_text = argv[2];
  } else {
    return 2;
  }
  char* end = nullptr;
  const auto pid = static_cast<DWORD>(std::strtoul(pid_text, &end, 10));
  if (!pid || !end || *end)
    return 2;
  Handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
  if (!process) {
    std::cout << "open_failed error=" << GetLastError() << "\n";
    return 1;
  }
  wchar_t path[32768]{};
  DWORD path_size = 32768;
  if (!QueryFullProcessImageNameW(process.get(), 0, path, &path_size)) {
    std::cout << "path_failed\n";
    return 1;
  }
  const std::wstring executable(path, path_size);
  const bool steam = ends_with(executable, kSteamSuffix) || contains_text(executable, L"\\steamapps\\");
  std::wstring package_name;
  const auto package = store_package_executable(process.get(), package_name);
  const bool store = !steam && ends_with(executable, kExecutableSuffix) && !package.empty() && file_ids_match(executable, package) &&
                     contains_text(package, kStoreMarker) && contains_text(package, kStoreVersion);
  const bool file_version = file_version_1_9_12_0(executable);
  std::wcout << L"image_path=" << executable << L"\n";
  std::wcout << L"package_name=" << package_name << L"\n";
  std::wcout << L"package_path=" << package << L"\n";
  std::cout << "pid=" << pid << " edition=" << (store_edition ? "store" : "steam") << " steam_path=" << (steam ? "yes" : "no")
            << " store_package=" << (store ? "yes" : "no") << " file_version_1_9_12_0=" << (file_version ? "yes" : "no") << "\n";
  WIN32_FILE_ATTRIBUTE_DATA attributes{};
  if (GetFileAttributesExW(executable.c_str(), GetFileExInfoStandard, &attributes)) {
    const auto file_size = (std::uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    std::cout << "file_size=" << file_size << "\n";
  }
  if (!same_user(process.get()) || (store_edition ? !store : !steam || store || !file_version)) {
    std::cout << "refused identity\n";
    return 1;
  }
  HMODULE module = nullptr;
  DWORD module_bytes = 0;
  if (!K32EnumProcessModulesEx(process.get(), &module, sizeof(module), &module_bytes, LIST_MODULES_64BIT) || !module) {
    std::cout << "module_failed\n";
    return 1;
  }
  std::uint8_t dos[64]{};
  SIZE_T copied = 0;
  if (!ReadProcessMemory(process.get(), module, dos, sizeof(dos), &copied) || copied != sizeof(dos)) {
    std::cout << "header_failed\n";
    return 1;
  }
  const auto pe = u32(dos + 60);
  std::uint8_t opt[256]{};
  if (!ReadProcessMemory(process.get(), reinterpret_cast<std::uint8_t*>(module) + pe, opt, sizeof(opt), &copied) || copied != sizeof(opt)) {
    std::cout << "optional_header_failed\n";
    return 1;
  }
  const auto image_size = u32(opt + 24 + 56);
  const auto timestamp = u32(opt + 8);
  std::cout << "loaded_base=0x" << std::hex << reinterpret_cast<std::uintptr_t>(module) << std::dec << " timestamp=" << timestamp
            << " image_size=" << image_size << "\n";
  MainImageReader reader(process.get(), module, image_size);
  const bool loaded_version = loaded_version_1_9_12_0(reader);
  std::cout << "loaded_version_1_9_12_0=" << (loaded_version ? "yes" : "no") << "\n";
  if (store_edition && !file_version && !loaded_version)
    std::cout << "version_resource_absent package_identity=1.9.12.0\n";
  if (!store_edition && !file_version) {
    std::cout << "refused version\n";
    return 1;
  }
  taxi_camera::discovery::Limits limits;
  limits.scan_bytes = 1;
  limits.export_names = 0;
  limits.records = 1;
  const auto image = taxi_camera::discovery::inspect_image(reader, limits);
  if (!image.valid_image) {
    std::cout << "image_invalid " << image.error << "\n";
    return 1;
  }
  std::cout << "sections=" << image.section_count << " exception_rva=" << image.exception_rva << " exception_size=" << image.exception_size
            << " checksum=" << image.checksum << "\n";
  bool unpacked = false;
  bool file_compared = false;
  for (const auto& section : image.sections) {
    if (section.name == ".text" && section.size >= 16) {
      std::uint8_t live[16]{}, file[16]{};
      HANDLE file_handle = CreateFileW(executable.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file_handle == INVALID_HANDLE_VALUE || !reader.read(section.rva, live, sizeof(live))) {
        std::cout << "unpack_check_failed error=" << GetLastError() << "\n";
        if (file_handle != INVALID_HANDLE_VALUE)
          CloseHandle(file_handle);
        if (!store_edition)
          return 1;
        break;
      }
      // The packed file's first .text page is not a loaded RVA. Compare only
      // enough to prove the process page is not the on-disk page. Store may
      // publish an unpacked file; the bytes below still come from the process.
      DWORD read_file = 0;
      const auto file_offset = 0x1000;
      if (!SetFilePointer(file_handle, file_offset, nullptr, FILE_BEGIN) ||
          !ReadFile(file_handle, file, sizeof(file), &read_file, nullptr) || read_file != sizeof(file)) {
        CloseHandle(file_handle);
        std::cout << "file_page_failed\n";
        if (!store_edition)
          return 1;
        break;
      }
      CloseHandle(file_handle);
      file_compared = true;
      unpacked = std::memcmp(live, file, sizeof(live)) != 0;
      std::cout << "text_unpacked=" << (unpacked ? "yes" : "no") << " live0=" << std::hex << int(live[0]) << " file0=" << int(file[0])
                << std::dec << "\n";
      break;
    }
  }
  if (!store_edition && !unpacked) {
    std::cout << "refused packed_or_file_image\n";
    return 1;
  }
  if (store_edition && file_compared && !unpacked)
    std::cout << "store_file_page_matches_live\n";
  auto model = taxi_camera::native_camera::camera_release_contract::model();
  std::vector<std::uint32_t> unique;
  const auto setup_rva = scan_templates(reader, image, model, unique);
  for (std::size_t i = 0; i < model.code.size(); ++i)
    if (model.symbols[model.code[i].symbol].name == "setup_entry")
      unique[i] = setup_rva;
  std::vector<RelocSlot> slots;
  std::uint32_t reloc_rva = 0, reloc_size = 0;
  if (!read_relocs(reader, slots, reloc_rva, reloc_size)) {
    std::cout << "reloc_walk_failed\n";
    return 1;
  }
  std::cout << "reloc_directory rva=" << reloc_rva << " size=" << reloc_size << "\n";
  classify_relocs(slots);
  classify_operands(model, unique, slots);
  live_rtti(reader, image);
  activation_constants(reader, image);
  bool setup_ok = false;
  bool model_matches = false;
  if (!setup_rva) {
    std::cout << "setup_entry_not_isolated\n";
  } else {
    std::uint8_t live[7]{};
    if (!reader.read(setup_rva + 377, live, sizeof(live))) {
      std::cout << "setup_bytes_unreadable\n";
    } else {
      std::cout << "setup_live";
      for (const auto byte : live)
        std::cout << " " << std::hex << int(byte);
      std::cout << std::dec << " rva=" << setup_rva << "\n";
      setup_ok =
          live[0] == 0x48 && live[1] == 0x8b && live[2] == 0x81 && live[3] == 0x50 && live[4] == 0x06 && live[5] == 0 && live[6] == 0;
      if (!setup_ok)
        std::cout << "setup_displacement_not_650\n";
      for (const auto& code : model.code) {
        if (model.symbols[code.symbol].name != "setup_entry" || code.bytes.size() <= 383)
          continue;
        model_matches = code.bytes[377] == live[0] && code.bytes[378] == live[1] && code.bytes[379] == live[2] &&
                        code.bytes[380] == live[3] && code.bytes[381] == live[4] && code.bytes[382] == live[5] &&
                        code.bytes[383] == live[6];
      }
      std::cout << "setup_model_matches_live=" << (model_matches ? "yes" : "no") << "\n";
    }
  }
  const auto resolved = taxi_camera::native_camera::resolve_camera_contract(reader, image, reinterpret_cast<std::uint64_t>(module));
  std::cout << "contract valid=" << (resolved.valid ? "yes" : "no") << " error=" << resolved.error << " ranges=" << resolved.matched_ranges
            << " scanned=" << resolved.scanned_bytes << "\n";
  if (!resolved.valid || !setup_ok || !model_matches)
    return 1;
  std::cout << "bound set_fov=" << resolved.contract.functions.set_fov << " set_target=" << resolved.contract.functions.set_target
            << " set_up=" << resolved.contract.functions.set_up
            << " setup_related_mask=" << resolved.contract.layout.activation_disable_mask
            << " controller_method=" << resolved.contract.layout.aircraft_controller_method
            << " selected_method=" << resolved.contract.layout.aircraft_selected_method << "\n";
  const auto padding = [&](const char* name, std::uint32_t rva, std::uint32_t skip) {
    std::uint8_t bytes[4]{};
    if (!rva || !reader.read(rva + skip, bytes, sizeof(bytes)))
      return;
    std::cout << name << "_padding";
    for (const auto byte : bytes)
      std::cout << " " << std::hex << int(byte);
    std::cout << std::dec << "\n";
  };
  padding("controller", resolved.contract.layout.aircraft_controller_method, 7);
  padding("selected", resolved.contract.layout.aircraft_selected_method, 8);
  std::uint32_t manager = 0;
  for (std::size_t i = 0; i < model.code.size(); ++i)
    if (model.symbols[model.code[i].symbol].name == "manager_update")
      manager = unique[i];
  std::uint8_t relative[4]{};
  if (manager && reader.read(manager + 326, relative, sizeof(relative))) {
    const auto target = static_cast<std::uint32_t>(std::uint64_t(manager) + 330u + static_cast<std::int32_t>(u32(relative)));
    std::cout << "code_18_from_manager_update=" << target << "\n";
    padding("code_18", target, 19);
  }
  return 0;
}
