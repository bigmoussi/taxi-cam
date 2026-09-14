#include "main_module_inventory.hpp"
#include "../native-camera/profile.hpp"
#include "llvm_decoder.hpp"

#include <psapi.h>

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kMaximumImageSize = 0x80000000;

bool readable_image_page(DWORD protection) {
  if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
    return false;
  }
  // Packaged executables can map headers/read-only sections as copy-on-write.
  // Reading does not trigger a write; the parser separately excludes writable
  // PE sections. PAGE_EXECUTE alone grants no documented read permission.
  const auto base_protection = protection & 0xff;
  return base_protection == PAGE_READONLY || base_protection == PAGE_READWRITE || base_protection == PAGE_WRITECOPY ||
         base_protection == PAGE_EXECUTE_READ || base_protection == PAGE_EXECUTE_READWRITE || base_protection == PAGE_EXECUTE_WRITECOPY;
}

class MainImageReader final : public ImageReader {
 public:
  MainImageReader(HANDLE process, HMODULE module, std::uint32_t image_size)
      : process_(process),
        module_(module),
        base_(reinterpret_cast<std::uintptr_t>(module)),
        limit_(std::min(image_size, kMaximumImageSize)) {}

  ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    if (rva >= limit_ || maximum == 0 || rva > std::numeric_limits<std::uintptr_t>::max() - base_) {
      return {};
    }
    maximum = std::min(maximum, limit_ - rva);
    const auto address = base_ + rva;
    MEMORY_BASIC_INFORMATION region{};
    const auto queried = process_ == GetCurrentProcess()
                             ? VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region))
                             : VirtualQueryEx(process_, reinterpret_cast<const void*>(address), &region, sizeof(region));
    if (queried != sizeof(region)) {
      return {};
    }
    const auto region_base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (region_base > address || region.RegionSize == 0 || region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - region_base) {
      return {};
    }
    const auto region_end = region_base + region.RegionSize;
    if (region_end <= address) {
      return {};
    }
    const auto length = static_cast<std::uint32_t>(std::min<std::uintptr_t>(maximum, region_end - address));
    const bool readable =
        region.State == MEM_COMMIT && region.Type == MEM_IMAGE && region.AllocationBase == module_ && readable_image_page(region.Protect);
    return {length, readable};
  }

  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    if (rva >= limit_ || size > limit_ - rva || (size != 0 && destination == nullptr)) {
      return false;
    }
    auto* output = static_cast<std::uint8_t*>(destination);
    std::size_t completed = 0;
    while (completed < size) {
      const auto offset = rva + static_cast<std::uint32_t>(completed);
      const auto window = query(offset, static_cast<std::uint32_t>(size - completed));
      if (!window.readable || window.size == 0 || window.size > size - completed) {
        return false;
      }
      SIZE_T copied = 0;
      // VirtualQuery is a snapshot. ReadProcessMemory checks the selected process
      // mapping again and fails safely if protection changes before this read.
      read_attempted_ = true;
      if (!ReadProcessMemory(process_, reinterpret_cast<const void*>(base_ + offset), output + completed, window.size, &copied) ||
          copied != window.size) {
        last_read_error_ = GetLastError();
        return false;
      }
      completed += window.size;
    }
    return true;
  }

  DWORD last_read_error() const { return last_read_error_; }
  bool read_attempted() const { return read_attempted_; }

 private:
  HANDLE process_;
  HMODULE module_;
  std::uintptr_t base_;
  std::uint32_t limit_;
  DWORD last_read_error_ = 0;
  bool read_attempted_ = false;
};

// Only the explicit service mode uses this one-shot object reader. It reads the
// cached renderer object's vptr word, never its payload or other heap objects.
class ServiceObjectReader final : public ObjectVptrReader {
 public:
  explicit ServiceObjectReader(HANDLE process) : process_(process) {}
  bool read_vptr(std::uint64_t object_address, std::uint64_t& value) override {
    value = 0;
    if (used_)
      return false;
    used_ = true;
    if (object_address == 0 || object_address % alignof(std::uint64_t) != 0 ||
        object_address > std::numeric_limits<std::uintptr_t>::max() - sizeof(value))
      return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQueryEx(process_, reinterpret_cast<const void*>(object_address), &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || region.Type != MEM_PRIVATE || !readable_image_page(region.Protect))
      return false;
    const auto region_base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (region_base > object_address || object_address - region_base > region.RegionSize ||
        sizeof(value) > region.RegionSize - (object_address - region_base))
      return false;
    SIZE_T copied = 0;
    std::uint64_t word = 0;
    if (!ReadProcessMemory(process_, reinterpret_cast<const void*>(object_address), &word, sizeof(word), &copied) || copied != sizeof(word))
      return false;
    value = word;
    return true;
  }

