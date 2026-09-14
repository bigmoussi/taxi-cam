#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "../src/resource_observation.hpp"

namespace {

using namespace reshade::api;

unsigned int checks = 0;

void expect(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

resource_desc ordinary_target() {
  return {768, 1024, 1, 1, format::r8g8b8a8_unorm, 1, memory_heap::default_,
          resource_usage::render_target | resource_usage::shader_resource};
}

void expect_observed_only(const resource_desc& desc, const char* reason) {
  expect(taxi_camera::tracked_texture(desc), "Unsupported calibration target remains observable");
  expect(!taxi_camera::calibration_eligible(desc), "Observation must not enable calibration");
  const char* actual = taxi_camera::calibration_block_reason(desc);
  expect(actual != nullptr && std::strcmp(actual, reason) == 0, reason);
  auto copyDesc = desc;
  copyDesc.usage |= resource_usage::copy_dest;
  expect(taxi_camera::copy_calibration_eligible(copyDesc) == (std::strcmp(reason, "No render-target usage") == 0),
         "Unsupported resource metadata still blocks copy calibration");
}

void expect_invalid_mip(const resource_desc& desc, std::uint32_t level) {
  std::uint32_t width = 123;
  std::uint32_t height = 456;
  expect(!taxi_camera::bound_mip_extent(desc, level, width, height), "Invalid bound mip is rejected");
  expect(width == 123 && height == 456, "Rejected bound mip preserves outputs");
}

}  // namespace

int main() {
  const auto ordinary = ordinary_target();
  expect(taxi_camera::tracked_texture(ordinary), "Ordinary PFD-sized target is observed");
  expect(taxi_camera::calibration_eligible(ordinary), "Ordinary PFD-sized target remains eligible");
  expect(taxi_camera::calibration_block_reason(ordinary) == nullptr, "Eligible target has no block reason");
  expect(!taxi_camera::copy_calibration_eligible(ordinary), "RTV eligibility does not imply copy-destination usage");
  auto copyOnly = ordinary;
  copyOnly.usage = resource_usage::copy_dest;
  expect(taxi_camera::tracked_texture(copyOnly), "Copy-only color target remains observable");
  expect(taxi_camera::copy_calibration_eligible(copyOnly), "Copy-only RGBA target permits copy calibration");
  expect(!taxi_camera::calibration_eligible(copyOnly), "Copy-only RGBA target does not permit RTV calibration");
  copyOnly.usage = resource_usage::copy_source | resource_usage::shader_resource;
  expect(!taxi_camera::copy_calibration_eligible(copyOnly), "Copy-source usage does not permit copy calibration");
  copyOnly.usage = resource_usage::undefined;
  expect(!taxi_camera::copy_calibration_eligible(copyOnly), "Missing usage does not permit copy calibration");

  const std::array<format, 4> typedFormats{format::r8g8b8a8_unorm, format::r8g8b8a8_unorm_srgb, format::b8g8r8a8_unorm,
                                        format::b8g8r8a8_unorm_srgb};
  for (const auto colorFormat : typedFormats) {
    auto desc = ordinary;
    desc.texture.format = colorFormat;
    expect(taxi_camera::typed_color_format(colorFormat), "Existing typed RTV format remains supported");
    expect(taxi_camera::calibration_eligible(desc), "Existing typed resource format remains eligible");
    desc.usage = resource_usage::copy_dest;
    expect(taxi_camera::copy_calibration_eligible(desc), "Existing typed color format permits copy-only calibration");
  }
  for (const auto colorFormat : {format::r8g8b8a8_typeless, format::b8g8r8a8_typeless}) {
    auto desc = ordinary;
    desc.texture.format = colorFormat;
    expect(!taxi_camera::typed_color_format(colorFormat), "Typeless resource format must not qualify as a typed RTV");
    expect(taxi_camera::calibration_eligible(desc), "Existing typeless resource format remains eligible");
    desc.usage = resource_usage::copy_dest;
    expect(taxi_camera::copy_calibration_eligible(desc), "Existing typeless color format permits copy-only calibration");
  }

  auto desc = ordinary;
  // The observed PFD resources have five mips. Both truncated and complete
  // chains permit RTV calibration, while upload calibration remains single-mip.
  for (std::uint16_t levels = 1; levels <= 11; ++levels) {
    desc = ordinary;
    desc.texture.levels = levels;
    desc.usage |= resource_usage::copy_dest;
    expect(taxi_camera::calibration_eligible(desc), "Valid PFD mip chain permits RTV calibration");
    expect(taxi_camera::copy_calibration_eligible(desc) == (levels == 1), "Copy calibration remains single-mip only");
  }
  desc = ordinary;
  desc.texture.levels = 11;
  const std::array<std::array<std::uint32_t, 2>, 11> expectedExtents{{
      {768, 1024}, {384, 512}, {192, 256}, {96, 128}, {48, 64}, {24, 32}, {12, 16}, {6, 8}, {3, 4}, {1, 2}, {1, 1},
  }};
  for (std::uint32_t level = 0; level < expectedExtents.size(); ++level) {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    expect(taxi_camera::bound_mip_extent(desc, level, width, height), "Each declared mip has a valid extent");
    expect(width == expectedExtents[level][0] && height == expectedExtents[level][1], "PFD mip extent matches expected dimensions");
  }
  expect_invalid_mip(desc, 11);
  expect_invalid_mip(desc, std::numeric_limits<std::uint32_t>::max());
  desc.texture.levels = 5;
  expect_invalid_mip(desc, 5);
  for (const std::uint16_t levels : {0u, 12u, 65535u}) {
    desc = ordinary;
    desc.texture.levels = levels;
    expect_observed_only(desc, "Invalid mip count");
    expect_invalid_mip(desc, 0);
  }
  desc = ordinary;
  desc.texture.width = 33;
  desc.texture.height = 65;
  desc.texture.levels = 7;
  std::uint32_t mipWidth = 0;
  std::uint32_t mipHeight = 0;
  expect(taxi_camera::calibration_eligible(desc), "Non-power-of-two full mip chain is valid");
  expect(taxi_camera::bound_mip_extent(desc, 5, mipWidth, mipHeight) && mipWidth == 1 && mipHeight == 2,
         "Non-power-of-two rectangular mip extent rounds down and clamps each axis");
  expect(taxi_camera::bound_mip_extent(desc, 6, mipWidth, mipHeight) && mipWidth == 1 && mipHeight == 1,
         "Smallest non-power-of-two mip has one pixel");
  desc.texture.levels = 8;
  expect_observed_only(desc, "Invalid mip count");
  expect_invalid_mip(desc, 0);
  desc = ordinary;
  desc.texture.depth_or_layers = 6;
  expect_observed_only(desc, "Requires one array layer");
  desc = ordinary;
  desc.texture.samples = 4;
  expect_observed_only(desc, "Requires one sample");
  desc = ordinary;
  desc.usage = resource_usage::copy_dest | resource_usage::shader_resource;
  expect_observed_only(desc, "No render-target usage");
  desc.usage = resource_usage::shader_resource;
  expect_observed_only(desc, "No render-target usage");
  desc = ordinary;
  desc.flags = resource_flags::sparse_binding;
  expect_observed_only(desc, "Sparse resource");
  for (const auto colorFormat : {format::r16g16b16a16_float, format::r10g10b10a2_unorm, format::r8_unorm, format::unknown}) {
    desc = ordinary;
    desc.texture.format = colorFormat;
    expect(!taxi_camera::typed_color_format(colorFormat), "Observation-only format is not a supported typed RTV");
    expect_observed_only(desc, "Unsupported calibration format");
  }

  // Broadened observation must not accidentally restore eligibility when
  // multiple previously unsupported metadata properties occur together.
  desc = ordinary;
  desc.texture.levels = 4;
  desc.texture.depth_or_layers = 8;
  desc.texture.samples = 4;
  desc.texture.format = format::r16g16b16a16_float;
  desc.usage = resource_usage::copy_dest | resource_usage::shader_resource;
  desc.flags = resource_flags::sparse_binding;
  expect_observed_only(desc, "Requires one array layer");

  for (const std::uint32_t size : {32u, 16384u}) {
    desc = ordinary;
    desc.texture.width = size;
    desc.texture.height = size;
    expect(taxi_camera::tracked_texture(desc), "Inclusive observation size boundary");
    expect(taxi_camera::calibration_eligible(desc), "Inclusive calibration size boundary");
    desc.usage = resource_usage::copy_dest;
    expect(taxi_camera::copy_calibration_eligible(desc), "Inclusive copy calibration size boundary");
    desc.usage |= resource_usage::render_target;
    desc.texture.levels = size == 32 ? 6 : 15;
    expect(taxi_camera::calibration_eligible(desc), "Full mip chain at inclusive base size boundary");
    expect(taxi_camera::bound_mip_extent(desc, desc.texture.levels - 1, mipWidth, mipHeight) && mipWidth == 1 && mipHeight == 1,
           "Boundary base textures resolve their one-pixel final mip");
    ++desc.texture.levels;
    expect_observed_only(desc, "Invalid mip count");
    expect_invalid_mip(desc, 0);
  }
  for (const std::uint32_t size : {0u, 31u, 16385u, std::numeric_limits<std::uint32_t>::max()}) {
    for (const bool changeWidth : {false, true}) {
      desc = ordinary;
      if (changeWidth) {
        desc.texture.width = size;
      } else {
        desc.texture.height = size;
      }
      expect(!taxi_camera::tracked_texture(desc), "Out-of-range texture is not tracked");
      expect(!taxi_camera::calibration_eligible(desc), "Out-of-range texture is not calibrated");
      desc.usage |= resource_usage::copy_dest;
      expect(!taxi_camera::copy_calibration_eligible(desc), "Out-of-range texture is not copy calibrated");
      expect_invalid_mip(desc, 0);
      expect(std::strcmp(taxi_camera::calibration_block_reason(desc), "Dimensions outside 32..16384") == 0,
             "Out-of-range size has an explicit reason");
    }
  }

  for (const auto usage : {resource_usage::depth_stencil, resource_usage::depth_stencil_read, resource_usage::depth_stencil_write}) {
    desc = ordinary;
    desc.usage |= usage;
    expect(!taxi_camera::tracked_texture(desc), "Depth/stencil resources are not observed");
    expect(!taxi_camera::calibration_eligible(desc), "Depth/stencil resources are not calibrated");
    desc.usage |= resource_usage::copy_dest;
    expect(!taxi_camera::copy_calibration_eligible(desc), "Depth/stencil resources are not copy calibrated");
    expect_invalid_mip(desc, 0);
    expect(std::strcmp(taxi_camera::calibration_block_reason(desc), "Depth/stencil usage") == 0,
           "Depth/stencil exclusion has an explicit reason");
  }

  for (const auto type : {resource_type::buffer, resource_type::texture_1d, resource_type::texture_3d}) {
    desc = ordinary;
    desc.type = type;
    expect(!taxi_camera::tracked_texture(desc), "Only 2D texture resources are observed");
    expect(!taxi_camera::calibration_eligible(desc), "Non-2D resources are not calibrated");
    desc.usage |= resource_usage::copy_dest;
    expect(!taxi_camera::copy_calibration_eligible(desc), "Non-2D resources are not copy calibrated");
    expect_invalid_mip(desc, 0);
    expect(std::strcmp(taxi_camera::calibration_block_reason(desc), "Not a 2D texture") == 0,
           "Resource type exclusion has an explicit reason");
  }

  std::printf("PASS: %u metadata checks keep broad observation separate from conservative calibration eligibility.\n", checks);
  return 0;
}
