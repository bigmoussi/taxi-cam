#include "import_slot.hpp"

#include <array>
#include <utility>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kMaximumBytes = 131072;
constexpr std::uint32_t kMaximumDescriptors = 256;
constexpr std::uint32_t kMaximumThunks = 2048;
constexpr std::uint32_t kReadable = 0x40000000;
constexpr std::uint32_t kExecutable = 0x20000000;

std::uint16_t u16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0] | (std::uint16_t(bytes[1]) << 8));
}

std::uint32_t u32(const std::uint8_t* bytes) {
  return bytes[0] | (std::uint32_t(bytes[1]) << 8) | (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

std::uint64_t u64(const std::uint8_t* bytes) {
  return u32(bytes) | (std::uint64_t(u32(bytes + 4)) << 32);
}

bool within(std::uint64_t rva, std::uint64_t size, std::uint64_t begin, std::uint64_t extent) {
  return rva >= begin && rva - begin <= extent && size <= extent - (rva - begin);
}

bool overlap(std::uint64_t a, std::uint64_t a_size, std::uint64_t b, std::uint64_t b_size) {
  return a_size != 0 && b_size != 0 && a < b + b_size && b < a + a_size;
}

class Parser {
 public:
  Parser(ImageReader& reader, const Inventory& image, std::uint32_t slot) : reader_(reader), image_(image) { result_.iat_slot_rva = slot; }

  ImportSlotInventory run() {
    if (!validate_image() || !headers()) {
      return std::move(result_);
    }
    if (import_rva_ == 0 && import_size_ == 0) {
      unavailable("no_import_directory");
      return std::move(result_);
    }
    if (iat_rva_ == 0 && iat_size_ == 0) {
      unavailable("iat_extent_unavailable");
      return std::move(result_);
    }
    if (iat_rva_ == 0 || iat_size_ < 8 || (iat_size_ & 7) != 0 || !data_section(iat_rva_, iat_size_)) {
      fail("The declared IAT extent is invalid or not readable non-executable image data.");
      return std::move(result_);
    }
    if (!within(result_.iat_slot_rva, 8, iat_rva_, iat_size_) || ((result_.iat_slot_rva - iat_rva_) & 7) != 0) {
      unavailable("slot_outside_iat");
      return std::move(result_);
    }
    if (import_rva_ == 0 || import_size_ < 20 || !data_section(import_rva_, import_size_) ||
        overlap(import_rva_, import_size_, iat_rva_, iat_size_)) {
      fail("The import directory is invalid or overlaps the excluded IAT.");
      return std::move(result_);
    }

    bool missing_lookup = false;
    bool thunk_limit = false;
    for (std::uint32_t descriptor_index = 0; descriptor_index < kMaximumDescriptors; ++descriptor_index) {
      const auto position = std::uint64_t(import_rva_) + descriptor_index * 20;
      if (!within(position, 20, import_rva_, import_size_)) {
        fail("The import directory ends before a null descriptor or matching slot.");
        return std::move(result_);
      }
      std::array<std::uint8_t, 20> descriptor{};
      if (!data(position, descriptor.data(), descriptor.size())) {
        return std::move(result_);
      }
      ++result_.descriptors_read;
      const auto lookup = u32(descriptor.data());
      const auto module = u32(descriptor.data() + 12);
      const auto first = u32(descriptor.data() + 16);
      if (lookup == 0 && u32(descriptor.data() + 4) == 0 && u32(descriptor.data() + 8) == 0 && module == 0 && first == 0) {
        unavailable(missing_lookup ? "lookup_table_absent" : thunk_limit ? "thunk_limit_reached" : "slot_not_found");
        return std::move(result_);
      }
      if (module == 0 || first == 0 || !within(first, 8, iat_rva_, iat_size_) || ((first - iat_rva_) & 7) != 0) {
        fail("An import descriptor has invalid module or IAT metadata.");
        return std::move(result_);
      }
      if (result_.iat_slot_rva < first) {
        continue;
      }
      const auto requested_index = (result_.iat_slot_rva - first) / 8;
      if (requested_index >= kMaximumThunks) {
        thunk_limit = true;
        continue;
      }
      if (lookup == 0) {
        missing_lookup = true;
        continue;
      }
      // Do not inspect unrelated hint/name strings. Preceding lookup entries
      // only establish that the requested slot precedes this table's sentinel.
      for (std::uint32_t index = 0; index <= requested_index; ++index) {
        const auto thunk_rva = std::uint64_t(lookup) + std::uint64_t(index) * 8;
        std::array<std::uint8_t, 8> thunk{};
        if (!data(thunk_rva, thunk.data(), thunk.size())) {
          return std::move(result_);
        }
        ++result_.thunks_read;
        const auto value = u64(thunk.data());
        if (value == 0) {
          break;
        }
        const bool ordinal = (value >> 63) != 0;
        if ((ordinal && (value & 0x7fffffffffff0000ull) != 0) || (!ordinal && value > 0x7fffffffull)) {
          fail("A lookup thunk has nonzero reserved bits; bound IAT addresses are not name RVAs.");
          return std::move(result_);
        }
        if (index != requested_index) {
          continue;
        }
        result_.descriptor_rva = static_cast<std::uint32_t>(position);
        result_.lookup_thunk_rva = static_cast<std::uint32_t>(thunk_rva);
        if (!literal(module, result_.module)) {
          return std::move(result_);
        }
        if (ordinal) {
          result_.by_ordinal = true;
          result_.ordinal = static_cast<std::uint16_t>(value);
          unavailable("ordinal_only");
          return std::move(result_);
        }
        result_.name_rva = static_cast<std::uint32_t>(value);
        // Validate the hint and first name byte as one field; the hint itself
        // is not needed to identify the imported name and is not read.
        if (!allowed_data(value, 3) || !literal(value + 2, result_.symbol)) {
          return std::move(result_);
        }
        result_.valid = true;
        result_.available = true;
        result_.status = "named_import";
        return std::move(result_);
      }
    }
    unavailable("descriptor_limit_reached");
    return std::move(result_);
  }

 private:
  bool fail(const char* message) {
    result_.valid = false;
    result_.available = false;
    result_.status = "invalid_metadata";
    result_.error = message;
    return false;
  }

  void unavailable(const char* status) {
    result_.valid = true;
    result_.available = false;
    result_.status = status;
  }

  bool validate_image() {
    if (!image_.valid_image || image_.machine != 0x8664 || image_.image_size == 0 || image_.image_size > 0x80000000u ||
        image_.sections.empty() || image_.sections.size() > image_.section_count || image_.section_count == 0 ||
        image_.section_count > 96 || !within(result_.iat_slot_rva, 8, 0, image_.image_size)) {
      return fail("Invalid AMD64 loaded-image or requested slot metadata.");
    }
    for (std::size_t index = 0; index < image_.sections.size(); ++index) {
      const auto& section = image_.sections[index];
      if (!within(section.rva, section.size, 0, image_.image_size)) {
        return fail("An image section extends outside the validated image.");
      }
      if (section.size != 0 && section.rva < header_limit_) {
        header_limit_ = section.rva;
      }
      for (std::size_t previous = 0; previous < index; ++previous) {
        const auto& other = image_.sections[previous];
        if (overlap(section.rva, section.size, other.rva, other.size)) {
          return fail("Overlapping image sections are unsupported.");
        }
      }
    }
    return true;
  }

  const ImageSection* data_section(std::uint64_t rva, std::uint64_t size) const {
    if (size == 0 || !within(rva, size, 0, image_.image_size)) {
      return nullptr;
    }
    for (const auto& section : image_.sections) {
      if ((section.flags & kReadable) != 0 && (section.flags & kExecutable) == 0 && within(rva, size, section.rva, section.size)) {
        return &section;
      }
    }
    return nullptr;
  }

  bool allowed_data(std::uint64_t rva, std::uint32_t size) {
    if (!data_section(rva, size) || overlap(rva, size, iat_rva_, iat_size_)) {
      return fail("Import metadata is outside readable non-executable image data or overlaps the excluded IAT.");
    }
    return true;
  }

  bool exact(std::uint64_t rva, void* output, std::uint32_t size) {
    // This scope exclusion precedes even the first header query. A malformed
    // requested slot must never be read through an alias in header metadata.
    if (overlap(rva, size, result_.iat_slot_rva, 8)) {
      return fail("A metadata request overlaps the excluded requested IAT slot.");
    }
    if (!within(rva, size, 0, image_.image_size)) {
      return fail("A metadata request is outside the validated image.");
    }
    if (size > kMaximumBytes - result_.read_bytes) {
      result_.status = "byte_limit_reached";
      result_.error = "The import metadata read allowance is exhausted.";
      return false;
    }
    result_.read_bytes += size;
    std::uint32_t offset = 0;
    while (offset < size) {
      const auto next = static_cast<std::uint32_t>(rva + offset);
      const auto window = reader_.query(next, size - offset);
      if (!window.readable || window.size == 0 || window.size > size - offset) {
        ++result_.read_failures;
        return fail("An import metadata query window is unreadable or invalid.");
      }
      if (!reader_.read(next, static_cast<std::uint8_t*>(output) + offset, window.size)) {
        ++result_.read_failures;
        return fail("Import metadata could not be read exactly.");
      }
      offset += window.size;
    }
    return true;
  }

  bool data(std::uint64_t rva, void* output, std::uint32_t size) { return allowed_data(rva, size) && exact(rva, output, size); }

  bool header(std::uint64_t rva, void* output, std::uint32_t size) {
    // Fresh e_lfanew must not redirect a header read into any loaded section
    // (and therefore into IAT contents) before its directories are available.
    if (!within(rva, size, 0, header_limit_)) {
      return fail("A PE header request reaches a declared image section.");
    }
    return exact(rva, output, size);
  }

  bool literal(std::uint64_t rva, std::string& output) {
    const auto* section = data_section(rva, 1);
    if (!section) {
      return fail("An import name begins outside readable non-executable image data.");
    }
    std::string value;
    for (std::uint32_t index = 0; index < 128; ++index) {
      if (!within(rva + index, 1, section->rva, section->size)) {
        return fail("An import name crosses its image section before NUL.");
      }
      std::uint8_t byte = 0;
      if (!data(rva + index, &byte, 1)) {
        return false;
      }
      if (byte == 0) {
        if (value.empty()) {
          return fail("An import name is empty.");
        }
        output = std::move(value);
        return true;
      }
      if (byte < 32 || byte > 126) {
        return fail("An import name is not printable ASCII.");
      }
      value.push_back(static_cast<char>(byte));
    }
    return fail("An import name is not NUL-terminated within 128 bytes.");
  }

  bool headers() {
    // Microsoft PE/COFF: DOS e_lfanew is +0x3c; the signature+COFF header
    // occupies 24 bytes. In PE32+, NumberOfRvaAndSizes is optional+108,
    // import directory is optional+120 and IAT directory is optional+208.
    // https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
    std::array<std::uint8_t, 64> dos{};
    if (!header(0, dos.data(), dos.size())) {
      return false;
    }
    const auto pe = u32(dos.data() + 60);
    if (u16(dos.data()) != 0x5a4d || pe < 64 || pe > 1024 * 1024) {
      return fail("Invalid DOS signature or PE header offset.");
    }
    std::array<std::uint8_t, 24> coff{};
    if (!header(pe, coff.data(), coff.size())) {
      return false;
    }
    const auto optional_size = u16(coff.data() + 20);
    if (u32(coff.data()) != 0x4550 || u16(coff.data() + 4) != image_.machine || u16(coff.data() + 6) != image_.section_count ||
        u32(coff.data() + 8) != image_.timestamp || optional_size < 112 || optional_size > 4096) {
      return fail("The PE header no longer matches the validated image or has invalid bounds.");
    }
    const auto optional_rva = std::uint64_t(pe) + 24;
    std::array<std::uint8_t, 112> optional{};
    if (!header(optional_rva, optional.data(), optional.size())) {
      return false;
    }
    const auto header_size = u32(optional.data() + 60);
    const auto directories = u32(optional.data() + 108);
    if (u16(optional.data()) != 0x20b || u32(optional.data() + 56) != image_.image_size || u32(optional.data() + 64) != image_.checksum ||
        header_size == 0 || header_size > image_.image_size ||
        !within(optional_rva, std::uint64_t(optional_size) + image_.section_count * 40, 0, header_size) ||
        directories > (optional_size - 112u) / 8u) {
      return fail("The PE32+ optional header has invalid identity, directory or section-table bounds.");
    }
    for (const auto& section : image_.sections) {
      if (section.size != 0 && section.rva < header_size) {
        return fail("A declared image section overlaps the PE headers.");
      }
    }
    std::array<std::uint8_t, 8> directory{};
    if (directories > 1) {
      if (!header(optional_rva + 120, directory.data(), directory.size())) {
        return false;
      }
      import_rva_ = u32(directory.data());
      import_size_ = u32(directory.data() + 4);
    }
    if (directories > 12) {
      if (!header(optional_rva + 208, directory.data(), directory.size())) {
        return false;
      }
      iat_rva_ = u32(directory.data());
      iat_size_ = u32(directory.data() + 4);
    }
    return true;
  }

  ImageReader& reader_;
  const Inventory& image_;
  ImportSlotInventory result_;
  std::uint32_t header_limit_ = 0xffffffff;
  std::uint32_t import_rva_ = 0;
  std::uint32_t import_size_ = 0;
  std::uint32_t iat_rva_ = 0;
  std::uint32_t iat_size_ = 0;
};

}  // namespace

ImportSlotInventory inspect_import_slot(ImageReader& reader, const Inventory& image, std::uint32_t iat_slot_rva) {
  return Parser(reader, image, iat_slot_rva).run();
}

}  // namespace taxi_camera::discovery
