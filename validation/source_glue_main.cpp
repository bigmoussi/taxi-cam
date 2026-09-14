// Compile the production add-on callbacks in this isolated executable. Reuse
// only GPU fixture utilities; its separate test entry point is never called.
#include "../src/taxi_camera_addon.cpp"
#define main unused_manager_fixture_entry
#include "../src/scene_capture_manager_test.cpp"
#undef main

namespace {
api::device* observed_device = nullptr;
api::command_list* observed_list = nullptr;
api::command_queue* observed_queue = nullptr;
unsigned observed_init_events = 0, observed_reset_events = 0;
void remember_device(api::device* device) {
  observed_device = device;
}
void remember_list(api::command_list* list) {
  observed_list = list;
  ++observed_init_events;
}
void remember_reset(api::command_list*) {
  ++observed_reset_events;
}
void remember_queue(api::command_queue* queue) {
  observed_queue = queue;
}

enum class SubmissionScenario { baseline, immediate, unrelated_bundle, unknown_native, native_reset };
const char* scenario_name(SubmissionScenario scenario) {
  switch (scenario) {
    case SubmissionScenario::immediate:
      return "immediate";
    case SubmissionScenario::unrelated_bundle:
      return "unrelated-bundle";
    case SubmissionScenario::unknown_native:
      return "unknown-native";
    case SubmissionScenario::native_reset:
      return "native-reset";
    default:
      return "baseline";
  }
}

void verify_disarm_routes() {
  DeviceData data;
  const auto prepare = [&] {
    data.taxi_button_control = true;
    data.device_lost = false;
    data.taxi_routes.targets = {101, 202};
    data.taxi_active_mask = 3;
    data.camera_enabled.store(true);
    data.selected_id = 999;
    data.enabled = true;
  };
  prepare();
  disarm(data);
  require(data.camera_enabled.load() && data.selected_id == 0 && !data.enabled,
          "Inventory filtering clears calibration but preserves armed automatic routes");
  prepare();
  data.taxi_routes.forget(999);
  disarm(data);
  require(data.camera_enabled.load(), "Destroying an unrelated selected resource preserves automatic routes");
  prepare();
  data.taxi_routes.forget(101);
  disarm(data);
  require(data.camera_enabled.load(), "Destroying one active side preserves the other live side");
  data.taxi_routes.forget(202);
  disarm(data);
  require(!data.camera_enabled.load(), "Destroying both active routes disarms the camera");
  prepare();
  data.taxi_active_mask = 1;
  data.taxi_routes.forget(101);
  disarm(data);
  require(!data.camera_enabled.load(), "A retained but inactive side cannot keep the camera armed");
  prepare();
  data.camera_enabled.store(false);
  disarm(data);
  require(!data.camera_enabled.load(), "Inventory changes cannot re-enable a refused native startup");
  prepare();
  data.taxi_button_control = false;
  disarm(data);
  require(!data.camera_enabled.load(), "Manual selection changes still disarm the camera");
  prepare();
  data.device_lost = true;
  disarm(data);
  require(!data.camera_enabled.load(), "Device loss always disarms the camera");
  prepare();
  data.taxi_active_mask = 0;
  disarm(data);
  require(!data.camera_enabled.load(), "No active TAXI side cannot preserve an armed camera");

  // Exercise the actual named-route integration without starting a provider or
  // requiring simulator memory. The isolated host has no valid TAXI sample.
  require(!taxi_camera::native_camera::get_taxi_buttons().valid, "Isolated route fixture must not start simulator telemetry");
  data.taxi_telemetry_started = true;
  data.next_telemetry_retry_ms = UINT64_MAX;
  data.taxi_routes = {};
  api::resource_desc desc{};
  desc.type = api::resource_type::texture_2d;
  desc.texture.width = 768;
  desc.texture.height = 1024;
  desc.texture.depth_or_layers = desc.texture.samples = 1;
  desc.texture.levels = 5;
  desc.texture.format = api::format::r8g8b8a8_unorm;
  desc.usage = api::resource_usage::render_target;
  Target left{desc, 701}, right{desc, 509};
  std::strcpy(left.debug_name.data(), "SCREEN_DU_PFDL");
  std::strcpy(right.debug_name.data(), "SCREEN_DU_PFDR");
  data.targets.emplace(1, left);
  data.targets.emplace(2, right);
  service_taxi_buttons(nullptr, data, GetTickCount64());
  require(data.taxi_routes.targets == std::array<std::uint64_t, 2>{701, 509}, "Actual named detector assigns both PFD sides");
  require(data.taxi_routes.matches(701, 1) && !data.taxi_routes.matches(509, 1) && data.taxi_routes.matches(509, 2) &&
              !data.taxi_routes.matches(701, 2),
          "Named automatic routes remain side-specific");
  require(!data.camera_enabled.load() && !data.taxi_scene_requested, "Invalid TAXI telemetry does not arm named targets");

  std::uint64_t detection_time = GetTickCount64() + 1000;
  const auto detect_replacement = [&] {
    data.pfd_detector.reset();
    data.next_detection_ms = 0;
    for (unsigned window = 0; window < 4; ++window) {
      for (auto& [resource, target] : data.targets) {
        (void)resource;
        target.debug_name = {};
        target.eligible_draws += 100;
      }
      service_taxi_buttons(nullptr, data, detection_time);
      detection_time += 1000;
    }
  };
  data.targets.erase(2);
  data.taxi_routes.forget(509);
  data.targets.emplace(3, Target{desc, 900});
  detect_replacement();
  require(data.taxi_routes.targets == std::array<std::uint64_t, 2>{701, 900},
          "Actual activity detector retains LEFT when RIGHT receives a higher resource ID");
  data.targets.erase(1);
  data.taxi_routes.forget(701);
  data.targets.emplace(4, Target{desc, 1200});
  detect_replacement();
  require(data.taxi_routes.targets == std::array<std::uint64_t, 2>{1200, 900},
          "Actual activity detector retains RIGHT when LEFT receives a higher resource ID");
  data.targets.clear();
  data.taxi_routes.forget(1200);
  data.taxi_routes.forget(900);
  data.targets.emplace(5, Target{desc, 1500});
  data.targets.emplace(6, Target{desc, 1600});
  detect_replacement();
  require(data.taxi_routes.targets == std::array<std::uint64_t, 2>{},
          "Actual detector refuses implicit side reassignment after both historical targets disappear");
  require(std::strstr(data.taxi_detection_status, "ambiguous") != nullptr, "Ambiguous replacement is diagnosed");
  require(data.taxi_routes.assign(0, 1500) && data.taxi_routes.assign(1, 1600), "Explicit sides can restore lost assignments");
  data.taxi_scene_requested = true;  // Already running: fixture must not invoke native Start.
  data.taxi_intent.observe(detection_time, true, true, false);
  service_taxi_buttons(nullptr, data, detection_time + 8);
  require(data.taxi_intent_state.held && data.taxi_active_mask == 1 && data.camera_enabled.load() && data.taxi_scene_requested,
          "Actual service holds an accepted ON across a transient missing response without restarting scenes");
  data.taxi_scene_requested = false;
}

void run_source_glue(bool warp_requested, SubmissionScenario scenario) {
  require(DllMain(GetModuleHandleW(nullptr), DLL_PROCESS_ATTACH, nullptr), "Register actual production add-on callbacks");
  verify_disarm_routes();
  reshade::register_event<reshade::addon_event::init_device>(remember_device);
  reshade::register_event<reshade::addon_event::init_command_list>(remember_list);
  reshade::register_event<reshade::addon_event::reset_command_list>(remember_reset);
  reshade::register_event<reshade::addon_event::init_command_queue>(remember_queue);
  Ref<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "Create real ReShade factory");
  Ref<IDXGIAdapter> warp;
  if (warp_requested)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put())), "Get WARP");
  Ref<ID3D12Device> device;
  check(D3D12CreateDevice(warp.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "Create application device");
  require(observed_device && private_data<DeviceData>(observed_device), "Production device callback missing");
  auto* native_device = reinterpret_cast<ID3D12Device*>(observed_device->get_native());
  const auto device_key = reinterpret_cast<std::uint64_t>(observed_device);
  auto& manager = taxi_camera::scene_runtime::manager();
  auto& handoff = taxi_camera::scene_handoff();
  // The first actual init_resource event obtains the public ReShade device
  // proxy and installs creation-scope observation. Its own creation was not
  // enclosed by those wrappers and must not seed a source state retrospectively.
  Ref<ID3D12Resource> bootstrap;
  D3D12_RESOURCE_DESC bootstrap_desc{};
  bootstrap_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  bootstrap_desc.Width = bootstrap_desc.Height = 32;
  bootstrap_desc.DepthOrArraySize = bootstrap_desc.MipLevels = 1;
  bootstrap_desc.SampleDesc.Count = 1;
  bootstrap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  const auto default_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
  check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &bootstrap_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                        IID_PPV_ARGS(bootstrap.put())),
        "Bootstrap creation observation through actual application resource callback");
  Ref<ID3D12Device10> device10;
  check(device->QueryInterface(IID_PPV_ARGS(device10.put())), "Get actual application Device10 for enhanced creation proof");
  Commands producer, consumer;
  producer.initialize(device.p);
  auto* api_producer = observed_list;
  auto* api_producer_queue = observed_queue;
  require(api_producer_queue != nullptr, "Actual application queue callback missing");
  require(api_producer && private_data<CommandData>(api_producer)->capture_ready, "Production list/adaptor registration failed");
  auto* native_producer = reinterpret_cast<ID3D12GraphicsCommandList*>(api_producer->get_native());
  const auto object_generation = private_data<CommandData>(api_producer)->object_generation;
  consumer.initialize(native_device);  // Private queue/list: no mirrored app callbacks.
  Ref<ID3D12GraphicsCommandList7> list7;
  check(producer.list->QueryInterface(IID_PPV_ARGS(list7.put())), "Get application list7");
  require(boundary_hook_failures.load() == 0, "Production native boundary hook failed");
  Ref<ID3D12Fence> completed;
  check(native_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(completed.put())), "Create fixture fence");
  std::uint64_t value = 0;
  const auto drain = [&](ID3D12CommandQueue* queue) {
    check(queue->Signal(completed.p, ++value), "Signal fixture fence");
    wait([&] { return completed->GetCompletedValue() >= value; });
  };
  Ref<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap_desc.NumDescriptors = 3;
  check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(rtvs.put())), "Create application RTV heap");
  const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  std::array<Ref<ID3D12Resource>, 2> sources, readbacks, retired_sources;
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> handles{};
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints{};
  std::array<UINT64, 2> bytes{};
  std::array<std::uint64_t, 2> ids{};
  constexpr std::array<UINT, 2> heights{255, 504};
  const auto create_sources = [&] {
    for (unsigned feed = 0; feed < 2; ++feed) {
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = 768;
      desc.Height = heights[feed];
      desc.DepthOrArraySize = desc.MipLevels = 1;
      desc.SampleDesc.Count = 1;
      desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      if (feed == 0) {
        check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                              IID_PPV_ARGS(sources[feed].put())),
              "Create legacy pane born RT through application proxy");
      } else {
        D3D12_RESOURCE_DESC1 enhanced_desc{};
        enhanced_desc.Dimension = desc.Dimension;
        enhanced_desc.Width = desc.Width;
        enhanced_desc.Height = desc.Height;
        enhanced_desc.DepthOrArraySize = desc.DepthOrArraySize;
        enhanced_desc.MipLevels = desc.MipLevels;
        enhanced_desc.Format = desc.Format;
        enhanced_desc.SampleDesc = desc.SampleDesc;
        enhanced_desc.Flags = desc.Flags;
        check(device10->CreateCommittedResource3(&default_heap, D3D12_HEAP_FLAG_NONE, &enhanced_desc, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                                                 nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(sources[feed].put())),
              "Create enhanced pane born RT through application proxy");
      }
      handles[feed].ptr = rtvs->GetCPUDescriptorHandleForHeapStart().ptr + feed * stride;
      device->CreateRenderTargetView(sources[feed].p, nullptr, handles[feed]);
      const auto& target = private_data<DeviceData>(observed_device)->targets.at(reinterpret_cast<std::uint64_t>(sources[feed].p));
      ids[feed] = target.id;
      require(target.desc.texture.width == 768 && target.desc.texture.height == heights[feed] &&
                  target.desc.texture.format == api::format::r11g11b10_float,
              "Production resource descriptor/identity mismatch");
      const auto actual_description = sources[feed]->GetDesc();
      if (target.initial_model != (feed == 0 ? taxi_camera::source_state::Model::legacy_rt : taxi_camera::source_state::Model::enhanced_rt))
        std::fprintf(stderr, "Creation mismatch feed=%u model=%u observer=%s inputAlignment=%llu actualAlignment=%llu\n", feed,
                     static_cast<unsigned>(target.initial_model), private_data<DeviceData>(observed_device)->creation_status.load(),
                     static_cast<unsigned long long>(desc.Alignment), static_cast<unsigned long long>(actual_description.Alignment));
      require(
          target.initial_model == (feed == 0 ? taxi_camera::source_state::Model::legacy_rt : taxi_camera::source_state::Model::enhanced_rt),
          "Actual production init_resource did not preserve the enclosing creation model");
      native_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprints[feed], nullptr, nullptr, &bytes[feed]);
      D3D12_RESOURCE_DESC buffer{};
      buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      buffer.Width = bytes[feed];
      buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
      buffer.SampleDesc.Count = 1;
      buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      const auto readback_heap = heap(D3D12_HEAP_TYPE_READBACK);
      if (!readbacks[feed].p)
        check(native_device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(readbacks[feed].put())),
              "Create fixture-only readback");
    }
  };
  create_sources();
  require(manager.statistics().source_candidates == 2, "Production resource callback did not register both pane candidates");
  check(producer.list->Close(), "Close empty prepublication recording without source transitions");
  ID3D12CommandList* original = producer.list.p;
  producer.queue->ExecuteCommandLists(1, &original);
  drain(producer.queue.p);
  require(manager.statistics().tail_captures == 0, "Prepublication empty recording unexpectedly captured");
  handoff.begin_scene();
  const auto ticket = handoff.begin_capture();
  require(handoff.publish(ticket, {44, 2}, {9001, 9002},
                          {reinterpret_cast<std::uint64_t>(sources[0].p), reinterpret_cast<std::uint64_t>(sources[1].p)}),
          "Publish registered source pair");
  manager.begin_source_tracking();
  Ref<ID3D12CommandAllocator> unrelated_allocator, bundle_allocator;
  Ref<ID3D12GraphicsCommandList> unrelated_list, bundle;
  unsigned native_unobserved_submissions = 0, native_resets = 0, native_clean_submissions = 0;
  if (scenario == SubmissionScenario::unrelated_bundle || scenario == SubmissionScenario::unknown_native ||
      scenario == SubmissionScenario::native_reset) {
    // This recording never references either pane. Use a separate registered
    // list for the bundle case so the pane producer's own flags stay valid.
    auto* unrelated_device = scenario == SubmissionScenario::unrelated_bundle ? device.p : native_device;
    const auto init_events_before = observed_init_events, reset_events_before = observed_reset_events;
    check(unrelated_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(unrelated_allocator.put())),
          "Create unrelated DIRECT allocator");
    check(unrelated_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, unrelated_allocator.p, nullptr,
                                              IID_PPV_ARGS(unrelated_list.put())),
          "Create unrelated DIRECT list");
    if (scenario != SubmissionScenario::unrelated_bundle)
      require(observed_init_events == init_events_before && observed_reset_events == reset_events_before,
              "Hidden native list unexpectedly emitted public initialization/reset events");
    if (scenario == SubmissionScenario::unrelated_bundle) {
      require(observed_list && private_data<CommandData>(observed_list)->capture_ready,
              "Unrelated DIRECT list was not registered through production callbacks");
      check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(bundle_allocator.put())),
            "Create unrelated BUNDLE allocator");
      check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.p, nullptr, IID_PPV_ARGS(bundle.put())),
            "Create empty BUNDLE through actual ReShade proxy");
      check(bundle->Close(), "Close empty unrelated BUNDLE");
      unrelated_list->ExecuteBundle(bundle.p);
    }
    check(unrelated_list->Close(), "Close unrelated recording with no source references");
    ID3D12CommandList* unrelated = unrelated_list.p;
    producer.queue->ExecuteCommandLists(1, &unrelated);
    drain(producer.queue.p);
    if (scenario == SubmissionScenario::native_reset) {
      ++native_unobserved_submissions;
      // Replaying the original closed recording is valid D3D12 usage. Merely
      // adopting its native identity must never make its missing history safe.
      producer.queue->ExecuteCommandLists(1, &unrelated);
      drain(producer.queue.p);
      ++native_unobserved_submissions;
    }
    require(SUCCEEDED(device->GetDeviceRemovedReason()), "Unrelated submission removed the device");
  }
  DrawFixture draw;
  draw.initialize(device.p, DXGI_FORMAT_R11G11B10_FLOAT);
  if (scenario == SubmissionScenario::native_reset) {
    check(producer.allocator->Reset(), "Reset pre-adoption test allocator");
    check(producer.list->Reset(producer.allocator.p, nullptr), "Reset registered producer for unobserved negative");
    const float pre_reset_color[]{1, 0, 0, 1};
    for (unsigned feed = 0; feed < 2; ++feed)
      draw.record(list7.p, handles[feed], pre_reset_color, false, 768, heights[feed]);
    check(producer.list->Close(), "Close pre-adoption negative source recording");
    producer.queue->ExecuteCommandLists(1, &original);
    drain(producer.queue.p);
    require(manager.statistics().tail_captures == 0 && std::strcmp(manager.statistics().tail_status, "unknown_source_state") == 0,
            "Unobserved native recording or replay incorrectly permitted source capture");
    const auto resets_before = manager.statistics().resets;
    const auto public_resets_before = observed_reset_events;
    check(unrelated_allocator->Reset(), "Reset drained hidden allocator");
    check(unrelated_list->Reset(unrelated_allocator.p, nullptr), "Actual native Reset starts the first observed hidden recording");
    check(unrelated_list->Close(), "Close clean observed hidden recording");
    require(observed_reset_events == public_resets_before, "Hidden native Reset unexpectedly used the public ReShade event path");
    if (manager.statistics().resets != resets_before + 1) {
      std::printf(
          "{\"passed\":false,\"scenario\":\"native-reset\",\"warp\":%s,"
          "\"failure\":\"native_reset_not_observed\",\"preResetSubmissions\":%u,"
          "\"preResetCaptureRefused\":true,\"tailCaptures\":0}\n",
          warp_requested ? "true" : "false", native_unobserved_submissions);
      std::fflush(stdout);
      require(false, "Actual hidden-list Reset was not adopted by production callbacks");
    }
    ++native_resets;
    manager.stop_source_tracking();
    // Preserve the old objects so fresh identities cannot accidentally reuse
    // their addresses. Reset observation never reseeds those invalid resources.
    const auto old_ids = ids;
    for (unsigned feed = 0; feed < 2; ++feed) {
      retired_sources[feed].p = sources[feed].p;
      sources[feed].p = nullptr;
    }
    create_sources();
    require(ids[0] != old_ids[0] && ids[1] != old_ids[1], "Fresh source creation reused an old resource generation");
    handoff.begin_scene();
    const auto fresh_ticket = handoff.begin_capture();
    require(handoff.publish(fresh_ticket, {44, 3}, {9003, 9004},
                            {reinterpret_cast<std::uint64_t>(sources[0].p), reinterpret_cast<std::uint64_t>(sources[1].p)}),
            "Publish only the newly created source pair after successful native Reset");
    manager.begin_source_tracking();
  }
  const auto source_draw_offset = manager.statistics().source_draws;
  Ref<ID3D12Resource> scratch, indices;
  auto scratch_desc = sources[0]->GetDesc();
  scratch_desc.Width = 32;
  scratch_desc.Height = 16;
  check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &scratch_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(scratch.put())),
        "Create unrelated ordinary-pass target");
  D3D12_CPU_DESCRIPTOR_HANDLE scratch_rtv{rtvs->GetCPUDescriptorHandleForHeapStart().ptr + 2 * stride};
  device->CreateRenderTargetView(scratch.p, nullptr, scratch_rtv);
  unsigned immediate_flushes = 0, immediate_resets = 0;
  const auto flush_unrelated_immediate = [&] {
    auto* immediate = api_producer_queue->get_immediate_command_list();
    require(immediate != nullptr, "Get actual ReShade immediate command list");
    auto* immediate_command = private_data<CommandData>(immediate);
    require(immediate_command && immediate_command->capture_ready,
            "Production lifecycle did not register the actual immediate command list");
    const auto native_identity = immediate->get_native();
    const auto generation = immediate_command->object_generation;
    const auto resets_before = manager.statistics().resets;
    const float unrelated_color[]{0, 0, 1, 1};
    immediate->clear_render_target_view(api::resource_view{scratch_rtv.ptr}, unrelated_color, 0, nullptr);
    api_producer_queue->flush_immediate_command_list();
    drain(producer.queue.p);
    require(immediate->get_native() == native_identity && immediate_command->object_generation == generation &&
                manager.statistics().resets == resets_before + 1,
            "Successful native immediate Reset did not retire its exact recording generation");
    ++immediate_resets;
    ++immediate_flushes;
    require(SUCCEEDED(device->GetDeviceRemovedReason()), "Actual immediate-list flush removed the device");
  };
  if (scenario == SubmissionScenario::immediate)
    flush_unrelated_immediate();  // After creation seeding, before the first draw.
  D3D12_RESOURCE_DESC index_desc{};
  index_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  index_desc.Width = 6;
  index_desc.Height = index_desc.DepthOrArraySize = index_desc.MipLevels = 1;
  index_desc.SampleDesc.Count = 1;
  index_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const auto upload_heap = heap(D3D12_HEAP_TYPE_UPLOAD);
  check(native_device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &index_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(indices.put())),
        "Create fixture index buffer");
  void* mapped_indices = nullptr;
  const D3D12_RANGE no_read{0, 0};
  check(indices->Map(0, &no_read, &mapped_indices), "Map fixture index buffer");
  constexpr std::uint16_t triangle[]{0, 1, 2};
  std::memcpy(mapped_indices, triangle, sizeof(triangle));
  indices->Unmap(0, nullptr);
  const D3D12_INDEX_BUFFER_VIEW index_view{indices->GetGPUVirtualAddress(), sizeof(triangle), DXGI_FORMAT_R16_UINT};
  constexpr float colors[2][2][4] = {{{0.25f, 0.5f, 0.75f, 1}, {2, 4, 0.125f, 1}}, {{1, 0, 0.5f, 1}, {0.125f, 1, 2, 1}}};
  constexpr std::array<std::array<std::uint32_t, 2>, 2> words{
      {{0x340u | (0x380u << 11) | (0x1d0u << 22), 0x400u | (0x440u << 11) | (0x180u << 22)},
       {0x3c0u | (0x1c0u << 22), 0x300u | (0x3c0u << 11) | (0x200u << 22)}}};
  std::uint64_t checked_pixels = 0;
  for (unsigned frame = 0; frame < 2; ++frame) {
    if (frame && scenario == SubmissionScenario::immediate)
      flush_unrelated_immediate();
    if (scenario == SubmissionScenario::native_reset) {
      ID3D12CommandList* clean = unrelated_list.p;
      producer.queue->ExecuteCommandLists(1, &clean);
      drain(producer.queue.p);
      ++native_clean_submissions;
    }
    Sleep(70);
    check(producer.allocator->Reset(), "Reset completed application allocator");
    check(producer.list->Reset(producer.allocator.p, nullptr), "Reset through actual ReShade proxy");
    auto* command = private_data<CommandData>(api_producer);
    require(command->object_generation == object_generation && command->source_count == 0 && !command->source_recording_unsupported,
            "Production reset did not clear binding/effect scope");
    if (frame) {
      const float scratch_color[]{0, 1, 0, 1};
      draw.record(list7.p, scratch_rtv, scratch_color, true);
      require(!command->source_recording_unsupported && command->source_count == 0,
              "Unrelated ordinary pass poisoned later source drawing");
    }
    const auto draws_before = manager.statistics().source_draws;
    for (unsigned feed = 0; feed < 2; ++feed) {
      // The second frame recycles the CPU descriptor, then verifies that actual
      // proxy OMSetRT mapping resolves the current resource generation.
      if (frame)
        device->CreateRenderTargetView(sources[feed].p, nullptr, handles[1 - feed]);
      const auto rtv = handles[frame ? 1 - feed : feed];
      const float first[]{1, 0, 1, 1};
      draw.record(list7.p, rtv, first, false, 768, heights[feed]);
      require(command->source_count == 1 && command->source_targets[0] == sources[feed].p && command->source_generations[0] == ids[feed],
              "Production RTV mapping used stale source/generation");
      if (frame && feed) {
        producer.list->SetGraphicsRoot32BitConstants(0, 4, colors[frame][feed], 0);
        producer.list->IASetIndexBuffer(&index_view);
        producer.list->DrawIndexedInstanced(3, 1, 0, 0, 0);
      } else {
        draw.record(list7.p, rtv, colors[frame][feed], false, 768, heights[feed]);
      }
      const auto before_zero = manager.statistics().source_draws;
      producer.list->DrawInstanced(0, 1, 0, 0);
      require(manager.statistics().source_draws == before_zero, "Zero-count proxy draw consumed stale source stage");
      // A subsequent native zero draw cannot consume the previous public stage.
      native_producer->DrawInstanced(0, 1, 0, 0);
      require(manager.statistics().source_draws == before_zero, "Native draw consumed an already-cleared stage twice");
    }
    require(manager.statistics().source_draws == draws_before + 4, "Production public/native draw staging was missing or duplicated");
    check(producer.list->Close(), "Close source recording without terminal copy/state exit");
    producer.queue->ExecuteCommandLists(1, &original);
    if (manager.statistics().tail_captures != (frame + 1) * 2) {
      // The tail is recorded synchronously inside the completed Execute wrapper.
      // Drain original work before reporting failure rather than timing out or
      // releasing the test's application resources while GPU work is pending.
      drain(producer.queue.p);
      const auto capture = manager.statistics();
      const bool expected_refusal = scenario == SubmissionScenario::unknown_native && capture.tail_captures == 0 &&
                                    capture.source_draws == 4 && std::strcmp(capture.tail_status, "unknown_source_state") == 0;
      std::printf(
          "{\"passed\":%s,\"expectedRefusal\":%s,\"realReShade\":true,\"productionCallbacks\":true,\"warp\":%s,\"scenario\":\"%s\","
          "\"failure\":\"missing_capture_after_submission\",\"frame\":%u,\"legacyBornRT\":true,\"enhancedBornRT\":true,"
          "\"initialSourceBarriers\":0,\"sourceDraws\":%llu,\"tailCaptures\":%llu,\"immediateFlushes\":%u,\"status\":\"%s\"}\n",
          expected_refusal ? "true" : "false", expected_refusal ? "true" : "false", warp_requested ? "true" : "false",
          scenario_name(scenario), frame, static_cast<unsigned long long>(capture.source_draws),
          static_cast<unsigned long long>(capture.tail_captures), immediate_flushes, capture.tail_status);
      std::fflush(stdout);
      if (expected_refusal) {
        manager.stop_source_tracking();
        return;
      }
      require(false, "Unrelated submission prevented capture of unchanged born-RT source panes");
    }
    std::array<Manager::Frame, 2> frames{};
    std::size_t frame_count = 0;
    wait([&] {
      frame_count += manager.poll_completed_frames(frames.data() + frame_count, frames.size() - frame_count);
      return frame_count == 2;
    });
    if (frame) {
      check(consumer.allocator->Reset(), "Reset fixture readback allocator");
      check(consumer.list->Reset(consumer.allocator.p, nullptr), "Reset fixture readback list");
    }
    for (const auto& result : frames) {
      const auto feed = result.match.feed;
      require(feed < 2 && result.match.resource.resource_id == ids[feed], "Completed frame has wrong production resource identity");
      barrier(consumer.list.p, result.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION from{}, to{};
      from.pResource = result.resource;
      from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      to.pResource = readbacks[feed].p;
      to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      to.PlacedFootprint = footprints[feed];
      consumer.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      barrier(consumer.list.p, result.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    check(consumer.list->Close(), "Close fixture readback");
    const auto consume = manager.begin_private_submission(device_key, consumer.queue.p);
    require(consume.receipt != 0, "Begin synchronized fixture readback");
    ID3D12CommandList* read = consumer.list.p;
    consumer.queue->ExecuteCommandLists(1, &read);
    require(manager.end_private_submission(consume.receipt), "End synchronized fixture readback");
    for (const auto& result : frames)
      require(manager.finish_consumption(result.token, consume.fence, consume.value), "Finish completed frame lease");
    drain(consumer.queue.p);
    for (unsigned feed = 0; feed < 2; ++feed) {
      void* mapped = nullptr;
      const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes[feed])};
      check(readbacks[feed]->Map(0, &range, &mapped), "Map isolated test result");
      for (UINT y = 0; y < heights[feed]; ++y)
        for (UINT x = 0; x < 768; ++x) {
          std::uint32_t pixel = 0;
          std::memcpy(
              &pixel,
              static_cast<const std::uint8_t*>(mapped) + footprints[feed].Offset + UINT64(y) * footprints[feed].Footprint.RowPitch + x * 4,
              4);
          require(pixel == words[frame][feed], "Completed packed RGB differs from final actual proxy draw");
          ++checked_pixels;
        }
      const D3D12_RANGE empty{0, 0};
      readbacks[feed]->Unmap(0, &empty);
    }
  }
  manager.stop_source_tracking();
  require(scenario != SubmissionScenario::unknown_native, "Arbitrary unknown native command lists must remain refused");
  require(manager.statistics().tail_captures == 4 && manager.statistics().source_draws == source_draw_offset + 8,
          "Unexpected source staging/capture totals");
  for (const auto& observation : native_source_observations)
    require(observation.transitions.load() == 0 && observation.copy_sources.load() == 0 && observation.copy_destinations.load() == 0,
            "Born-RT fixture unexpectedly supplied an application source transition or copy");
  require(boundary_hook_failures.load() == 0 && taxi_camera::pfd_adapter::statistics().native_observer_failures == 0,
          "Production native observation reported failure");
  require(SUCCEEDED(device->GetDeviceRemovedReason()), "Device removed during production-glue validation");
  std::printf(
      "{\"passed\":true,\"realReShade\":true,\"productionCallbacks\":true,\"warp\":%s,\"pixels\":%llu,"
      "\"sourceDraws\":8,\"tailCaptures\":4,\"nativeFormat\":26,\"persistentRT\":true,"
      "\"legacyBornRT\":true,\"enhancedBornRT\":true,\"initialSourceBarriers\":0,\"scenario\":\"%s\","
      "\"immediateFlushes\":%u,\"immediateResets\":%u,\"immediateRegistered\":%s,"
      "\"nativeUnobservedSubmissions\":%u,\"nativeResets\":%u,\"nativeCleanSubmissions\":%u,"
      "\"preResetCaptureRefused\":%s,\"preResetSourceDraws\":%llu,\"totalSourceDraws\":%llu,\"checks\":%u}\n",
      warp_requested ? "true" : "false", static_cast<unsigned long long>(checked_pixels), scenario_name(scenario), immediate_flushes,
      immediate_resets, immediate_flushes ? "true" : "false", native_unobserved_submissions, native_resets, native_clean_submissions,
      scenario == SubmissionScenario::native_reset ? "true" : "false", static_cast<unsigned long long>(source_draw_offset),
      static_cast<unsigned long long>(manager.statistics().source_draws), checks);
}
}  // namespace

