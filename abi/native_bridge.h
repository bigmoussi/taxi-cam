#pragma once
#include <cstdint>
namespace reshade::api {
struct device;
struct resource_view_desc;
}  // namespace reshade::api

// Implemented in an MSVC-target object. Only scalar arguments/returns and a
// pointer to the ABI-checked public descriptor cross this C boundary.
extern "C" uint64_t taxi_camera_resource_from_view(reshade::api::device* device, uint64_t view);
extern "C" void taxi_camera_resource_view_desc(reshade::api::device* device, uint64_t view, reshade::api::resource_view_desc* out);
