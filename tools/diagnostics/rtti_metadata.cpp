#include "rtti_metadata.hpp"

#include <array>
#include <bit>
#include <limits>

namespace taxi_camera::discovery {
namespace {

// Layout source: clang/lib/CodeGen/MicrosoftCXXABI.cpp, the
// getCompleteObjectLocatorType/getClassHierarchyDescriptorType/
// getBaseClassDescriptorType methods and MSRTTIBuilder initializers.
// https://github.com/llvm/llvm-project/blob/main/clang/lib/CodeGen/MicrosoftCXXABI.cpp
constexpr std::uint32_t kReadable = 0x40000000;
constexpr std::uint32_t kWritable = 0x80000000;
constexpr std::uint32_t kExecutable = 0x20000000;
constexpr std::uint32_t kDiscardable = 0x02000000;
constexpr std::uint32_t kReadBudget = 4096;
constexpr std::uint32_t kMaximumBases = 64;
constexpr std::uint32_t kHasHierarchyDescriptor = 64;

bool valid_sections(const Inventory& image) {
  if (image.sections.empty() || image.sections.size() > 96)
    return false;
  for (std::size_t index = 0; index < image.sections.size(); ++index) {
    const auto& section = image.sections[index];
    if (section.rva > image.image_size || section.size > image.image_size - section.rva)
      return false;
    for (std::size_t previous = 0; previous < index; ++previous) {
      const auto& other = image.sections[previous];
      if (section.size != 0 && other.size != 0 && section.rva < std::uint64_t(other.rva) + other.size &&
          other.rva < std::uint64_t(section.rva) + section.size)
        return false;
    }
  }
  return true;
}

bool data_range(const Inventory& image, std::uint32_t rva, std::uint32_t size, bool allow_writable = false) {
  if (rva == 0 || rva >= image.image_size || size == 0 || size > image.image_size - rva)
    return false;
  const auto excluded = kExecutable | kDiscardable | (allow_writable ? 0 : kWritable);
  for (const auto& section : image.sections) {
    if ((section.flags & kReadable) != 0 && (section.flags & excluded) == 0 && rva >= section.rva && rva - section.rva < section.size &&
        size <= section.size - (rva - section.rva))
      return true;
  }
  return false;
}

bool type_identity(const Inventory& image, std::uint32_t rva) {
  // Only classify the known 16-byte TypeDescriptor header's image range. Do
  // not query or read it: its writable fields and trailing name are out of scope.
  return rva % 8 == 0 && data_range(image, rva, 16, true);
}

bool normalize(const Inventory& image, std::uint64_t base, std::uint64_t pointer, std::uint32_t& rva) {
  if (pointer < base || pointer - base >= image.image_size)
    return false;
  rva = static_cast<std::uint32_t>(pointer - base);
  return true;
}

bool read_static(ImageReader& reader,
                 const Inventory& image,
                 std::uint32_t rva,
                 std::uint8_t* destination,
                 std::uint32_t size,
                 RttiMetadata& result) {
  if (rva % 4 != 0 || !data_range(image, rva, size)) {
    result.error = "RTTI metadata does not fit aligned read-only, non-executable static image data.";
    return false;
  }
  if (size > kReadBudget - result.read_bytes) {
    result.error = "The RTTI metadata read budget is exhausted.";
    return false;
  }
  for (std::uint32_t done = 0; done < size;) {
    const auto window = reader.query(rva + done, size - done);
    if (!window.readable || window.size == 0 || window.size > size - done) {
      ++result.read_failures;
      result.error = "RTTI metadata is not wholly contained in readable query windows.";
      return false;
    }
    done += window.size;
  }
  result.read_bytes += size;
  if (!reader.read(rva, destination, size)) {
    ++result.read_failures;
    result.error = "RTTI metadata could not be read exactly.";
    return false;
  }
  return true;
}

std::uint32_t u32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (std::uint32_t index = 0; index < 4; ++index)
    value |= std::uint32_t(bytes[index]) << (index * 8);
  return value;
}

std::uint64_t u64(const std::uint8_t* bytes) {
  return std::uint64_t(u32(bytes)) | (std::uint64_t(u32(bytes + 4)) << 32);
}

}  // namespace