int main(int argc, char** argv) {
  try {
    bool warp = false;
    auto scenario = SubmissionScenario::immediate;
    bool scenario_selected = false;
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--warp") == 0 && !warp)
        warp = true;
      else if (!scenario_selected && (std::strcmp(argv[i], "--baseline") == 0 || std::strcmp(argv[i], "--immediate") == 0 ||
                                      std::strcmp(argv[i], "--unrelated-bundle") == 0 || std::strcmp(argv[i], "--unknown-native") == 0 ||
                                      std::strcmp(argv[i], "--native-reset") == 0)) {
        scenario_selected = true;
        scenario = std::strcmp(argv[i], "--baseline") == 0           ? SubmissionScenario::baseline
                   : std::strcmp(argv[i], "--unrelated-bundle") == 0 ? SubmissionScenario::unrelated_bundle
                   : std::strcmp(argv[i], "--unknown-native") == 0   ? SubmissionScenario::unknown_native
                   : std::strcmp(argv[i], "--native-reset") == 0     ? SubmissionScenario::native_reset
                                                                     : SubmissionScenario::immediate;
      } else
        require(false,
                "Usage: source-glue-validation [--warp] [--baseline|--immediate|--unrelated-bundle|--unknown-native|--native-reset]");
    }
    run_source_glue(warp, scenario);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