 private:
  HANDLE process_;
  bool used_ = false;
};

// This reader serves only the fixed command-list type chain. It cannot scan:
// only four/eight-byte fields and32 attempted bytes are accepted per snapshot.
class CommandListMemoryReader final : public CommandListObjectReader {
 public:
  explicit CommandListMemoryReader(HANDLE process) : process_(process) {}
  bool read(std::uint64_t address, void* destination, std::size_t size) override {
    if (destination == nullptr || (size != 4 && size != 8) || address == 0 || address % size != 0 ||
        address > std::numeric_limits<std::uintptr_t>::max() - size || attempted_ > 32 - size)
      return false;
    attempted_ += size;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQueryEx(process_, reinterpret_cast<const void*>(address), &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || region.Type != MEM_PRIVATE || !readable_image_page(region.Protect))
      return false;
    const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (base > address || region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base || address - base > region.RegionSize ||
        size > region.RegionSize - (address - base))
      return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(process_, reinterpret_cast<const void*>(address), destination, size, &copied) && copied == size;
  }

 private:
  HANDLE process_;
  std::size_t attempted_ = 0;
};

// The aircraft graph and source-pose parsers supply only explicit fixed field
// reads. All stages share one 8192-byte allowance; no memory scan is exposed.
class AircraftMemoryReader final : public AircraftObjectReader, public engine_camera::MemoryReader {
 public:
  explicit AircraftMemoryReader(HANDLE process) : process_(process) {}
  bool read(std::uint64_t address, void* destination, std::size_t size) override {
    if (destination == nullptr || (size != 1 && size != 2 && size != 4 && size != 8 && size != 16 && size != 24) || address == 0 ||
        address > std::numeric_limits<std::uintptr_t>::max() - size || attempted_ > 8192 - size)
      return false;
    attempted_ += size;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQueryEx(process_, reinterpret_cast<const void*>(address), &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || region.Type != MEM_PRIVATE || !readable_image_page(region.Protect))
      return false;
    const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (base > address || region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base || address - base > region.RegionSize ||
        size > region.RegionSize - (address - base))
      return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(process_, reinterpret_cast<const void*>(address), destination, size, &copied) && copied == size;
  }

 private:
  HANDLE process_;
  std::size_t attempted_ = 0;
};

struct OwnedHandle {
  HANDLE value = nullptr;
  ~OwnedHandle() {
    if (value != nullptr) {
      CloseHandle(value);
    }
  }
  OwnedHandle() = default;
  OwnedHandle(const OwnedHandle&) = delete;
  OwnedHandle& operator=(const OwnedHandle&) = delete;
};

bool fail(MainModuleInventory& result, const char* message, DWORD error = 0) {
  result.windows_error = error;
  result.image.error = message;
  return false;
}

std::wstring basename(const std::wstring& path) {
  const auto separator = path.find_last_of(L"/\\");
  return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

bool file_identity(const std::wstring& path, FILE_ID_INFO& identity, MainModuleInventory& result) {
  // This opens attributes only, not file contents. The mapped executable can
  // have Xbox/package path aliases even when both names refer to the same file.
  const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return fail(result, "Opening executable file attributes for the alias identity check failed.", GetLastError());
  }
  OwnedHandle file;
  file.value = handle;
  if (!GetFileInformationByHandleEx(file.value, FileIdInfo, &identity, sizeof(identity))) {
    return fail(result, "Reading executable file identity for the alias check failed.", GetLastError());
  }
  return true;
}

bool same_executable_file(const std::wstring& process_path, const std::wstring& module_path, MainModuleInventory& result) {
  if (CompareStringOrdinal(module_path.c_str(), static_cast<int>(module_path.size()), process_path.c_str(),
                           static_cast<int>(process_path.size()), TRUE) == CSTR_EQUAL) {
    result.identity_method = "matching_path";
    return true;
  }
  FILE_ID_INFO process_file{};
  FILE_ID_INFO module_file{};
  if (!file_identity(process_path, process_file, result) || !file_identity(module_path, module_file, result)) {
    return false;
  }
  if (process_file.VolumeSerialNumber != module_file.VolumeSerialNumber ||
      !std::equal(std::begin(process_file.FileId.Identifier), std::end(process_file.FileId.Identifier),
                  std::begin(module_file.FileId.Identifier))) {
    return fail(result, "The process executable and enumerated module have different Windows file identities.");
  }
  result.identity_method = "matching_file_id";
  return true;
}

