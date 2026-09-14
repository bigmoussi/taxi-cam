#include "pfd_state_adapter.hpp"
#include <array>
#include <mutex>
#include <reshade.hpp>
#include <unordered_map>
#include "../engine-hook/pfd_state_observer.hpp"
#include "../engine-hook/render_boundary_observer.hpp"

namespace taxi_camera::pfd_adapter {
namespace {
namespace api = reshade::api;
namespace native_observer = engine_hook::pfd_state;
struct Key {
  std::uint64_t device = 0, handle = 0;
  bool operator==(const Key&) const = default;
};
struct KeyHash {
  std::size_t operator()(const Key& k) const noexcept { return std::hash<std::uint64_t>{}(k.handle ^ (k.device * 0x9e3779b97f4a7c15ull)); }
};
struct Layout {
  PfdRootLayout metadata;
  std::uint64_t generation = 0;
};
struct List {
  PfdGraphicsState graphics;
  ID3D12GraphicsCommandList* native = nullptr;
  ID3D12Device* device_native = nullptr;
  std::uint64_t device = 0, object_generation = 0, recording = 1, pipeline = 0, pipeline_generation = 0;
};
struct NativeList {
  ID3D12GraphicsCommandList* native = nullptr;
  std::uint64_t device = 0, object_generation = 0;
  bool active = false, observer_ready = false, native_reset_ready = false;
};
struct Registry {
  std::mutex mutex;
  std::once_flag registrations;
  SceneCaptureManager* manager = nullptr;
  std::unordered_map<api::command_list*, List> lists;
  std::unordered_map<ID3D12GraphicsCommandList*, api::command_list*> native_lists;
  std::unordered_map<Key, Layout, KeyHash> layouts;
  std::unordered_map<Key, std::uint64_t, KeyHash> pipelines;
  std::uint64_t next_generation = 0;
  Statistics stats;
  // Registry is intentionally never destroyed. Each occupied slot retains its
  // native COM object even after tracking stops, excluding pointer reuse.
  std::array<NativeList, 128> retained_native_lists{};
};
Registry& registry() {
  static auto* value = new Registry;
  return *value;
}
std::uint64_t next_generation(Registry& r) noexcept {
  return r.next_generation == UINT64_MAX ? 0 : ++r.next_generation;
}
List* find(Registry& r, api::command_list* cmd) noexcept {
  const auto it = r.lists.find(cmd);
  return it == r.lists.end() ? nullptr : &it->second;
}
List* find_native(Registry& r, ID3D12GraphicsCommandList* cmd, std::uint64_t generation) noexcept {
  const auto it = r.native_lists.find(cmd);
  if (it == r.native_lists.end())
    return nullptr;
  auto* result = find(r, it->second);
  return result && result->object_generation == generation ? result : nullptr;
}
NativeList* find_native_only(Registry& r, ID3D12GraphicsCommandList* cmd) noexcept {
  for (auto& item : r.retained_native_lists)
    if (item.native == cmd && cmd)
      return &item;
  return nullptr;
}
bool graphics(api::shader_stage stages) noexcept {
  return (static_cast<UINT>(stages) & static_cast<UINT>(api::shader_stage::all_graphics)) != 0;
}
bool bind_layout(Registry& r, List& list, std::uint64_t handle, bool native_change = false) noexcept {
  const auto it = r.layouts.find({list.device, handle});
  if (it == r.layouts.end()) {
    list.graphics.bind_root(nullptr, 0, {});
    return false;
  }
  list.graphics.bind_root(reinterpret_cast<ID3D12RootSignature*>(handle), it->second.generation, it->second.metadata, native_change);
  return it->second.metadata.valid;
}
void pipeline(Registry& r, List& list, std::uint64_t handle) noexcept {
  list.pipeline = handle;
  const auto found = r.pipelines.find({list.device, handle});
  list.pipeline_generation = found == r.pipelines.end() ? 0 : found->second;
  list.graphics.bind_pipeline(list.pipeline_generation ? reinterpret_cast<ID3D12PipelineState*>(handle) : nullptr);
}
void heaps_changed(void*,
                   ID3D12GraphicsCommandList* cmd,
                   std::uint64_t generation,
                   UINT count,
                   ID3D12DescriptorHeap* const* heaps) noexcept {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  if (auto* list = find_native(r, cmd, generation))
    list->graphics.descriptor_heaps(count, heaps);
}
void graphics_cbv(void*, ID3D12GraphicsCommandList* cmd, std::uint64_t generation, UINT index, UINT64 address) noexcept {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  if (auto* list = find_native(r, cmd, generation))
    list->graphics.descriptor(index, PfdRootKind::cbv, address);
}
void graphics_root(void*, ID3D12GraphicsCommandList* cmd, std::uint64_t generation, ID3D12RootSignature* root) noexcept {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  if (auto* list = find_native(r, cmd, generation)) {
    ++r.stats.native_roots;
    bind_layout(r, *list, reinterpret_cast<std::uint64_t>(root), true);
  }
}
void graphics_table(void*,
                    ID3D12GraphicsCommandList* cmd,
                    std::uint64_t generation,
                    UINT index,
                    D3D12_GPU_DESCRIPTOR_HANDLE handle) noexcept {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  if (auto* list = find_native(r, cmd, generation)) {
    ++r.stats.native_tables;
    list->graphics.table(index, handle.ptr);
  }
}
void reset_completed(void*,
                     ID3D12GraphicsCommandList* cmd,
                     std::uint64_t generation,
                     HRESULT status,
                     ID3D12PipelineState* initial) noexcept {
  // This callback follows the actual native Reset. A newly discovered native
  // list remains pending until both observers have been installed and armed.
  auto& r = registry();
  SceneCaptureManager* manager = nullptr;
  bool observe_boundary = false;
  bool native_only = false;
  {
    const std::lock_guard lock(r.mutex);
    auto* list = find_native(r, cmd, generation);
    if (list) {
      observe_boundary = true;
      if (status != S_OK || list->recording == UINT64_MAX) {
        list->graphics.invalidate();
      } else {
        list->graphics.reset(++list->recording, true);
        pipeline(r, *list, reinterpret_cast<std::uint64_t>(initial));
        manager = r.manager;
      }
    } else if (auto* native = find_native_only(r, cmd); native && native->active && native->object_generation == generation &&
                                                        native->observer_ready && native->native_reset_ready) {
      observe_boundary = true;
      native_only = true;
      if (status == S_OK)
        manager = r.manager;
    }
  }
  if (!observe_boundary)
    return;
  if (native_only && (!native_observer::operational() || !engine_hook::render_boundary::operational()))
    return;
  // Keep boundary bookkeeping outside the adapter lock. Neither a public reset
  // notification nor a Reset observed before arming clears an unknown record.
  if (status == S_OK)
    engine_hook::render_boundary::successful_reset(cmd, generation);
  else
    engine_hook::render_boundary::reset_failed(cmd, generation);
  // No adapter lock while retiring capture membership. Generation is rechecked
  // by the manager; an intervening destruction/reuse cannot retire a new list.
  if (manager)
    manager->successful_reset(cmd, generation);
}
const native_observer::Callbacks NativeCallbacks{nullptr, heaps_changed, graphics_cbv, reset_completed, graphics_root, graphics_table};

void on_init_layout(api::device* device, UINT count, const api::pipeline_layout_param* params, api::pipeline_layout handle) {
  if (device->get_api() != api::device_api::d3d12 || !handle.handle)
    return;
  const auto metadata = parse_pfd_root_layout(count, params);
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  const Key key{reinterpret_cast<std::uint64_t>(device), handle.handle};
  r.layouts.erase(key);
  const auto generation = next_generation(r);
  if (!generation || r.layouts.size() >= 16384)
    return;
  try {
    r.layouts.emplace(key, Layout{metadata, generation});
  } catch (...) {
    r.stats.last_error = "layout_capacity";
  }
}
void on_destroy_layout(api::device* device, api::pipeline_layout handle) {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  r.layouts.erase({reinterpret_cast<std::uint64_t>(device), handle.handle});
}
void on_init_pipeline(api::device* device, api::pipeline_layout, UINT, const api::pipeline_subobject*, api::pipeline handle) {
  if (device->get_api() != api::device_api::d3d12 || !handle.handle)
    return;
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  const Key key{reinterpret_cast<std::uint64_t>(device), handle.handle};
  r.pipelines.erase(key);
  const auto generation = next_generation(r);
  if (!generation || r.pipelines.size() >= 32768)
    return;
  try {
    r.pipelines.emplace(key, generation);
  } catch (...) {
    r.stats.last_error = "pipeline_capacity";
  }
}
void on_destroy_pipeline(api::device* device, api::pipeline handle) {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  r.pipelines.erase({reinterpret_cast<std::uint64_t>(device), handle.handle});
}
void on_bind_pipeline(api::command_list* cmd, api::pipeline_stage stages, api::pipeline handle) {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  auto* list = find(r, cmd);
  if (!list)
    return;
  // D3D12 SetPipelineState emits all. SetPipelineState1 emits all_ray_tracing and
  // carries another native interface type, so never restore it as a graphics PSO.
  if (stages != api::pipeline_stage::all) {
    list->graphics.invalidate("unsupported_pipeline_stage");
    return;
  }
  pipeline(r, *list, handle.handle);
}
void on_bind_states(api::command_list* cmd, UINT count, const api::dynamic_state* states, const UINT* values) {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  auto* list = find(r, cmd);
  if (!list)
    return;
  if (count > 128 || (count && (!states || !values))) {
    list->graphics.invalidate();
    return;
  }
  for (UINT n = 0; n < count; ++n)
    if (states[n] == api::dynamic_state::primitive_topology) {
      const auto v = values[n];
      if (!(v <= 5 || (v >= 10 && v <= 13) || (v >= 33 && v <= 64))) {
        list->graphics.invalidate();
        return;
      }
      list->graphics.topology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(v));
    }
}
void on_viewports(api::command_list* cmd, UINT first, UINT count, const api::viewport* values) {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  auto* list = find(r, cmd);
  if (!list)
    return;
  static_assert(sizeof(api::viewport) == sizeof(D3D12_VIEWPORT));
  // Copy avoids type-aliasing assumptions in the add-on compiler; the pinned
  // ReShade event itself forwards precisely these six native float fields.
  std::array<D3D12_VIEWPORT, 16> native{};
  if (count <= 16 && count && values)
    std::memcpy(native.data(), values, count * sizeof(D3D12_VIEWPORT));
  list->graphics.viewports(first, count, values ? native.data() : nullptr);
}
void on_scissors(api::command_list* cmd, UINT first, UINT count, const api::rect* values) {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  auto* list = find(r, cmd);
  if (!list)
    return;
  static_assert(sizeof(api::rect) == sizeof(D3D12_RECT));
  std::array<D3D12_RECT, 16> native{};
  if (count <= 16 && count && values)
    std::memcpy(native.data(), values, count * sizeof(D3D12_RECT));
  list->graphics.scissors(first, count, values ? native.data() : nullptr);
}
void on_constants(api::command_list* cmd,
                  api::shader_stage stages,
                  api::pipeline_layout layout,
                  UINT index,
                  UINT first,
                  UINT count,
                  const void* data) {
  if (!graphics(stages))
    return;
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  auto* list = find(r, cmd);
  if (!list)
    return;
  if (bind_layout(r, *list, layout.handle))
    list->graphics.constants(index, first, count, data);
}
void on_tables(api::command_list* cmd,
               api::shader_stage stages,
               api::pipeline_layout layout,
               UINT first,
               UINT count,
               const api::descriptor_table* tables,
               UINT dynamic_count,
               const UINT*) {
  if (!graphics(stages))
    return;
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  auto* list = find(r, cmd);
  if (!list)
    return;
  if (!bind_layout(r, *list, layout.handle))
    return;
  if (dynamic_count || first > 64 || count > 64 - first || (count && !tables)) {
    list->graphics.invalidate();
    return;
  }
  for (UINT n = 0; n < count; ++n)
    list->graphics.table(first + n, tables[n].handle);
}
void on_descriptors(api::command_list* cmd,
                    api::shader_stage stages,
                    api::pipeline_layout layout,
                    UINT index,
                    const api::descriptor_table_update& update) {
  if (!graphics(stages) || update.type == api::descriptor_type::constant_buffer)
    return;
  // CBV native observation captures raw GPUVA even when ReShade cannot resolve
  // it to a resource. Public CBV callbacks are not authoritative or required.
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  auto* list = find(r, cmd);
  if (!list)
    return;
  if (!bind_layout(r, *list, layout.handle))
    return;
  if (update.count != 1 || update.binding != 0 || update.array_offset != 0 || !update.descriptors) {
    list->graphics.invalidate("root_descriptor_update_bounds");
    return;
  }
  PfdRootKind kind;
  if (update.type == api::descriptor_type::buffer_shader_resource_view)
    kind = PfdRootKind::srv;
  else if (update.type == api::descriptor_type::buffer_unordered_access_view)
    kind = PfdRootKind::uav;
  else {
    list->graphics.invalidate();
    return;
  }
  UINT64 address = 0;
  std::memcpy(&address, update.descriptors, 8);
  list->graphics.descriptor(index, kind, address);
}
void invalidate(api::command_list* cmd) {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  if (auto* list = find(r, cmd))
    list->graphics.invalidate();
}
void on_bundle(api::command_list* cmd, api::command_list*) {
  invalidate(cmd);
}
bool on_indirect(api::command_list* cmd, api::indirect_command, api::resource, UINT64, UINT, UINT) {
  invalidate(cmd);
  return false;
}
void on_destroy_device(api::device* device) {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  const auto key = reinterpret_cast<std::uint64_t>(device);
  for (auto it = r.layouts.begin(); it != r.layouts.end();)
    if (it->first.device == key)
      it = r.layouts.erase(it);
    else
      ++it;
  for (auto it = r.pipelines.begin(); it != r.pipelines.end();)
    if (it->first.device == key)
      it = r.pipelines.erase(it);
    else
      ++it;
  // Normally all lists have been destroyed first. Do not claim their lifetime
  // from a device event; invalidate any retained observation pending retirement.
  for (auto& item : r.lists)
    if (item.second.device == key)
      item.second.graphics.invalidate();
  for (auto& item : r.retained_native_lists)
    if (item.device == key) {
      item.active = false;
      item.observer_ready = false;
      item.native_reset_ready = false;
    }
}
}  // namespace

