#pragma once

#include "reference_inventory.hpp"

namespace taxi_camera::discovery {

struct SlotPrefixRequest {
  std::uint32_t slot_rva = 0;
  std::uint32_t entry_rva = 0;
};

struct SlotPrefix {
  std::uint32_t proof_slot_rva = 0;
  std::uint32_t entry_rva = 0;
  // Attempted exact prefix-read bytes, including failed reads. A successful
  // read may include bytes after the first instruction-aligned RET.
  std::uint32_t bytes_read = 0;
  std::uint32_t decoded_bytes = 0;
  bool ret_observed = false;
  std::string stop_reason;
  std::string error;
  std::vector<InstructionMetadata> instructions;
};

struct SlotPrefixInventory {
  bool valid = false;
  std::string error;
  // Attempted target bytes (maximum 512) and separate slot-proof bytes
  // (maximum 64). Failed peers do not discard successful prefix metadata.
  std::uint32_t read_bytes = 0;
  std::uint32_t proof_bytes = 0;
  std::uint32_t read_failures = 0;
  std::vector<SlotPrefix> entries;
};

// Inspect one to eight explicitly supplied static vtable slots and expected
// entry RVAs. Each aligned eight-byte slot must be readable, non-writable,
// non-executable, non-discardable main-image data. Its freshly read pointer must
// normalize to exactly the requested readable static executable entry.
// Read at most 64 target bytes, clipped to that section/image, and linearly
// decode until an aligned C3/C2 RET or the prefix boundary. No .pdata or prior
// call boundary is required; the caller owns fresh vtable/object identity proof.
// Never read an object, follow a branch, execute a target or emit the loaded
// image base/absolute slot value. A valid prefix, with or without RET, does not
// establish logical method extent, ABI, reachability or object lifetime.
SlotPrefixInventory inspect_slot_prefixes(ImageReader& reader,
                                          const Inventory& image,
                                          InstructionDecoder& decoder,
                                          std::uint64_t loaded_image_base,
                                          const std::vector<SlotPrefixRequest>& requests);

}  // namespace taxi_camera::discovery