bool token_user(HANDLE process, std::vector<std::uint8_t>& data, MainModuleInventory& result) {
  OwnedHandle token;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) {
    return fail(result, "Reading the process user token failed.", GetLastError());
  }
  DWORD required = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &required);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0 || required > 65536) {
    return fail(result, "The process user token has an invalid size.", GetLastError());
  }
  data.resize(required);
  if (!GetTokenInformation(token.value, TokenUser, data.data(), required, &required)) {
    return fail(result, "Reading the process user token failed.", GetLastError());
  }
  return true;
}

std::vector<std::uint32_t> camera_context_requests(CameraContextKind kind) {
  switch (kind) {
    case CameraContextKind::creation:
      return {57418038, 66846576, 57402512, 57971334, 66843808, 65115456, 70016368, 66257872};
    case CameraContextKind::methods:
      return {70018288, 57390544, 70017040, 70017280, 17648544, 17642943};
    case CameraContextKind::view_setup:
      return {31727952, 66809728, 66823440, 66823664, 66825216, 66843280, 17641776, 17641440};
    case CameraContextKind::view_state:
      // Pose/getter leaf calls require these complete, proven caller contexts.
      return {17648544, 17642240, 55910592, 55910752, 4195952};
    case CameraContextKind::service:
      return {4164736, 66809728, 17646000};
    case CameraContextKind::leaves:
      return {64652608, 17648544, 31727952};
    case CameraContextKind::aircraft:
      return {37638528, 58093024, 57963344, 17648544, 56053760, 4164736, 31727952};
    case CameraContextKind::command_list:
      // Captured resource worker call at 64658928; its output is stored in
      // renderer+237744 and passed to native resource-record creation.
      return {64597824};
    case CameraContextKind::none:
      return {};
  }
  return {};
}

