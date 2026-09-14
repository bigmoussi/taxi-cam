// Negative controls: these real APIs are NOT compatible between the targets.
#include <imgui.h>
#include <reshade_api_device.hpp>
#define RESHADE_API_LIBRARY_EXPORT
#include <reshade_overlay.hpp>
namespace api = reshade::api;
extern "C" void probe_large_return(api::device* device, api::resource resource, api::resource_desc* out) {
  *out = device->get_resource_desc(resource);
}
extern "C" uint64_t probe_handle_return(api::device* device, api::resource_view view) {
  return device->get_resource_from_view(view).handle;
}
extern "C" void probe_vec_return(imgui_function_table* table, ImVec2* out) {
  *out = table->GetWindowPos();
}
