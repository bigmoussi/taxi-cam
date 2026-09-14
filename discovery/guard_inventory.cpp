#include "guard_inventory.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kPrefixSize = 148;
constexpr std::uint32_t kReadable = 0x40000000;
constexpr std::uint32_t kExcluded = 0x80000000 | 0x20000000 | 0x02000000;

std::uint32_t u32(const std::uint8_t* bytes) {
  return bytes[0] | (static_cast<std::uint32_t>(bytes[1]) << 8) | (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t u64(const std::uint8_t* bytes) {
  return u32(bytes) | (static_cast<std::uint64_t>(u32(bytes + 4)) << 32);
}

bool static_storage(const Inventory& image, std::uint32_t rva, std::uint32_t size) {
  if (size == 0 || rva >= image.image_size || size > image.image_size - rva)
    return false;
  return std::any_of(image.sections.begin(), image.sections.end(), [&](const ImageSection& section) {
    const auto end = static_cast<std::uint64_t>(section.rva) + section.size;
    return (section.flags & kReadable) != 0 && (section.flags & kExcluded) == 0 && end <= image.image_size && rva >= section.rva &&
           rva < end && size <= end - rva;
  });
}

}  // namespace

GuardInventory inspect_guard_metadata(ImageReader& reader, const Inventory& image, std::uint64_t loaded_image_base) {
  GuardInventory result;
  result.present = image.load_config_rva != 0 || image.load_config_size != 0;
  result.guard_cf_characteristic = (image.dll_characteristics & 0x4000) != 0;
  if (!image.valid_image || image.machine != 0x8664 || image.image_size == 0 || loaded_image_base == 0 ||
      image.image_size > std::numeric_limits<std::uint64_t>::max() - loaded_image_base) {
    result.error = "A validated AMD64 image and non-overflowing loaded-image bounds are required.";
    return result;
  }
  if (!result.present) {
    result.valid = true;
    return result;
  }
  if (image.load_config_rva == 0 || image.load_config_size < kPrefixSize ||
      !static_storage(image, image.load_config_rva, image.load_config_size)) {
    result.error = "The load-config directory is truncated or outside declared read-only, non-executable static storage.";
    return result;
  }
  std::array<std::uint8_t, kPrefixSize> bytes{};
  result.read_bytes = kPrefixSize;
  if (!reader.read(image.load_config_rva, bytes.data(), bytes.size())) {
    result.read_failures = 1;
    result.error = "The bounded load-config prefix is unreadable.";
    return result;
  }
  result.declared_size = u32(bytes.data());
  if (result.declared_size < kPrefixSize || result.declared_size > image.load_config_size) {
    result.error = "The declared load-config structure size does not cover the CFG fields within its directory.";
    return result;
  }
  // IMAGE_LOAD_CONFIG_DIRECTORY64 offsets, verified against the bundled header.
  // GuardCFDispatchFunctionPointer is the VA OF a pointer slot, not its contents.
  result.guard_flags = u32(bytes.data() + 144);
  result.cf_instrumented = (result.guard_flags & 0x100) != 0;
  result.function_table_present = (result.guard_flags & 0x400) != 0;
  const auto slot_va = u64(bytes.data() + 120);
  if (slot_va != 0) {
    if (slot_va < loaded_image_base || slot_va - loaded_image_base >= image.image_size) {
      result.error = "The declared CFG dispatch slot does not belong to the selected loaded image; its address may be stale.";
      return result;
    }
    const auto slot_rva = static_cast<std::uint32_t>(slot_va - loaded_image_base);
    if (!static_storage(image, slot_rva, 8)) {
      result.error = "The declared CFG dispatch slot is outside read-only, non-executable static pointer storage.";
      return result;
    }
    result.dispatch_slot_present = true;
    result.dispatch_slot_rva = slot_rva;
  }
  result.valid = true;
  return result;
}

}  // namespace taxi_camera::discovery
