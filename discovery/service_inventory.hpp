#pragma once

#include "image_inventory.hpp"

namespace taxi_camera::discovery {

inline constexpr std::uint32_t kServiceGlobalRva = 173790384;
inline constexpr std::uint32_t kServiceMethodOffset = 808;
inline constexpr std::uint32_t kServiceCleanupMethodOffset = 104;

class ObjectVptrReader {
 public:
  virtual ~ObjectVptrReader() = default;
  // Read exactly the object's first eight bytes. The native adapter owns page
  // and process checks; it must not read other fields or follow this pointer.
  virtual bool read_vptr(std::uint64_t object_address, std::uint64_t& value) = 0;
};

struct ServiceInventory {
  // A null cached service is well-formed but unavailable, not a resolved method.
  bool valid = false;
  bool cached_present = false;
  std::string stage = "image_validation";
  std::string error;
  // Attempted bytes, including failed reads. At most 24 image + 8 object bytes.
  std::uint32_t image_bytes = 0;
  std::uint32_t object_bytes = 0;
  std::uint32_t read_failures = 0;
  std::uint32_t vtable_rva = 0;
  std::uint32_t method_slot_rva = 0;
  std::uint32_t method_rva = 0;
  std::uint32_t cleanup_slot_rva = 0;
  std::uint32_t cleanup_method_rva = 0;
};

// Fixed, build-specific metadata chain only: cached service pointer, one object
// vptr word, then vtable slots +808 and +104. The cached global must be writable
// image data; the entire vtable prefix must be read-only image data and both
// method targets executable image code. No code or intermediate fields are read.
// Resolved fields remain available on a later failure, but valid stays false
// until both targets pass. The stage/error identify the incomplete operation.
// This is a point-in-time observation, not an object lifetime or ABI guarantee.
ServiceInventory inspect_service_metadata(ImageReader& reader,
                                          ObjectVptrReader& object_reader,
                                          const Inventory& image,
                                          std::uint64_t loaded_image_base);

}  // namespace taxi_camera::discovery
