#pragma once
#include "pfd_stamp_d3d12.hpp"
#include "scene_capture_manager.hpp"

namespace taxi_camera::pfd_adapter {
// Process-lifetime singleton, same manager on repeated initialization. Register
// events before application PSO/root creation. No late-object discovery fallback.
bool initialize(SceneCaptureManager&) noexcept;
void register_events();
// Root must first register this exact native list/object generation with the
// capture manager. DIRECT lists only. No COM ownership is retained here.
bool init_list(reshade::api::command_list*, std::uint64_t device_key, std::uint64_t object_generation) noexcept;
void destroy_list(reshade::api::command_list*, std::uint64_t object_generation) noexcept;
// Discovery at native submission starts with an unobserved recording. Root must
// register that pending generation with the manager first, then install both
// observers and arm it. Only a subsequent exact S_OK native Reset authorizes a
// fresh recording. These objects never enter the API-list/PFD-stamp registry.
// At most 128 distinct native objects retain one COM reference until process
// exit; destroy only disables tracking. Caller serializes discovery/arming with
// use of this command list and owns boundary/manager registration cleanup.
bool init_native_list(ID3D12GraphicsCommandList*, std::uint64_t device_key, std::uint64_t object_generation) noexcept;
bool arm_native_list(ID3D12GraphicsCommandList*, std::uint64_t object_generation) noexcept;
void destroy_native_list(ID3D12GraphicsCommandList*, std::uint64_t object_generation) noexcept;
// Root has validated selected sole RTV/current mip, no DSV/pass/bundle, matching
// device/typed format, and just forwarded the application's valid direct draw.
// Root MUST register_consumer_recording before invoking this function. It holds
// a short state-registry lock across the native stamp + exact restore; internal
// observer callbacks are bypassed, so no adapter/manager callback is re-entered.
bool record_stamp(reshade::api::command_list*, PfdStampD3D12&, ID3D12Device*, D3D12_GPU_VIRTUAL_ADDRESS, UINT width, UINT height) noexcept;
struct Statistics {
  std::uint64_t stamped = 0, refused = 0, lists = 0, native_observer_failures = 0;
  std::uint64_t native_roots = 0, native_tables = 0, stamps_with_undefined_tables = 0;
  const char* last_error = "not_initialized";
};
Statistics statistics() noexcept;
}  // namespace taxi_camera::pfd_adapter