void inspect_verified(MainModuleInventory& result, HANDLE process, HMODULE module, Limits limits) {
  MODULEINFO information{};
  if (!K32GetModuleInformation(process, module, &information, sizeof(information)) || information.lpBaseOfDll != module ||
      information.SizeOfImage == 0 || information.SizeOfImage > kMaximumImageSize) {
    fail(result, "Reading the main-image mapping bounds failed.", GetLastError());
    return;
  }
  MainImageReader reader(process, module, information.SizeOfImage);
  result.mapped_image_size = information.SizeOfImage;
  MEMORY_BASIC_INFORMATION header_region{};
  if (VirtualQueryEx(process, module, &header_region, sizeof(header_region)) == sizeof(header_region)) {
    result.header_region_state = header_region.State;
    result.header_region_type = header_region.Type;
    result.header_region_protection = header_region.Protect;
    result.header_allocation_matches = header_region.AllocationBase == module;
  } else {
    result.header_query_error = GetLastError();
  }
  result.image = inspect_image(reader, limits);
  result.last_memory_read_error = reader.last_read_error();
  result.memory_read_attempted = reader.read_attempted();
  if (result.image.valid_image && result.image.image_size != information.SizeOfImage) {
    result.image.valid_image = false;
    result.image.error = "The PE image size does not match the verified main-module mapping.";
    result.image.records.clear();
  }
  if ((result.references_requested || result.functions_requested) && result.image.valid_image) {
    std::string decoder_error;
    auto decoder = create_pinned_llvm_decoder(decoder_error);
    if (decoder) {
      result.llvm_decoder_ready = true;
      const std::vector<LiteralTarget> targets{{"camera_to_texture_diagnostic", 130434128, "CameraToTexture: Id = %d, (VpId = %d)"},
                                               {"camera_to_texture_name", 131739350, "8wCameraToTexture"},
                                               {"camera_to_texture_manager", 133576352, "CCameraToTextureMgr_G"}};
      if (result.references_requested) {
        result.references = result.callers_requested ? find_direct_call_references(reader, result.image, *decoder, targets,
                                                                                   {{"camera_entry_setup", 17642240},
                                                                                    {"camera_entry_creation", 17646976},
                                                                                    {"camera_entry_visibility", 17641776},
                                                                                    {"camera_manager_update", 17648544},
                                                                                    {"camera_record_reset", 75233904},
                                                                                    {"camera_entry_payload_destroy", 17640768}})
                                                     : find_literal_references(reader, result.image, *decoder, targets);
      }
      if (result.functions_requested) {
        // Earlier captures establish the registration site/callee, its stored
        // callback, and a manager-name use in the engine initialization function.
        // That last function is 8735 bytes, so this fixed request uses a 16 KiB
        // prefix cap while retaining the parser's 32 KiB aggregate code limit.
        // No discovered address is cast to a pointer or invoked.
        const auto requested = camera_context_requests(result.camera_context_kind);
        result.functions = inspect_function_contexts(reader, result.image, *decoder, targets, requested, 16384);
        if (result.functions.valid_targets && result.camera_context_kind == CameraContextKind::view_state) {
          result.setup_literals = inspect_known_pose_type_names(reader, result.image);
        }
        if (result.functions.valid_targets && !result.functions.partial && result.functions.error.empty()) {
          // The captured callback stores this static table address at object+0.
          // Normalize its bounded pointer prefix to RVAs; never call its entries.
          result.manager_pointer_table = inspect_pointer_table(reader, result.image, reinterpret_cast<std::uintptr_t>(module), 133571232);
          result.guard_metadata = inspect_guard_metadata(reader, result.image, reinterpret_cast<std::uintptr_t>(module));
          if (result.camera_context_kind == CameraContextKind::command_list) {
            CommandListMemoryReader object_reader(process);
            result.command_list_type =
                inspect_command_list_type(reader, object_reader, result.image, reinterpret_cast<std::uintptr_t>(module), result.functions);
          }
          if (result.camera_context_kind == CameraContextKind::view_state) {
            result.code_contract = native_camera::inspect_code_contract(reader, result.image, native_camera::profile_ranges());
            if (result.code_contract.valid)
              result.activation_disable_mask = native_camera::inspect_activation_disable_mask(reader, result.image);
            // Query this one known update-vtable slot's page metadata only.
            // This does not read its pointer, change protection or write memory.
            constexpr std::uint32_t slot_rva = 133571352;
            const auto image_base = reinterpret_cast<std::uintptr_t>(module);
            if (slot_rva <= result.image.image_size && 8 <= result.image.image_size - slot_rva &&
                image_base <= std::numeric_limits<std::uintptr_t>::max() - slot_rva - 8) {
              const auto slot_address = image_base + slot_rva;
              MEMORY_BASIC_INFORMATION region{};
              if (VirtualQueryEx(process, reinterpret_cast<const void*>(slot_address), &region, sizeof(region)) == sizeof(region)) {
                const auto start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
                result.manager_slot_protection = region.Protect;
                result.manager_slot_protection_valid =
                    region.State == MEM_COMMIT && region.Type == MEM_IMAGE && region.AllocationBase == module &&
                    readable_image_page(region.Protect) && start <= slot_address &&
                    region.RegionSize <= std::numeric_limits<std::uintptr_t>::max() - start && slot_address - start < region.RegionSize &&
                    8 <= region.RegionSize - (slot_address - start);
              } else {
                result.manager_slot_query_error = GetLastError();
              }
            }
            result.pose_constants = inspect_static_numeric(reader, result.image,
                                                           {{134592512, StaticNumericKind::float64, 3},
                                                            {134592416, StaticNumericKind::float64, 3},
                                                            {130053792, StaticNumericKind::float64, 1},
                                                            {129242640, StaticNumericKind::float32, 1},
                                                            {129524392, StaticNumericKind::float32, 1}});
            result.callee_prefixes = inspect_callee_prefixes(reader, result.image, *decoder, result.functions,
                                                             {{17648926, 55910592},
                                                              {17648941, 55910752},
                                                              {17648982, 4195952},
                                                              {17643563, 66859264},
                                                              {17650314, 66859296},
                                                              {17650342, 66859312},
                                                              {55910608, 70721312}});
          }
          if (result.camera_context_kind == CameraContextKind::leaves) {
            result.callee_prefixes = inspect_callee_prefixes(reader, result.image, *decoder, result.functions,
                                                             {{64652794, 66500352},
                                                              {17652131, 66859376},
                                                              {17652187, 66859344},
                                                              {17648869, 55914960},
                                                              {31727996, 55910528},
                                                              {17649193, 67705600}});
            result.branch_prefixes = inspect_branch_prefixes(reader, result.image, *decoder, result.callee_prefixes,
                                                             {{55914970, 80718912}, {55910556, 55910579}, {67705632, 67705650}});
          }
          const auto has_instruction = [&](std::uint32_t rva, const char* text, std::uint32_t call_target = 0) {
            for (const auto& function : result.functions.functions)
              for (const auto& instruction : function.instructions)
                if (instruction.rva == rva && instruction.text == text && instruction.direct_call_target_rva == call_target)
                  return true;
            return false;
          };
          if (result.camera_context_kind == CameraContextKind::aircraft) {
            result.callee_prefixes = inspect_callee_prefixes(reader, result.image, *decoder, result.functions,
                                                             {{17649193, 67705600}, {17648869, 55914960}, {17648898, 122292224}});
            result.branch_prefixes = inspect_branch_prefixes(reader, result.image, *decoder, result.callee_prefixes,
                                                             {{67705632, 67705650}, {55914970, 80718912}});
            result.component_branch_prefixes =
                inspect_branch_prefixes(reader, result.image, *decoder, result.branch_prefixes, {{80718942, 80718962}});
            const auto has_prefix = [](const auto& prefixes, std::uint32_t rva, const char* text) {
              for (const auto& prefix : prefixes.entries)
                for (const auto& instruction : prefix.instructions)
                  if (instruction.rva == rva && instruction.text == text)
                    return true;
              return false;
            };
            if (result.callee_prefixes.valid && has_prefix(result.callee_prefixes, 122292224, "\tjmpq\t*91376474(%rip)")) {
              result.cast_import = inspect_import_slot(reader, result.image, 213668704);
            }
            if (!result.callee_prefixes.valid || !result.branch_prefixes.valid ||
                !has_instruction(57963359, "\tmovq\t115827130(%rip), %rdx") || !has_instruction(57963373, "\tcmpl\t%ebx, 20(%rdx)") ||
                !has_instruction(57963392, "\tmovq\t24(%rdx), %rsi") || !has_instruction(58093106, "\tcmpl\t736(%rax), %ebx") ||
                !has_instruction(58093121, "\taddq\t784(%rax), %rdx") || !has_instruction(37638554, "\tmovq\t448(%rax), %rcx") ||
                !has_instruction(37638564, "\tmovq\t1256(%rax), %rax") ||
                !has_prefix(result.callee_prefixes, 67705628, "\tcmpl\t$0, 120(%rax)") ||
                !has_prefix(result.callee_prefixes, 67705634, "\tcmpl\t$9, 124(%rax)") ||
                !has_prefix(result.callee_prefixes, 67705638, "\tleaq\t80(%rax), %rcx") ||
                !has_prefix(result.branch_prefixes, 67705650, "\tmovl\t$4294967295, %eax")) {
              result.aircraft.stage = "build_context";
              result.aircraft.error = "The captured aircraft lookup context no longer matches; no aircraft-object fields were read.";
            } else {
              AircraftMemoryReader object_reader(process);
              result.aircraft = inspect_aircraft_metadata(reader, object_reader, result.image, reinterpret_cast<std::uintptr_t>(module));
              if (result.aircraft.valid && result.aircraft.selected && result.aircraft.method_rva != 0) {
                result.aircraft_method_prefix =
                    inspect_slot_prefixes(reader, result.image, *decoder, reinterpret_cast<std::uintptr_t>(module),
                                          {{result.aircraft.method_slot_rva, result.aircraft.method_rva}});
                if (result.aircraft_method_prefix.valid && result.aircraft.facade_vtable_rva == 133538936 &&
                    result.aircraft.method_rva == 56053760 &&
                    has_prefix(result.aircraft_method_prefix, 56053766, "\tmovq\t1648(%rcx), %rdi") &&
                    has_prefix(result.aircraft_method_prefix, 56053779, "\tmovq\t432(%rax), %rax")) {
                  // Reuse the same reader so all graph captures share one
                  // 8192-byte object-read cap. The second capture revalidates
                  // its complete chain before reporting the controller slot.
                  result.aircraft_controller =
                      inspect_aircraft_metadata(reader, object_reader, result.image, reinterpret_cast<std::uintptr_t>(module), 133538936);
                  if (result.aircraft_controller.valid && result.aircraft_controller.object_present &&
                      result.aircraft_controller.object_method_rva != 0) {
                    result.aircraft_controller_method_prefix = inspect_slot_prefixes(
                        reader, result.image, *decoder, reinterpret_cast<std::uintptr_t>(module),
                        {{result.aircraft_controller.object_method_slot_rva, result.aircraft_controller.object_method_rva}});
                    if (result.aircraft_controller_method_prefix.valid && result.aircraft_controller.object_vtable_rva == 133534840 &&
                        result.aircraft_controller.object_method_rva == 56030192 &&
                        has_prefix(result.aircraft_controller_method_prefix, 56030192, "\tmovl\t676(%rcx), %eax") &&
                        has_prefix(result.aircraft_controller_method_prefix, 56030198, "\tretq") &&
                        has_instruction(56053792, "\ttestl\t%eax, %eax") && has_instruction(56053794, "\tjns\t8") &&
                        has_instruction(4164816, "\tmovq\t64(%rcx), %rax") && has_instruction(56053843, "\tleaq\t117736470(%rip), %rcx") &&
                        has_instruction(56053850, "\tcallq\t-51889119", 4164736) && has_instruction(56053855, "\txorl\t%ebx, %ebx") &&
                        has_instruction(56053857, "\tcmpb\t$1, 2800(%rax)") && has_instruction(56053864, "\tjne\t72") &&
                        has_instruction(56053882, "\tmovq\t296(%rdi), %rcx") && has_instruction(56053889, "\tmovl\t304(%rdi), %eax") &&
                        has_instruction(56053895, "\tcmpl\t%eax, 28(%rcx)") && has_instruction(56053918, "\tmovq\t752(%rax), %rax") &&
                        has_instruction(56053994, "\tmovslq\t672(%rdi), %rax") && has_instruction(56054001, "\tshlq\t$4, %rax") &&
                        has_instruction(56054005, "\taddq\t752(%rcx), %rax") && has_instruction(56054025, "\tmovl\t8(%rax), %eax") &&
                        has_instruction(56054028, "\tcmpl\t%eax, 28(%rcx)") && has_instruction(56054038, "\tmovq\t(%rcx), %rcx") &&
                        has_instruction(56054044, "\tmovl\t$4294967295, %edx") && has_instruction(56054049, "\tmovq\t344(%rax), %rax") &&
                        has_instruction(56054056, "\tcallq\t*71851546(%rip)")) {
                      result.aircraft_selected_object = inspect_aircraft_metadata(
                          reader, object_reader, result.image, reinterpret_cast<std::uintptr_t>(module), 133538936, true);
                      if (result.aircraft_selected_object.valid && result.aircraft_selected_object.available &&
                          result.aircraft_selected_object.selected_object_present) {
                        result.aircraft_selected_object_method_prefix =
                            inspect_slot_prefixes(reader, result.image, *decoder, reinterpret_cast<std::uintptr_t>(module),
                                                  {{result.aircraft_selected_object.selected_object_method_slot_rva,
                                                    result.aircraft_selected_object.selected_object_method_rva}});
                        // The selected method returns its embedded handle;
                        // this stage resolves it and records only the first
                        // type-5 component's vtable metadata, not its payload.
                        if (result.aircraft_selected_object_method_prefix.valid && result.component_branch_prefixes.valid &&
                            result.aircraft_selected_object.selected_object_vtable_rva == 133503528 &&
                            result.aircraft_selected_object.selected_object_method_rva == 55874752 &&
                            has_prefix(result.aircraft_selected_object_method_prefix, 55874752, "\tleaq\t368(%rcx), %rax") &&
                            has_prefix(result.aircraft_selected_object_method_prefix, 55874759, "\tretq") &&
                            has_prefix(result.callee_prefixes, 55914960, "\tmovq\t19272(%rcx), %rcx") &&
                            has_prefix(result.branch_prefixes, 80718912, "\tmovslq\t36(%rcx), %rax") &&
                            has_prefix(result.branch_prefixes, 80718923, "\tmovq\t40(%rcx), %r11") &&
                            has_prefix(result.branch_prefixes, 80718939, "\tcmpl\t%edx, 32(%rcx)") &&
                            has_prefix(result.component_branch_prefixes, 80718962, "\tmovslq\t%r9d, %rax") &&
                            has_prefix(result.component_branch_prefixes, 80718965, "\tmovq\t(%r11,%rax,8), %rax") &&
                            has_prefix(result.component_branch_prefixes, 80718969, "\tretq") &&
                            has_instruction(17648675, "\txorl\t%r12d, %r12d") && has_instruction(17648861, "\tleal\t5(%r12), %edx") &&
                            has_instruction(17648869, "\tcallq\t38266086", 55914960)) {
                          std::uint64_t verified_source = 0;
                          result.aircraft_component =
                              inspect_aircraft_metadata(reader, object_reader, result.image, reinterpret_cast<std::uintptr_t>(module),
                                                        133538936, true, true, false, &verified_source);
                          if (result.aircraft_component.valid && result.aircraft_component.available &&
                              result.aircraft_component.component_present && verified_source != 0) {
                            result.code_contract =
                                native_camera::verify_code_contract(reader, result.image, native_camera::verified_profile());
                            if (result.code_contract.valid) {
                              // No engine call is made. The source is borrowed only
                              // for this bounded consistency-checked diagnostic.
                              result.source_pose = native_camera::inspect_source_pose(object_reader, verified_source);
                            } else {
                              result.source_pose.error = "The fresh code contract did not match; no source-pose fields were read.";
                            }
                          }
                          if (result.aircraft_component.valid && result.aircraft_component.available &&
                              result.aircraft_component.component_present && has_instruction(17648874, "\tmovq\t%rax, %rcx") &&
                              has_instruction(17648877, "\tmovl\t%r12d, 32(%rsp)") &&
                              has_instruction(17648882, "\tleaq\t148288495(%rip), %r9") &&
                              has_instruction(17648889, "\tleaq\t148480808(%rip), %r8") &&
                              has_instruction(17648896, "\txorl\t%edx, %edx") &&
                              has_instruction(17648898, "\tcallq\t104643321", 122292224)) {
                            result.component_rtti = inspect_rtti_metadata(reader, result.image, reinterpret_cast<std::uintptr_t>(module),
                                                                          result.aircraft_component.component_vtable_rva);
                            if (verified_camera_component_layout(result.component_rtti) && result.cast_import.valid &&
                                result.cast_import.available && result.cast_import.symbol == "__RTDynamicCast" &&
                                has_instruction(31727967, "\tcmpl\t$0, 108(%rcx)") && has_instruction(31727974, "\tmovq\t%rcx, %rdi") &&
                                has_instruction(31728008, "\tmovl\t108(%rdi), %r9d") &&
                                has_instruction(31728037, "\tmovq\t112(%rdi), %r10") &&
                                has_instruction(31728051, "\tmovq\t(%r10,%r8,8), %rcx") &&
                                has_instruction(31728055, "\tcmpl\t%r11d, 72(%rcx)") &&
                                has_instruction(31728064, "\tcmpl\t%eax, 76(%rcx)") &&
                                has_instruction(31728072, "\tcmpl\t%eax, 80(%rcx)") &&
                                has_instruction(31728080, "\tcmpl\t%eax, 84(%rcx)")) {
                              result.camera_keys =
                                  inspect_aircraft_metadata(reader, object_reader, result.image, reinterpret_cast<std::uintptr_t>(module),
                                                            133538936, true, true, true);
                            } else {
                              result.camera_keys.stage = "key_layout_context";
                              result.camera_keys.error = "The fixed component layout, cast import or camera-key lookup is not verified.";
                            }
                          }
                        } else {
                          result.aircraft_component.stage = "component_context";
                          result.aircraft_component.error = "The aircraft handle and component lookup context no longer match.";
                        }
                      }
                    } else {
                      result.aircraft_selected_object.stage = "selection_context";
                      result.aircraft_selected_object.error =
                          "The controller getter and selected-object accessor context no longer match; no selection fields were read.";
                    }
                  }
                }
              }
            }
          }
          if (result.camera_context_kind == CameraContextKind::service) {
            // Tie the writable-slot/object-read scope to this captured build
            // and the decoded cached offset and virtual-call context.
            if (result.image.timestamp != 1787653788 || result.image.image_size != 235963904 || result.image.section_count != 14 ||
                !has_instruction(4164816, "\tmovq\t64(%rcx), %rax") || !has_instruction(66811430, "\tleaq\t106978883(%rip), %rcx") ||
                !has_instruction(66811437, "\tcallq\t-62646706", 4164736) || !has_instruction(66811448, "\tmovq\t808(%rcx), %r11") ||
                !has_instruction(17646417, "\tcallq\t-13481686", 4164736) || !has_instruction(17646428, "\tmovq\t104(%rcx), %rax") ||
                !has_instruction(17646432, "\tmovq\t%rbx, %rdx") || !has_instruction(17646435, "\tmovq\t%r8, %rcx") ||
                !has_instruction(17646438, "\tcallq\t*110259164(%rip)")) {
              result.renderer_service.stage = "build_context";
              result.renderer_service.error =
                  "The captured build and service accessor/call context no longer match; no pointer words were read.";
            } else {
              ServiceObjectReader object_reader(process);
              result.renderer_service =
                  inspect_service_metadata(reader, object_reader, result.image, reinterpret_cast<std::uintptr_t>(module));
            }
          }
          if (result.camera_context_kind == CameraContextKind::view_setup) {
            // Exact RIP-relative LEA targets observed in the setup and output
            // methods; not a section sweep or live-object/string-pointer read.
            result.setup_literals = inspect_static_literals(
                reader, result.image, {129524228, 129524296, 134609320, 130479680, 129600184, 134609352, 134609384, 134609424});
          }
        }
      }
    } else {
      if (result.references_requested)
        result.references.error = decoder_error;
      if (result.functions_requested)
        result.functions.error = decoder_error;
    }
  }
  result.last_memory_read_error = reader.last_read_error();
  result.memory_read_attempted = reader.read_attempted();
}

}  // namespace

