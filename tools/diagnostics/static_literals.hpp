#pragma once

#include "../../src/camera/image_inventory.hpp"

namespace taxi_camera::discovery {

struct StaticLiteral {
  std::uint32_t rva = 0;
  // Set only after printable ASCII and a terminating NUL were read. A fixed
  // pose name with an unexpected prefix retains text as an unverified diagnostic
  // with a nonempty error; incomplete or unreadable strings expose no text.
  std::string text;
  std::string error;
  std::uint32_t bytes = 0;
};

struct StaticLiteralInventory {
  // True only when the request metadata and every requested literal succeeded.
  bool valid = false;
  std::string error;
  // Exact read attempts, including failed read() calls; never more than 1024.
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
  std::vector<StaticLiteral> records;
};

// Inspect one to eight explicit RVAs only, with at most 128 byte-read attempts
// per request INCLUDING its terminating NUL (at most 127 printable characters).
// Reads stop at NUL and stay inside one declared readable, non-writable,
// non-executable, non-discardable image section. No pointers are followed and
// no process or platform API is used. Failed requests preserve successful peers.
StaticLiteralInventory inspect_static_literals(ImageReader& reader, const Inventory& image, const std::vector<std::uint32_t>& rvas);

// Build-specific exception for exactly two MSVC TypeDescriptor name fields at
// RVAs 165937400 and 166129720. Requires the captured AMD64 build identity
// (timestamp 1787653788, image size 235963904, section count 14) before any access.
// Allows writable image data for these names only, never executable/discardable
// storage. At most 128 byte attempts per name including NUL; no descriptor
// headers/pointers are read. Verification requires ".?AV" or ".?AU". Terminated
// printable names with another prefix remain in text as unverified diagnostics;
// they have a nonempty error and make the inventory invalid.
StaticLiteralInventory inspect_known_pose_type_names(ImageReader& reader, const Inventory& image);

}  // namespace taxi_camera::discovery
