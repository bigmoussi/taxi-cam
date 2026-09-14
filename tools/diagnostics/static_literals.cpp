#include "static_literals.hpp"

#include <utility>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kReadable = 0x40000000;
constexpr std::uint32_t kWritable = 0x80000000;
constexpr std::uint32_t kExcluded = 0x20000000 | 0x02000000;
constexpr std::uint32_t kMaximumRequests = 8;
constexpr std::uint32_t kMaximumBytes = 128;

enum class LiteralScope { read_only, fixed_pose_type_name };

bool valid_sections(const Inventory& image) {
  if (image.sections.empty() || image.sections.size() > 96) {
    return false;
  }
  for (std::size_t index = 0; index < image.sections.size(); ++index) {
    const auto& section = image.sections[index];
    if (section.rva > image.image_size || section.size > image.image_size - section.rva) {
      return false;
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      const auto& other = image.sections[previous];
      if (section.size != 0 && other.size != 0 && section.rva < std::uint64_t(other.rva) + other.size &&
          other.rva < std::uint64_t(section.rva) + section.size) {
        return false;
      }
    }
  }
  return true;
}

StaticLiteral read_literal(ImageReader& reader,
                           const Inventory& image,
                           std::uint32_t rva,
                           StaticLiteralInventory& result,
                           LiteralScope scope) {
  StaticLiteral record;
  record.rva = rva;
  const ImageSection* storage = nullptr;
  const auto excluded = kExcluded | (scope == LiteralScope::read_only ? kWritable : 0);
  for (const auto& section : image.sections) {
    if ((section.flags & kReadable) != 0 && (section.flags & excluded) == 0 && rva >= section.rva && rva - section.rva < section.size) {
      storage = &section;
      break;
    }
  }
  if (!storage) {
    record.error = scope == LiteralScope::read_only ? "The literal RVA is not in readable, non-writable static data."
                                                    : "The fixed type name is not in readable, non-executable image data.";
    return record;
  }

  std::string text;
  for (std::uint32_t offset = 0; offset < kMaximumBytes; ++offset) {
    // Validate the entire next read before either querying or reading it. The
    // containing section was checked against image_size without overflowing.
    const auto next = std::uint64_t(rva) + offset;
    if (next >= std::uint64_t(storage->rva) + storage->size || next >= image.image_size) {
      record.error = "The literal reaches its static-data section boundary before NUL.";
      return record;
    }
    const auto position = static_cast<std::uint32_t>(next);
    const auto window = reader.query(position, 1);
    if (!window.readable || window.size != 1) {
      ++result.read_failures;
      record.error = "A literal byte is not in a readable query window.";
      return record;
    }
    std::uint8_t byte = 0;
    ++record.bytes;
    ++result.read_bytes;
    if (!reader.read(position, &byte, 1)) {
      ++result.read_failures;
      record.error = "A literal byte could not be read exactly.";
      return record;
    }
    if (byte == 0) {
      record.text = std::move(text);
      return record;
    }
    if (byte < 32 || byte > 126) {
      record.error = "The literal contains a non-printable or non-ASCII byte.";
      return record;
    }
    text.push_back(static_cast<char>(byte));
  }
  record.error = "The literal is not NUL-terminated within 128 bytes.";
  return record;
}

}  // namespace

StaticLiteralInventory inspect_static_literals(ImageReader& reader, const Inventory& image, const std::vector<std::uint32_t>& rvas) {
  StaticLiteralInventory result;
  if (!image.valid_image || image.machine != 0x8664 || image.image_size == 0 || !valid_sections(image)) {
    result.error = "Invalid loaded-image or section metadata.";
    return result;
  }
  if (rvas.empty() || rvas.size() > kMaximumRequests) {
    result.error = "Request one to eight explicit literal RVAs.";
    return result;
  }
  result.valid = true;
  result.records.reserve(rvas.size());
  for (const auto rva : rvas) {
    result.records.push_back(read_literal(reader, image, rva, result, LiteralScope::read_only));
    if (!result.records.back().error.empty()) {
      result.valid = false;
      result.error = "One or more requested static literals could not be verified.";
    }
  }
  return result;
}

StaticLiteralInventory inspect_known_pose_type_names(ImageReader& reader, const Inventory& image) {
  StaticLiteralInventory result;
  if (!image.valid_image || image.machine != 0x8664 || image.timestamp != 1787653788 || image.image_size != 235963904 ||
      image.section_count != 14 || !valid_sections(image)) {
    result.error = "The fixed pose type names require the exact validated AMD64 build and section metadata.";
    return result;
  }
  result.valid = true;
  result.records.reserve(2);
  for (const auto rva : {165937400u, 166129720u}) {
    auto record = read_literal(reader, image, rva, result, LiteralScope::fixed_pose_type_name);
    if (record.error.empty() && !record.text.starts_with(".?AV") && !record.text.starts_with(".?AU")) {
      record.error = "Unverified diagnostic text: the fixed name lacks the expected MSVC class or struct TypeDescriptor prefix.";
    }
    if (!record.error.empty()) {
      result.valid = false;
      result.error = "One or more fixed pose type names could not be verified.";
    }
    result.records.push_back(std::move(record));
  }
  return result;
}

}  // namespace taxi_camera::discovery
