#include "command_list_inventory.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace taxi_camera::discovery {
namespace {
constexpr std::uint32_t Global = 173790384;
constexpr std::uint32_t Vtable = 134482536;
constexpr std::uint32_t TypeOffset = 284576;
constexpr std::uint32_t Readable = 0x40000000;
constexpr std::uint32_t Writable = 0x80000000;
constexpr std::uint32_t Executable = 0x20000000;
constexpr std::uint32_t Discardable = 0x02000000;

bool section(const Inventory& image, std::uint32_t rva, std::uint32_t size, std::uint32_t require, std::uint32_t exclude) {
  if (rva >= image.image_size || size == 0 || size > image.image_size - rva)
    return false;
  return std::any_of(image.sections.begin(), image.sections.end(), [&](const ImageSection& entry) {
    const auto end = static_cast<std::uint64_t>(entry.rva) + entry.size;
    return (entry.flags & require) == require && (entry.flags & exclude) == 0 && end <= image.image_size && rva >= entry.rva && rva < end &&
           size <= end - rva;
  });
}

std::uint64_t little(const std::uint8_t* bytes, std::size_t size) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < size; ++i)
    value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
  return value;
}

bool proof(const Inventory& image, const FunctionInventory& functions) {
  if (!functions.valid_targets || functions.partial || !functions.error.empty() || functions.read_failures != 0 ||
      functions.missing_function_bounds != 0 || functions.functions.size() != 1 || functions.code_bytes != 680 ||
      functions.decoded_bytes != 680 || !section(image, 64597824, 680, Readable | Executable, Writable | Discardable))
    return false;
  const auto& f = functions.functions.front();
  if (f.requested_rva != 64597824 || f.function_begin_rva != 64597824 || f.function_end_rva != 64598504 || f.bytes_read != 680 ||
      f.decoded_bytes != 680 || f.truncated || f.decode_failed || !f.requested_boundary_found || !f.error.empty())
    return false;
  const auto has = [&](std::uint32_t rva, const char* text) {
    return std::count_if(f.instructions.begin(), f.instructions.end(), [&](const InstructionMetadata& i) {
             return i.rva == rva && i.text == text && i.direct_call_target_rva == 0;
           }) == 1;
  };
  return has(64597849, "\tmovq\t%rcx, %rdi") && has(64598075, "\tmovq\t(%rdi), %rcx") && has(64598089, "\tmovq\t(%rcx), %rdx") &&
         has(64598099, "\tmovq\t96(%rdx), %rax") && has(64598103, "\tmovq\t%r8, 48(%rsp)") && has(64598115, "\tmovq\t%rdx, 40(%rsp)") &&
         has(64598120, "\tmovq\t%rbx, 32(%rsp)") && has(64598125, "\tmovq\t16(%rdi), %r9") && has(64598129, "\tmovl\t208(%rdi), %r8d") &&
         has(64598136, "\txorl\t%edx, %edx") && has(64598138, "\tcallq\t*63307464(%rip)") && has(64598461, "\tmovq\t24(%rdi), %rax") &&
         has(64598465, "\tmovq\t%rax, (%r15)") && has(64598503, "\tretq");
}
}  // namespace

CommandListInventory inspect_command_list_type(ImageReader& image_reader,
                                               CommandListObjectReader& object_reader,
                                               const Inventory& image,
                                               std::uint64_t loaded_image_base,
                                               const FunctionInventory& fresh_proof) {
  CommandListInventory out;
  if (!image.valid_image || image.machine != 0x8664 || image.timestamp != 1787653788 || image.image_size != 235963904 ||
      image.section_count != 14 || loaded_image_base == 0 ||
      loaded_image_base > std::numeric_limits<std::uint64_t>::max() - image.image_size || !proof(image, fresh_proof)) {
    out.error = "The fixed build and complete CreateCommandList context are not verified.";
    return out;
  }
  if (!section(image, Global, 8, Readable | Writable, Executable | Discardable) ||
      !section(image, Vtable, 8, Readable, Writable | Executable | Discardable)) {
    out.stage = "image_fields";
    out.error = "The fixed cached global or renderer vtable is outside its declared image section.";
    return out;
  }
  std::array<std::uint8_t, 8> bytes{};
  const auto global_read = [&](std::uint64_t& value) {
    out.image_bytes += 8;
    const auto window = image_reader.query(Global, 8);
    if (!window.readable || window.size != 8 || !image_reader.read(Global, bytes.data(), 8)) {
      ++out.read_failures;
      out.error = "The fixed cached-renderer word is unreadable.";
      return false;
    }
    value = little(bytes.data(), 8);
    return true;
  };
  std::uint64_t renderer = 0;
  out.stage = "cached_global";
  if (!global_read(renderer))
    return out;
  if (renderer == 0) {
    out.valid = true;
    out.stage = "unavailable";
    return out;
  }
  out.stage = "object_bounds";
  if (renderer % 8 != 0 || renderer > std::numeric_limits<std::uint64_t>::max() - TypeOffset - 4) {
    out.error = "The renderer fields are misaligned or overflow.";
    return out;
  }
  const auto object_read = [&](std::uint32_t offset, std::size_t size, std::uint64_t& value) {
    out.object_bytes += static_cast<std::uint32_t>(size);
    if (!object_reader.read(renderer + offset, bytes.data(), size)) {
      ++out.read_failures;
      out.error = "The fixed renderer field is unreadable.";
      return false;
    }
    value = little(bytes.data(), size);
    return true;
  };
  std::uint64_t vptr = 0;
  out.stage = "renderer_vptr";
  if (!object_read(0, 8, vptr))
    return out;
  if (vptr != loaded_image_base + Vtable) {
    out.error = "The cached renderer does not have the verified main-image vtable.";
    return out;
  }
  out.vtable_rva = Vtable;
  std::uint64_t type = 0;
  out.stage = "command_list_type";
  if (!object_read(TypeOffset, 4, type))
    return out;
  if (type > 3) {
    out.error = "The command-list type is outside the four supported graphics-list types.";
    return out;
  }
  std::uint64_t checked = 0;
  out.stage = "type_recheck";
  if (!object_read(TypeOffset, 4, checked))
    return out;
  if (checked != type) {
    out.error = "The command-list type changed during inspection.";
    return out;
  }
  out.stage = "vptr_recheck";
  if (!object_read(0, 8, checked))
    return out;
  if (checked != vptr) {
    out.error = "The renderer vptr changed during inspection.";
    return out;
  }
  out.stage = "global_recheck";
  if (!global_read(checked))
    return out;
  if (checked != renderer) {
    out.error = "The cached renderer changed during inspection.";
    return out;
  }
  out.type = static_cast<std::uint32_t>(type);
  out.available = true;
  out.valid = true;
  out.stage = "complete";
  return out;
}
}  // namespace taxi_camera::discovery
