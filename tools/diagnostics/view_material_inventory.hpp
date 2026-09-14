#pragma once
#include <array>
#include <cstdint>
#include "../../src/camera/memory_reader.hpp"
namespace taxi_camera::discovery {
struct ViewBitmapMetadata {
  std::uint32_t slot = 0;
  bool present = false, stale = false, resource_present = false;
  std::uint32_t handle_generation = 0, current_generation = 0;
  std::uint32_t bitmap_width = 0, bitmap_height = 0;
  // Equality ordinal valid ONLY within this snapshot. No pointer/COM identity
  // or graphics lifetime generation is published by this read-only inspector.
  std::uint32_t resource_ordinal = 0;
  std::uint32_t dimension = 0, height = 0, format = 0, samples = 0, flags = 0;
  std::uint64_t width = 0;
  std::uint16_t layers = 0, mips = 0;
};
struct ViewMaterialMetadata {
  std::uint32_t index = 0;
  bool material_present = false, flags_stable = false;
  std::array<std::uint32_t, 6> dimensions{};
  std::array<std::uint64_t, 2> flags_before{}, flags_after{};
  std::array<ViewBitmapMetadata, 2> bitmaps{};
};
struct ViewMaterialInventory {
  bool complete = false;
  const char* error = "not_inspected";
  std::uint32_t read_bytes = 0, read_failures = 0;
  std::array<ViewMaterialMetadata, 8> views{};
};
// Fixed build's renderer+2752 eight-pointer pool, P+16..39/P+48..63/P+144,
// Material+520/+664, Bitmap+40/+44/+88, record+16, wrapper+96 cached native
// descriptor and wrapper+168. All identity/descriptor bytes are reread. Flags
// are separately sampled twice because the scheduler may toggle activation.
// Each field<=16bytes, attempted bytes<=32768. No code/COM calls or pointers
// in output. Caller supplies same-user process/image/build/region validation.
ViewMaterialInventory inspect_view_materials(engine_camera::MemoryReader& reader, std::uint64_t renderer);
}  // namespace taxi_camera::discovery
