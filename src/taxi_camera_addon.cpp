#include <imgui.h>
#include <windows.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../abi/native_bridge.h"
#include "../engine-hook/render_boundary_observer.hpp"
#include "../engine-hook/resource_creation_observer.hpp"
#include "../native-camera/body_pose_provider.hpp"
#include "../native-camera/mount_config.hpp"
#include "../native-camera/probe.hpp"
#include "calibration_copy_d3d12.hpp"
#include "calibration_d3d12.hpp"
#include "display_exposure.hpp"
#include "pfd_state_adapter.hpp"
#include "resource_observation.hpp"
#include "resource_debug_name.hpp"
#include "scene_handoff.hpp"
#include "scene_runtime.hpp"
#include "taxi_button_routes.hpp"
#include "pfd_target_detector.hpp"
#include "write_budget.hpp"

namespace {

namespace api = reshade::api;
HMODULE addon_module = nullptr;
constexpr std::size_t kMaximumTargets = 16384;
constexpr std::size_t kMaximumViews = 16384;
namespace render_boundary = taxi_camera::engine_hook::render_boundary;
namespace resource_creation = taxi_camera::engine_hook::resource_creation;
std::atomic<std::uint64_t> boundary_hook_failures{0};
std::atomic<const char*> boundary_hook_status{"inactive"};
std::atomic<unsigned> live_d3d12_devices{0};
constexpr std::uint64_t NativeOnlyGenerationBit = 1ull << 63;
struct NativeSourceObservation {
  std::atomic<std::uint64_t> transitions{0}, open_window{0}, copy_sources{0}, copy_destinations{0};
  std::atomic<std::uint32_t> model{0}, before{0}, after{0}, scope{0}, flags{0}, subresource{0};
};
std::array<NativeSourceObservation, 2> native_source_observations;

void observe_transition(ID3D12Resource* resource,
                        std::uint32_t model,
                        std::uint32_t before,
                        std::uint32_t after,
                        std::uint32_t scope,
                        std::uint32_t flags,
                        std::uint32_t subresource) noexcept {
  const auto handle = reinterpret_cast<std::uint64_t>(resource);
  const auto feed = taxi_camera::scene_handoff().observed_feed(handle);
  if (feed < 0)
    return;
  auto& observation = native_source_observations[feed];
  ++observation.transitions;
  if (taxi_camera::scene_handoff().may_match_resource(handle))
    ++observation.open_window;
  observation.model.store(model, std::memory_order_relaxed);
  observation.before.store(before, std::memory_order_relaxed);
  observation.after.store(after, std::memory_order_relaxed);
  observation.scope.store(scope, std::memory_order_relaxed);
  observation.flags.store(flags, std::memory_order_relaxed);
  observation.subresource.store(subresource, std::memory_order_relaxed);
}

void observe_legacy_transition(void*,
                               ID3D12GraphicsCommandList* list,
                               std::uint64_t generation,
                               const D3D12_RESOURCE_BARRIER& barrier,
                               std::uint32_t scope) noexcept {
  taxi_camera::scene_runtime::manager().observe_source_legacy(list, generation, barrier);
  if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
    observe_transition(barrier.Transition.pResource, 1, barrier.Transition.StateBefore, barrier.Transition.StateAfter, scope, barrier.Flags,
                       barrier.Transition.Subresource);
}

void observe_enhanced_transition(void*,
                                 ID3D12GraphicsCommandList7* list,
                                 std::uint64_t generation,
                                 const D3D12_TEXTURE_BARRIER& barrier,
                                 std::uint32_t scope) noexcept {
  taxi_camera::scene_runtime::manager().observe_source_enhanced(list, generation, barrier);
  observe_transition(barrier.pResource, 2, barrier.LayoutBefore, barrier.LayoutAfter, scope, barrier.Flags,
                     barrier.Subresources.IndexOrFirstMipLevel);
}

void observe_native_copy(ID3D12Resource* source, ID3D12Resource* destination) noexcept {
  const auto source_feed = taxi_camera::scene_handoff().observed_feed(reinterpret_cast<std::uint64_t>(source));
  const auto destination_feed = taxi_camera::scene_handoff().observed_feed(reinterpret_cast<std::uint64_t>(destination));
  if (source_feed >= 0)
    ++native_source_observations[source_feed].copy_sources;
  if (destination_feed >= 0)
    ++native_source_observations[destination_feed].copy_destinations;
}

void after_native_copy_resource(void*,
                                ID3D12GraphicsCommandList* list,
                                std::uint64_t generation,
                                ID3D12Resource* destination,
                                ID3D12Resource* source,
                                bool capture_allowed) noexcept {
  observe_native_copy(source, destination);
  taxi_camera::scene_runtime::manager().record_copy_after_forward(list, source, destination, capture_allowed, generation);
}

void after_native_copy_texture(void*,
                               ID3D12GraphicsCommandList* list,
                               std::uint64_t generation,
                               const D3D12_TEXTURE_COPY_LOCATION* destination,
                               UINT x,
                               UINT y,
                               UINT z,
                               const D3D12_TEXTURE_COPY_LOCATION* source,
                               const D3D12_BOX* box,
                               bool capture_allowed) noexcept {
  if (source && destination)
    observe_native_copy(source->pResource, destination->pResource);
  taxi_camera::scene_runtime::manager().record_texture_copy_after_forward(list, destination, x, y, z, source, box, capture_allowed,
                                                                          generation);
}

void before_legacy_output_transition(void*,
                                     ID3D12GraphicsCommandList* list,
                                     std::uint64_t generation,
                                     const D3D12_RESOURCE_TRANSITION_BARRIER& barrier) noexcept {
  taxi_camera::scene_runtime::manager().record_render_target_before_transition(list, barrier.pResource, true, generation);
}

void before_enhanced_output_transition(void*,
                                       ID3D12GraphicsCommandList7* list,
                                       std::uint64_t generation,
                                       const D3D12_TEXTURE_BARRIER& barrier) noexcept {
  taxi_camera::scene_runtime::manager().record_render_target_before_enhanced_transition(list, barrier.pResource, true, generation);
}

void after_native_source_draw(void*, ID3D12GraphicsCommandList* list, std::uint64_t generation, bool allowed) noexcept {
  taxi_camera::scene_runtime::manager().after_source_draw(list, generation, allowed);
}

void invalidate_native_source_recording(void*, ID3D12GraphicsCommandList* list, std::uint64_t generation, std::uint32_t reasons) noexcept {
  // Public Begin supplies the actual bound targets. An ordinary pass on an
  // unrelated resource does not change the persistent camera state model.
  constexpr auto scoped_reasons =
      render_boundary::InvalidationPassBegin | render_boundary::InvalidationSplitBarrier | render_boundary::InvalidationAliasOrDiscard;
  if ((reasons & ~scoped_reasons) != 0 || ((generation & NativeOnlyGenerationBit) && (reasons & render_boundary::InvalidationPassBegin)))
    taxi_camera::scene_runtime::manager().invalidate_source_recording(list, generation, true);
}

const render_boundary::Callbacks BoundaryCallbacks{nullptr,
                                                   before_legacy_output_transition,
                                                   before_enhanced_output_transition,
                                                   observe_legacy_transition,
                                                   observe_enhanced_transition,
                                                   after_native_copy_resource,
                                                   after_native_copy_texture,
                                                   after_native_source_draw,
                                                   invalidate_native_source_recording};

void observe_unknown_native_list(void*, ID3D12GraphicsCommandList* native, std::uint64_t device_key) noexcept {
  // Serialized by the manager's submission lock, outside its metadata lock.
  // Retain identities even after a refused installation to prevent pointer reuse
  // and unbounded per-frame installation retries. No engine functions are called.
  struct Entry {
    ID3D12GraphicsCommandList* native = nullptr;
  };
  static std::array<Entry, 128> retained{};
  static std::size_t used = 0;
  for (std::size_t i = 0; i < used; ++i)
    if (retained[i].native == native)
      return;
  if (!native || used == retained.size() || native->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return;
  native->AddRef();
  retained[used++].native = native;
  const auto generation = NativeOnlyGenerationBit | used;
  auto& manager = taxi_camera::scene_runtime::manager();
  const bool registered = manager.register_unobserved_command_list(native, device_key, generation);
  bool ready = registered && taxi_camera::pfd_adapter::init_native_list(native, device_key, generation);
  const char* status = "reset_observer_refused";
  if (ready) {
    const auto boundary = render_boundary::register_list(native, generation, BoundaryCallbacks);
    status = boundary.status;
    ready = boundary.ready && boundary.protection_restored && taxi_camera::pfd_adapter::arm_native_list(native, generation);
  }
  if (!ready) {
    render_boundary::unregister_list(native, generation);
    taxi_camera::pfd_adapter::destroy_native_list(native, generation);
  }
  char message[256];
  std::snprintf(message, sizeof(message),
                "Taxi Camera: discovered native DIRECT list=%p generation=%llu observer_ready=%d status=%s; "
                "existing recording remains unobserved until successful native Reset.",
                static_cast<void*>(native), static_cast<unsigned long long>(generation), ready ? 1 : 0, status);
  reshade::log::message(ready ? reshade::log::level::info : reshade::log::level::warning, message);
}

struct Target {
  api::resource_desc desc;
  std::uint64_t id = 0;
  std::uint64_t draws = 0;
  std::uint64_t last_frame = 0;
  std::uint32_t views = 0;
  std::uint64_t srv_registrations = 0;
  std::uint64_t eligible_draws = 0;
  std::uint64_t copies = 0;
  std::uint64_t eligible_copies = 0;
  std::uint64_t copies_out = 0;
  std::uint64_t uploads = 0;
  std::uint64_t resolves = 0;
  std::uint64_t passes = 0;
  std::uint64_t indirect = 0;
  std::uint64_t last_source_id = 0;
  taxi_camera::source_state::Model initial_model = taxi_camera::source_state::Model::unknown;
  bool name_checked = false;
  std::array<char, 256> debug_name{};
};

struct View {
  std::uint64_t resource = 0;
  std::uint64_t target_id = 0;
  std::uint64_t generation = 0;
};

struct PfdDrawStatus {
  std::uint64_t draws = 0, attempted = 0, observer_blocked = 0, depth_blocked = 0, pass_blocked = 0;
  DXGI_FORMAT color = DXGI_FORMAT_UNKNOWN, depth = DXGI_FORMAT_UNKNOWN;
  std::uint32_t mip = 0;
};

struct DeviceData {
  inline static constexpr std::uint8_t key[16] = {0x23, 0x05, 0xfe, 0xc1, 0xb2, 0xd5, 0xce, 0x45,
                                                  0x81, 0x12, 0x4d, 0xb1, 0xed, 0x55, 0x54, 0x28};
  std::mutex mutex;
  std::mutex control_mutex;
  // Resource/view lifetimes remain observed while expensive per-command
  // activity discovery is paused. Epoch changes require fresh RTV bindings.
  std::atomic<bool> activity_tracking{true};
  std::atomic<bool> source_candidates_seen{false};
  std::atomic<bool> creation_observation_attempted{false};
  std::atomic<const char*> creation_status{"not_observed"};
  std::atomic<std::uint64_t> activity_epoch{1};
  std::array<std::atomic<std::uint64_t>, 2> scene_copy_sources{};
  std::array<std::atomic<std::uint64_t>, 2> scene_copy_destinations{};
  std::unordered_map<std::uint64_t, Target> targets;
  std::unordered_map<std::uint64_t, View> views;
  std::uint64_t next_id = 1;
  std::uint64_t next_view_generation = 1;
  std::uint64_t selected_id = 0;
  api::resource_view owned_view{};
  std::uint64_t owned_target_id = 0;
  std::uint32_t owned_mip = 0;
  api::format owned_format = api::format::unknown;
  std::uint64_t recorded_writes = 0;
  std::uint64_t skipped_recordings = 0;
  std::atomic<std::uint64_t> frame{0};
  taxi_camera::WriteBudget write_budget;
  std::uint64_t last_throttle_log_ms = 0;
  std::atomic<std::uint64_t> next_feed_log_ms{0};
  bool throttle_logged = false;
  bool enabled = false;
  std::atomic<bool> camera_enabled{false};
  // Session resource generations only: never persist simulator texture IDs.
  taxi_camera::TaxiButtonRoutes taxi_routes;
  taxi_camera::TaxiButtonIntent taxi_intent;
  taxi_camera::TaxiButtonIntentSnapshot taxi_intent_state;
  bool taxi_button_control = true;
  unsigned taxi_active_mask = 0;
  bool taxi_scene_requested = false;
  bool taxi_start_failed = false;
  bool taxi_auto_detect = true;
  bool taxi_telemetry_started = false;
  std::uint64_t next_telemetry_retry_ms = 0;
  taxi_camera::PfdTargetDetector pfd_detector;
  std::uint64_t next_detection_ms = 0;
  const char* taxi_detection_status = "Waiting for PFD activity";
  PfdDrawStatus pfd_draws;
  bool device_lost = false;
  bool capacity_reached = false;
  bool view_creation_failed = false;
  bool copy_creation_failed = false;
  bool retain_uploads = false;
  taxi_camera::CopyCalibration copy_calibration;
  int filter_width = 768;
  int filter_height = 1024;
  bool used_recently = false;
  bool show_performance = false;
  // Serialized with overlay edits by control_mutex; never read camera pixels.
  bool automatic_exposure = true;
  float manual_exposure_ev = taxi_camera::DisplayExposureController::DayExposureEv;
  float night_exposure_boost_ev = taxi_camera::DisplayExposureController::DefaultNightBoostEv;
  taxi_camera::DisplayExposureController exposure_controller;
  taxi_camera::DisplayExposureState exposure_state;
  std::uint64_t previous_body_count = 0;
  std::uint64_t previous_body_log_ms = 0;
  float body_arrivals_per_second = 0;
  bool body_rate_available = false;
};

struct CommandData {
  inline static constexpr std::uint8_t key[16] = {0xf3, 0x39, 0xec, 0xf8, 0x31, 0xed, 0x10, 0x4a,
                                                  0xa0, 0x49, 0x38, 0x9c, 0x1d, 0x78, 0x09, 0x47};
  std::uint64_t view = 0;
  std::uint64_t activity_epoch = 0;
  View binding{};
  api::resource_view_desc bound_desc{};
  std::uint32_t bound_width = 0;
  std::uint32_t bound_height = 0;
  DXGI_FORMAT bound_depth_format = DXGI_FORMAT_UNKNOWN;
  bool direct = false;
  bool copy_capable = false;
  bool inside_native_pass = false;
  bool unsupported_recording = false;
  bool source_recording_unsupported = false;
  bool has_depth_stencil = false;
  bool capture_ready = false;
  std::uint64_t object_generation = 0;
  std::array<View, 8> observed{};
  std::uint32_t observed_count = 0;
  std::array<ID3D12Resource*, 8> source_targets{};
  std::array<std::uint64_t, 8> source_generations{};
  std::uint32_t source_count = 0;
};

struct QueueData {
  inline static constexpr std::uint8_t key[16] = {0x87, 0x42, 0x68, 0x29, 0x69, 0xce, 0x45, 0x2d,
                                                  0xb8, 0x28, 0x95, 0x21, 0x43, 0x7a, 0xe6, 0xc0};
  api::command_list* immediate = nullptr;
  ID3D12GraphicsCommandList* native = nullptr;
  std::uint64_t generation = 0;
};

bool possible_camera_source(const api::resource_desc& desc) {
  return desc.type == api::resource_type::texture_2d && desc.texture.width == 768 &&
         (desc.texture.height == 255 || desc.texture.height == 504) && desc.texture.levels == 1 && desc.texture.depth_or_layers == 1 &&
         desc.texture.samples == 1 && (desc.usage & api::resource_usage::render_target) != api::resource_usage::undefined &&
         (desc.flags & api::resource_flags::sparse_binding) == api::resource_flags::none;
}

template <typename T>
T* private_data(api::api_object* object) {
  std::uint64_t value = 0;
  object->get_private_data(T::key, &value);
  return reinterpret_cast<T*>(static_cast<std::uintptr_t>(value));
}

template <typename T>
T* create_private_data(api::api_object* object) {
  auto* value = new T();
  object->set_private_data(T::key, reinterpret_cast<std::uintptr_t>(value));
  return value;
}

template <typename T>
void destroy_private_data(api::api_object* object) {
  auto* value = private_data<T>(object);
  object->set_private_data(T::key, 0);
  delete value;
}

void disarm(DeviceData& data) {
  data.enabled = false;
  // Inventory selection/filtering belongs to manual calibration. Keep an
  // already armed EFIS route while its resource incarnation remains live;
  // destruction forgets that route before reaching here. Never re-enable a
  // camera that native startup has already refused.
  const bool retain_automatic = data.taxi_button_control && !data.device_lost &&
                                data.taxi_routes.active_mask(true, (data.taxi_active_mask & 1u) != 0,
                                                               (data.taxi_active_mask & 2u) != 0) != 0;
  if (!retain_automatic)
    data.camera_enabled.store(false, std::memory_order_release);
  data.selected_id = 0;
}

void set_activity_tracking(DeviceData& data, bool enabled) {
  const std::lock_guard lock(data.mutex);
  if (data.activity_tracking.load(std::memory_order_relaxed) == enabled)
    return;
  // Pausing counters must not detach an explicitly selected live camera target.
  data.enabled = false;
  data.activity_epoch.fetch_add(1, std::memory_order_acq_rel);
  data.activity_tracking.store(enabled, std::memory_order_release);
}

// Called with the device mutex held. Quota denial leaves the selected resource
// enabled and returns control to ReShade to issue the original operation once.
bool acquire_calibration_write(DeviceData& data, std::uint64_t frame) {
  const auto now = GetTickCount64();
  if (data.write_budget.try_acquire(frame, now)) {
    return true;
  }
  if (!data.throttle_logged || now - data.last_throttle_log_ms >= 5000) {
    char message[384];
    std::snprintf(message, sizeof(message),
                  "Taxi Camera: candidate=%llu calibration remains enabled; quota limited. present=%llu window=%llu writes=%u/%u "
                  "session_skipped=%llu timed_window_resets=%llu. Original rendering continues; calibration resumes next window.",
                  static_cast<unsigned long long>(data.selected_id), static_cast<unsigned long long>(frame),
                  static_cast<unsigned long long>(data.write_budget.window_epoch), data.write_budget.window_accepted,
                  data.write_budget.limit(), static_cast<unsigned long long>(data.write_budget.total_skipped),
                  static_cast<unsigned long long>(data.write_budget.timed_window_resets));
    reshade::log::message(reshade::log::level::info, message);
    data.last_throttle_log_ms = now;
    data.throttle_logged = true;
  }
  return false;
}

void forget_view(DeviceData& data, std::uint64_t handle) {
  const auto view = data.views.find(handle);
  if (view == data.views.end()) {
    return;
  }
  const auto target = data.targets.find(view->second.resource);
  if (target != data.targets.end() && target->second.id == view->second.target_id) {
    // Selection is bound to the live resource generation, not an application
    // descriptor slot. Calibration uses its own descriptor when required.
    if (target->second.views > 0) {
      --target->second.views;
    }
  }
  data.views.erase(view);
}

void on_init_device(api::device* device) {
  if (device->get_api() == api::device_api::d3d12) {
    create_private_data<DeviceData>(device);
    live_d3d12_devices.fetch_add(1, std::memory_order_acq_rel);
    taxi_camera::scene_handoff().register_device(reinterpret_cast<std::uintptr_t>(device));
    taxi_camera::scene_runtime::init_device(reinterpret_cast<std::uintptr_t>(device),
                                            reinterpret_cast<ID3D12Device*>(device->get_native()));
    taxi_camera::scene_runtime::manager().set_unknown_list_observer(observe_unknown_native_list, nullptr);
    reshade::log::message(reshade::log::level::info, "Taxi Camera Native Probe: D3D12 device initialized; calibration disabled.");
  }
}

void on_destroy_device(api::device* device) {
  if (private_data<DeviceData>(device) && live_d3d12_devices.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    // Both workers are shared across graphics devices. An unrelated secondary
    // device must not stop active cameras; final teardown runs outside DllMain.
    taxi_camera::native_camera::shutdown_mount_config();
    taxi_camera::native_camera::request_scene_stop(false);
  }
  resource_creation::unregister_device(reinterpret_cast<std::uintptr_t>(device));
  taxi_camera::scene_handoff().unregister_device(reinterpret_cast<std::uintptr_t>(device));
  taxi_camera::scene_runtime::destroy_device(reinterpret_cast<std::uintptr_t>(device));
  if (auto* data = private_data<DeviceData>(device)) {
    char message[192];
    {
      std::lock_guard<std::mutex> lock(data->mutex);
      std::snprintf(message, sizeof(message), "Taxi Camera Native Probe: device destroyed; targets=%zu, recorded_writes=%llu.",
                    data->targets.size(), static_cast<unsigned long long>(data->recorded_writes));
      data->device_lost = true;
      disarm(*data);
    }
    reshade::log::message(reshade::log::level::info, message);
    if (data->owned_view.handle != 0) {
      device->destroy_resource_view(data->owned_view);
      data->owned_view = {};
    }
    // Queue destruction verifies GPU completion before uploads are released.
    // An unverified drain retains references until process exit instead.
    if (data->retain_uploads) {
      data->copy_calibration.abandon();
    } else {
      data->copy_calibration.release();
    }
  }
  destroy_private_data<DeviceData>(device);
}

void on_init_command_list(api::command_list* command_list);

void on_init_command_queue(api::command_queue* queue) {
  if (queue->get_device()->get_api() == api::device_api::d3d12) {
    taxi_camera::scene_runtime::init_queue(reinterpret_cast<std::uintptr_t>(queue->get_device()),
                                           reinterpret_cast<ID3D12CommandQueue*>(queue->get_native()));
    // ReShade creates this native list before init_command_queue, without
    // emitting init_command_list. Its UI/effect flushes use the app queue too.
    // Observe its actual barriers and successful native Resets, rather than
    // treating every flush as unidentified work that erases camera state.
    if (auto* immediate = queue->get_immediate_command_list(); immediate && !private_data<CommandData>(immediate)) {
      on_init_command_list(immediate);
      auto* command = private_data<CommandData>(immediate);
      auto* data = create_private_data<QueueData>(queue);
      data->immediate = immediate;
      data->native = reinterpret_cast<ID3D12GraphicsCommandList*>(immediate->get_native());
      data->generation = command->object_generation;
      reshade::log::message(reshade::log::level::info, command->capture_ready
                                                           ? "Taxi Camera: ReShade immediate command list observed."
                                                           : "Taxi Camera: ReShade immediate command list registration failed.");
    }
  }
}

void on_destroy_command_queue(api::command_queue* queue) {
  if (auto* observed = private_data<QueueData>(queue)) {
    // The API object is still alive here. Use the saved native identity even
    // if ReShade replaced its failed list internally; never unregister a newer
    // object's incarnation through the old registration.
    render_boundary::unregister_list(observed->native, observed->generation);
    taxi_camera::pfd_adapter::destroy_list(observed->immediate, observed->generation);
    taxi_camera::scene_runtime::manager().destroy_command_list(observed->native, observed->generation);
    destroy_private_data<CommandData>(observed->immediate);
    destroy_private_data<QueueData>(queue);
  }
  auto* data = private_data<DeviceData>(queue->get_device());
  if (data == nullptr) {
    return;
  }
  bool has_uploads = false;
  {
    std::lock_guard<std::mutex> lock(data->mutex);
    has_uploads = data->copy_calibration.entry_count() != 0;
  }
  if (has_uploads) {
    // A bounded shutdown-only fence protects immutable uploads from outstanding
    // copies. Do not hold the registry mutex across a GPU wait.
    const bool drained = taxi_camera::drain_copy_queue(reinterpret_cast<ID3D12CommandQueue*>(queue->get_native()),
                                                       reinterpret_cast<ID3D12Device*>(queue->get_device()->get_native()));
    if (!drained) {
      std::lock_guard<std::mutex> lock(data->mutex);
      data->retain_uploads = true;
      reshade::log::message(reshade::log::level::warning,
                            "Taxi Camera: queue completion unverified; retaining immutable upload storage until process exit.");
    }
  }
}

void on_init_command_list(api::command_list* command_list) {
  if (command_list->get_device()->get_api() != api::device_api::d3d12) {
    return;
  }
  auto* data = create_private_data<CommandData>(command_list);
  auto* native = reinterpret_cast<ID3D12GraphicsCommandList*>(command_list->get_native());
  data->direct = native->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT;
  data->copy_capable = data->direct || native->GetType() == D3D12_COMMAND_LIST_TYPE_COPY;
  static std::atomic<std::uint64_t> generations{0};
  data->object_generation = ++generations;
  if (data->direct && taxi_camera::scene_runtime::manager().register_command_list(
                          native, reinterpret_cast<std::uintptr_t>(command_list->get_device()), data->object_generation)) {
    data->capture_ready = taxi_camera::pfd_adapter::init_list(command_list, reinterpret_cast<std::uintptr_t>(command_list->get_device()),
                                                              data->object_generation);
    if (data->capture_ready) {
      const auto boundary = render_boundary::register_list(native, data->object_generation, BoundaryCallbacks);
      boundary_hook_status.store(boundary.status, std::memory_order_relaxed);
      if (!boundary.ready)
        ++boundary_hook_failures;
    }
  }
}

void on_destroy_command_list(api::command_list* command_list) {
  if (auto* data = private_data<CommandData>(command_list)) {
    render_boundary::unregister_list(reinterpret_cast<ID3D12GraphicsCommandList*>(command_list->get_native()), data->object_generation);
    taxi_camera::pfd_adapter::destroy_list(command_list, data->object_generation);
    taxi_camera::scene_runtime::manager().destroy_command_list(reinterpret_cast<ID3D12GraphicsCommandList*>(command_list->get_native()),
                                                               data->object_generation);
  }
  destroy_private_data<CommandData>(command_list);
}

void on_reset_command_list(api::command_list* command_list) {
  if (auto* data = private_data<CommandData>(command_list)) {
    const bool direct = data->direct;
    const bool copy_capable = data->copy_capable;
    const auto generation = data->object_generation;
    const bool capture_ready = data->capture_ready;
    *data = {};
    data->direct = direct;
    data->copy_capable = copy_capable;
    data->object_generation = generation;
    data->capture_ready = capture_ready;
  }
}

void on_close_command_list(api::command_list* command_list) {
  if (auto* data = private_data<CommandData>(command_list)) {
    data->view = 0;
  }
}

void on_init_resource(api::device* device,
                      const api::resource_desc& desc,
                      const api::subresource_data*,
                      api::resource_usage,
                      api::resource resource) {
  auto* data = private_data<DeviceData>(device);
  if (data && !data->creation_observation_attempted.exchange(true, std::memory_order_acq_rel)) {
    // ReShade installs this actual resource's GetDevice proxy substitution
    // before init_resource. No cast from the unrelated api::device ABI is used.
    ID3D12Device* proxy = nullptr;
    auto* native_resource = reinterpret_cast<ID3D12Resource*>(resource.handle);
    if (resource.handle && SUCCEEDED(native_resource->GetDevice(IID_PPV_ARGS(&proxy))) && proxy) {
      const auto observed = resource_creation::register_device(reinterpret_cast<std::uintptr_t>(device), proxy,
                                                               reinterpret_cast<ID3D12Device*>(device->get_native()));
      data->creation_status.store(observed.status, std::memory_order_release);
      proxy->Release();
      char message[192];
      std::snprintf(message, sizeof(message), "Taxi Camera resource creation observer: ready=%d methods=%u status=%s",
                    observed.ready ? 1 : 0, observed.hooked_methods, observed.status);
      reshade::log::message(reshade::log::level::info, message);
    } else {
      if (proxy)
        proxy->Release();
      data->creation_status.store("proxy_device_unavailable", std::memory_order_release);
    }
  }
  if (data == nullptr || !taxi_camera::tracked_texture(desc)) {
    taxi_camera::scene_handoff().register_resource(reinterpret_cast<std::uintptr_t>(device), resource.handle, 0);
    return;
  }
  std::unique_lock<std::mutex> lock(data->mutex);
  if (data->targets.size() >= kMaximumTargets && data->targets.find(resource.handle) == data->targets.end()) {
    data->capacity_reached = true;
    taxi_camera::scene_handoff().register_resource(reinterpret_cast<std::uintptr_t>(device), resource.handle, 0);
    return;
  }
  if (const auto previous = data->targets.find(resource.handle); previous != data->targets.end()) {
    data->taxi_routes.forget(previous->second.id);
    if (data->selected_id == previous->second.id) {
      disarm(*data);
    }
  }
  const auto id = data->next_id++;
  data->targets.insert_or_assign(resource.handle, Target{desc, id});
  taxi_camera::scene_handoff().register_resource(reinterpret_cast<std::uintptr_t>(device), resource.handle, id);
  lock.unlock();
  if (possible_camera_source(desc)) {
    // The event supplies the actual resource, independently of the engine's
    // opaque output identity. Preserve creation-time barriers before publication.
    auto* native = reinterpret_cast<ID3D12Resource*>(resource.handle);
    const auto native_desc = native->GetDesc();
    const auto initial = resource_creation::initial_model(reinterpret_cast<std::uintptr_t>(device), native, native_desc);
    if (taxi_camera::scene_runtime::manager().register_source_candidate(reinterpret_cast<std::uintptr_t>(device), native, id, native_desc,
                                                                        initial))
      data->source_candidates_seen.store(true, std::memory_order_release);
    lock.lock();
    if (auto target = data->targets.find(resource.handle); target != data->targets.end() && target->second.id == id)
      target->second.initial_model = initial;
  }
}

void on_destroy_resource(api::device* device, api::resource resource) {
  taxi_camera::scene_handoff().unregister_resource(reinterpret_cast<std::uintptr_t>(device), resource.handle);
  auto* data = private_data<DeviceData>(device);
  if (data == nullptr) {
    return;
  }
  std::unique_lock<std::mutex> lock(data->mutex);
  const auto target = data->targets.find(resource.handle);
  if (target == data->targets.end()) {
    return;
  }
  data->taxi_routes.forget(target->second.id);
  if (data->selected_id == target->second.id) {
    disarm(*data);
  }
  if (data->owned_target_id == target->second.id) {
    device->destroy_resource_view(data->owned_view);
    data->owned_view = {};
    data->owned_target_id = 0;
  }
  for (auto view = data->views.begin(); view != data->views.end();) {
    if (view->second.resource == resource.handle) {
      view = data->views.erase(view);
    } else {
      ++view;
    }
  }
  const auto generation = target->second.id;
  data->targets.erase(target);
  lock.unlock();
  taxi_camera::scene_runtime::manager().unregister_source_candidate(reinterpret_cast<std::uintptr_t>(device),
                                                                    reinterpret_cast<ID3D12Resource*>(resource.handle), generation);
}

void on_init_resource_view(api::device* device,
                           api::resource resource,
                           api::resource_usage usage,
                           const api::resource_view_desc& desc,
                           api::resource_view view) {
  auto* data = private_data<DeviceData>(device);
  if (data == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(data->mutex);
  // D3D12 CPU descriptors can be overwritten without a destroy notification.
  forget_view(*data, view.handle);
  const auto target = data->targets.find(resource.handle);
  if (target != data->targets.end() && usage == api::resource_usage::shader_resource) {
    ++target->second.srv_registrations;
  }
  if (target == data->targets.end() || usage != api::resource_usage::render_target || desc.type != api::resource_view_type::texture_2d ||
      !taxi_camera::typed_color_format(desc.format) || desc.texture.first_level >= target->second.desc.texture.levels ||
      desc.texture.levels != 1) {
    return;
  }
  // A non-array Texture2D RTV ignores layer fields. ReShade's native converter
  // leaves them at zero; the underlying resource was already checked for one layer.
  if (data->views.size() >= kMaximumViews) {
    data->capacity_reached = true;
    return;
  }
  data->views.emplace(view.handle, View{resource.handle, target->second.id, data->next_view_generation++});
  ++target->second.views;
}

void on_destroy_resource_view(api::device* device, api::resource_view view) {
  if (auto* data = private_data<DeviceData>(device)) {
    std::lock_guard<std::mutex> lock(data->mutex);
    forget_view(*data, view.handle);
  }
}

void observe_bindings(api::command_list* command_list, std::uint32_t count, const api::resource_view* views) {
  auto* command = private_data<CommandData>(command_list);
  auto* data = private_data<DeviceData>(command_list->get_device());
  if (command == nullptr || data == nullptr) {
    return;
  }
  command->observed_count = 0;
  command->source_count = 0;
  command->activity_epoch = data->activity_epoch.load(std::memory_order_acquire);
  const bool track_activity = data->activity_tracking.load(std::memory_order_acquire);
  if (views == nullptr || (!track_activity && !data->source_candidates_seen.load(std::memory_order_acquire))) {
    return;
  }
  if (count > command->observed.size()) {
    taxi_camera::scene_runtime::manager().invalidate_source_recording(
        reinterpret_cast<ID3D12GraphicsCommandList*>(command_list->get_native()), command->object_generation, true);
    command->source_recording_unsupported = true;
    return;
  }
  for (std::uint32_t i = 0; i < std::min<std::uint32_t>(count, command->observed.size()); ++i) {
    if (views[i].handle == 0) {
      continue;
    }
    const auto resource = taxi_camera_resource_from_view(command_list->get_device(), views[i].handle);
    std::lock_guard<std::mutex> lock(data->mutex);
    if (const auto target = data->targets.find(resource); target != data->targets.end()) {
      bool duplicate = false;
      for (std::uint32_t j = 0; j < command->observed_count; ++j) {
        duplicate |= command->observed[j].target_id == target->second.id;
      }
      if (duplicate) {
        continue;
      }
      command->observed[command->observed_count++] = {resource, target->second.id, 0};
      if (possible_camera_source(target->second.desc)) {
        command->source_targets[command->source_count] = reinterpret_cast<ID3D12Resource*>(resource);
        command->source_generations[command->source_count++] = target->second.id;
      }
    }
  }
}

void on_bind_render_targets(api::command_list* command_list,
                            std::uint32_t count,
                            const api::resource_view* views,
                            api::resource_view depth) {
  observe_bindings(command_list, count, views);
  auto* command = private_data<CommandData>(command_list);
  auto* data = private_data<DeviceData>(command_list->get_device());
  if (command == nullptr || data == nullptr) {
    return;
  }
  command->view = 0;
  command->has_depth_stencil = depth.handle != 0;
  command->bound_depth_format = DXGI_FORMAT_UNKNOWN;
  if ((!data->activity_tracking.load(std::memory_order_acquire) && !data->camera_enabled.load(std::memory_order_acquire)) ||
      !command->direct || command->unsupported_recording || count != 1 || views == nullptr || views[0].handle == 0) {
    return;
  }
  // RTV CopyDescriptors does not emit an add-on view event in ReShade 6.8.
  // Read its current internal mapping while still inside the wrapped OMSetRT
  // call. The MSVC-target C bridge handles the host's struct-return ABI.
  const auto resource = taxi_camera_resource_from_view(command_list->get_device(), views[0].handle);
  api::resource_view_desc desc{};
  taxi_camera_resource_view_desc(command_list->get_device(), views[0].handle, &desc);
  if (desc.type != api::resource_view_type::texture_2d || !taxi_camera::typed_color_format(desc.format) || desc.texture.levels != 1) {
    return;
  }
  if (command->has_depth_stencil) {
    api::resource_view_desc depth_desc{};
    taxi_camera_resource_view_desc(command_list->get_device(), depth.handle, &depth_desc);
    if (depth_desc.type == api::resource_view_type::texture_2d && depth_desc.texture.levels == 1) {
      switch (static_cast<DXGI_FORMAT>(depth_desc.format)) {
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
          command->bound_depth_format = static_cast<DXGI_FORMAT>(depth_desc.format);
          break;
        default:
          break;
      }
    }
  }
  std::lock_guard<std::mutex> lock(data->mutex);
  if (const auto target = data->targets.find(resource);
      target != data->targets.end() && taxi_camera::calibration_eligible(target->second.desc)) {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!taxi_camera::bound_mip_extent(target->second.desc, desc.texture.first_level, width, height) || width < 32 || height < 32) {
      return;
    }
    command->view = views[0].handle;
    command->binding = {resource, target->second.id, 0};
    command->bound_desc = desc;
    command->bound_width = width;
    command->bound_height = height;
  }
}

void on_barrier(api::command_list* command_list,
                std::uint32_t count,
                const api::resource* resources,
                const api::resource_usage*,
                const api::resource_usage*) {
  auto* command = private_data<CommandData>(command_list);
  if (command == nullptr || command->view == 0) {
    return;
  }
  // ReShade 6.8's event omits D3D12 split flags/enhanced layouts. Never infer
  // state or emit a transition from this event; require a subsequent RTV bind.
  for (std::uint32_t i = 0; i < count; ++i) {
    if (resources[i].handle == command->binding.resource || resources[i].handle == 0) {
      command->view = 0;
      break;
    }
  }
}

void unsupported_recording(api::command_list* command_list, bool invalidate_sources = true, bool affects_all_sources = false) {
  if (auto* command = private_data<CommandData>(command_list)) {
    if (invalidate_sources) {
      taxi_camera::scene_runtime::manager().invalidate_source_recording(
          reinterpret_cast<ID3D12GraphicsCommandList*>(command_list->get_native()), command->object_generation, affects_all_sources);
      command->source_recording_unsupported = true;
    }
    command->source_count = 0;
    if (command->unsupported_recording) {
      return;
    }
    command->view = 0;
    command->unsupported_recording = true;
    if (auto* data = private_data<DeviceData>(command_list->get_device());
        data != nullptr && data->activity_tracking.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lock(data->mutex);
      if (data->activity_tracking.load(std::memory_order_acquire))
        ++data->skipped_recordings;
    }
  }
}

bool on_begin_render_pass(api::command_list* command_list,
                          std::uint32_t count,
                          const api::render_pass_render_target_desc* targets,
                          const api::render_pass_depth_stencil_desc*,
                          api::render_pass_flags) {
  std::array<api::resource_view, 8> views{};
  const auto observed_count = targets == nullptr ? 0 : std::min<std::uint32_t>(count, views.size());
  for (std::uint32_t i = 0; i < observed_count; ++i) {
    views[i] = targets[i].view;
  }
  observe_bindings(command_list, observed_count, views.data());
  auto* command = private_data<CommandData>(command_list);
  auto* data = private_data<DeviceData>(command_list->get_device());
  if (command != nullptr && data != nullptr) {
    command->inside_native_pass = true;
    if (data->activity_tracking.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lock(data->mutex);
      for (std::uint32_t i = 0;
           data->activity_tracking.load(std::memory_order_acquire) &&
           command->activity_epoch == data->activity_epoch.load(std::memory_order_acquire) && i < command->observed_count;
           ++i) {
        if (auto target = data->targets.find(command->observed[i].resource);
            target != data->targets.end() && target->second.id == command->observed[i].target_id) {
          ++target->second.passes;
          target->second.last_frame = data->frame.load(std::memory_order_relaxed);
        }
      }
    }
  }
  // Observe native passes, but skip writes for the remainder of this recording.
  if (command && command->source_count) {
    taxi_camera::scene_runtime::manager().invalidate_source_targets(
        reinterpret_cast<ID3D12GraphicsCommandList*>(command_list->get_native()), command->object_generation, command->source_count,
        command->source_targets.data(), command->source_generations.data());
  }
  unsupported_recording(command_list, false);
  return false;
}

bool on_end_render_pass(api::command_list* command_list) {
  if (auto* command = private_data<CommandData>(command_list)) {
    command->inside_native_pass = false;
    command->observed_count = 0;
    command->source_count = 0;
  }
  return false;
}

void observe_draw(api::command_list* command_list, std::uint32_t count, std::uint32_t instances) {
  auto* command = private_data<CommandData>(command_list);
  auto* data = private_data<DeviceData>(command_list->get_device());
  if (command == nullptr || data == nullptr || !data->activity_tracking.load(std::memory_order_acquire) ||
      command->activity_epoch != data->activity_epoch.load(std::memory_order_acquire) || command->observed_count == 0 || count == 0 ||
      instances == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(data->mutex);
  if (!data->activity_tracking.load(std::memory_order_acquire) ||
      command->activity_epoch != data->activity_epoch.load(std::memory_order_acquire))
    return;
  for (std::uint32_t i = 0; i < command->observed_count; ++i) {
    if (auto target = data->targets.find(command->observed[i].resource);
        target != data->targets.end() && target->second.id == command->observed[i].target_id) {
      ++target->second.draws;
      target->second.last_frame = data->frame.load(std::memory_order_relaxed);
    }
  }
}

enum class Transfer { copy, upload, resolve };

void observe_transfer(api::command_list* command_list, api::resource source, api::resource destination, Transfer kind) {
  auto* data = private_data<DeviceData>(command_list->get_device());
  if (data == nullptr || !data->activity_tracking.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(data->mutex);
  if (!data->activity_tracking.load(std::memory_order_acquire))
    return;
  const auto frame = data->frame.load(std::memory_order_relaxed);
  const auto from = data->targets.find(source.handle);
  if (from != data->targets.end()) {
    ++from->second.copies_out;
    from->second.last_frame = frame;
  }
  if (auto to = data->targets.find(destination.handle); to != data->targets.end()) {
    if (kind == Transfer::copy) {
      ++to->second.copies;
    } else if (kind == Transfer::upload) {
      ++to->second.uploads;
    } else {
      ++to->second.resolves;
    }
    to->second.last_source_id = from != data->targets.end() ? from->second.id : 0;
    to->second.last_frame = frame;
  }
}

template <typename Copy>
bool after_copy(api::command_list* command_list, api::resource destination, Copy copy) {
  auto* command = private_data<CommandData>(command_list);
  auto* device = command_list->get_device();
  auto* data = private_data<DeviceData>(device);
  if (command == nullptr || data == nullptr || !data->activity_tracking.load(std::memory_order_acquire) || !command->copy_capable ||
      command->inside_native_pass) {
    return false;
  }
  std::unique_lock<std::mutex> lock(data->mutex);
  if (!data->activity_tracking.load(std::memory_order_acquire))
    return false;
  const auto target = data->targets.find(destination.handle);
  if (target == data->targets.end() || !taxi_camera::copy_calibration_eligible(target->second.desc) || data->device_lost) {
    return false;
  }
  ++target->second.eligible_copies;
  if (!data->enabled || data->selected_id != target->second.id) {
    return false;
  }
  const auto frame = data->frame.load(std::memory_order_relaxed);
  if (!acquire_calibration_write(*data, frame)) {
    return false;
  }
  // These public methods forward the original copy exactly once without another
  // add-on event. The application's own copy establishes COPY_DEST here. Append
  // calibration in that same state; never infer or change a destination state.
  // Native copy capture may retire a packet and release an application source,
  // invoking resource destruction. It must not run under the inventory lock.
  const auto target_id = target->second.id;
  lock.unlock();
  copy();
  lock.lock();
  const auto current = data->targets.find(destination.handle);
  if (current == data->targets.end() || current->second.id != target_id || data->selected_id != target_id || !data->enabled ||
      !data->activity_tracking.load(std::memory_order_acquire) || data->device_lost)
    return true;  // The application copy already ran, even if selection changed.
  const render_boundary::ScopedBypass calibration_commands;
  if (data->copy_calibration.record(
          reinterpret_cast<ID3D12GraphicsCommandList*>(command_list->get_native()), reinterpret_cast<ID3D12Device*>(device->get_native()),
          reinterpret_cast<ID3D12Resource*>(destination.handle), current->second.desc.texture.width, current->second.desc.texture.height,
          static_cast<DXGI_FORMAT>(current->second.desc.texture.format), frame)) {
    ++data->recorded_writes;
  } else {
    data->enabled = false;
    data->copy_creation_failed = true;
    reshade::log::message(reshade::log::level::warning,
                          "Taxi Camera: calibration stopped; immutable copy-calibration allocation or validation failed.");
  }
  // Even if calibration allocation failed, the original copy already ran.
  return true;
}

bool on_copy_resource(api::command_list* command_list, api::resource source, api::resource destination) {
  const auto scene = taxi_camera::scene_handoff().observe_copy(reinterpret_cast<std::uintptr_t>(command_list->get_device()), source.handle,
                                                               destination.handle);
  if (auto* data = private_data<DeviceData>(command_list->get_device())) {
    if (scene.source.matched)
      ++data->scene_copy_sources[scene.source.feed];
    if (scene.destination.matched)
      ++data->scene_copy_destinations[scene.destination.feed];
  }
  observe_transfer(command_list, source, destination, Transfer::copy);
  // Camera capture happens only after the actual native copy, with complete
  // native scope and object-generation proof. Do not capture it twice here.
  return after_copy(command_list, destination, [&]() { command_list->copy_resource(source, destination); });
}

bool on_copy_texture(api::command_list* command_list,
                     api::resource source,
                     std::uint32_t source_subresource,
                     const api::subresource_box* source_box,
                     api::resource destination,
                     std::uint32_t destination_subresource,
                     const api::subresource_box* destination_box,
                     api::filter_mode) {
  const auto scene = taxi_camera::scene_handoff().observe_copy(reinterpret_cast<std::uintptr_t>(command_list->get_device()), source.handle,
                                                               destination.handle);
  if (auto* data = private_data<DeviceData>(command_list->get_device())) {
    if (scene.source.matched)
      ++data->scene_copy_sources[scene.source.feed];
    if (scene.destination.matched)
      ++data->scene_copy_destinations[scene.destination.feed];
  }
  observe_transfer(command_list, source, destination, Transfer::copy);
  const auto forward = [&]() {
    // The D3D12 event preserves both SUBRESOURCE_INDEX locations, source box
    // (including null), and destination XYZ. Native replay supports a nonzero
    // destination origin with a null source box as well. get_native() returns
    // the original list, so forwarding does not recurse through these callbacks.
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = reinterpret_cast<ID3D12Resource*>(source.handle);
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = source_subresource;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = reinterpret_cast<ID3D12Resource*>(destination.handle);
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = destination_subresource;
    D3D12_BOX box{};
    if (source_box != nullptr) {
      box = {source_box->left, source_box->top, source_box->front, source_box->right, source_box->bottom, source_box->back};
    }
    auto* native = reinterpret_cast<ID3D12GraphicsCommandList*>(command_list->get_native());
    native->CopyTextureRegion(&to, destination_box != nullptr ? destination_box->left : 0,
                              destination_box != nullptr ? destination_box->top : 0,
                              destination_box != nullptr ? destination_box->front : 0, &from, source_box != nullptr ? &box : nullptr);
  };
  if (destination_subresource != 0)
    return false;
  return after_copy(command_list, destination, forward);
}

bool on_upload(api::command_list* command_list,
               api::resource source,
               std::uint64_t,
               std::uint32_t,
               std::uint32_t,
               api::resource destination,
               std::uint32_t,
               const api::subresource_box*) {
  observe_transfer(command_list, source, destination, Transfer::upload);
  // This event loses the original source box origin and native row pitch.
  // Forwarding it through the public API is not guaranteed to preserve the copy.
  return false;
}

bool on_resolve(api::command_list* command_list,
                api::resource source,
                std::uint32_t,
                const api::subresource_box*,
                api::resource destination,
                std::uint32_t,
                std::uint32_t,
                std::uint32_t,
                std::uint32_t,
                api::format) {
  observe_transfer(command_list, source, destination, Transfer::resolve);
  return false;
}

bool on_indirect(api::command_list* command_list, api::indirect_command, api::resource, std::uint64_t, std::uint32_t, std::uint32_t) {
  auto* command = private_data<CommandData>(command_list);
  auto* data = private_data<DeviceData>(command_list->get_device());
  if (command != nullptr && data != nullptr && data->activity_tracking.load(std::memory_order_acquire) &&
      command->activity_epoch == data->activity_epoch.load(std::memory_order_acquire) && command->observed_count != 0) {
    std::lock_guard<std::mutex> lock(data->mutex);
    for (std::uint32_t i = 0;
         data->activity_tracking.load(std::memory_order_acquire) &&
         command->activity_epoch == data->activity_epoch.load(std::memory_order_acquire) && i < command->observed_count;
         ++i) {
      if (auto target = data->targets.find(command->observed[i].resource);
          target != data->targets.end() && target->second.id == command->observed[i].target_id) {
        ++target->second.indirect;
        target->second.last_frame = data->frame.load(std::memory_order_relaxed);
      }
    }
  }
  if (command && command->source_count) {
    taxi_camera::scene_runtime::manager().invalidate_source_targets(
        reinterpret_cast<ID3D12GraphicsCommandList*>(command_list->get_native()), command->object_generation, command->source_count,
        command->source_targets.data(), command->source_generations.data());
  }
  unsupported_recording(command_list, false);
  return false;
}

void on_execute_secondary(api::command_list* command_list, api::command_list*) {
  // D3D12 bundles cannot contain ResourceBarrier or OMSetRenderTargets. They
  // affect draw/pipeline bookkeeping, not every texture's known state model.
  // https://learn.microsoft.com/windows/win32/direct3d12/recording-command-lists-and-bundles
  unsupported_recording(command_list, false);
}

template <typename Draw>
bool after_direct_draw(api::command_list* command_list, std::uint32_t count, std::uint32_t instances, Draw draw) {
  auto* command = private_data<CommandData>(command_list);
  if (command == nullptr || command->view == 0 || command->unsupported_recording || count == 0 || instances == 0) {
    return false;
  }
  auto* data = private_data<DeviceData>(command_list->get_device());
  if (data == nullptr ||
      (!data->activity_tracking.load(std::memory_order_acquire) && !data->camera_enabled.load(std::memory_order_acquire)) ||
      command->activity_epoch != data->activity_epoch.load(std::memory_order_acquire)) {
    return false;
  }
  std::unique_lock<std::mutex> lock(data->mutex);
  const auto target = data->targets.find(command->binding.resource);
  if ((!data->activity_tracking.load(std::memory_order_acquire) && !data->camera_enabled.load(std::memory_order_acquire)) ||
      command->activity_epoch != data->activity_epoch.load(std::memory_order_acquire) || target == data->targets.end() ||
      target->second.id != command->binding.target_id || data->device_lost) {
    command->view = 0;
    return false;
  }
  const auto frame = data->frame.load(std::memory_order_relaxed);
  if (data->activity_tracking.load(std::memory_order_relaxed)) {
    ++target->second.eligible_draws;
    target->second.last_frame = frame;
  }
  if (!target->second.name_checked && target->second.desc.texture.width == 768 && target->second.desc.texture.height == 1024) {
    // Query once, after the first real draw, so names set after resource creation
    // are available. Optional D3D metadata is not an inferred material binding.
    auto& observed = target->second;
    observed.name_checked = true;
    auto* native_resource = reinterpret_cast<ID3D12Resource*>(command->binding.resource);
    taxi_camera::read_resource_debug_name(native_resource, observed.debug_name);
    char message[400];
    std::snprintf(message, sizeof(message), "Taxi Camera PFD label: id=%llu name=%s",
                  static_cast<unsigned long long>(observed.id), observed.debug_name[0] ? observed.debug_name.data() : "<absent>");
    reshade::log::message(reshade::log::level::info, message);
  }
  const bool selected_camera = data->camera_enabled.load(std::memory_order_relaxed) &&
                               (data->taxi_button_control ? data->taxi_routes.matches(target->second.id, data->taxi_active_mask)
                                                         : data->selected_id == target->second.id);
  const bool known_depth = !command->has_depth_stencil || command->bound_depth_format != DXGI_FORMAT_UNKNOWN;
  if (selected_camera) {
    auto& status = data->pfd_draws;
    ++status.draws;
    status.color = static_cast<DXGI_FORMAT>(command->bound_desc.format);
    status.depth = command->bound_depth_format;
    status.mip = command->bound_desc.texture.first_level;
    status.observer_blocked += !command->capture_ready;
    status.depth_blocked += !known_depth;
    status.pass_blocked += command->inside_native_pass;
  }
  if (selected_camera && command->capture_ready && known_depth && !command->inside_native_pass) {
    // Packet retirement may release an application resource and synchronously
    // trigger ReShade's resource-destroy callback. Never enter runtime/manager
    // locks while holding the inventory mutex that callback needs. The actual
    // application draw establishes the live bound RTV for this command list.
    const auto format = static_cast<DXGI_FORMAT>(command->bound_desc.format);
    const auto width = command->bound_width;
    const auto height = command->bound_height;
    const auto depth_format = command->bound_depth_format;
    ++data->pfd_draws.attempted;
    lock.unlock();
    draw();
    taxi_camera::scene_runtime::stamp(command_list, reinterpret_cast<std::uintptr_t>(command_list->get_device()), format, width, height,
                                      depth_format);
    return true;
  }
  if (!data->enabled || data->selected_id != target->second.id) {
    return false;
  }
  if (!acquire_calibration_write(*data, frame)) {
    return false;
  }
  // A CPU RTV can legally be recycled after OMSetRT returns. Never clear through
  // the application's descriptor. Own a stable descriptor for the selected live
  // resource; these pinned public create/destroy methods bypass add-on callbacks.
  if (data->owned_target_id != target->second.id || data->owned_mip != command->bound_desc.texture.first_level ||
      data->owned_format != command->bound_desc.format) {
    if (data->owned_view.handle != 0) {
      command_list->get_device()->destroy_resource_view(data->owned_view);
      data->owned_view = {};
      data->owned_target_id = 0;
    }
    if (!command_list->get_device()->create_resource_view(api::resource{command->binding.resource}, api::resource_usage::render_target,
                                                          command->bound_desc, &data->owned_view)) {
      data->enabled = false;
      data->view_creation_failed = true;
      reshade::log::message(reshade::log::level::warning, "Taxi Camera: calibration stopped; owned render-target view allocation failed.");
      return false;
    }
    data->owned_target_id = target->second.id;
    data->owned_mip = command->bound_desc.texture.first_level;
    data->owned_format = command->bound_desc.format;
  }
  // Native post-draw observation may take the capture-manager mutex even for
  // a calibrated RGBA camera-sized target. Release inventory ownership before
  // forwarding, then recheck the selected resource and owned descriptor before
  // issuing calibration. The application draw must still run exactly once.
  const auto target_id = target->second.id;
  const auto owned_view = data->owned_view.handle;
  lock.unlock();
  draw();
  lock.lock();
  const auto current = data->targets.find(command->binding.resource);
  if (current == data->targets.end() || current->second.id != target_id || data->selected_id != target_id || !data->enabled ||
      data->device_lost || data->owned_target_id != target_id || data->owned_view.handle != owned_view ||
      data->owned_mip != command->bound_desc.texture.first_level || data->owned_format != command->bound_desc.format)
    return true;
  auto* native = reinterpret_cast<ID3D12GraphicsCommandList*>(command_list->get_native());
  taxi_camera::record_calibration(native, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(data->owned_view.handle)}, command->bound_width,
                                  command->bound_height, frame);
  ++data->recorded_writes;
  return true;
}

void stage_source_draw(api::command_list* command_list, std::uint32_t count, std::uint32_t instances) {
  auto* command = private_data<CommandData>(command_list);
  if (!command || !command->direct || !command->capture_ready)
    return;
  const auto sources =
      count && instances && !command->inside_native_pass && !command->source_recording_unsupported ? command->source_count : 0;
  taxi_camera::scene_runtime::manager().stage_source_draw(reinterpret_cast<ID3D12GraphicsCommandList*>(command_list->get_native()),
                                                          command->object_generation, sources, command->source_targets.data(),
                                                          command->source_generations.data());
}

bool on_draw(api::command_list* command_list,
             std::uint32_t count,
             std::uint32_t instances,
             std::uint32_t first,
             std::uint32_t first_instance) {
  stage_source_draw(command_list, count, instances);
  observe_draw(command_list, count, instances);
  return after_direct_draw(command_list, count, instances, [&]() { command_list->draw(count, instances, first, first_instance); });
}

bool on_draw_indexed(api::command_list* command_list,
                     std::uint32_t count,
                     std::uint32_t instances,
                     std::uint32_t first,
                     std::int32_t offset,
                     std::uint32_t first_instance) {
  stage_source_draw(command_list, count, instances);
  observe_draw(command_list, count, instances);
  return after_direct_draw(command_list, count, instances,
                           [&]() { command_list->draw_indexed(count, instances, first, offset, first_instance); });
}

bool start_camera_scene(api::device* device, bool reuse_calibration = false) {
  const auto key = reinterpret_cast<std::uintptr_t>(device);
  if (!taxi_camera::scene_runtime::prepare(key))
    return false;
  taxi_camera::scene_runtime::reset_feed(key);
  taxi_camera::scene_runtime::manager().begin_source_tracking();
  taxi_camera::native_camera::request_scene_test(reuse_calibration);
  const auto scene = taxi_camera::native_camera::scene_snapshot();
  if (scene.hook_installed) {
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(addon_module, path, static_cast<DWORD>(std::size(path)));
    if (length && length < std::size(path))
      taxi_camera::native_camera::initialize_mount_config(path);
  }
  return scene.accepting_requests;
}

void stop_camera_scene(api::device* device, bool keep_telemetry) {
  taxi_camera::native_camera::shutdown_mount_config();
  taxi_camera::scene_runtime::manager().stop_source_tracking();
  taxi_camera::native_camera::request_scene_stop(keep_telemetry);
  taxi_camera::scene_runtime::reset_feed(reinterpret_cast<std::uintptr_t>(device));
}

void service_taxi_buttons(api::device* device, DeviceData& data, std::uint64_t now) {
  const std::lock_guard control_lock(data.control_mutex);
  bool initialize = false;
  {
    const std::lock_guard lock(data.mutex);
    if (!data.taxi_button_control || data.device_lost)
      return;
    if (!data.taxi_telemetry_started || now >= data.next_telemetry_retry_ms) {
      // Validation hosts load the same DLL. Only the simulator starts telemetry.
      wchar_t path[32768]{};
      const auto length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
      const auto* basename = std::wcsrchr(path, L'\\');
      if (!length || length >= std::size(path) || _wcsicmp(basename ? basename + 1 : path, L"FlightSimulator2024.exe"))
        return;
      data.taxi_telemetry_started = true;
      data.next_telemetry_retry_ms = now + 2000;
      initialize = true;
    }
    if (data.taxi_auto_detect && now >= data.next_detection_ms &&
        (!data.taxi_routes.targets[0] || !data.taxi_routes.targets[1])) {
      data.next_detection_ms = now + 1000;
      std::vector<taxi_camera::PfdTargetObservation> observations;
      observations.reserve(data.targets.size());
      std::array<std::uint64_t, 2> named{};
      bool names_ambiguous = false;
      for (const auto& [resource, target] : data.targets) {
        (void)resource;
        if (!taxi_camera::calibration_eligible(target.desc))
          continue;
        observations.push_back({target.id, target.eligible_draws, target.desc.texture.width, target.desc.texture.height,
                                target.desc.texture.levels, static_cast<unsigned>(target.desc.texture.format)});
        for (unsigned side = 0; side < 2; ++side) {
          if (std::strstr(target.debug_name.data(), side == 0 ? "SCREEN_DU_PFDL" : "SCREEN_DU_PFDR")) {
            names_ambiguous |= named[side] != 0;
            named[side] = target.id;
          }
        }
      }
      const auto& detected = data.pfd_detector.observe(observations.data(), observations.size(), now);
      data.taxi_detection_status = detected.status;
      if (!names_ambiguous && named[0] && named[1] && named[0] != named[1]) {
        data.taxi_routes.adopt_detected(named, true);
        data.taxi_detection_status = "Matched named PFD textures";
      } else if (detected.valid) {
        if (data.taxi_routes.adopt_detected(detected.targets))
          data.taxi_detection_status = "Matched stable PFD pair; surviving side assignments retained";
        else
          data.taxi_detection_status = "PFD replacement sides ambiguous; awaiting named textures or manual assignment";
      }
      if (data.taxi_routes.targets[0] && data.taxi_routes.targets[1]) {
        char message[256];
        std::snprintf(message, sizeof(message), "Taxi Camera PFD detection: left=%llu right=%llu method=%s",
                      static_cast<unsigned long long>(data.taxi_routes.targets[0]),
                      static_cast<unsigned long long>(data.taxi_routes.targets[1]), data.taxi_detection_status);
        reshade::log::message(reshade::log::level::info, message);
      }
    }
  }
  if (initialize && !taxi_camera::native_camera::initialize_body_pose_provider()) {
    const std::lock_guard lock(data.mutex);
    data.taxi_detection_status = "Waiting for simulator telemetry connection";
    return;
  }
  const auto buttons = taxi_camera::native_camera::get_taxi_buttons();
  bool start = false, stop = false;
  {
    const std::lock_guard lock(data.mutex);
    data.taxi_intent_state = data.taxi_intent.observe(now, buttons.valid, buttons.left_on, buttons.right_on);
    const auto mask = data.taxi_routes.active_mask(true, (data.taxi_intent_state.buttons & 1u) != 0,
                                                  (data.taxi_intent_state.buttons & 2u) != 0);
    if (!mask)
      data.taxi_start_failed = false;
    if (mask != data.taxi_active_mask) {
      data.taxi_active_mask = mask;
      data.enabled = false;
      data.activity_epoch.fetch_add(1, std::memory_order_acq_rel);
      char message[256];
      std::snprintf(message, sizeof(message), "Taxi Camera TAXI buttons: valid=%d left=%d right=%d active_mask=%u held=%d timed_out=%d",
                    buttons.valid, buttons.left_on, buttons.right_on, mask, data.taxi_intent_state.held, data.taxi_intent_state.timed_out);
      reshade::log::message(reshade::log::level::info, message);
    }
    data.camera_enabled.store(mask != 0 && !data.taxi_start_failed, std::memory_order_release);
    start = mask && !data.taxi_scene_requested;
    stop = !mask && data.taxi_scene_requested;
    if (start || stop)
      data.taxi_scene_requested = start;
  }
  if (start && !start_camera_scene(device, true)) {
    const std::lock_guard lock(data.mutex);
    // Require a button off/on edge after a native initialization failure.
    data.taxi_start_failed = true;
    data.camera_enabled.store(false, std::memory_order_release);
  }
  if (stop)
    stop_camera_scene(device, true);
}

void service_display_exposure(api::device* device, DeviceData& data, std::uint64_t now) {
  const std::lock_guard lock(data.control_mutex);
  const auto lighting = taxi_camera::native_camera::get_lighting();
  data.exposure_state = data.exposure_controller.update(now, data.manual_exposure_ev, data.automatic_exposure,
                                                        data.night_exposure_boost_ev, lighting.valid, lighting.ambient,
                                                        lighting.sample_ms);
  taxi_camera::scene_runtime::set_display_exposure(reinterpret_cast<std::uintptr_t>(device), data.exposure_state.applied_ev);
}

void on_present(api::command_queue* queue, api::swapchain*, const api::rect*, const api::rect*, std::uint32_t, const api::rect*) {
  const auto speed = taxi_camera::native_camera::get_ground_speed();
  taxi_camera::scene_runtime::set_ground_speed(reinterpret_cast<std::uintptr_t>(queue->get_device()), static_cast<float>(speed.knots),
                                               speed.valid);
  auto* device = queue->get_device();
  auto* data = private_data<DeviceData>(device);
  if (data == nullptr) {
    taxi_camera::scene_runtime::service();
    return;
  }
  const auto now = GetTickCount64();
  service_taxi_buttons(device, *data, now);
  service_display_exposure(device, *data, now);
  taxi_camera::scene_runtime::service();
  auto next_log = data->next_feed_log_ms.load(std::memory_order_relaxed);
  if (now >= next_log && data->next_feed_log_ms.compare_exchange_strong(next_log, now + 5000)) {
    const auto lighting = taxi_camera::native_camera::get_lighting();
    const auto body_timing = taxi_camera::native_camera::get_body_telemetry_timing();
    {
      const std::lock_guard lock(data->control_mutex);
      char message[384];
      std::snprintf(message, sizeof(message),
                    "Taxi Camera exposure: automatic=%d ambient=%.3f brightness=%.3f base_ev=%.2f night_boost_ev=%.2f "
                    "target_ev=%.2f applied_ev=%.2f lighting=%s state=%s",
                    data->automatic_exposure, lighting.ambient, lighting.brightness, data->manual_exposure_ev,
                    data->night_exposure_boost_ev, data->exposure_state.target_ev, data->exposure_state.applied_ev,
                    lighting.valid ? "live" : lighting.error, data->exposure_state.status);
      reshade::log::message(reshade::log::level::info, message);
      data->body_rate_available = data->previous_body_log_ms != 0 && now > data->previous_body_log_ms &&
                                  body_timing.accepted_samples >= data->previous_body_count;
      const auto elapsed = data->body_rate_available ? now - data->previous_body_log_ms : 0;
      data->body_arrivals_per_second = data->body_rate_available
          ? static_cast<float>(1000.0 * (body_timing.accepted_samples - data->previous_body_count) / elapsed) : 0;
      data->previous_body_count = body_timing.accepted_samples;
      data->previous_body_log_ms = now;
      std::snprintf(message, sizeof(message),
                    "Taxi Camera body timing: stream=SIM_FRAME samples=%llu rate_available=%d arrivals_per_second=%.2f "
                    "window_ms=%llu last_interval_ms=%llu fresh=%d",
                    static_cast<unsigned long long>(body_timing.accepted_samples), data->body_rate_available,
                    data->body_arrivals_per_second, static_cast<unsigned long long>(elapsed),
                    static_cast<unsigned long long>(body_timing.last_interval_ms), body_timing.fresh);
      reshade::log::message(reshade::log::level::info, message);
    }
    const auto buttons = taxi_camera::native_camera::get_taxi_buttons();
    {
      const std::lock_guard lock(data->mutex);
      char message[384];
      std::snprintf(message, sizeof(message),
                    "Taxi Camera automatic: enabled=%d signals=%s left=%d right=%d mask=%u left_pfd=%llu right_pfd=%llu held=%d timed_out=%d start_failed=%d detection=%s",
                    data->taxi_button_control, buttons.valid ? "live" : buttons.error, buttons.left_on, buttons.right_on,
                    data->taxi_active_mask, static_cast<unsigned long long>(data->taxi_routes.targets[0]),
                    static_cast<unsigned long long>(data->taxi_routes.targets[1]), data->taxi_intent_state.held,
                    data->taxi_intent_state.timed_out, data->taxi_start_failed, data->taxi_detection_status);
      reshade::log::message(reshade::log::level::info, message);
    }
    const auto feed = taxi_camera::scene_runtime::snapshot(reinterpret_cast<std::uintptr_t>(device));
    if (feed.initialized || feed.failed) {
      const auto state = taxi_camera::pfd_adapter::statistics();
      const auto scene = taxi_camera::native_camera::scene_snapshot();
      std::uint64_t selected_id = 0;
      PfdDrawStatus pfd_draws;
      {
        const std::lock_guard lock(data->mutex);
        selected_id = data->selected_id;
        pfd_draws = data->pfd_draws;
      }
      char message[768];
      std::snprintf(
          message, sizeof(message),
          "Taxi Camera feed: target=%llu armed=%d captured=%llu completed=%llu composed=%llu stamps=%llu state_skips=%llu quarantined=%llu "
          "matched=%d output1=%dx%d output2=%dx%d probe_ms=%.3f state=%s status=%s",
          static_cast<unsigned long long>(selected_id), data->camera_enabled.load(std::memory_order_relaxed) ? 1 : 0,
          static_cast<unsigned long long>(feed.capture.captures), static_cast<unsigned long long>(feed.capture.completed),
          static_cast<unsigned long long>(feed.frames), static_cast<unsigned long long>(feed.stamps),
          static_cast<unsigned long long>(feed.state_skips), static_cast<unsigned long long>(feed.capture.quarantined),
          scene.outputs_matched ? 1 : 0, scene.dimensions[0][2][0], scene.dimensions[0][2][1], scene.dimensions[1][2][0],
          scene.dimensions[1][2][1], scene.observer_last_ms, state.last_error, feed.message);
      reshade::log::message(reshade::log::level::info, message);
      std::snprintf(message, sizeof(message),
                    "Taxi Camera native lifecycle: pair_state=%u pose_waiting=%d recovery_pending=%d retries=%u stop_sequence=%llu reason=%s detail=%s status=%s",
                    static_cast<unsigned>(scene.pair.state), scene.pose_waiting, scene.recovery_pending, scene.recovery_attempts,
                    static_cast<unsigned long long>(scene.stop_sequence), taxi_camera::native_camera::scene_stop_reason_name(scene.stop_reason),
                    scene.stop_detail.c_str(), scene.message.c_str());
      reshade::log::message(reshade::log::level::info, message);
      std::snprintf(message, sizeof(message),
                    "Taxi Camera PFD root state: native_roots=%llu native_tables=%llu stamps_with_undefined_tables=%llu",
                    static_cast<unsigned long long>(state.native_roots), static_cast<unsigned long long>(state.native_tables),
                    static_cast<unsigned long long>(state.stamps_with_undefined_tables));
      reshade::log::message(reshade::log::level::info, message);
      std::snprintf(message, sizeof(message), "Taxi Camera ground speed: valid=%d knots=%.3f sample_ms=%llu", speed.valid ? 1 : 0,
                    speed.knots, static_cast<unsigned long long>(speed.sample_ms));
      reshade::log::message(reshade::log::level::info, message);
      const auto mount_file = taxi_camera::native_camera::mount_config_status();
      std::snprintf(message, sizeof(message), "Taxi Camera mount config: state=%s checks=%llu applications=%llu", mount_file.state,
                    static_cast<unsigned long long>(mount_file.checks), static_cast<unsigned long long>(mount_file.applications));
      reshade::log::message(reshade::log::level::info, message);
      for (unsigned i = 0; i < scene.mounts.size(); ++i) {
        const auto& mount = scene.mounts[i];
        std::snprintf(message, sizeof(message), "Taxi Camera mount %u: position=%.3f,%.3f,%.3f pitch=%.3f yaw=%.3f fov=%.3f", i + 1,
                      mount.position_m[0], mount.position_m[1], mount.position_m[2], mount.pitch_degrees, mount.yaw_degrees,
                      mount.fov_radians);
        reshade::log::message(reshade::log::level::info, message);
      }
      std::snprintf(message, sizeof(message),
                    "Taxi Camera PFD draws: selected=%llu attempts=%llu observer_blocked=%llu depth_blocked=%llu pass_blocked=%llu "
                    "color=%u depth=%u mip=%u",
                    static_cast<unsigned long long>(pfd_draws.draws), static_cast<unsigned long long>(pfd_draws.attempted),
                    static_cast<unsigned long long>(pfd_draws.observer_blocked), static_cast<unsigned long long>(pfd_draws.depth_blocked),
                    static_cast<unsigned long long>(pfd_draws.pass_blocked), static_cast<unsigned>(pfd_draws.color),
                    static_cast<unsigned>(pfd_draws.depth), pfd_draws.mip);
      reshade::log::message(reshade::log::level::info, message);
      std::snprintf(
          message, sizeof(message), "Taxi Camera queue-tail: candidates=%llu source_draws=%llu batches=%llu captures=%llu status=%s",
          static_cast<unsigned long long>(feed.capture.source_candidates), static_cast<unsigned long long>(feed.capture.source_draws),
          static_cast<unsigned long long>(feed.capture.tail_submissions), static_cast<unsigned long long>(feed.capture.tail_captures),
          feed.capture.tail_status);
      reshade::log::message(reshade::log::level::info, message);
      const auto boundary = render_boundary::statistics();
      std::snprintf(message, sizeof(message),
                    "Taxi Camera source state: unknown_lists=%llu invalid_recordings=%llu scoped_invalidations=%llu",
                    static_cast<unsigned long long>(feed.capture.unknown_submitted_lists),
                    static_cast<unsigned long long>(feed.capture.invalid_source_recordings),
                    static_cast<unsigned long long>(feed.capture.scoped_source_invalidations));
      reshade::log::message(reshade::log::level::info, message);
      std::snprintf(
          message, sizeof(message),
          "Taxi Camera render boundaries: state=%s active=%d hook_failures=%llu legacy=%llu/%llu enhanced=%llu/%llu "
          "pass_refusals=%llu batch_refusals=%llu captured=%llu refreshed=%llu copy_sources=%llu,%llu copy_destinations=%llu,%llu",
          boundary_hook_status.load(std::memory_order_relaxed), render_boundary::operational() ? 1 : 0,
          static_cast<unsigned long long>(boundary_hook_failures.load(std::memory_order_relaxed)),
          static_cast<unsigned long long>(boundary.legacy_candidates), static_cast<unsigned long long>(boundary.legacy_calls),
          static_cast<unsigned long long>(boundary.enhanced_candidates), static_cast<unsigned long long>(boundary.enhanced_calls),
          static_cast<unsigned long long>(boundary.pass_refusals), static_cast<unsigned long long>(boundary.batch_refusals),
          static_cast<unsigned long long>(feed.capture.render_target_writes),
          static_cast<unsigned long long>(feed.capture.render_target_rewrites),
          static_cast<unsigned long long>(data->scene_copy_sources[0].load(std::memory_order_relaxed)),
          static_cast<unsigned long long>(data->scene_copy_sources[1].load(std::memory_order_relaxed)),
          static_cast<unsigned long long>(data->scene_copy_destinations[0].load(std::memory_order_relaxed)),
          static_cast<unsigned long long>(data->scene_copy_destinations[1].load(std::memory_order_relaxed)));
      reshade::log::message(reshade::log::level::info, message);
      std::snprintf(message, sizeof(message),
                    "Taxi Camera native copies: resource=%llu texture=%llu captured=%llu refreshed=%llu metadata_truncated=%llu",
                    static_cast<unsigned long long>(boundary.copy_resource_calls),
                    static_cast<unsigned long long>(boundary.copy_texture_calls), static_cast<unsigned long long>(feed.capture.copy_writes),
                    static_cast<unsigned long long>(feed.capture.copy_rewrites),
                    static_cast<unsigned long long>(boundary.metadata_truncated_calls));
      reshade::log::message(reshade::log::level::info, message);
      for (unsigned i = 0; i < feed.capture.render_targets.size(); ++i) {
        const auto& target = feed.capture.render_targets[i];
        std::snprintf(message, sizeof(message),
                      "Taxi Camera source %u: matched_boundaries=%llu actual_size=%llux%u format=%u mips=%u refusal=%s", i + 1,
                      static_cast<unsigned long long>(target.matched_boundaries), static_cast<unsigned long long>(target.width),
                      target.height, target.format, target.mips, target.last_refusal);
        reshade::log::message(reshade::log::level::info, message);
        const auto& copy = feed.capture.copies[i];
        std::snprintf(message, sizeof(message),
                      "Taxi Camera copy source %u: matched_copies=%llu actual_size=%llux%u format=%u mips=%u refusal=%s", i + 1,
                      static_cast<unsigned long long>(copy.matched_copies), static_cast<unsigned long long>(copy.width), copy.height,
                      copy.format, copy.mips, copy.last_refusal);
        reshade::log::message(reshade::log::level::info, message);
        const auto& raw = native_source_observations[i];
        std::snprintf(message, sizeof(message),
                      "Taxi Camera base-table source %u: transitions=%llu admission_hint=%llu model=%u before=0x%x after=0x%x "
                      "scope=0x%x flags=0x%x subresource=%u copy_sources=%llu copy_destinations=%llu",
                      i + 1, static_cast<unsigned long long>(raw.transitions.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(raw.open_window.load(std::memory_order_relaxed)),
                      raw.model.load(std::memory_order_relaxed), raw.before.load(std::memory_order_relaxed),
                      raw.after.load(std::memory_order_relaxed), raw.scope.load(std::memory_order_relaxed),
                      raw.flags.load(std::memory_order_relaxed), raw.subresource.load(std::memory_order_relaxed),
                      static_cast<unsigned long long>(raw.copy_sources.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(raw.copy_destinations.load(std::memory_order_relaxed)));
        reshade::log::message(reshade::log::level::info, message);
      }
      const auto handoff = taxi_camera::scene_handoff().diagnostics();
      std::array<Target, 2> published_sources{};
      if (handoff.published) {
        const std::lock_guard lock(data->mutex);
        for (const auto& item : data->targets)
          for (unsigned i = 0; i < published_sources.size(); ++i)
            if (item.second.id == handoff.resource_ids[i])
              published_sources[i] = item.second;
      }
      for (unsigned i = 0; i < published_sources.size(); ++i) {
        const auto& source = published_sources[i];
        std::snprintf(message, sizeof(message),
                      "Taxi Camera published source %u: target=%llu registered=%d size=%ux%u format=%u mips=%u usage=0x%x draws=%llu "
                      "initial_model=%u",
                      i + 1, static_cast<unsigned long long>(handoff.resource_ids[i]), source.id ? 1 : 0,
                      source.id ? source.desc.texture.width : 0, source.id ? source.desc.texture.height : 0,
                      source.id ? static_cast<unsigned>(source.desc.texture.format) : 0, source.id ? source.desc.texture.levels : 0,
                      source.id ? static_cast<unsigned>(source.desc.usage) : 0, static_cast<unsigned long long>(source.draws),
                      static_cast<unsigned>(source.initial_model));
        reshade::log::message(reshade::log::level::info, message);
      }
      std::snprintf(
          message, sizeof(message),
          "Taxi Camera handoff: published=%d publications=%llu unrelated_events=%llu target_invalidations=%llu ticket_invalidations=%llu",
          handoff.published ? 1 : 0, static_cast<unsigned long long>(handoff.publications),
          static_cast<unsigned long long>(handoff.unrelated_events), static_cast<unsigned long long>(handoff.target_invalidations),
          static_cast<unsigned long long>(handoff.ticket_invalidations));
      reshade::log::message(reshade::log::level::info, message);
      const auto& p = scene.performance;
      std::snprintf(message, sizeof(message),
                    "Taxi Camera CPU: manager=%.3f pool=%.3f lifecycle=%.3f entries=%.3f view1=%.3f view2=%.3f handoff=%.3f "
                    "activation=%.3f publication=%.3f queries=%llu/%.3fms reads=%llu/%.3fms bytes=%llu entries=%u buckets=%u "
                    "cache_hits=%llu cache_failures=%llu",
                    p.stage_ms[0], p.stage_ms[1], p.stage_ms[2], p.stage_ms[3], p.stage_ms[4], p.stage_ms[5], p.stage_ms[6], p.stage_ms[7],
                    p.stage_ms[8], static_cast<unsigned long long>(p.query_calls), p.query_ms,
                    static_cast<unsigned long long>(p.read_calls), p.read_ms, static_cast<unsigned long long>(p.requested_bytes),
                    p.entry_count, p.bucket_count, static_cast<unsigned long long>(p.query_cache_hits),
                    static_cast<unsigned long long>(p.query_cache_validation_failures));
      reshade::log::message(reshade::log::level::info, message);
    }
  }
  ++data->frame;
  auto* native = reinterpret_cast<ID3D12Device*>(device->get_native());
  if (FAILED(native->GetDeviceRemovedReason())) {
    std::lock_guard<std::mutex> lock(data->mutex);
    data->device_lost = true;
    disarm(*data);
  }
}

void draw_overlay(api::effect_runtime* runtime) {
  auto* data = private_data<DeviceData>(runtime->get_device());
  ImGui::TextWrapped("Native camera probe 0.7.11. EFIS TAXI control and upper-PFD cameras.");
  if (data == nullptr) {
    ImGui::TextUnformatted("D3D12 is required.");
    return;
  }
  const std::lock_guard control_lock(data->control_mutex);
  if (ImGui::Button("Close overlay"))
    runtime->open_overlay(false, api::input_source::mouse);
  ImGui::TextWrapped(
      "Nose-wheel camera above; tail camera below. Mount position and lens can be adjusted live. "
      "The native update observer remains installed until MSFS exits.");
  if (ImGui::Button("Start native scene test")) {
    {
      const std::lock_guard lock(data->mutex);
      data->enabled = false;
      data->taxi_button_control = false;
      data->taxi_intent.reset();
      data->taxi_intent_state = {};
      data->taxi_active_mask = 0;
      data->taxi_scene_requested = false;
    }
    data->camera_enabled.store(false, std::memory_order_release);
    if (start_camera_scene(runtime->get_device())) {
      reshade::log::message(reshade::log::level::info, "Taxi Camera: scene start requested; output panes 768x255 and 768x504.");
    }
  }
  if (ImGui::Button("Stop native scene test")) {
    {
      const std::lock_guard lock(data->mutex);
      data->taxi_button_control = false;
      data->taxi_intent.reset();
      data->taxi_intent_state = {};
      data->taxi_active_mask = 0;
      data->taxi_scene_requested = false;
      data->taxi_telemetry_started = false;
      data->camera_enabled.store(false, std::memory_order_release);
    }
    stop_camera_scene(runtime->get_device(), false);
    reshade::log::message(reshade::log::level::info, "Taxi Camera: scene stop requested.");
  }
  bool button_control;
  bool auto_detect;
  std::array<std::uint64_t, 2> assigned;
  const char* detection_status;
  {
    const std::lock_guard lock(data->mutex);
    button_control = data->taxi_button_control;
    auto_detect = data->taxi_auto_detect;
    assigned = data->taxi_routes.targets;
    detection_status = data->taxi_detection_status;
  }
  if (ImGui::Checkbox("Follow left / right EFIS TAXI buttons", &button_control)) {
    {
      const std::lock_guard lock(data->mutex);
      data->taxi_button_control = button_control;
      data->taxi_intent.reset();
      data->taxi_intent_state = {};
      data->taxi_scene_requested = false;
      data->taxi_active_mask = 0;
      data->taxi_telemetry_started = false;
      data->camera_enabled.store(false, std::memory_order_release);
      data->activity_epoch.fetch_add(1, std::memory_order_acq_rel);
    }
    stop_camera_scene(runtime->get_device(), button_control);
  }
  if (ImGui::Checkbox("Automatically detect A380 PFD pair", &auto_detect)) {
    const std::lock_guard lock(data->mutex);
    data->taxi_auto_detect = auto_detect;
    if (auto_detect) {
      data->pfd_detector.reset();
      data->next_detection_ms = 0;
    }
  }
  const auto buttons = taxi_camera::native_camera::get_taxi_buttons();
  ImGui::Text("TAXI signals: %s | left %s | right %s", buttons.valid ? "live" : buttons.error,
              buttons.left_on ? "ON" : "off", buttons.right_on ? "ON" : "off");
  ImGui::Text("PFD assignment: left %llu | right %llu", static_cast<unsigned long long>(assigned[0]),
              static_cast<unsigned long long>(assigned[1]));
  ImGui::TextWrapped("%s", detection_status);
  const auto scene = taxi_camera::native_camera::scene_snapshot();
  if (ImGui::CollapsingHeader("Nose and tail mounts")) {
    const auto mount_file = taxi_camera::native_camera::mount_config_status();
    ImGui::Text("Mount file: %s | applied %llu", mount_file.state, static_cast<unsigned long long>(mount_file.applications));
    auto mounts = scene.mounts;
    bool mounts_changed = false;
    for (unsigned i = 0; i < mounts.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      ImGui::TextUnformatted(i == 0 ? "Nose-wheel / upper pane" : "Tail / lower pane");
      auto& mount = mounts[i];
      float position[3]{static_cast<float>(mount.position_m[0]), static_cast<float>(mount.position_m[1]),
                        static_cast<float>(mount.position_m[2])};
      if (ImGui::DragFloat3("Right / up / forward (m)", position, 0.05f, -500.0f, 500.0f, "%.2f")) {
        for (unsigned axis = 0; axis < 3; ++axis)
          mount.position_m[axis] = position[axis];
        mounts_changed = true;
      }
      if (ImGui::Button("Lower 0.25 m")) {
        mount.position_m[1] -= 0.25;
        mounts_changed = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("Raise 0.25 m")) {
        mount.position_m[1] += 0.25;
        mounts_changed = true;
      }
      if (ImGui::Button("Aft 1 m")) {
        mount.position_m[2] -= 1;
        mounts_changed = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("Forward 1 m")) {
        mount.position_m[2] += 1;
        mounts_changed = true;
      }
      float pitch = static_cast<float>(mount.pitch_degrees), yaw = static_cast<float>(mount.yaw_degrees);
      if (ImGui::SliderFloat("Look pitch", &pitch, -89.0f, 89.0f, "%.1f deg")) {
        mount.pitch_degrees = pitch;
        mounts_changed = true;
      }
      if (ImGui::SliderFloat("Look yaw", &yaw, -180.0f, 180.0f, "%.1f deg")) {
        mount.yaw_degrees = yaw;
        mounts_changed = true;
      }
      mounts_changed |= ImGui::SliderFloat("Lens (radians)", &mount.fov_radians, 0.05f, 1.55f, "%.3f");
      ImGui::PopID();
    }
    if (ImGui::Button("Reset nose and tail mounts")) {
      mounts = taxi_camera::native_camera::default_mounts();
      mounts_changed = true;
    }
    if (mounts_changed)
      taxi_camera::native_camera::request_scene_mounts(mounts);
    ImGui::TextWrapped("Positions are metres from the aircraft datum. Positive pitch looks up; positive yaw looks right.");
  }
  int requested_rate = static_cast<int>(scene.requested_rate);
  bool one_feed = scene.requested_feeds == 1;
  bool schedule_changed = ImGui::InputInt("Camera rate limit (15-60 fps per camera)", &requested_rate);
  schedule_changed |= ImGui::Checkbox("Render only the first camera (performance test)", &one_feed);
  if (one_feed)
    ImGui::TextWrapped("Single-camera performance test: the combined PFD feed pauses without new second-camera frames.");
  if (schedule_changed) {
    taxi_camera::scene_runtime::manager().set_source_rate(static_cast<unsigned>(std::clamp(requested_rate, 15, 60)));
    taxi_camera::native_camera::request_scene_rate(static_cast<unsigned>(std::clamp(requested_rate, 15, 60)), one_feed ? 1u : 2u);
  }
  ImGui::TextWrapped("%s", scene.message.c_str());
  const auto feed = taxi_camera::scene_runtime::snapshot(reinterpret_cast<std::uintptr_t>(runtime->get_device()));
  ImGui::Checkbox("Automatic night exposure", &data->automatic_exposure);
  ImGui::SliderFloat(data->automatic_exposure ? "Daytime exposure (EV)" : "Camera exposure (EV)",
                     &data->manual_exposure_ev, -16.0f, 4.0f, "%.1f");
  if (data->automatic_exposure)
    ImGui::SliderFloat("Maximum night boost (EV)", &data->night_exposure_boost_ev, 0.0f, 8.0f, "%.1f");
  ImGui::Text("Applied exposure %.1f EV | %s", feed.display_exposure_ev, data->exposure_state.status);
  ImGui::TextWrapped("Automatic exposure follows ambient light gradually. Disable it for direct manual adjustment. "
                    "Both panes update without restarting the cameras.");
  if (data->body_rate_available)
    ImGui::Text("Aircraft pose: %.1f received updates/s", data->body_arrivals_per_second);
  ImGui::Text("Top output %dx%d | Bottom output %dx%d", scene.dimensions[0][2][0], scene.dimensions[0][2][1], scene.dimensions[1][2][0],
              scene.dimensions[1][2][1]);
  ImGui::Text("Probe CPU %.3f ms | Captured %llu | Composed %llu | PFD stamps %llu", scene.observer_last_ms,
              static_cast<unsigned long long>(feed.capture.captures), static_cast<unsigned long long>(feed.frames),
              static_cast<unsigned long long>(feed.stamps));
  ImGui::TextWrapped("PFD feed: %s", feed.message);
  if (ImGui::CollapsingHeader("Camera diagnostics")) {
    ImGui::Text("Update hook: %s | Updates: %llu | Free views: %u", scene.hook_installed ? "installed" : "inactive",
                static_cast<unsigned long long>(scene.updates), scene.free_views);
    for (unsigned i = 0; i < 2; ++i) {
      ImGui::Text("Scene %u | ID %llu | Ready: %s | Output pointer: %s", i + 1, static_cast<unsigned long long>(scene.pair.owned_ids[i]),
                  scene.ready[i] ? "yes" : "no", scene.resource_present[i] ? "present" : "pending");
      ImGui::Text("  View %dx%d | Output dimensions %dx%d | Gate: %s | Pulses: %llu", scene.dimensions[i][0][0], scene.dimensions[i][0][1],
                  scene.dimensions[i][2][0], scene.dimensions[i][2][1], scene.gates[i] ? "on" : "off",
                  static_cast<unsigned long long>(scene.activation_counts[i]));
      ImGui::Text("  Matched copy events: source %llu | destination %llu",
                  static_cast<unsigned long long>(data->scene_copy_sources[i].load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(data->scene_copy_destinations[i].load(std::memory_order_relaxed)));
    }
    ImGui::Text("Camera outputs matched to live resources: %s", scene.outputs_matched ? "yes" : "pending");
    ImGui::Text("Probe CPU: %.3f ms last / %.3f ms max | Inspections %llu | Created %llu", scene.observer_last_ms, scene.observer_max_ms,
                static_cast<unsigned long long>(scene.inspection_count), static_cast<unsigned long long>(scene.created_total));
    const auto& performance = scene.performance;
    ImGui::Text("Memory queries %llu / %.3f ms | reads %llu / %.3f ms | bytes %llu | entries %u / buckets %u",
                static_cast<unsigned long long>(performance.query_calls), performance.query_ms,
                static_cast<unsigned long long>(performance.read_calls), performance.read_ms,
                static_cast<unsigned long long>(performance.requested_bytes), performance.entry_count, performance.bucket_count);
    ImGui::Text("Query cache hits %llu | failed stage validations %llu", static_cast<unsigned long long>(performance.query_cache_hits),
                static_cast<unsigned long long>(performance.query_cache_validation_failures));
    const auto handoff = taxi_camera::scene_handoff().diagnostics();
    ImGui::Text("Handoff published %s | publications %llu | unrelated lifecycle events %llu", handoff.published ? "yes" : "no",
                static_cast<unsigned long long>(handoff.publications), static_cast<unsigned long long>(handoff.unrelated_events));
    ImGui::Text("Handoff target invalidations %llu | interrupted inspections %llu",
                static_cast<unsigned long long>(handoff.target_invalidations),
                static_cast<unsigned long long>(handoff.ticket_invalidations));
    ImGui::Text("Published camera texture IDs: %llu / %llu", static_cast<unsigned long long>(handoff.resource_ids[0]),
                static_cast<unsigned long long>(handoff.resource_ids[1]));
    ImGui::Checkbox("Show CPU stage timings", &data->show_performance);
    if (data->show_performance)
      for (std::size_t i = 0; i < performance.stage_ms.size(); ++i)
        ImGui::Text("  %s: %.3f ms", taxi_camera::native_camera::kProbeStageNames[i], performance.stage_ms[i]);
    ImGui::TextWrapped(
        "Rate limits count requested activation opportunities, not completed camera frames. Probe CPU excludes engine rendering and GPU "
        "time.");
    ImGui::TextWrapped("An output pointer does not confirm a rendered frame. Stop requests removal on the next manager update.");
    const auto pfd_state = taxi_camera::pfd_adapter::statistics();
    ImGui::TextWrapped("PFD feed: %s", feed.message);
    ImGui::Text("PFD state: %s | observed lists %llu | hook failures %llu", pfd_state.last_error,
                static_cast<unsigned long long>(pfd_state.lists), static_cast<unsigned long long>(pfd_state.native_observer_failures));
    ImGui::Text("Native roots %llu | tables %llu | stamps preserving undefined tables %llu",
                static_cast<unsigned long long>(pfd_state.native_roots), static_cast<unsigned long long>(pfd_state.native_tables),
                static_cast<unsigned long long>(pfd_state.stamps_with_undefined_tables));
    ImGui::Text("Captured %llu | completed %llu | composed %llu | PFD stamps %llu | state skips %llu",
                static_cast<unsigned long long>(feed.capture.captures), static_cast<unsigned long long>(feed.capture.completed),
                static_cast<unsigned long long>(feed.frames), static_cast<unsigned long long>(feed.stamps),
                static_cast<unsigned long long>(feed.state_skips));
    ImGui::Text("Capture skips %llu | quarantined %llu | snapshot memory %.1f MiB", static_cast<unsigned long long>(feed.capture.skipped),
                static_cast<unsigned long long>(feed.capture.quarantined), static_cast<double>(feed.capture.bytes) / (1024.0 * 1024.0));
    ImGui::Text("Source tracking: %llu candidates | %llu draws", static_cast<unsigned long long>(feed.capture.source_candidates),
                static_cast<unsigned long long>(feed.capture.source_draws));
    ImGui::Text("Resource creation observer: %s", data->creation_status.load(std::memory_order_acquire));
    ImGui::Text("Queue-tail captures %llu in %llu batches | %s", static_cast<unsigned long long>(feed.capture.tail_captures),
                static_cast<unsigned long long>(feed.capture.tail_submissions), feed.capture.tail_status);
    ImGui::Text("Rendered-output captures %llu | later-pass refreshes %llu",
                static_cast<unsigned long long>(feed.capture.render_target_writes),
                static_cast<unsigned long long>(feed.capture.render_target_rewrites));
    const auto boundary = render_boundary::statistics();
    ImGui::Text("Render observer: %s / %s | hook failures %llu", boundary_hook_status.load(std::memory_order_relaxed),
                render_boundary::operational() ? "active" : "disabled",
                static_cast<unsigned long long>(boundary_hook_failures.load(std::memory_order_relaxed)));
    ImGui::Text("Render boundaries: legacy %llu / %llu | enhanced %llu / %llu", static_cast<unsigned long long>(boundary.legacy_candidates),
                static_cast<unsigned long long>(boundary.legacy_calls), static_cast<unsigned long long>(boundary.enhanced_candidates),
                static_cast<unsigned long long>(boundary.enhanced_calls));
    ImGui::Text("Native copies: resource %llu | texture %llu | captures %llu | refreshes %llu",
                static_cast<unsigned long long>(boundary.copy_resource_calls), static_cast<unsigned long long>(boundary.copy_texture_calls),
                static_cast<unsigned long long>(feed.capture.copy_writes), static_cast<unsigned long long>(feed.capture.copy_rewrites));
    ImGui::TextWrapped(
        "Base-table observations below are cumulative metadata. Active-pass tables are excluded; a match is not proof of a completed "
        "frame.");
    for (unsigned i = 0; i < feed.capture.render_targets.size(); ++i) {
      const auto& target = feed.capture.render_targets[i];
      ImGui::Text("Source %u: %llu matches | %llux%u fmt %u mips %u | %s", i + 1,
                  static_cast<unsigned long long>(target.matched_boundaries), static_cast<unsigned long long>(target.width), target.height,
                  target.format, target.mips, target.last_refusal);
      const auto& copy = feed.capture.copies[i];
      ImGui::Text("  Copies %llu | %llux%u fmt %u mips %u | %s", static_cast<unsigned long long>(copy.matched_copies),
                  static_cast<unsigned long long>(copy.width), copy.height, copy.format, copy.mips, copy.last_refusal);
      const auto& raw = native_source_observations[i];
      ImGui::Text("  Base-table transitions %llu | admission hint %llu | copies source %llu / destination %llu",
                  static_cast<unsigned long long>(raw.transitions.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(raw.open_window.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(raw.copy_sources.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(raw.copy_destinations.load(std::memory_order_relaxed)));
      ImGui::Text("  Model %u | 0x%x -> 0x%x | scope 0x%x | flags 0x%x | subresource %u", raw.model.load(std::memory_order_relaxed),
                  raw.before.load(std::memory_order_relaxed), raw.after.load(std::memory_order_relaxed),
                  raw.scope.load(std::memory_order_relaxed), raw.flags.load(std::memory_order_relaxed),
                  raw.subresource.load(std::memory_order_relaxed));
    }
  }
  ImGui::TextUnformatted("PFD TARGET AND CAMERA FEED");
  bool track_activity = data->activity_tracking.load(std::memory_order_acquire);
  if (ImGui::Checkbox("Track PFD texture activity", &track_activity))
    set_activity_tracking(*data, track_activity);
  if (!track_activity)
    ImGui::TextWrapped(
        "Activity counters are paused. You can select a PFD and enable its camera below; enable tracking to identify targets by activity.");
  struct Snapshot {
    std::vector<Target> targets;
    std::uint64_t selected_id;
    std::uint64_t writes;
    std::uint64_t skipped;
    bool enabled;
    bool lost;
    bool capacity;
    taxi_camera::WriteBudget budget;
    bool view_creation_failed;
    bool copy_creation_failed;
    int filter_width;
    int filter_height;
    bool used_recently;
  } snapshot;
  {
    std::lock_guard<std::mutex> lock(data->mutex);
    snapshot = {{},
                data->selected_id,
                data->recorded_writes,
                data->skipped_recordings,
                data->enabled,
                data->device_lost,
                data->capacity_reached,
                data->write_budget,
                data->view_creation_failed,
                data->copy_creation_failed,
                data->filter_width,
                data->filter_height,
                data->used_recently};
    snapshot.targets.reserve(data->targets.size());
    for (const auto& entry : data->targets) {
      snapshot.targets.push_back(entry.second);
    }
  }
  bool filter_changed = ImGui::InputInt("Width (0 = any)", &snapshot.filter_width);
  filter_changed |= ImGui::InputInt("Height (0 = any)", &snapshot.filter_height);
  filter_changed |= ImGui::Checkbox("Only active in the last 120 frames", &snapshot.used_recently);
  if (ImGui::Button("Show all texture sizes")) {
    snapshot.filter_width = 0;
    snapshot.filter_height = 0;
    filter_changed = true;
  }
  snapshot.filter_width = std::max(0, std::min(16384, snapshot.filter_width));
  snapshot.filter_height = std::max(0, std::min(16384, snapshot.filter_height));
  if (filter_changed) {
    std::lock_guard<std::mutex> lock(data->mutex);
    data->filter_width = snapshot.filter_width;
    data->filter_height = snapshot.filter_height;
    data->used_recently = snapshot.used_recently;
    disarm(*data);
    snapshot.selected_id = 0;
    snapshot.enabled = false;
  }
  std::sort(snapshot.targets.begin(), snapshot.targets.end(), [](const Target& a, const Target& b) { return a.id < b.id; });
  const auto frame = data->frame.load(std::memory_order_relaxed);
  const auto matches_filter = [&](const Target& target) {
    const bool active = target.draws != 0 || target.copies != 0 || target.copies_out != 0 || target.uploads != 0 || target.resolves != 0 ||
                        target.passes != 0 || target.indirect != 0;
    return (snapshot.filter_width == 0 || target.desc.texture.width == static_cast<std::uint32_t>(snapshot.filter_width)) &&
           (snapshot.filter_height == 0 || target.desc.texture.height == static_cast<std::uint32_t>(snapshot.filter_height)) &&
           (!snapshot.used_recently || (active && frame - target.last_frame <= 120));
  };
  ImGui::TextWrapped("Render targets, shader resources and copied/uploaded textures are listed. IDs stay stable while counters change.");
  if (ImGui::BeginChild("Candidates", ImVec2(0, 210), true)) {
    for (const auto& target : snapshot.targets) {
      if (!matches_filter(target)) {
        continue;
      }
      char label[192];
      std::snprintf(label, sizeof(label), "%llu | %ux%u | fmt %u | draw %llu | copy %llu | upload %llu###candidate-%llu",
                    static_cast<unsigned long long>(target.id), target.desc.texture.width, target.desc.texture.height,
                    static_cast<unsigned int>(target.desc.texture.format), static_cast<unsigned long long>(target.draws),
                    static_cast<unsigned long long>(target.copies), static_cast<unsigned long long>(target.uploads),
                    static_cast<unsigned long long>(target.id));
      if (ImGui::Selectable(label, snapshot.selected_id == target.id)) {
        std::lock_guard<std::mutex> lock(data->mutex);
        if (!data->taxi_button_control)
          disarm(*data);
        for (const auto& live : data->targets) {
          if (live.second.id == target.id && !data->device_lost) {
            data->selected_id = target.id;
            break;
          }
        }
        snapshot.selected_id = data->selected_id;
        snapshot.enabled = false;
      }
    }
  }
  ImGui::EndChild();
  const auto selected = std::find_if(snapshot.targets.begin(), snapshot.targets.end(),
                                     [&](const Target& target) { return target.id == snapshot.selected_id; });
  const bool writable = selected != snapshot.targets.end() && (selected->eligible_draws != 0 || selected->eligible_copies != 0);
  const bool camera_target = selected != snapshot.targets.end() && taxi_camera::calibration_eligible(selected->desc);
  if (selected != snapshot.targets.end() && selected->debug_name[0])
    ImGui::Text("D3D label: %s", selected->debug_name.data());
  ImGui::BeginDisabled(!camera_target || snapshot.lost);
  for (unsigned side = 0; side < 2; ++side) {
    if (side)
      ImGui::SameLine();
    if (ImGui::Button(side == 0 ? "Assign selected to LEFT PFD" : "Assign selected to RIGHT PFD")) {
      const std::lock_guard lock(data->mutex);
      if (data->selected_id == snapshot.selected_id && data->taxi_routes.assign(side, snapshot.selected_id)) {
        data->taxi_auto_detect = false;
        data->taxi_detection_status = "Manual session assignment";
      }
    }
  }
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!camera_target || snapshot.lost || feed.failed);
  bool camera_enabled = data->camera_enabled.load(std::memory_order_acquire);
  if (ImGui::Checkbox("Enable camera on selected PFD", &camera_enabled)) {
    std::lock_guard<std::mutex> lock(data->mutex);
    if (data->selected_id == snapshot.selected_id && data->selected_id && !data->device_lost) {
      data->enabled = false;
      data->taxi_button_control = false;
      data->taxi_intent.reset();
      data->taxi_intent_state = {};
      data->taxi_active_mask = 0;
      data->taxi_scene_requested = false;
      snapshot.enabled = false;
      data->camera_enabled.store(camera_enabled, std::memory_order_release);
      data->pfd_draws = {};
      data->activity_epoch.fetch_add(1, std::memory_order_acq_rel);
      char message[160];
      std::snprintf(message, sizeof(message), "Taxi Camera: selected PFD=%llu camera=%s.",
                    static_cast<unsigned long long>(data->selected_id), camera_enabled ? "enabled" : "disabled");
      reshade::log::message(reshade::log::level::info, message);
    }
  }
  ImGui::EndDisabled();
  if (camera_enabled && !feed.output)
    ImGui::TextUnformatted("Camera armed: waiting for two completed scene images.");
  if (camera_enabled) {
    PfdDrawStatus selected_draws;
    {
      const std::lock_guard lock(data->mutex);
      selected_draws = data->pfd_draws;
    }
    ImGui::Text("PFD draws %llu | camera attempts %llu | depth format %u", static_cast<unsigned long long>(selected_draws.draws),
                static_cast<unsigned long long>(selected_draws.attempted), static_cast<unsigned>(selected_draws.depth));
    if (selected_draws.observer_blocked || selected_draws.depth_blocked || selected_draws.pass_blocked)
      ImGui::Text("Blocked: observer %llu | unknown depth %llu | native pass %llu",
                  static_cast<unsigned long long>(selected_draws.observer_blocked),
                  static_cast<unsigned long long>(selected_draws.depth_blocked),
                  static_cast<unsigned long long>(selected_draws.pass_blocked));
  }
  ImGui::TextUnformatted("PFD CALIBRATION (optional target identification)");
  ImGui::BeginDisabled(!writable || snapshot.lost || !track_activity);
  bool enabled = snapshot.enabled;
  if (ImGui::Checkbox("Enable calibration", &enabled)) {
    std::lock_guard<std::mutex> lock(data->mutex);
    if (data->selected_id == snapshot.selected_id && data->selected_id != 0 && !data->device_lost) {
      data->enabled = enabled;
      if (enabled) {
        data->taxi_button_control = false;
        data->taxi_intent.reset();
        data->taxi_intent_state = {};
        data->taxi_active_mask = 0;
        data->taxi_scene_requested = false;
        data->camera_enabled.store(false, std::memory_order_release);
        data->write_budget.reset_session(data->frame.load(std::memory_order_relaxed), GetTickCount64());
        data->throttle_logged = false;
      }
      data->view_creation_failed = false;
      data->copy_creation_failed = false;
      snapshot.enabled = enabled;
      snapshot.budget = data->write_budget;
      char message[192];
      std::snprintf(message, sizeof(message), "Taxi Camera: candidate=%llu calibration=%s present=%llu quota=%u fallback_ms=50.",
                    static_cast<unsigned long long>(data->selected_id), enabled ? "enabled" : "disabled",
                    static_cast<unsigned long long>(data->frame.load(std::memory_order_relaxed)), data->write_budget.limit());
      reshade::log::message(reshade::log::level::info, message);
    }
  }
  ImGui::EndDisabled();
  int write_limit = static_cast<int>(snapshot.budget.limit());
  if (ImGui::InputInt("Calibration batches per window", &write_limit)) {
    std::lock_guard<std::mutex> lock(data->mutex);
    data->write_budget.set_limit(static_cast<std::uint32_t>(std::max(0, write_limit)));
    snapshot.budget = data->write_budget;
  }
  if (snapshot.selected_id != 0) {
    ImGui::Text("Selected candidate: %llu", static_cast<unsigned long long>(snapshot.selected_id));
  } else {
    ImGui::TextUnformatted("Select a candidate above to enable its camera feed.");
  }
  if (selected != snapshot.targets.end()) {
    ImGui::Text("Mips %u | layers %u | samples %u | RTVs %u | SRV registrations %llu", selected->desc.texture.levels,
                selected->desc.texture.depth_or_layers, selected->desc.texture.samples, selected->views,
                static_cast<unsigned long long>(selected->srv_registrations));
    ImGui::Text("Native passes %llu | copies out %llu | last source %llu", static_cast<unsigned long long>(selected->passes),
                static_cast<unsigned long long>(selected->copies_out), static_cast<unsigned long long>(selected->last_source_id));
    ImGui::Text("Indirect operations with this target bound: %llu", static_cast<unsigned long long>(selected->indirect));
    ImGui::Text("Writable draw events %llu | writable copy events %llu", static_cast<unsigned long long>(selected->eligible_draws),
                static_cast<unsigned long long>(selected->eligible_copies));
    if (!writable) {
      ImGui::TextWrapped("Observation only: no supported write event has been seen for this texture. Its metadata can be logged below.");
      if (const auto reason = taxi_camera::calibration_block_reason(selected->desc)) {
        ImGui::Text("RTV path: %s", reason);
      }
    }
  }
  if (ImGui::Button("Stop and deselect")) {
    std::lock_guard<std::mutex> lock(data->mutex);
    disarm(*data);
    snapshot.enabled = false;
  }
  if (ImGui::Button("Log filtered texture inventory")) {
    reshade::log::message(reshade::log::level::info, "Taxi Camera inventory BEGIN (up to 512 filtered textures; metadata only).");
    std::size_t logged = 0;
    for (const auto& target : snapshot.targets) {
      if (!matches_filter(target)) {
        continue;
      }
      if (logged++ == 512) {
        reshade::log::message(reshade::log::level::info, "Taxi Camera inventory truncated at 512 rows; narrow size filters.");
        break;
      }
      char message[640];
      std::snprintf(message, sizeof(message),
                    "Taxi Camera target=%llu size=%ux%u format=%u mips=%u layers=%u samples=%u usage=0x%x flags=0x%x rtvs=%u srvs=%llu "
                    "draws=%llu drawable=%llu copies=%llu copyable=%llu uploads=%llu resolves=%llu passes=%llu indirect=%llu outgoing=%llu "
                    "source=%llu selected=%d",
                    static_cast<unsigned long long>(target.id), target.desc.texture.width, target.desc.texture.height,
                    static_cast<unsigned int>(target.desc.texture.format), target.desc.texture.levels, target.desc.texture.depth_or_layers,
                    target.desc.texture.samples, static_cast<unsigned int>(target.desc.usage), static_cast<unsigned int>(target.desc.flags),
                    target.views, static_cast<unsigned long long>(target.srv_registrations), static_cast<unsigned long long>(target.draws),
                    static_cast<unsigned long long>(target.eligible_draws), static_cast<unsigned long long>(target.copies),
                    static_cast<unsigned long long>(target.eligible_copies), static_cast<unsigned long long>(target.uploads),
                    static_cast<unsigned long long>(target.resolves), static_cast<unsigned long long>(target.passes),
                    static_cast<unsigned long long>(target.indirect), static_cast<unsigned long long>(target.copies_out),
                    static_cast<unsigned long long>(target.last_source_id), target.id == snapshot.selected_id ? 1 : 0);
      reshade::log::message(reshade::log::level::info, message);
    }
    reshade::log::message(reshade::log::level::info, "Taxi Camera inventory END.");
  }
  ImGui::Text("Recorded calibration writes: %llu | unsupported operations: %llu", static_cast<unsigned long long>(snapshot.writes),
              static_cast<unsigned long long>(snapshot.skipped));
  ImGui::Text("Present callbacks: %llu | window usage: %u/%u", static_cast<unsigned long long>(frame), snapshot.budget.window_accepted,
              snapshot.budget.limit());
  ImGui::Text("Session quota skips: %llu | peak batches/window: %u | timed resets: %llu",
              static_cast<unsigned long long>(snapshot.budget.total_skipped), snapshot.budget.peak_accepted,
              static_cast<unsigned long long>(snapshot.budget.timed_window_resets));
  ImGui::TextWrapped(
      "Each batch records five rectangles. Allowance renews on a Present callback or after 50 ms without an advance. "
      "The adjustable limit is 64-16384 batches; it is not a camera performance measurement.");
  ImGui::TextWrapped(
      "Full width, upper 763/1024. The trim area below is preserved. Observation is broader than write support. "
      "A texture under another HTML layer will still appear behind its symbology; it is not the final PFD surface.");
  ImGui::TextWrapped(
      "Calibration follows supported direct draws or texture copies. Upload-only, native-pass and other unsupported paths remain "
      "observable.");
  ImGui::TextWrapped(
      "Stop affects future recording. Recorded command lists retain calibration until the application records them again; "
      "the application must redraw the texture to replace the overlay. Camera frames remain on the GPU; no CPU frame readback is used.");
  if (snapshot.enabled && snapshot.budget.total_skipped != 0) {
    ImGui::TextWrapped(
        "Calibration remains enabled. Some writes were quota limited; recording resumes automatically next window. "
        "Normal PFD graphics may show through while writes are skipped.");
  }
  if (snapshot.capacity) {
    ImGui::TextUnformatted("Tracking capacity reached; candidate list may be incomplete.");
  }
  if (snapshot.view_creation_failed) {
    ImGui::TextUnformatted("Writes stopped: creating an owned render-target view failed.");
  }
  if (snapshot.copy_creation_failed) {
    ImGui::TextUnformatted("Writes stopped: immutable copy-calibration allocation failed or exceeded its memory limit.");
  }
  if (snapshot.lost) {
    ImGui::TextUnformatted("Device lost: selection and calibration disabled.");
  }
}

}  // namespace

extern "C" __declspec(dllexport) const char* NAME = "Taxi Camera Native Probe";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Native scene capture, two-camera GPU composition and selected upper-PFD live feed.";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    addon_module = module;
    if (!reshade::register_addon(module)) {
      return FALSE;
    }
    reshade::register_event<reshade::addon_event::init_device>(on_init_device);
    reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::register_event<reshade::addon_event::init_command_queue>(on_init_command_queue);
    reshade::register_event<reshade::addon_event::destroy_command_queue>(on_destroy_command_queue);
    reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
    reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
    reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
    reshade::register_event<reshade::addon_event::close_command_list>(on_close_command_list);
    reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
    reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
    reshade::register_event<reshade::addon_event::init_resource_view>(on_init_resource_view);
    reshade::register_event<reshade::addon_event::destroy_resource_view>(on_destroy_resource_view);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
    reshade::register_event<reshade::addon_event::barrier>(on_barrier);
    reshade::register_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass);
    reshade::register_event<reshade::addon_event::end_render_pass>(on_end_render_pass);
    reshade::register_event<reshade::addon_event::copy_resource>(on_copy_resource);
    reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_texture);
    reshade::register_event<reshade::addon_event::copy_buffer_to_texture>(on_upload);
    reshade::register_event<reshade::addon_event::resolve_texture_region>(on_resolve);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
    reshade::register_event<reshade::addon_event::execute_secondary_command_list>(on_execute_secondary);
    reshade::register_event<reshade::addon_event::draw>(on_draw);
    reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
    reshade::register_event<reshade::addon_event::present>(on_present);
    taxi_camera::pfd_adapter::register_events();
    reshade::register_overlay("Taxi Camera Native Probe", draw_overlay);
    reshade::log::message(reshade::log::level::info,
                          "Taxi Camera Native Probe 0.7.11: default 15 Hz per camera, maximum 60 Hz; "
                          "EFIS TAXI recovery, automatic A380 PFD detection, ambient-light exposure with -8.8 EV day baseline.");
  } else if (reason == DLL_PROCESS_DETACH) {
    reshade::unregister_addon(module);
  }
  return TRUE;
}
