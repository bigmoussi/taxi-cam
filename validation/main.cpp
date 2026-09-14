#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/calibration_d3d12.hpp"
#include "../src/resource_debug_name.hpp"

namespace {

constexpr UINT Width = 768;
constexpr UINT Height = 1024;
constexpr UINT CameraHeight = 763;
constexpr UINT LowerHeight = Height - CameraHeight;
constexpr std::array<unsigned char, 4> BaselineBytes{17, 34, 51, 255};
constexpr std::array<float, 4> BaselineColor{17.0f / 255.0f, 34.0f / 255.0f, 51.0f / 255.0f, 1.0f};

template <typename T>
class ComObject {
 public:
  ComObject() = default;
  ComObject(const ComObject&) = delete;
  ComObject& operator=(const ComObject&) = delete;
  ~ComObject() {
    if (value_) {
      value_->Release();
    }
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
  ~Handle() {
    if (value_) {
      CloseHandle(value_);
    }
  }
  HANDLE get() const { return value_; }

 private:
  HANDLE value_;
};

void requireSuccess(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    char message[256]{};
    std::snprintf(message, sizeof(message), "%s failed (HRESULT 0x%08lx)", operation, static_cast<unsigned long>(result));
    throw std::runtime_error(message);
  }
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  list->ResourceBarrier(1, &barrier);
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES result{};
  result.Type = type;
  result.CreationNodeMask = 1;
  result.VisibleNodeMask = 1;
  return result;
}

bool matchesBaseline(const unsigned char* pixel) {
  for (UINT channel = 0; channel < BaselineBytes.size(); ++channel) {
    if (pixel[channel] != BaselineBytes[channel]) {
      return false;
    }
  }
  return true;
}

std::uint64_t validateDebugNames(ID3D12Object* object) {
  std::uint64_t checks = 0;
  auto require = [&](bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
    ++checks;
  };
  struct GuardedName {
    std::array<char, 16> before;
    std::array<char, 256> value;
    std::array<char, 16> after;
  } name{};
  name.before.fill('x');
  name.after.fill('y');
  const auto before = name.before;
  const auto after = name.after;
  require(!taxi_camera::read_resource_debug_name(object, name.value) && name.value[0] == 0, "Absent debug label was not empty");
  constexpr char ansi[] = "SCREEN_DU_PFDL";
  requireSuccess(object->SetPrivateData(WKPDID_D3DDebugObjectName, sizeof(ansi) - 1, ansi), "Set nonterminated ANSI debug label");
  require(taxi_camera::read_resource_debug_name(object, name.value) && std::strcmp(name.value.data(), ansi) == 0,
          "ANSI private-data label did not round-trip");
  struct SmallBuffer {
    std::array<unsigned char, 16> before;
    std::array<unsigned char, 4> value;
    std::array<unsigned char, 16> after;
  } small{};
  small.before.fill(0x5a);
  small.after.fill(0xa5);
  const auto smallBefore = small.before;
  const auto smallAfter = small.after;
  UINT bytes = static_cast<UINT>(small.value.size());
  require(FAILED(object->GetPrivateData(WKPDID_D3DDebugObjectName, &bytes, small.value.data())) && bytes == sizeof(ansi) - 1,
          "Undersized private-data request did not report its required size");
  require(small.before == smallBefore && small.after == smallAfter, "GetPrivateData overran the supplied buffer");
  requireSuccess(object->SetPrivateData(WKPDID_D3DDebugObjectName, 0, nullptr), "Remove ANSI debug label");
  requireSuccess(object->SetName(L"SCREEN_DU_PFDR-\u00e9"), "Set Unicode debug label");
  require(taxi_camera::read_resource_debug_name(object, name.value) && std::strcmp(name.value.data(), "SCREEN_DU_PFDR-\xc3\xa9") == 0,
          "SetName label did not round-trip through the official wide GUID and UTF-8 conversion");
  requireSuccess(object->SetPrivateData(taxi_camera::D3DDebugObjectNameWide, 0, nullptr), "Remove wide debug label");
  std::array<char, 512> oversized{};
  oversized.fill('q');
  requireSuccess(object->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(oversized.size()), oversized.data()),
                 "Set oversized ANSI label");
  require(!taxi_camera::read_resource_debug_name(object, name.value) && name.value[0] == 0, "Oversized label was partially published");
  requireSuccess(object->SetPrivateData(WKPDID_D3DDebugObjectName, 0, nullptr), "Remove oversized ANSI label");
  requireSuccess(object->SetPrivateData(taxi_camera::D3DDebugObjectNameWide, 3, "bad"), "Set malformed wide label");
  require(!taxi_camera::read_resource_debug_name(object, name.value) && name.value[0] == 0, "Odd-byte UTF-16 label was accepted");
  require(!taxi_camera::read_resource_debug_name(nullptr, name.value) && name.value[0] == 0, "Null debug object was accepted");
  require(name.before == before && name.after == after, "Debug-label reader crossed its fixed output buffer");
  return checks;
}

