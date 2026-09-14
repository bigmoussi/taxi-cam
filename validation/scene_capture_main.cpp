#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "../src/scene_capture_d3d12.hpp"

namespace {
using Capture = taxi_camera::SceneCaptureD3D12;
constexpr UINT Width = 64;
constexpr UINT Height = 48;
unsigned checks = 0;

template <typename T>
struct Reference {
  T* value = nullptr;
  ~Reference() {
    if (value)
      value->Release();
  }
  T* operator->() const { return value; }
  T** put() { return &value; }
  Reference() = default;
  Reference(const Reference&) = delete;
  Reference& operator=(const Reference&) = delete;
};

void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}
void check(HRESULT result, const char* message) {
  require(SUCCEEDED(result), message);
}
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES result{};
  result.Type = type;
  result.CreationNodeMask = result.VisibleNodeMask = 1;
  return result;
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, 0, before, after};
  list->ResourceBarrier(1, &barrier);
}
template <typename Predicate>
void wait_until(Predicate ready, const char* message) {
  const auto start = GetTickCount64();
  while (!ready()) {
    require(GetTickCount64() - start < 10000, message);
    Sleep(1);
  }
}

struct Submission {
  Reference<ID3D12CommandQueue> queue;
  Reference<ID3D12CommandAllocator> allocator;
  Reference<ID3D12GraphicsCommandList> list;
  void initialize(ID3D12Device* device) {
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.put())), "Create queue");
    check(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(allocator.put())), "Create allocator");
    check(device->CreateCommandList(0, desc.Type, allocator.value, nullptr, IID_PPV_ARGS(list.put())), "Create list");
  }
  void submit() {
    check(list->Close(), "Close list");
    ID3D12CommandList* lists[]{list.value};
    queue->ExecuteCommandLists(1, lists);
  }
  void reset() {
    check(allocator->Reset(), "Reset completed allocator");
    check(list->Reset(allocator.value, nullptr), "Reset completed list");
  }
};