RttiMetadata inspect_rtti_metadata(ImageReader& reader,
                                   const Inventory& image,
                                   std::uint64_t loaded_image_base,
                                   std::uint32_t vtable_rva,
                                   std::uint32_t source_type_rva,
                                   std::uint32_t target_type_rva) {
  RttiMetadata result;
  result.vtable_rva = vtable_rva;
  result.source_type_rva = source_type_rva;
  result.target_type_rva = target_type_rva;
  if (!image.valid_image || image.machine != 0x8664 || image.image_size == 0 || !valid_sections(image) || loaded_image_base == 0 ||
      loaded_image_base % 8 != 0 || image.image_size > std::numeric_limits<std::uint64_t>::max() - loaded_image_base) {
    result.error = "A validated AMD64 image with non-overlapping sections and non-overflowing aligned loaded-image bounds is required.";
    return result;
  }
  result.stage = "requested_identities";
  if (vtable_rva < 8 || vtable_rva % 8 != 0 || !data_range(image, vtable_rva - 8, 16) || !type_identity(image, source_type_rva) ||
      !type_identity(image, target_type_rva)) {
    result.error = "The supplied vtable prefix or numeric source/target TypeDescriptor identity is outside eligible image data.";
    return result;
  }
  result.stage = "locator_pointer";
  std::array<std::uint8_t, 8> pointer{};
  if (!read_static(reader, image, vtable_rva - 8, pointer.data(), pointer.size(), result))
    return result;
  if (!normalize(image, loaded_image_base, u64(pointer.data()), result.locator_rva)) {
    result.error = "The vtable's preceding pointer does not normalize into the selected main image.";
    return result;
  }
  result.stage = "locator";
  std::array<std::uint8_t, 24> locator{};
  if (!read_static(reader, image, result.locator_rva, locator.data(), locator.size(), result))
    return result;
  result.locator_signature = u32(locator.data());
  result.offset = u32(locator.data() + 4);
  result.cd_offset = u32(locator.data() + 8);
  result.type_rva = u32(locator.data() + 12);
  result.hierarchy_rva = u32(locator.data() + 16);
  result.self_rva = u32(locator.data() + 20);
  if (result.locator_signature != 1 || result.self_rva != result.locator_rva || !type_identity(image, result.type_rva)) {
    result.error = "The AMD64 locator signature, self-reference or numeric TypeDescriptor identity is invalid.";
    return result;
  }
  result.stage = "hierarchy";
  std::array<std::uint8_t, 16> hierarchy{};
  if (!read_static(reader, image, result.hierarchy_rva, hierarchy.data(), hierarchy.size(), result))
    return result;
  result.hierarchy_signature = u32(hierarchy.data());
  result.hierarchy_attributes = u32(hierarchy.data() + 4);
  result.base_count = u32(hierarchy.data() + 8);
  result.base_array_rva = u32(hierarchy.data() + 12);
  if (result.hierarchy_signature != 0 || result.base_count == 0 || result.base_count > kMaximumBases) {
    result.error = "The hierarchy signature or bounded base-descriptor count is invalid.";
    return result;
  }
  result.stage = "base_array";
  std::array<std::uint8_t, kMaximumBases * 4> base_array{};
  if (!read_static(reader, image, result.base_array_rva, base_array.data(), result.base_count * 4, result))
    return result;
  result.bases.reserve(result.base_count);
  for (std::uint32_t index = 0; index < result.base_count; ++index) {
    result.stage = "base_descriptor";
    RttiBaseDescriptor entry;
    entry.index = index;
    entry.descriptor_rva = u32(base_array.data() + index * 4);
    std::array<std::uint8_t, 28> descriptor{};
    if (!read_static(reader, image, entry.descriptor_rva, descriptor.data(), descriptor.size(), result))
      return result;
    entry.type_rva = u32(descriptor.data());
    entry.num_contained_bases = u32(descriptor.data() + 4);
    entry.mdisp = std::bit_cast<std::int32_t>(u32(descriptor.data() + 8));
    entry.pdisp = std::bit_cast<std::int32_t>(u32(descriptor.data() + 12));
    entry.vdisp = std::bit_cast<std::int32_t>(u32(descriptor.data() + 16));
    entry.attributes = u32(descriptor.data() + 20);
    entry.hierarchy_rva = u32(descriptor.data() + 24);
    if (!type_identity(image, entry.type_rva) || entry.num_contained_bases > result.base_count - index - 1 ||
        (entry.hierarchy_rva != 0 && (entry.hierarchy_rva % 4 != 0 || !data_range(image, entry.hierarchy_rva, 16))) ||
        ((entry.attributes & kHasHierarchyDescriptor) != 0 && entry.hierarchy_rva == 0)) {
      result.error = "A base descriptor has an invalid type identity, contained-base range or numeric hierarchy reference.";
      return result;
    }
    if (index == 0 && (entry.type_rva != result.type_rva || entry.num_contained_bases != result.base_count - 1)) {
      result.error = "The root base descriptor does not match the locator type and hierarchy count.";
      return result;
    }
    result.source_type_present |= entry.type_rva == source_type_rva;
    result.target_type_present |= entry.type_rva == target_type_rva;
    result.bases.push_back(entry);
  }
  result.valid = true;
  result.stage = "complete";
  return result;
}