struct Result {
  std::uint64_t upperChanged = 0;
  std::uint64_t lowerChanged = 0;
  std::uint64_t debugErrors = 0;
  std::uint64_t debugNameChecks = 0;
  bool warp = false;
  bool debugLayer = false;
};

Result run(bool forceWarp) {
  Result result{};

  // The optional OS graphics debugging component may be absent. Report that
  // explicitly; successful pixels alone do not validate every state barrier.
  ComObject<ID3D12Debug> debug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())))) {
    debug->EnableDebugLayer();
    result.debugLayer = true;
  }

  ComObject<IDXGIFactory4> factory;
  requireSuccess(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "CreateDXGIFactory2");

  ComObject<ID3D12Device> device;
  HRESULT deviceResult = E_FAIL;
  if (!forceWarp) {
    deviceResult = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put()));
  }
  ComObject<IDXGIAdapter> warp;
  if (FAILED(deviceResult)) {
    requireSuccess(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put())), "EnumWarpAdapter");
    requireSuccess(D3D12CreateDevice(warp.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "D3D12CreateDevice(WARP)");
    result.warp = true;
  }
  ComObject<ID3D12InfoQueue> debugMessages;
  if (result.debugLayer && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(debugMessages.put())))) {
    debugMessages->ClearStoredMessages();
  }

  D3D12_COMMAND_QUEUE_DESC queueDescription{};
  queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComObject<ID3D12CommandQueue> queue;
  requireSuccess(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(queue.put())), "CreateCommandQueue");
  ComObject<ID3D12CommandAllocator> allocator;
  requireSuccess(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())), "CreateCommandAllocator");
  ComObject<ID3D12GraphicsCommandList> commandList;
  requireSuccess(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(commandList.put())),
                 "CreateCommandList");

  D3D12_RESOURCE_DESC targetDescription{};
  targetDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  targetDescription.Width = Width;
  targetDescription.Height = Height;
  targetDescription.DepthOrArraySize = 1;
  targetDescription.MipLevels = 1;
  targetDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  targetDescription.SampleDesc.Count = 1;
  targetDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  targetDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const auto defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  ComObject<ID3D12Resource> target;
  requireSuccess(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &targetDescription, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 nullptr, IID_PPV_ARGS(target.put())),
                 "CreateCommittedResource(render target)");
  result.debugNameChecks = validateDebugNames(target.get());

  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDescription{};
  rtvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvHeapDescription.NumDescriptors = 1;
  ComObject<ID3D12DescriptorHeap> rtvHeap;
  requireSuccess(device->CreateDescriptorHeap(&rtvHeapDescription, IID_PPV_ARGS(rtvHeap.put())), "CreateDescriptorHeap");
  const auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.get(), nullptr, rtv);

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 readbackSize = 0;
  device->GetCopyableFootprints(&targetDescription, 0, 1, 0, &footprint, nullptr, nullptr, &readbackSize);
  D3D12_RESOURCE_DESC readbackDescription{};
  readbackDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  readbackDescription.Width = readbackSize;
  readbackDescription.Height = 1;
  readbackDescription.DepthOrArraySize = 1;
  readbackDescription.MipLevels = 1;
  readbackDescription.SampleDesc.Count = 1;
  readbackDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const auto readbackHeap = heapProperties(D3D12_HEAP_TYPE_READBACK);
  ComObject<ID3D12Resource> readback;
  requireSuccess(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescription, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(readback.put())),
                 "CreateCommittedResource(readback)");

  commandList->ClearRenderTargetView(rtv, BaselineColor.data(), 0, nullptr);
  // Invoke precisely the native helper used by the production callback. The
  // caller supplies an ordinary, open command list and an RTV in RENDER_TARGET
  // state; the helper deliberately does not infer or change resource states.
  if (!taxi_camera::record_calibration(commandList.get(), rtv, Width, Height, 17)) {
    throw std::runtime_error("Production calibration operation rejected the valid render target");
  }
  transition(commandList.get(), target.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION source{};
  source.pResource = target.get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION destination{};
  destination.pResource = readback.get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint = footprint;
  commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  requireSuccess(commandList->Close(), "Close command list");
  ID3D12CommandList* lists[] = {commandList.get()};
  queue->ExecuteCommandLists(1, lists);

  ComObject<ID3D12Fence> fence;
  requireSuccess(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "CreateFence");
  Handle completion(CreateEventW(nullptr, FALSE, FALSE, nullptr));
  if (!completion.get()) {
    throw std::runtime_error("CreateEventW failed");
  }
  requireSuccess(queue->Signal(fence.get(), 1), "Signal fence");
  requireSuccess(fence->SetEventOnCompletion(1, completion.get()), "SetEventOnCompletion");
  if (WaitForSingleObject(completion.get(), 30000) != WAIT_OBJECT_0) {
    throw std::runtime_error("GPU did not complete within 30 seconds");
  }
  requireSuccess(device->GetDeviceRemovedReason(), "Device health");

  const D3D12_RANGE readRange{0, static_cast<SIZE_T>(readbackSize)};
  unsigned char* pixels = nullptr;
  requireSuccess(readback->Map(0, &readRange, reinterpret_cast<void**>(&pixels)), "Map readback");
  for (UINT y = 0; y < Height; ++y) {
    const auto* row = pixels + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch;
    for (UINT x = 0; x < Width; ++x) {
      if (!matchesBaseline(row + static_cast<std::size_t>(x) * 4)) {
        if (y < CameraHeight) {
          ++result.upperChanged;
        } else {
          ++result.lowerChanged;
        }
      }
    }
  }
  const D3D12_RANGE writtenRange{0, 0};
  readback->Unmap(0, &writtenRange);
  if (debugMessages.get()) {
    for (UINT64 index = 0; index < debugMessages->GetNumStoredMessages(); ++index) {
      SIZE_T messageSize = 0;
      requireSuccess(debugMessages->GetMessage(index, nullptr, &messageSize), "Query debug message size");
      std::vector<unsigned char> storage(messageSize);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      requireSuccess(debugMessages->GetMessage(index, message, &messageSize), "Read debug message");
      if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
        ++result.debugErrors;
        std::fprintf(stderr, "D3D12 error: %s\n", message->pDescription);
      }
    }
  }
  return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc > 2 || (argc == 2 && std::wstring(argv[1]) != L"--warp")) {
    std::fprintf(stderr, "Usage: taxi-camera-gpu-validation.exe [--warp]\n");
    return 2;
  }
  try {
    const Result result = run(argc == 2);
    const bool passed =
        result.upperChanged == static_cast<std::uint64_t>(Width) * CameraHeight && result.lowerChanged == 0 && result.debugErrors == 0;
    std::printf(
        "{\"passed\":%s,\"adapter\":\"%s\",\"debug_layer\":%s,\"width\":%u,\"height\":%u,"
        "\"upper_height\":%u,\"lower_height\":%u,\"upper_changed_pixels\":%llu,\"lower_changed_pixels\":%llu,"
        "\"debug_errors\":%llu,\"debug_name_checks\":%llu}\n",
        passed ? "true" : "false", result.warp ? "warp" : "hardware", result.debugLayer ? "true" : "false", Width, Height, CameraHeight,
        LowerHeight, static_cast<unsigned long long>(result.upperChanged), static_cast<unsigned long long>(result.lowerChanged),
        static_cast<unsigned long long>(result.debugErrors), static_cast<unsigned long long>(result.debugNameChecks));
    return passed ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Validation failed: %s\n", error.what());
    return 1;
  }
}
