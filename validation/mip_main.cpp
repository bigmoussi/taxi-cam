#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/calibration_d3d12.hpp"

namespace {

// Explicit expected dimensions and boundaries avoid deriving the test oracle
// from the production helper. All levels belong to one physical texture.
struct Mip {
  UINT width;
  UINT height;
  UINT upper;
  std::uint64_t upper_changed = 0;
  std::uint64_t lower_preserved = 0;
};
constexpr std::array<Mip, 5> ExpectedMips{{{768, 1024, 763}, {384, 512, 381}, {192, 256, 190}, {96, 128, 95}, {48, 64, 47}}};
constexpr std::array<unsigned char, 4> BaselineBytes{17, 34, 51, 255};
constexpr std::array<float, 4> BaselineColor{17.0f / 255.0f, 34.0f / 255.0f, 51.0f / 255.0f, 1.0f};

template <typename T>
class ComObject {
 public:
  ComObject() = default;
  ComObject(const ComObject&) = delete;
  ComObject& operator=(const ComObject&) = delete;
  ~ComObject() {
    if (value_)
      value_->Release();
  }
  T* get() const { return value_; }
  T* operator->() const { return value_; }
  T** put() { return &value_; }

 private:
  T* value_ = nullptr;
};

class Handle {
 public:
  explicit Handle(HANDLE value) : value_(value) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() {
    if (value_)
      CloseHandle(value_);
  }
  HANDLE get() const { return value_; }

 private:
  HANDLE value_;
};

void check(HRESULT value, const char* operation) {
  if (FAILED(value)) {
    char message[256]{};
    std::snprintf(message, sizeof(message), "%s failed (HRESULT 0x%08lx)", operation, static_cast<unsigned long>(value));
    throw std::runtime_error(message);
  }
}

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES result{};
  result.Type = type;
  result.CreationNodeMask = result.VisibleNodeMask = 1;
  return result;
}

struct Result {
  std::array<Mip, 5> mips = ExpectedMips;
  std::uint64_t checked_pixels = 0;
  std::uint64_t preserved_lower_pixels = 0;
  std::uint64_t debug_errors = 0;
  bool warp = false;
  bool debug_layer = false;
};

