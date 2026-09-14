#include "service_inventory.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kWordSize = 8;
constexpr std::uint32_t kReadable = 0x40000000;
constexpr std::uint32_t kWritable = 0x80000000;
constexpr std::uint32_t kExecutable = 0x20000000;
constexpr std::uint32_t kDiscardable = 0x02000000;
constexpr std::uint32_t kVtablePrefixSize = std::max(kServiceMethodOffset, kServiceCleanupMethodOffset) + kWordSize;

std::uint64_t u64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (std::uint32_t i = 0; i < kWordSize; ++i)
    value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
  return value;
}

bool section_range(const Inventory& image, std::uint32_t rva, std::uint32_t size, std::uint32_t required, std::uint32_t excluded) {
  if (size == 0 || rva >= image.image_size || size > image.image_size - rva)
    return false;
  return std::any_of(image.sections.begin(), image.sections.end(), [&](const ImageSection& section) {
    const auto end = static_cast<std::uint64_t>(section.rva) + section.size;
    return (section.flags & required) == required && (section.flags & excluded) == 0 && end <= image.image_size && rva >= section.rva &&
           rva < end && size <= end - rva;
  });
}

bool normalize(const Inventory& image, std::uint64_t base, std::uint64_t address, std::uint32_t& rva) {
  if (address < base || address - base >= image.image_size)
    return false;
  rva = static_cast<std::uint32_t>(address - base);
  return true;
}

}  // namespace

ServiceInventory inspect_service_metadata(ImageReader& reader,
                                          ObjectVptrReader& object_reader,
                                          const Inventory& image,
                                          std::uint64_t loaded_image_base) {
  ServiceInventory result;
  if (!image.valid_image || image.machine != 0x8664 || image.image_size == 0 || loaded_image_base == 0 ||
      image.image_size > std::numeric_limits<std::uint64_t>::max() - loaded_image_base) {
    result.error = "A validated AMD64 image and non-overflowing loaded-image bounds are required.";
    return result;
  }

  result.stage = "cached_global";
  if (!section_range(image, kServiceGlobalRva, kWordSize, kReadable | kWritable, kExecutable | kDiscardable)) {
    result.error = "The fixed cached-service slot is outside declared readable, writable, non-executable image data.";
    return result;
  }
  std::array<std::uint8_t, kWordSize> bytes{};
  result.image_bytes = kWordSize;
  if (!reader.read(kServiceGlobalRva, bytes.data(), bytes.size())) {
    result.read_failures = 1;
    result.error = "The cached-service pointer word is unreadable.";
    return result;
  }
  const auto object_address = u64(bytes.data());
  if (object_address == 0) {
    result.valid = true;
    result.stage = "unavailable";
    return result;
  }
  result.cached_present = true;

  result.stage = "object_vptr";
  if (object_address > std::numeric_limits<std::uint64_t>::max() - kWordSize) {
    result.error = "The cached object pointer cannot contain a non-overflowing eight-byte word.";
    return result;
  }
  std::uint64_t vptr = 0;
  result.object_bytes = kWordSize;
  if (!object_reader.read_vptr(object_address, vptr)) {
    result.read_failures = 1;
    result.error = "The cached object's single vptr word is unreadable.";
    return result;
  }

  result.stage = "vtable_validation";
  std::uint32_t vtable_rva = 0;
  if (!normalize(image, loaded_image_base, vptr, vtable_rva) ||
      !section_range(image, vtable_rva, kVtablePrefixSize, kReadable, kWritable | kExecutable | kDiscardable)) {
    result.error = "The vtable and both fixed method slots do not fit declared read-only, non-executable image data.";
    return result;
  }
  result.vtable_rva = vtable_rva;
  result.method_slot_rva = vtable_rva + kServiceMethodOffset;
  result.cleanup_slot_rva = vtable_rva + kServiceCleanupMethodOffset;

  result.stage = "method_slot";
  result.image_bytes += kWordSize;
  if (!reader.read(result.method_slot_rva, bytes.data(), bytes.size())) {
    result.read_failures = 1;
    result.error = "The fixed vtable method pointer word is unreadable.";
    return result;
  }

  result.stage = "method_target";
  std::uint32_t method_rva = 0;
  if (!normalize(image, loaded_image_base, u64(bytes.data()), method_rva) ||
      !section_range(image, method_rva, 1, kReadable | kExecutable, kWritable | kDiscardable)) {
    result.error = "The method pointer is outside declared read-only, executable code in the selected image.";
    return result;
  }
  result.method_rva = method_rva;

  result.stage = "cleanup_slot";
  result.image_bytes += kWordSize;
  if (!reader.read(result.cleanup_slot_rva, bytes.data(), bytes.size())) {
    result.read_failures = 1;
    result.error = "The fixed cleanup vtable pointer word is unreadable.";
    return result;
  }

  result.stage = "cleanup_target";
  std::uint32_t cleanup_method_rva = 0;
  if (!normalize(image, loaded_image_base, u64(bytes.data()), cleanup_method_rva) ||
      !section_range(image, cleanup_method_rva, 1, kReadable | kExecutable, kWritable | kDiscardable)) {
    result.error = "The cleanup method pointer is outside declared read-only, executable code in the selected image.";
    return result;
  }
  result.cleanup_method_rva = cleanup_method_rva;
  result.valid = true;
  result.stage = "complete";
  return result;
}

}  // namespace taxi_camera::discovery