bool initialize(SceneCaptureManager& manager) noexcept {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  if (r.manager && r.manager != &manager)
    return false;
  r.manager = &manager;
  r.stats.last_error = "ready";
  return true;
}
void register_events() {
  auto& r = registry();
  std::call_once(r.registrations, [] {
    reshade::register_event<reshade::addon_event::init_pipeline_layout>(on_init_layout);
    reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(on_destroy_layout);
    reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline_states>(on_bind_states);
    reshade::register_event<reshade::addon_event::bind_viewports>(on_viewports);
    reshade::register_event<reshade::addon_event::bind_scissor_rects>(on_scissors);
    reshade::register_event<reshade::addon_event::push_constants>(on_constants);
    reshade::register_event<reshade::addon_event::push_descriptors>(on_descriptors);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(on_tables);
    reshade::register_event<reshade::addon_event::execute_secondary_command_list>(on_bundle);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
    // CreateCommandList emits a synthetic public reset event after init, even
    // though no native Reset occurred. Only the native afterReset observation
    // changes our recording state; same-list API calls must be serialized.
    reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
  });
}
bool init_list(api::command_list* cmd, std::uint64_t device_key, std::uint64_t generation) noexcept {
  if (!cmd || !generation || !device_key || cmd->get_device()->get_api() != api::device_api::d3d12 ||
      reinterpret_cast<std::uint64_t>(cmd->get_device()) != device_key)
    return false;
  auto* native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
  if (!native || native->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return false;
  auto& r = registry();
  {
    const std::lock_guard lock(r.mutex);
    if (!r.manager || r.lists.size() >= SceneCaptureManager::MaximumLists || r.lists.contains(cmd) || r.native_lists.contains(native) ||
        find_native_only(r, native))
      return false;
    List list;
    list.native = native;
    list.device = device_key;
    list.object_generation = generation;
    list.device_native = reinterpret_cast<ID3D12Device*>(cmd->get_device()->get_native());
    list.graphics.reset(1, false);
    try {
      r.lists.emplace(cmd, list);
      r.native_lists.emplace(native, cmd);
    } catch (...) {
      r.lists.erase(cmd);
      r.native_lists.erase(native);
      return false;
    }
  }
  const auto installed = native_observer::register_list(native, generation, NativeCallbacks);
  const std::lock_guard lock(r.mutex);
  auto* list = find(r, cmd);
  if (!list || list->object_generation != generation)
    return false;
  if (!installed.ready || !installed.protection_restored) {
    r.stats.last_error = installed.status;
    ++r.stats.native_observer_failures;
    list->graphics.invalidate();
    return false;
  }
  list->graphics.reset(1, true);
  r.stats.lists = r.lists.size();
  return true;
}
void destroy_list(api::command_list* cmd, std::uint64_t generation) noexcept {
  auto& r = registry();
  ID3D12GraphicsCommandList* native = nullptr;
  {
    const std::lock_guard lock(r.mutex);
    auto* list = find(r, cmd);
    if (!list || list->object_generation != generation)
      return;
    native = list->native;
    r.native_lists.erase(native);
    r.lists.erase(cmd);
    r.stats.lists = r.lists.size();
  }
  native_observer::unregister_list(native, generation);
}
bool init_native_list(ID3D12GraphicsCommandList* native, std::uint64_t device_key, std::uint64_t generation) noexcept {
  if (!native || !device_key || !generation || native->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return false;
  auto& r = registry();
  {
    const std::lock_guard lock(r.mutex);
    if (!r.manager || r.native_lists.contains(native))
      return false;
    auto* item = find_native_only(r, native);
    if (item) {
      if (item->device != device_key || item->object_generation != generation)
        return false;
      if (item->active && item->observer_ready)
        return true;
    } else {
      for (auto& candidate : r.retained_native_lists)
        if (!candidate.native) {
          item = &candidate;
          break;
        }
      if (!item) {
        r.stats.last_error = "native_only_capacity";
        return false;
      }
      // The caller's live submission argument owns the object during this call.
      // Keep this extra reference for the bounded process-lifetime registry.
      native->AddRef();
      *item = {native, device_key, generation};
    }
    item->active = true;
    item->observer_ready = false;
    item->native_reset_ready = false;
  }
  const auto installed = native_observer::register_list(native, generation, NativeCallbacks);
  bool ready = false;
  {
    const std::lock_guard lock(r.mutex);
    auto* item = find_native_only(r, native);
    if (item && item->active && item->object_generation == generation) {
      ready = installed.ready && installed.protection_restored;
      item->observer_ready = ready;
      if (!ready) {
        item->active = false;
        r.stats.last_error = installed.status;
        ++r.stats.native_observer_failures;
      }
    }
  }
  if (!ready)
    native_observer::unregister_list(native, generation);
  return ready;
}
bool arm_native_list(ID3D12GraphicsCommandList* native, std::uint64_t generation) noexcept {
  if (!native_observer::operational() || !engine_hook::render_boundary::operational())
    return false;
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  auto* item = find_native_only(r, native);
  if (!item || !item->active || !item->observer_ready || item->object_generation != generation)
    return false;
  item->native_reset_ready = true;
  return true;
}
void destroy_native_list(ID3D12GraphicsCommandList* native, std::uint64_t generation) noexcept {
  auto& r = registry();
  {
    const std::lock_guard lock(r.mutex);
    auto* item = find_native_only(r, native);
    if (!item || item->object_generation != generation)
      return;
    item->active = false;
    item->observer_ready = false;
    item->native_reset_ready = false;
  }
  native_observer::unregister_list(native, generation);
}
bool record_stamp(api::command_list* cmd,
                  PfdStampD3D12& stamp,
                  ID3D12Device* device,
                  D3D12_GPU_VIRTUAL_ADDRESS address,
                  UINT width,
                  UINT height) noexcept {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  auto* list = find(r, cmd);
  const char* reason = nullptr;
  if (!native_observer::operational())
    reason = "native_observation_disabled";
  else if (!list || list->device_native != device)
    reason = "untracked_list_or_device";
  else {
    const auto pso = r.pipelines.find({list->device, list->pipeline});
    const auto layout = r.layouts.find({list->device, reinterpret_cast<std::uint64_t>(list->graphics.root())});
    if (pso == r.pipelines.end() || pso->second != list->pipeline_generation)
      reason = "pipeline_generation";
    else if (layout == r.layouts.end() || layout->second.generation != list->graphics.layout_generation())
      reason = "root_generation";
    else if (!list->graphics.complete())
      reason = list->graphics.incomplete_reason();
    else if (!stamp.record_buffer(list->native, list->graphics, device, address, width, height))
      reason = "stamp_input_refused";
  }
  if (reason) {
    ++r.stats.refused;
    r.stats.last_error = reason;
    return false;
  }
  ++r.stats.stamped;
  if (list->graphics.undefined_table_count())
    ++r.stats.stamps_with_undefined_tables;
  r.stats.last_error = "ready";
  return true;
}
Statistics statistics() noexcept {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  return r.stats;
}
}  // namespace taxi_camera::pfd_adapter
