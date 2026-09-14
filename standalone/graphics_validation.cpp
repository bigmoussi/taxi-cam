#undef WIN32_LEAN_AND_MEAN
#undef NOMINMAX
// Existing independent gradient and pixel oracles; its main is not called.
#define wmain compositor_test_not_called
#include "../validation/compositor_main.cpp"
#undef wmain
#include <tlhelp32.h>
#include "d3d12_bridge.hpp"
#include "native_hooks.hpp"
namespace {
namespace win = taxi_camera::standalone;
namespace runtime = taxi_camera::scene_runtime;
void ensure_no_reshade() {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  require(snapshot != INVALID_HANDLE_VALUE, "Enumerate validation modules");
  MODULEENTRY32W module{};
  module.dwSize = sizeof(module);
  if (Module32FirstW(snapshot, &module))
    do {
      HMODULE handle = GetModuleHandleW(module.szModule);
      require(!GetProcAddress(handle, "ReShadeRegisterAddon"), "ReShade present in native validation");
    } while (Module32NextW(snapshot, &module));
  CloseHandle(snapshot);
}
void native_case(bool warp) {
  Reference<ID3D12Debug> debug;
  const bool debug_enabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())));
  if (debug_enabled)
    debug->EnableDebugLayer();
  Reference<IDXGIFactory4> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "Factory");
  Reference<IDXGIAdapter> adapter;
  if (warp)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "WARP");
  Reference<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put())), "Device");
  Reference<ID3D12InfoQueue> messages;
  if (debug_enabled)
    device->QueryInterface(IID_PPV_ARGS(messages.put()));
  // These common graphics objects predate exe.xml companion startup.
  GradientGenerator generator(device.get());
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  Reference<ID3D12CommandQueue> queue;
  Reference<ID3D12CommandAllocator> allocator;
  Reference<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.put())), "Pre-existing application queue");
  check(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(allocator.put())), "Pre-existing allocator");
  check(device->CreateCommandList(0, qd.Type, allocator.get(), nullptr, IID_PPV_ARGS(list.put())), "Pre-existing list");
  check(list->Close(), "Close pre-existing list");
  require(win::initialize_graphics(device.get()), win::graphics_status().error);
  check(list->Reset(allocator.get(), nullptr), "Observe first actual Reset of pre-existing list");
  ensure_no_reshade();
  const auto key = win::graphics_status().device;
  require(runtime::prepare(key), "Native compositor prepare");
  runtime::manager().begin_source_tracking();
  runtime::manager().set_source_rate(60);
  std::array<Reference<ID3D12Resource>, 4> textures;
  for (UINT i = 0; i < 4; ++i) {
    auto d = texture_description(768, i == 0 ? 255 : i == 1 ? 504 : 1024, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (i > 1)
      d.MipLevels = 5;
    create_texture(device.get(), d, textures[i].put());
  }
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = 8;
  Reference<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(heap.put())), "RTV heap");
  const auto stride = device->GetDescriptorHandleIncrementSize(hd.Type);
  const auto base = heap->GetCPUDescriptorHandleForHeapStart();
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> rtvs;
  for (UINT i = 0; i < 4; ++i) {
    D3D12_RENDER_TARGET_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvs[i] = {base.ptr + SIZE_T{i} * stride};
    device->CreateRenderTargetView(textures[i].get(), &d, rtvs[i]);
  }
  // Exercise the two public descriptor copy routes, not just creation.
  device->CopyDescriptorsSimple(1, {base.ptr + 4 * stride}, rtvs[2], hd.Type);
  const D3D12_CPU_DESCRIPTOR_HANDLE dest{base.ptr + 5 * stride};
  device->CopyDescriptors(1, &dest, nullptr, 1, &rtvs[3], nullptr, hd.Type);
  rtvs[2] = {base.ptr + 4 * stride};
  rtvs[3] = dest;
  auto& handoff = taxi_camera::scene_handoff();
  handoff.begin_scene();
  require(handoff.publish(handoff.begin_capture(), {501, 1}, {701, 702},
                          {reinterpret_cast<std::uint64_t>(textures[0].get()), reinterpret_cast<std::uint64_t>(textures[1].get())}),
          "Native source creation identities published");
  const auto inventory = win::pfd_inventory();
  require(inventory.size() == 2, "Two native PFD candidates");
  require(win::assign_targets(inventory[0].id, inventory[1].id), "Explicit PFD pair");
  require(!win::assign_targets(inventory[0].id, inventory[0].id), "Reject duplicate PFD identity");

  auto submit = [&] {
    check(list->Close(), "Close application recording");
    ID3D12CommandList* batch[]{list.get()};
    queue->ExecuteCommandLists(1, batch);
    require(taxi_camera::drain_copy_queue(queue.get(), device.get()), "Application GPU completion");
  };
  auto reset = [&] {
    check(allocator->Reset(), "Allocator Reset");
    check(list->Reset(allocator.get(), nullptr), "Observed native Reset");
  };
  generator.record(list.get(), rtvs[0], 768, 255, false, 0, 0);
  generator.record(list.get(), rtvs[1], 768, 504, false, 0, 1);
  submit();
  reset();
  const auto deadline = GetTickCount64() + 10000;
  while (!runtime::snapshot(key).output && GetTickCount64() < deadline) {
    runtime::service();
    Sleep(1);
  }
  auto capture = runtime::snapshot(key);
  std::printf("native capture=%llu completed=%llu composed=%llu source_draws=%llu tail=%s\n",
              static_cast<unsigned long long>(capture.capture.captures), static_cast<unsigned long long>(capture.capture.completed),
              static_cast<unsigned long long>(capture.frames), static_cast<unsigned long long>(capture.capture.source_draws),
              capture.capture.tail_status);
  require(capture.output && capture.frames, "Actual native draw -> queue -> capture -> composition");
  win::set_target_mask(3);
  generator.record(list.get(), rtvs[2], 768, 1024, false, 0, 0);
  generator.record(list.get(), rtvs[3], 768, 1024, false, 0, 0);
  // Only change root constants and scissor. Original pipeline, signature,
  // topology and viewport must have survived the injected PFD draw.
  // Update one word only: previously set words must survive our root change.
  list->SetGraphicsRoot32BitConstant(0, 1, 2);
  const D3D12_RECT lower{0, 763, 768, 1024};
  list->RSSetScissorRects(1, &lower);
  list->DrawInstanced(3, 1, 0, 0);
  require(runtime::snapshot(key).stamps == 3, "Three automatic native PFD stamps");
  std::array<Reference<ID3D12Resource>, 2> readbacks;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes{};
  const auto pd = textures[2]->GetDesc();
  device->GetCopyableFootprints(&pd, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto bh = heap_properties(D3D12_HEAP_TYPE_READBACK);
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = bytes;
  bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  for (UINT i = 0; i < 2; ++i) {
    check(device->CreateCommittedResource(&bh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(readbacks[i].put())),
          "Test-only readback");
    transition(list.get(), textures[i + 2].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = textures[i + 2].get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readbacks[i].get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.get(), textures[i + 2].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  submit();
  reset();
  std::uint64_t pixels = 0;
  for (UINT side = 0; side < 2; ++side) {
    void* mapped{};
    const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    check(readbacks[side]->Map(0, &range, &mapped), "Map verification readback");
    const auto* data = static_cast<const unsigned char*>(mapped);
    for (UINT y = 0; y < 1024; ++y)
      for (UINT x = 0; x < 768; ++x) {
        const auto* pixel = data + SIZE_T{y} * footprint.Footprint.RowPitch + 4 * x;
        // Independent broad regions deliberately exclude reference marks and GS.
        if (x >= 350 && x < 400 && y >= 100 && y < 150)
          require(pixel[2] >= 49 && pixel[2] <= 53, "Nose frame on PFD");
        if (x >= 350 && x < 400 && y >= 400 && y < 450)
          require(pixel[2] >= 202 && pixel[2] <= 206, "Tail frame on PFD");
        if (y >= 255 && y < 259)
          require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0, "Black divider");
        if (y >= 800) {
          const UINT blue = side ? 153 : 51;
          require(pixel[2] >= blue - 1 && pixel[2] <= blue + 1, "Lower trim and application graphics state preserved");
        }
        ++pixels;
      }
    const D3D12_RANGE none{0, 0};
    readbacks[side]->Unmap(0, &none);
  }
  win::set_target_mask(0);
  const auto stamps = runtime::snapshot(key).stamps;
  generator.record(list.get(), rtvs[2], 768, 1024, false, 0, 0);
  require(runtime::snapshot(key).stamps == stamps, "OFF state stops new PFD stamping");
  submit();
  reset();
  list->Close();
  runtime::manager().stop_source_tracking();
  handoff.stop_scene();
  runtime::reset_feed(key);
  require(win::graphics_status().hook_failures == 0, "No native hook failures");
  std::uint64_t errors = 0;
  if (messages.get()) {
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
      SIZE_T size{};
      messages->GetMessage(i, nullptr, &size);
      std::vector<unsigned char> memory(size);
      auto* msg = reinterpret_cast<D3D12_MESSAGE*>(memory.data());
      if (SUCCEEDED(messages->GetMessage(i, msg, &size)) && msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
        ++errors;
        std::fprintf(stderr, "D3D12: %s\n", msg->pDescription);
      }
    }
  }
  require(errors == 0, "D3D12 validation errors");
  std::printf(
      "PASS native %s: two GPU feeds, two PFDs, pre-existing root/list/queue, partial state restoration, lower trim, descriptor copies, "
      "OFF; %llu pixels; debug=%d errors=%llu; ReShade absent\n",
      warp ? "WARP" : "hardware", static_cast<unsigned long long>(pixels), debug_enabled, static_cast<unsigned long long>(errors));
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    native_case(argc == 2 && std::wcscmp(argv[1], L"--warp") == 0);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL native graphics: %s\n", e.what());
    return 1;
  }
}
