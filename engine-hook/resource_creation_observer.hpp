#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include "../src/scene_source_state.hpp"

namespace taxi_camera::engine_hook::resource_creation {
struct Result {
  bool ready = false;
  bool protection_restored = true;
  unsigned hooked_methods = 0;
  const char* status = "not_registered";
};
// Caller must obtain this public ReShade COM proxy from the resource's actual
// GetDevice and prove its unwrapped identity matches the registered native
// device; registration independently verifies that identity. Never cast
// reshade::api::device to a COM interface. At most four live
// device identities share one verified proxy vtable. QI must return this same
// pointer before extended interface slots are used. Modules are pinned until
// process exit. Proxy objects are borrowed; caller retains its GetDevice result
// throughout registration and unregisters at the actual device destruction.
Result register_device(std::uint64_t device_key, ID3D12Device* proxy, ID3D12Device* expected_native) noexcept;
void unregister_device(std::uint64_t device_key) noexcept;
// Called only inside init_resource: exact active creation call, device key,
// output pointer and description must match. No scope survives its call. The
// first creation used to install the observer necessarily returns unknown.
source_state::Model initial_model(std::uint64_t device_key,
                                  ID3D12Resource* resource,
                                  const D3D12_RESOURCE_DESC& actual_description) noexcept;
// Disables observations and restores only owned slots; retained objects/code
// remain alive for already-fetched wrappers. Removal permanently consumes hooks.
Result remove() noexcept;
}  // namespace taxi_camera::engine_hook::resource_creation