Result run(bool force_warp) {
  Result result;
  ComObject<ID3D12Debug> debug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())))) {
    debug->EnableDebugLayer();
    result.debug_layer = true;
  }
  ComObject<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "CreateDXGIFactory2");
  ComObject<ID3D12Device> device;
  HRESULT created = E_FAIL;
  if (!force_warp)
    created = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put()));
  ComObject<IDXGIAdapter> warp;
  if (FAILED(created)) {
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put())), "EnumWarpAdapter");
    check(D3D12CreateDevice(warp.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "D3D12CreateDevice(WARP)");
    result.warp = true;
  }
  ComObject<ID3D12InfoQueue> messages;
  if (result.debug_layer && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(messages.put()))))
    messages->ClearStoredMessages();

  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComObject<ID3D12CommandQueue> queue;
  check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.put())), "CreateCommandQueue");
  ComObject<ID3D12CommandAllocator> allocator;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())), "CreateCommandAllocator");
  ComObject<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put())),
        "CreateCommandList");

  D3D12_RESOURCE_DESC target_desc{};
  target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  target_desc.Width = ExpectedMips[0].width;
  target_desc.Height = ExpectedMips[0].height;
  target_desc.DepthOrArraySize = 1;
  target_desc.MipLevels = static_cast<UINT16>(ExpectedMips.size());
  target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  target_desc.SampleDesc.Count = 1;
  target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
  ComObject<ID3D12Resource> target;
  check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(target.put())),
        "CreateCommittedResource(five-mip target)");

  D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
  descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  descriptors.NumDescriptors = static_cast<UINT>(ExpectedMips.size());
  ComObject<ID3D12DescriptorHeap> rtv_heap;
  check(device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(rtv_heap.put())), "CreateDescriptorHeap");
  const auto first_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 5> rtvs{};
  for (UINT level = 0; level < ExpectedMips.size(); ++level) {
    rtvs[level].ptr = first_rtv.ptr + static_cast<SIZE_T>(level) * stride;
    D3D12_RENDER_TARGET_VIEW_DESC view{};
    view.Format = target_desc.Format;
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipSlice = level;
    device->CreateRenderTargetView(target.get(), &view, rtvs[level]);
    list->ClearRenderTargetView(rtvs[level], BaselineColor.data(), 0, nullptr);
  }

  // The selected RTV determines the subresource. Pass that mip's dimensions,
  // rather than the base resource dimensions, to the unchanged production code.
  for (UINT level = 0; level < ExpectedMips.size(); ++level) {
    const auto& mip = ExpectedMips[level];
    if (!taxi_camera::record_calibration(list.get(), rtvs[level], mip.width, mip.height, 17 + level * 7)) {
      throw std::runtime_error("Production calibration rejected a valid mip RTV");
    }
  }

  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 5> footprints{};
  UINT64 readback_size = 0;
  device->GetCopyableFootprints(&target_desc, 0, static_cast<UINT>(footprints.size()), 0, footprints.data(), nullptr, nullptr,
                                &readback_size);
  for (UINT level = 0; level < ExpectedMips.size(); ++level) {
    if (footprints[level].Footprint.Width != ExpectedMips[level].width ||
        footprints[level].Footprint.Height != ExpectedMips[level].height) {
      throw std::runtime_error("Native mip footprint does not match the independent expected dimensions");
    }
  }
  D3D12_RESOURCE_DESC readback_desc{};
  readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  readback_desc.Width = readback_size;
  readback_desc.Height = 1;
  readback_desc.DepthOrArraySize = readback_desc.MipLevels = 1;
  readback_desc.SampleDesc.Count = 1;
  readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const auto readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
  ComObject<ID3D12Resource> readback;
  check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(readback.put())),
        "CreateCommittedResource(readback)");

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = target.get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1, &barrier);
  for (UINT level = 0; level < ExpectedMips.size(); ++level) {
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = target.get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = level;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprints[level];
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  }
  check(list->Close(), "Close command list");
  ID3D12CommandList* lists[] = {list.get()};
  queue->ExecuteCommandLists(1, lists);
  ComObject<ID3D12Fence> fence;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "CreateFence");
  Handle completion(CreateEventW(nullptr, FALSE, FALSE, nullptr));
  if (!completion.get())
    throw std::runtime_error("CreateEventW failed");
  check(queue->Signal(fence.get(), 1), "Signal fence");
  check(fence->SetEventOnCompletion(1, completion.get()), "SetEventOnCompletion");
  if (WaitForSingleObject(completion.get(), 30000) != WAIT_OBJECT_0)
    throw std::runtime_error("GPU did not complete within 30 seconds");
  check(device->GetDeviceRemovedReason(), "Device health");

  const D3D12_RANGE read_range{0, static_cast<SIZE_T>(readback_size)};
  unsigned char* pixels = nullptr;
  check(readback->Map(0, &read_range, reinterpret_cast<void**>(&pixels)), "Map readback");
  for (UINT level = 0; level < result.mips.size(); ++level) {
    auto& mip = result.mips[level];
    const auto& footprint = footprints[level];
    for (UINT y = 0; y < mip.height; ++y) {
      const auto* row = pixels + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch;
      for (UINT x = 0; x < mip.width; ++x) {
        const auto* pixel = row + static_cast<std::size_t>(x) * 4;
        bool baseline = true;
        for (UINT channel = 0; channel < BaselineBytes.size(); ++channel)
          baseline = baseline && pixel[channel] == BaselineBytes[channel];
        ++result.checked_pixels;
        if (y < mip.upper) {
          if (!baseline)
            ++mip.upper_changed;
        } else if (baseline) {
          ++mip.lower_preserved;
          ++result.preserved_lower_pixels;
        }
      }
    }
  }
  const D3D12_RANGE written_range{0, 0};
  readback->Unmap(0, &written_range);
  if (messages.get()) {
    for (UINT64 index = 0; index < messages->GetNumStoredMessages(); ++index) {
      SIZE_T size = 0;
      check(messages->GetMessage(index, nullptr, &size), "Query debug message size");
      std::vector<unsigned char> bytes(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
      check(messages->GetMessage(index, message, &size), "Read debug message");
      if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
        ++result.debug_errors;
        std::fprintf(stderr, "D3D12 error: %s\n", message->pDescription);
      }
    }
  }
  return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc > 2 || (argc == 2 && std::wstring(argv[1]) != L"--warp")) {
    std::fprintf(stderr, "Usage: taxi-camera-mip-validation.exe [--warp]\n");
    return 2;
  }
  try {
    const auto result = run(argc == 2);
    bool passed = result.debug_errors == 0;
    for (const auto& mip : result.mips) {
      passed = passed && mip.upper_changed == static_cast<std::uint64_t>(mip.width) * mip.upper &&
               mip.lower_preserved == static_cast<std::uint64_t>(mip.width) * (mip.height - mip.upper);
    }
    std::printf(
        "{\"passed\":%s,\"adapter\":\"%s\",\"debug_layer\":%s,\"debug_errors\":%llu,\"checked_pixels\":%llu,"
        "\"preserved_lower_pixels\":%llu,\"mips\":[",
        passed ? "true" : "false", result.warp ? "warp" : "hardware", result.debug_layer ? "true" : "false",
        static_cast<unsigned long long>(result.debug_errors), static_cast<unsigned long long>(result.checked_pixels),
        static_cast<unsigned long long>(result.preserved_lower_pixels));
    for (UINT level = 0; level < result.mips.size(); ++level) {
      const auto& mip = result.mips[level];
      std::printf(
          "%s{\"level\":%u,\"width\":%u,\"height\":%u,\"upper_height\":%u,\"upper_changed_pixels\":%llu,"
          "\"preserved_lower_pixels\":%llu}",
          level == 0 ? "" : ",", level, mip.width, mip.height, mip.upper, static_cast<unsigned long long>(mip.upper_changed),
          static_cast<unsigned long long>(mip.lower_preserved));
    }
    std::puts("]}");
    return passed ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Mip validation failed: %s\n", error.what());
    return 1;
  }
}
