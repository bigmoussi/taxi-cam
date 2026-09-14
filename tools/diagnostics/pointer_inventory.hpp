#pragma once

#include "../../src/camera/image_inventory.hpp"

namespace taxi_camera::discovery {

enum class PointerCategory { null_pointer, external, image_data, image_code };

const char* pointer_category_name(PointerCategory category);

struct PointerEntry {
  // The optional preceding word has index -1; table entries start at zero.
  std::int32_t index = 0;
  std::uint32_t source_rva = 0;
  // Meaningful only for image_data/image_code. Absolute pointer values are
  // consumed locally and never retained in the returned metadata.
  std::uint32_t target_rva = 0;
  PointerCategory category = PointerCategory::null_pointer;
};

struct PointerInventory {
  // The image and first table word passed static section/bounds validation.
  // Subsequent inaccessible words are reported through error/read_failures.
  bool valid_table = false;
  std::string error;
  std::uint32_t table_rva = 0;
  std::uint32_t entry_limit = 0;
  // Bytes requested from read(), including failed exact reads; at most 264.
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
  bool entry_limit_reached = false;
  bool stopped_on_non_code = false;
  bool preceding_available = false;
  PointerEntry preceding;
  std::vector<PointerEntry> entries;
};

// Reads at most 32 eight-byte words plus the optional immediately preceding
// word, from one declared readable, non-writable, non-executable data section.
// Stops after the first non-code word (which is included in the result).
// Classification queries target readability but never reads target bytes.
// A pointer run establishes neither a vtable's identity nor its extent, and
// the preceding word is metadata only: no RTTI interpretation is performed.
PointerInventory inspect_pointer_table(ImageReader& reader,
                                       const Inventory& image,
                                       std::uint64_t loaded_image_base,
                                       std::uint32_t table_rva,
                                       std::uint32_t max_entries = 24,
                                       bool include_previous = true);

}  // namespace taxi_camera::discovery
