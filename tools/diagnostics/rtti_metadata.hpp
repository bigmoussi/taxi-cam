#pragma once

#include "../../src/camera/image_inventory.hpp"

namespace taxi_camera::discovery {

struct RttiBaseDescriptor {
  std::uint32_t index = 0;
  std::uint32_t descriptor_rva = 0;
  std::uint32_t type_rva = 0;
  std::uint32_t num_contained_bases = 0;
  std::int32_t mdisp = 0;
  std::int32_t pdisp = 0;
  std::int32_t vdisp = 0;
  std::uint32_t attributes = 0;
  // Numeric reference only; this nested hierarchy is never traversed.
  std::uint32_t hierarchy_rva = 0;
};

struct RttiMetadata {
  bool valid = false;
  std::string stage = "image_validation";
  std::string error;
  // Exact read attempts, including failed reads, capped at 4096 bytes. The
  // current one-level layout requires at most 2096 bytes for 64 descriptors.
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
  std::uint32_t vtable_rva = 0;
  std::uint32_t locator_rva = 0;
  std::uint32_t locator_signature = 0;
  std::uint32_t offset = 0;
  std::uint32_t cd_offset = 0;
  std::uint32_t type_rva = 0;
  std::uint32_t hierarchy_rva = 0;
  std::uint32_t self_rva = 0;
  std::uint32_t hierarchy_signature = 0;
  std::uint32_t hierarchy_attributes = 0;
  std::uint32_t base_count = 0;
  std::uint32_t base_array_rva = 0;
  std::uint32_t source_type_rva = 0;
  std::uint32_t target_type_rva = 0;
  // Presence within verified base records only. A later failure leaves these
  // partial observations available but valid=false; they are not cast results.
  bool source_type_present = false;
  bool target_type_present = false;
  std::vector<RttiBaseDescriptor> bases;
};

// Inspect numeric MSVC AMD64 image-relative RTTI from an explicitly supplied
// static vtable. Read its preceding pointer, signature-1 24-byte COL, signature-0
// 16-byte CHD, at most 64 four-byte array entries and their 28-byte BCDs. All
// read ranges must fit readable, non-writable, non-executable, non-discardable
// main-image sections. TypeDescriptor references may identify writable image
// data; neither their header nor name is read or queried. No target heap, code,
// nested hierarchy, vbtable or object field is read. No process access or
// execution is involved.
// Bounds, COL self-reference and the root descriptor identity must agree.
// PMDs and attributes remain numeric metadata: no dynamic_cast success, public
// path, pointer adjustment, object lifetime or ABI compatibility is established.
RttiMetadata inspect_rtti_metadata(ImageReader& reader,
                                   const Inventory& image,
                                   std::uint64_t loaded_image_base,
                                   std::uint32_t vtable_rva,
                                   std::uint32_t source_type_rva = 166129704,
                                   std::uint32_t target_type_rva = 165937384);

// Pure fixed-profile check for the complete, freshly captured 1.8.16.0 A380
// component RTTI. Its unique source base and complete target have the observed
// zero-offset, nonvirtual PMDs. This is not a general dynamic_cast algorithm or
// proof that a cast ran. The caller still owns fresh component/vptr identity,
// build/code checks and bounded consistency-checked object reads. No object
// pointer is accepted, adjusted or returned, and no pose/key data is inspected.
bool verified_camera_component_layout(const RttiMetadata& metadata);

}  // namespace taxi_camera::discovery