bool verified_camera_component_layout(const RttiMetadata& metadata) {
  // Exact numeric profile saved in msfs-1.8.16.0-a380-component-type-metadata.json.
  // These descriptors form the observed six-entry chain; nested hierarchy
  // references remain identities, not an instruction to traverse or cast.
  constexpr std::array<std::uint32_t, 6> types{165937384, 166625656, 166129704, 166129616, 166129664, 166092480};
  constexpr std::array<std::uint32_t, 6> descriptors{146401672, 145841576, 145185440, 145185192, 145185288, 145130208};
  constexpr std::array<std::uint32_t, 6> hierarchies{146401592, 145841504, 145185376, 145185232, 145185328, 145130248};
  if (!metadata.valid || metadata.stage != "complete" || !metadata.error.empty() || metadata.read_failures != 0 ||
      metadata.read_bytes != 240 || metadata.vtable_rva != 133516648 || metadata.locator_rva != 146401552 ||
      metadata.locator_signature != 1 || metadata.offset != 0 || metadata.cd_offset != 0 || metadata.type_rva != types[0] ||
      metadata.hierarchy_rva != hierarchies[0] || metadata.self_rva != metadata.locator_rva || metadata.hierarchy_signature != 0 ||
      metadata.hierarchy_attributes != 0 || metadata.base_count != types.size() || metadata.bases.size() != types.size() ||
      metadata.base_array_rva == 0 || metadata.base_array_rva % 4 != 0 || metadata.source_type_rva != types[2] ||
      metadata.target_type_rva != types[0] || !metadata.source_type_present || !metadata.target_type_present)
    return false;
  for (std::size_t index = 0; index < types.size(); ++index) {
    const auto& entry = metadata.bases[index];
    if (entry.index != index || entry.descriptor_rva != descriptors[index] || entry.type_rva != types[index] ||
        entry.num_contained_bases != types.size() - index - 1 || entry.mdisp != 0 || entry.pdisp != -1 || entry.vdisp != 0 ||
        entry.attributes != 64 || entry.hierarchy_rva != hierarchies[index])
      return false;
  }
  return true;
}

}  // namespace taxi_camera::discovery
