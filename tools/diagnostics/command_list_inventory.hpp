#pragma once

#include "reference_inventory.hpp"

namespace taxi_camera::discovery {

class CommandListObjectReader {
 public:
  virtual ~CommandListObjectReader() = default;
  virtual bool read(std::uint64_t address, void* destination, std::size_t size) = 0;
};

struct CommandListInventory {
  bool valid = false;
  bool available = false;
  std::string stage = "code_proof";
  std::string error;
  std::uint32_t type = 0;
  std::uint32_t vtable_rva = 0;
  std::uint32_t image_bytes = 0;
  std::uint32_t object_bytes = 0;
  std::uint32_t read_failures = 0;
};

// Fixed MSFS1.8.16.0 metadata only. Fresh complete function64597824 proves the
// native CreateCommandList type at the already evidenced pool+208. Its caller's
// pool is renderer+284368. Reads at most16 image bytes and24 object bytes:
// cached renderer twice, vptr/type/type/vptr. No native calls or pointer output.
// Equality rechecks detect observed changes; they do not establish a lifetime.
CommandListInventory inspect_command_list_type(ImageReader& image_reader,
                                               CommandListObjectReader& object_reader,
                                               const Inventory& image,
                                               std::uint64_t loaded_image_base,
                                               const FunctionInventory& fresh_proof);

}  // namespace taxi_camera::discovery
