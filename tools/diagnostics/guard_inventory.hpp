#pragma once

#include "../../src/camera/image_inventory.hpp"

namespace taxi_camera::discovery {

struct GuardInventory {
  bool present = false;
  // Well-formed absence is valid. This does not mean CFG is enabled/enforced.
  bool valid = false;
  std::string error;
  // Bounded attempted metadata bytes, including a failed read.
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
  std::uint32_t declared_size = 0;
  std::uint32_t guard_flags = 0;
  bool guard_cf_characteristic = false;
  bool cf_instrumented = false;
  bool function_table_present = false;
  bool dispatch_slot_present = false;
  std::uint32_t dispatch_slot_rva = 0;
};

// Examines at most the first 148 bytes of the declared static load-config
// directory. The dispatch-slot address is normalized to an RVA and classified
// by section metadata; the slot contents, tables and target code are not read.
GuardInventory inspect_guard_metadata(ImageReader& reader, const Inventory& image, std::uint64_t loaded_image_base);

}  // namespace taxi_camera::discovery
