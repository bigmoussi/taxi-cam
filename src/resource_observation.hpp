#pragma once

#include <reshade_api_resource.hpp>

#include <cstdint>

namespace taxi_camera {

// Observation does not imply that a resource can safely receive calibration.
// In particular, copied HTML surfaces, arrays and other color
// formats must remain visible to diagnostics even when writes are unsupported.
inline bool tracked_texture(const reshade::api::resource_desc& desc) {
  using namespace reshade::api;
  return desc.type == resource_type::texture_2d && desc.texture.width >= 32 && desc.texture.width <= 16384 &&
         desc.texture.height >= 32 && desc.texture.height <= 16384 &&
         (desc.usage & resource_usage::depth_stencil) == resource_usage::undefined;
}

inline bool typed_color_format(reshade::api::format value) {
  using reshade::api::format;
  switch (value) {
    case format::r8g8b8a8_unorm:
    case format::r8g8b8a8_unorm_srgb:
    case format::b8g8r8a8_unorm:
    case format::b8g8r8a8_unorm_srgb:
      return true;
    default:
      return false;
  }
}

inline std::uint32_t maximum_mip_levels(const reshade::api::resource_desc& desc) {
  std::uint32_t largest = desc.texture.width > desc.texture.height ? desc.texture.width : desc.texture.height;
  std::uint32_t levels = 0;
  while (largest != 0) {
    ++levels;
    largest >>= 1;
  }
  return levels;
}

// Resolves a declared mip of an observable 2D texture. Mip extents may be below
// the base-resource observation limit and clamp to one pixel. Failure leaves
// both outputs unchanged; this does not imply write eligibility or a live RTV.
inline bool bound_mip_extent(const reshade::api::resource_desc& desc,
                             std::uint32_t mip_level,
                             std::uint32_t& width,
                             std::uint32_t& height) {
  if (!tracked_texture(desc) || desc.texture.levels == 0 || desc.texture.levels > maximum_mip_levels(desc) ||
      mip_level >= desc.texture.levels) {
    return false;
  }
  width = desc.texture.width >> mip_level;
  height = desc.texture.height >> mip_level;
  if (width == 0) {
    width = 1;
  }
  if (height == 0) {
    height = 1;
  }
  return true;
}

// A valid mip chain is permitted, but calibration must use the bound RTV's mip
// dimensions. A live compatible RTV and supported command recording are still
// required separately; this helper checks resource metadata alone.
inline const char* calibration_block_reason(const reshade::api::resource_desc& desc) {
  using namespace reshade::api;
  if (desc.type != resource_type::texture_2d) {
    return "Not a 2D texture";
  }
  if (desc.texture.width < 32 || desc.texture.width > 16384 || desc.texture.height < 32 || desc.texture.height > 16384) {
    return "Dimensions outside 32..16384";
  }
  if ((desc.usage & resource_usage::depth_stencil) != resource_usage::undefined) {
    return "Depth/stencil usage";
  }
  if (desc.texture.depth_or_layers != 1) {
    return "Requires one array layer";
  }
  if (desc.texture.levels == 0 || desc.texture.levels > maximum_mip_levels(desc)) {
    return "Invalid mip count";
  }
  if (desc.texture.samples != 1) {
    return "Requires one sample";
  }
  if (!typed_color_format(desc.texture.format) && desc.texture.format != format::r8g8b8a8_typeless &&
      desc.texture.format != format::b8g8r8a8_typeless) {
    return "Unsupported calibration format";
  }
  if ((desc.usage & resource_usage::render_target) == resource_usage::undefined) {
    return "No render-target usage";
  }
  if ((desc.flags & resource_flags::sparse_binding) != resource_flags::none) {
    return "Sparse resource";
  }
  return nullptr;
}

inline bool calibration_eligible(const reshade::api::resource_desc& desc) {
  return calibration_block_reason(desc) == nullptr;
}

// Copy calibration does not require an RTV and remains restricted to one mip.
// Other resource shape and format limits, resource state, copy boxes and recording
// support must still be checked by the caller before issuing a copy.
inline bool copy_calibration_eligible(const reshade::api::resource_desc& desc) {
  using namespace reshade::api;
  return tracked_texture(desc) && desc.texture.depth_or_layers == 1 && desc.texture.levels == 1 && desc.texture.samples == 1 &&
         (typed_color_format(desc.texture.format) || desc.texture.format == format::r8g8b8a8_typeless ||
          desc.texture.format == format::b8g8r8a8_typeless) &&
         (desc.usage & resource_usage::copy_dest) != resource_usage::undefined &&
         (desc.flags & resource_flags::sparse_binding) == resource_flags::none;
}

}  // namespace taxi_camera
