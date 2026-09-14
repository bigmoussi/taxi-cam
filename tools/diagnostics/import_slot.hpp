#pragma once

#include "../../src/camera/image_inventory.hpp"

namespace taxi_camera::discovery {

struct ImportSlotInventory {
  // Valid means a bounded metadata conclusion, including explicit unavailable
  // states. Available additionally requires a named import at the exact slot.
  bool valid = false;
  bool available = false;
  std::string status;
  std::string error;
  std::string module;
  std::string symbol;
  std::uint32_t iat_slot_rva = 0;
  std::uint32_t descriptor_rva = 0;
  std::uint32_t lookup_thunk_rva = 0;
  std::uint32_t name_rva = 0;  // Hint/name entry RVA; the ASCII name begins at +2.
  bool by_ordinal = false;
  std::uint16_t ordinal = 0;
  std::uint32_t descriptors_read = 0;
  std::uint32_t thunks_read = 0;
  // Attempted exact metadata bytes, including failed queries/reads. The hard
  // total is 131072; no request exceeding the remaining allowance is issued.
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
};

// Inspects ONE explicit PE32+ IAT slot in a validated AMD64 LOADED main image.
// Parses at most 256 import descriptors and 2048 lookup thunks per descriptor;
// each module/symbol has at most 128 bytes INCLUDING the terminating NUL.
// Requires a declared IAT data-directory extent and excludes that entire range
// from metadata reads: neither the requested slot nor other resolved function
// pointers are read. Missing OriginalFirstThunk is explicitly unavailable;
// loaded FirstThunk values are never substituted for name metadata.
// The explicit slot's eight bytes are excluded even from initial header reads,
// before the declared IAT extent is known; malformed header aliases are refused.
//
// Import metadata must remain in declared readable, non-executable image data.
// Writable/discardable section flags are permitted because these are loader
// metadata; every actual page must still pass ImageReader::query/read. Each
// accessed field/string stays within one section. No exports, other modules,
// process APIs, function calls, raw bytes or absolute addresses are involved.
// Header identity is checked against Inventory, but this is not an atomic
// snapshot, an import-target authenticity check or a C++ ABI/type guarantee.
ImportSlotInventory inspect_import_slot(ImageReader& reader, const Inventory& image, std::uint32_t iat_slot_rva);

}  // namespace taxi_camera::discovery