MainModuleInventory inspect_main_module(HMODULE explicitly_selected_main, Limits limits) {
  MainModuleInventory result;
  if (explicitly_selected_main == nullptr || explicitly_selected_main != GetModuleHandleW(nullptr)) {
    result.image.error = "Only an explicitly selected main executable module is permitted.";
    return result;
  }
  result.main_module_verified = true;
  result.identity_method = "current_main_handle";
  result.process_id = GetCurrentProcessId();
  std::array<wchar_t, 32768> path{};
  const auto length = GetModuleFileNameW(explicitly_selected_main, path.data(), static_cast<DWORD>(path.size()));
  if (length != 0 && length < path.size()) {
    const std::wstring full_path(path.data(), length);
    result.module_name = basename(full_path);
  }
  inspect_verified(result, GetCurrentProcess(), explicitly_selected_main, limits);
  return result;
}

MainModuleInventory inspect_simulator_main_module(std::uint32_t explicitly_selected_pid,
                                                  Limits limits,
                                                  bool include_references,
                                                  CameraContextKind camera_context,
                                                  bool include_callers) {
  MainModuleInventory result;
  result.process_id = explicitly_selected_pid;
  result.references_requested = include_references || include_callers;
  result.callers_requested = include_callers;
  result.camera_context_kind = camera_context;
  result.functions_requested = camera_context != CameraContextKind::none;
  if (result.references_requested && result.functions_requested) {
    fail(result, "Reference scanning and bounded camera contexts are separate inspection modes.");
    return result;
  }
  if (result.functions_requested || include_callers) {
    // Camera-context mode needs only PE metadata plus exact literals. Avoid
    // repeating the broad string/export inventory from earlier discovery.
    limits.scan_bytes = 0;
    limits.export_names = 0;
  }
  if (explicitly_selected_pid == 0) {
    fail(result, "An explicit nonzero simulator PID is required.");
    return result;
  }
  OwnedHandle process;
  process.value = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, explicitly_selected_pid);
  if (process.value == nullptr) {
    fail(result, "OpenProcess with PROCESS_QUERY_INFORMATION | PROCESS_VM_READ failed.", GetLastError());
    return result;
  }
  std::array<wchar_t, 32768> process_path{};
  DWORD path_size = static_cast<DWORD>(process_path.size());
  if (!QueryFullProcessImageNameW(process.value, 0, process_path.data(), &path_size) || path_size == 0 ||
      path_size >= process_path.size()) {
    fail(result, "Reading the selected process executable identity failed.", GetLastError());
    return result;
  }
  const std::wstring executable_path(process_path.data(), path_size);
  result.module_name = basename(executable_path);
  if (CompareStringOrdinal(result.module_name.c_str(), -1, L"FlightSimulator2024.exe", -1, TRUE) != CSTR_EQUAL) {
    fail(result, "The selected PID is not FlightSimulator2024.exe.");
    return result;
  }
  std::vector<std::uint8_t> current_user;
  std::vector<std::uint8_t> process_user;
  if (!token_user(GetCurrentProcess(), current_user, result) || !token_user(process.value, process_user, result)) {
    return result;
  }
  if (!EqualSid(reinterpret_cast<const TOKEN_USER*>(current_user.data())->User.Sid,
                reinterpret_cast<const TOKEN_USER*>(process_user.data())->User.Sid)) {
    fail(result, "The selected simulator process belongs to a different user.");
    return result;
  }
  // Request just the first module handle, then prove that its filename is the
  // selected process image. A mismatch is rejected, never searched around.
  HMODULE main_module = nullptr;
  DWORD module_bytes = 0;
  if (!K32EnumProcessModulesEx(process.value, &main_module, sizeof(main_module), &module_bytes, LIST_MODULES_64BIT) ||
      module_bytes < sizeof(main_module) || main_module == nullptr) {
    fail(result, "Reading the selected process main-module handle failed.", GetLastError());
    return result;
  }
  std::array<wchar_t, 32768> module_path{};
  const auto module_name_size =
      K32GetModuleFileNameExW(process.value, main_module, module_path.data(), static_cast<DWORD>(module_path.size()));
  if (module_name_size == 0 || module_name_size >= module_path.size()) {
    fail(result, "Reading the enumerated main-module filename failed.", GetLastError());
    return result;
  }
  const std::wstring enumerated_path(module_path.data(), module_name_size);
  result.candidate_module_name = basename(enumerated_path);
  if (CompareStringOrdinal(result.candidate_module_name.c_str(), -1, L"FlightSimulator2024.exe", -1, TRUE) != CSTR_EQUAL) {
    fail(result, "The first enumerated module is not FlightSimulator2024.exe.");
    return result;
  }
  if (!same_executable_file(executable_path, enumerated_path, result)) {
    return result;
  }
  result.main_module_verified = true;
  inspect_verified(result, process.value, main_module, limits);
  return result;
}

}  // namespace taxi_camera::discovery
