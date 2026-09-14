#if !defined(_MSC_VER) || defined(__MINGW32__)
#error Compile this bridge with the x86_64-pc-windows-msvc target.
#endif
#include "native_bridge.h"
#include <reshade_api_device.hpp>

extern "C" uint64_t taxi_camera_resource_from_view(reshade::api::device* device, uint64_t view) {
  return device->get_resource_from_view({view}).handle;
}

extern "C" void taxi_camera_resource_view_desc(reshade::api::device* device, uint64_t view, reshade::api::resource_view_desc* out) {
  *out = device->get_resource_view_desc({view});
}