void run(bool warp_requested, bool typeless, bool packed_float) {
  bool debug_enabled = false;
  Reference<ID3D12Debug> debug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())))) {
    debug->EnableDebugLayer();
    debug_enabled = true;
  }
  Reference<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "Create DXGI factory");
  Reference<IDXGIAdapter> adapter;
  if (warp_requested)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "Get WARP adapter");
  Reference<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.value, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "Create D3D12 device");
  Reference<ID3D12InfoQueue> info;
  if (debug_enabled)
    check(device->QueryInterface(IID_PPV_ARGS(info.put())), "Get debug info queue");

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = Width;
  desc.Height = Height;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Format = packed_float ? DXGI_FORMAT_R11G11B10_FLOAT : typeless ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const auto default_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
  Reference<ID3D12Resource> source, application_output;
  check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(source.put())),
        "Create application source");
  check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(application_output.put())),
        "Create application destination");
  Reference<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = 1;
  check(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(rtvs.put())), "Create source RTV heap");
  const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  D3D12_RENDER_TARGET_VIEW_DESC typed_rtv{};
  typed_rtv.Format = packed_float ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
  typed_rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  device->CreateRenderTargetView(source.value, &typed_rtv, rtv);

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
  const auto stride = (total + 511) & ~UINT64{511};
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = stride * 2;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const auto readback_heap = heap(D3D12_HEAP_TYPE_READBACK);
  Reference<ID3D12Resource> readback;
  check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(readback.put())),
        "Create readback");

  Submission producer, consumer;
  producer.initialize(device.value);
  consumer.initialize(device.value);
  Reference<ID3D12Fence> blocked, finished;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(blocked.put())), "Create blocking fence");
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(finished.put())), "Create consumer fence");
  Capture capture;
  auto unsupported = desc;
  unsupported.Format = DXGI_FORMAT_R32_TYPELESS;
  require(FAILED(capture.initialize(device.value, unsupported)), "Capture admitted an unrelated same-size typeless format");
  unsupported = desc;
  unsupported.MipLevels = 2;
  require(FAILED(capture.initialize(device.value, unsupported)), "Capture admitted a multi-subresource image");
  unsupported = desc;
  unsupported.Width = unsupported.Height = 8192;
  require(FAILED(capture.initialize(device.value, unsupported)), "Capture admitted an oversized allocation");
  check(capture.initialize(device.value, desc), "Initialize capture");
  Reference<ID3D12CommandAllocator> copy_allocator;
  Reference<ID3D12GraphicsCommandList> copy_list;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(copy_allocator.put())),
        "Create refusal-test copy allocator");
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copy_allocator.value, nullptr, IID_PPV_ARGS(copy_list.put())),
        "Create refusal-test copy list");
  require(!capture.record_copy_source(copy_list.value, source.value) && capture.state() == Capture::State::idle,
          "Capture admitted a COPY list with different resource-state decay semantics");
  check(copy_list->Close(), "Close unused refusal-test copy list");
  require(capture.allocation_bytes() <= Capture::MaximumBytes && capture.allocation_count() == 1, "Capture allocation is unbounded");
  require(!capture.retire_recording(producer.queue.value) && !capture.poll_ready() && capture.ready_resource() == nullptr,
          "Unrecorded capture was published");

  // This discarded recording exercises the new pre-transition packet contract
  // without submitting invalid commands: no proof means no recording, and a
  // repeated capture cannot silently switch its list or retained source.
  Capture render_proof;
  check(render_proof.initialize(device.value, desc), "Initialize RT proof packet");
  require(!render_proof.record_render_target_source(producer.list.value, source.value, false), "Unproved RT boundary accepted");
  require(!render_proof.record_render_target_source(copy_list.value, source.value, true), "RT capture accepted COPY list");
  require(render_proof.record_render_target_source(producer.list.value, source.value, true), "Initial RT packet recording failed");
  require(!render_proof.record_render_target_source(consumer.list.value, source.value, true), "RT packet switched recording");
  require(!render_proof.record_render_target_source(producer.list.value, application_output.value, true), "RT packet switched source");
  require(render_proof.record_render_target_source(producer.list.value, source.value, true), "RT packet repeat failed");
  check(producer.list->Close(), "Close unsubmitted RT proof test");
  producer.reset();
  require(render_proof.discard_unsubmitted_retired(), "Retired RT proof test retained its packet");
  require(render_proof.render_target_writes() == 2 && render_proof.recordings() == 1, "RT repeat created another recording");

  std::uint64_t checked_pixels = 0;
  for (unsigned frame = 0; frame < 4; ++frame) {
    if (frame) {
      producer.reset();
      consumer.reset();
    }
    std::array<unsigned char, 4> expected{static_cast<unsigned char>(17 + frame * 13), 91, 183, 255};
    std::array<float, 4> color{expected[0] / 255.0f, expected[1] / 255.0f, expected[2] / 255.0f, 1};
    if (packed_float) {
      // Independent exact encodings: exponent bias15, R/G mantissa6 and B5.
      // All values are binary fractions, so no rounding tolerance is needed.
      constexpr std::array<std::array<float, 4>, 4> colors{
          {{0.25f, 0.5f, 0.75f, 1}, {2, 4, 0.125f, 1}, {1, 0, 0.5f, 1}, {0.125f, 1, 2, 1}}};
      constexpr std::array<std::uint32_t, 4> words{{0x340u | (0x380u << 11) | (0x1d0u << 22), 0x400u | (0x440u << 11) | (0x180u << 22),
                                                    0x3c0u | (0u << 11) | (0x1c0u << 22), 0x300u | (0x3c0u << 11) | (0x200u << 22)}};
      color = colors[frame];
      std::memcpy(expected.data(), &words[frame], sizeof(words[frame]));
    }
    producer.list->ClearRenderTargetView(rtv, color.data(), 0, nullptr);
    transition(producer.list.value, source.value, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    // This is the application's original whole copy, forwarded exactly once.
    // Its destination can be the matched engine camera output. Mirror the SAME
    // source after forwarding; no engine destination read or transition occurs.
    producer.list->CopyResource(application_output.value, source.value);
    require(capture.record_copy_source(producer.list.value, source.value), capture.error());
    require(!capture.record_copy_source(producer.list.value, source.value), "A recording reused its pending capture storage");
    require(capture.ready_resource() == nullptr && !capture.release_idle(), "Pending GPU storage was published or released");
    transition(producer.list.value, source.value, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float changed[4]{1, 0, 1, 1};
    producer.list->ClearRenderTargetView(rtv, changed, 0, nullptr);
    const UINT64 producer_gate = frame * 2 + 1;
    check(producer.queue->Wait(blocked.value, producer_gate), "Block producer queue");
    producer.submit();
    // This harness owns the list and promises not to resubmit its old recording.
    require(capture.retire_recording(producer.queue.value), capture.error());
    require(!capture.poll_ready() && capture.ready_resource() == nullptr, "Pre-completion capture escaped its producer fence");
    check(blocked->Signal(producer_gate), "Release producer queue");
    wait_until([&] { return capture.poll_ready(); }, "Producer capture completion timed out");
    auto* snapshot = capture.ready_resource();
    require(snapshot != nullptr, "Completed capture has no owned texture");

    for (unsigned which = 0; which < 2; ++which) {
      auto* texture = which == 0 ? snapshot : application_output.value;
      transition(consumer.list.value, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION from{};
      from.pResource = texture;
      from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      D3D12_TEXTURE_COPY_LOCATION to{};
      to.pResource = readback.value;
      to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      to.PlacedFootprint = footprint;
      to.PlacedFootprint.Offset = which * stride;
      consumer.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      transition(consumer.list.value, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    const UINT64 consumer_gate = producer_gate + 1;
    check(consumer.queue->Wait(blocked.value, consumer_gate), "Block consumer queue");
    consumer.submit();
    check(consumer.queue->Signal(finished.value, frame + 1), "Signal consumer completion");
    require(capture.finish_consumption(finished.value, frame + 1), capture.error());
    require(!capture.recycle() && capture.ready_resource() == nullptr, "Capture recycled before all private reads completed");
    check(blocked->Signal(consumer_gate), "Release consumer queue");
    wait_until([&] { return capture.recycle(); }, "Consumer capture completion timed out");
    void* pixels = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(stride * 2)};
    check(readback->Map(0, &read_range, &pixels), "Map completed readback");
    for (unsigned which = 0; which < 2; ++which) {
      for (unsigned row = 0; row < Height; ++row) {
        const auto* bytes = static_cast<const unsigned char*>(pixels) + which * stride + row * footprint.Footprint.RowPitch;
        for (unsigned column = 0; column < Width; ++column) {
          require(std::memcmp(bytes + column * 4, expected.data(), 4) == 0,
                  "Snapshot/original destination pixels differ from the source at the copy event");
          ++checked_pixels;
        }
      }
    }
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
  }
  require(capture.recordings() == 4 && capture.allocation_count() == 1, "Capture did not reuse its original allocation across frames");
  require(capture.release_idle(), "Completed idle capture could not release resources");
  unsigned debug_errors = 0;
  if (info.value) {
    for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
      SIZE_T size = 0;
      check(info->GetMessage(index, nullptr, &size), "Get debug message size");
      std::vector<unsigned char> storage(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      check(info->GetMessage(index, message, &size), "Get debug message");
      if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
        ++debug_errors;
    }
  }
  require(debug_errors == 0, "D3D12 debug layer reported an error");
  std::printf(
      "{\"passed\":true,\"checks\":%u,\"checked_pixels\":%llu,\"recordings\":4,\"allocations\":1,"
      "\"debugLayer\":%s,\"debugErrors\":%u,\"adapter\":\"%s\",\"typeless\":%s,\"r11g11b10\":%s}\n",
      checks, static_cast<unsigned long long>(checked_pixels), debug_enabled ? "true" : "false", debug_errors,
      warp_requested ? "WARP" : "hardware", typeless ? "true" : "false", packed_float ? "true" : "false");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    bool warp = false, typeless = false, packed_float = false;
    require(argc <= 3, "Only --warp and one of --typeless/--r11g11b10 are accepted");
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--warp") == 0 && !warp)
        warp = true;
      else if (std::strcmp(argv[i], "--typeless") == 0 && !typeless && !packed_float)
        typeless = true;
      else if (std::strcmp(argv[i], "--r11g11b10") == 0 && !typeless && !packed_float)
        packed_float = true;
      else
        require(false, "Only --warp and one of --typeless/--r11g11b10 are accepted once each");
    }
    run(warp, typeless, packed_float);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
