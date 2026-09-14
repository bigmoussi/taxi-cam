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
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/calibration_copy_d3d12.hpp"

namespace {

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

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void check(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    char message[256]{};
    std::snprintf(message, sizeof(message), "%s failed (HRESULT 0x%08lx)", operation, static_cast<unsigned long>(result));
    throw std::runtime_error(message);
  }
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = type;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;
  return heap;
}

D3D12_RESOURCE_DESC textureDescription(UINT width, UINT height, DXGI_FORMAT format) {
  D3D12_RESOURCE_DESC description{};
  description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  description.Width = width;
  description.Height = height;
  description.DepthOrArraySize = 1;
  description.MipLevels = 1;
  description.Format = format;
  description.SampleDesc.Count = 1;
  description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  // Deliberately NO ALLOW_RENDER_TARGET: final uploaded instrument textures need
  // not have an RTV. This validator must prove that exact unsupported old case.
  description.Flags = D3D12_RESOURCE_FLAG_NONE;
  return description;
}

void createBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE heapType, ID3D12Resource** output) {
  auto heap = heapProperties(heapType);
  D3D12_RESOURCE_DESC description{};
  description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  description.Width = size;
  description.Height = 1;
  description.DepthOrArraySize = 1;
  description.MipLevels = 1;
  description.SampleDesc.Count = 1;
  description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const auto state = heapType == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr, IID_PPV_ARGS(output)), "Create buffer");
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

class Submission {
 public:
  Submission(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type) : device_(device) {
    D3D12_COMMAND_QUEUE_DESC description{};
    description.Type = type;
    check(device->CreateCommandQueue(&description, IID_PPV_ARGS(queue_.put())), "Create queue");
    check(device->CreateCommandAllocator(type, IID_PPV_ARGS(allocator_.put())), "Create allocator");
    check(device->CreateCommandList(0, type, allocator_.get(), nullptr, IID_PPV_ARGS(list_.put())), "Create list");
  }

  ID3D12GraphicsCommandList* list() const { return list_.get(); }

  void finish() {
    check(list_->Close(), "Close list");
    ID3D12CommandList* lists[]{list_.get()};
    queue_->ExecuteCommandLists(1, lists);
    require(taxi_camera::drain_copy_queue(queue_.get(), device_), "Checked production queue drain failed");
  }

 private:
  ComObject<ID3D12CommandQueue> queue_;
  ComObject<ID3D12CommandAllocator> allocator_;
  ComObject<ID3D12GraphicsCommandList> list_;
  ID3D12Device* device_;
};

constexpr std::array<unsigned char, 4> Baseline{17, 34, 51, 255};
constexpr std::array<std::array<unsigned char, 4>, 5> Colors{
    {{5, 41, 97, 255}, {31, 82, 8, 255}, {0, 230, 255, 255}, {255, 204, 0, 255}, {255, 255, 255, 255}}};
constexpr std::array<std::uint64_t, 2> Frames{17, 193};

std::array<unsigned char, 4> expectedPixel(UINT width, UINT height, UINT x, UINT y, std::uint64_t frame, bool bgra) {
  const UINT upper = static_cast<UINT>(std::uint64_t(height) * 763 / 1024);
  if (y >= upper) {
    return Baseline;
  }
  const UINT split = upper / 3;
  const UINT barWidth = width / 32;
  const UINT barX = static_cast<UINT>((frame * 3) % (width - barWidth));
  auto pixel = Colors[y < split ? 0 : 1];
  if (y < split && x >= barX && x < barX + barWidth) {
    pixel = Colors[2];
  }
  if (y >= split && x >= width - barX - barWidth && x < width - barX) {
    pixel = Colors[3];
  }
  if (y == split) {
    pixel = Colors[4];
  }
  if (bgra) {
    const auto red = pixel[0];
    pixel[0] = pixel[2];
    pixel[2] = red;
  }
  return pixel;
}

struct Result {
  bool warp = false;
  bool debugLayer = false;
  std::uint64_t cases = 0;
  std::uint64_t checkedPixels = 0;
  std::uint64_t lowerPixels = 0;
  std::uint64_t debugErrors = 0;
  std::uint64_t drains = 0;
};

void pixelCase(ID3D12Device* device,
               taxi_camera::CopyCalibration& calibration,
               D3D12_COMMAND_LIST_TYPE type,
               UINT width,
               UINT height,
               DXGI_FORMAT format,
               Result& result) {
  const bool bgra =
      format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
  const auto description = textureDescription(width, height, format);
  const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  ComObject<ID3D12Resource> target;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(target.put())),
        "Create texture without RTV flag");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes = 0;
  device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  ComObject<ID3D12Resource> baseline;
  createBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD, baseline.put());
  void* mapped = nullptr;
  const D3D12_RANGE noReads{0, 0};
  check(baseline->Map(0, &noReads, &mapped), "Map baseline");
  for (UINT y = 0; y < height; ++y) {
    auto* row = static_cast<unsigned char*>(mapped) + footprint.Offset + std::uint64_t(y) * footprint.Footprint.RowPitch;
    for (UINT x = 0; x < width; ++x) {
      std::memcpy(row + std::size_t(x) * 4, Baseline.data(), 4);
    }
  }
  const D3D12_RANGE writes{0, static_cast<SIZE_T>(bytes)};
  baseline->Unmap(0, &writes);

  std::array<ComObject<ID3D12Resource>, Frames.size()> readbacks;
  Submission commands(device, type);
  auto* list = commands.list();
  for (std::size_t index = 0; index < Frames.size(); ++index) {
    createBuffer(device, bytes, D3D12_HEAP_TYPE_READBACK, readbacks[index].put());
    D3D12_TEXTURE_COPY_LOCATION texture{};
    texture.pResource = target.get();
    texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION upload{};
    upload.pResource = baseline.get();
    upload.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    upload.PlacedFootprint = footprint;
    list->CopyTextureRegion(&texture, 0, 0, 0, &upload, nullptr);
    // Actual production helper, after an application-style copy. Both snapshots
    // are recorded BEFORE submission, so any mutable staging reuse corrupts the
    // first frame and fails the exact pixel check.
    require(calibration.record(list, device, target.get(), width, height, format, Frames[index]), "Valid copy calibration rejected");
    require(!calibration.record(list, device, target.get(), width + 1, height, format, 0), "Mismatching native dimensions accepted");
    require(!calibration.record(list, device, target.get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 0),
            "Unsupported format accepted");
    require(!calibration.record(nullptr, device, target.get(), width, height, format, 0), "Null command list accepted");
    transition(list, target.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION readback{};
    readback.pResource = readbacks[index].get();
    readback.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    readback.PlacedFootprint = footprint;
    list->CopyTextureRegion(&readback, 0, 0, 0, &texture, nullptr);
    if (index + 1 < Frames.size()) {
      transition(list, target.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
  }
  commands.finish();
  ++result.drains;
  check(device->GetDeviceRemovedReason(), "Device health after copies");
  for (std::size_t index = 0; index < Frames.size(); ++index) {
    const D3D12_RANGE reads{0, static_cast<SIZE_T>(bytes)};
    check(readbacks[index]->Map(0, &reads, &mapped), "Map copied pixels");
    for (UINT y = 0; y < height; ++y) {
      const auto* row = static_cast<const unsigned char*>(mapped) + footprint.Offset + std::uint64_t(y) * footprint.Footprint.RowPitch;
      for (UINT x = 0; x < width; ++x) {
        const auto expected = expectedPixel(width, height, x, y, Frames[index], bgra);
        if (std::memcmp(row + std::size_t(x) * 4, expected.data(), 4) != 0) {
          char message[256]{};
          std::snprintf(message, sizeof(message), "Pixel mismatch format=%u queue=%u frame=%llu x=%u y=%u", format, type,
                        static_cast<unsigned long long>(Frames[index]), x, y);
          throw std::runtime_error(message);
        }
        ++result.checkedPixels;
        if (y >= std::uint64_t(height) * 763 / 1024) {
          ++result.lowerPixels;
        }
      }
    }
    readbacks[index]->Unmap(0, &noReads);
    ++result.cases;
  }
}

void poolBounds(ID3D12Device* device, Result& result) {
  taxi_camera::CopyCalibration pool;
  require(taxi_camera::CopyCalibration::required_bytes(16384, 16384, DXGI_FORMAT_R8G8B8A8_UNORM) > pool.MaxBytes,
          "Large target should exceed upload budget");
  require(taxi_camera::CopyCalibration::required_bytes(31, 1024, DXGI_FORMAT_R8G8B8A8_UNORM) == 0, "Too-small width accepted");
  require(taxi_camera::CopyCalibration::required_bytes(768, 16385, DXGI_FORMAT_R8G8B8A8_UNORM) == 0, "Too-large height accepted");
  std::array<ComObject<ID3D12Resource>, taxi_camera::CopyCalibration::MaxEntries + 1> targets;
  Submission commands(device, D3D12_COMMAND_LIST_TYPE_COPY);
  const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  for (std::size_t index = 0; index < targets.size(); ++index) {
    const UINT width = 32 + static_cast<UINT>(index);
    const auto description = textureDescription(width, 32, DXGI_FORMAT_R8G8B8A8_UNORM);
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(targets[index].put())),
          "Create bounded pool target");
    const bool recorded = pool.record(commands.list(), device, targets[index].get(), width, 32, description.Format, 1);
    require(recorded == (index < pool.MaxEntries), "Immutable cache entry cap failed");
  }
  require(pool.entry_count() == pool.MaxEntries && pool.bytes_allocated() <= pool.MaxBytes, "Pool bounds exceeded");
  commands.finish();
  ++result.drains;
  pool.release();
  require(pool.entry_count() == 0 && pool.bytes_allocated() == 0, "Completed pool release failed");
}

Result run(bool forceWarp) {
  Result result;
  ComObject<ID3D12Debug> debug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())))) {
    debug->EnableDebugLayer();
    result.debugLayer = true;
  }
  ComObject<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "Create DXGI factory");
  ComObject<ID3D12Device> device;
  HRESULT created = E_FAIL;
  if (!forceWarp) {
    created = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put()));
  }
  ComObject<IDXGIAdapter> warp;
  if (FAILED(created)) {
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put())), "Get WARP adapter");
    check(D3D12CreateDevice(warp.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "Create WARP device");
    result.warp = true;
  }
  ComObject<ID3D12InfoQueue> messages;
  if (result.debugLayer && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(messages.put())))) {
    messages->ClearStoredMessages();
  }
  taxi_camera::CopyCalibration calibration;
  constexpr std::array<DXGI_FORMAT, 6> formats{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                               DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_TYPELESS};
  for (const auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COPY}) {
    for (const auto format : formats) {
      pixelCase(device.get(), calibration, type, 768, 1024, format, result);
    }
    // Odd dimensions exercise footprint pitch padding and scaled lower boundary.
    pixelCase(device.get(), calibration, type, 769, 1025, DXGI_FORMAT_R8G8B8A8_UNORM, result);
  }
  require(calibration.entry_count() == 3, "Equivalent typed formats or repeated frames allocated duplicate uploads");
  poolBounds(device.get(), result);
  calibration.release();
  if (messages.get()) {
    for (UINT64 index = 0; index < messages->GetNumStoredMessages(); ++index) {
      SIZE_T size = 0;
      check(messages->GetMessage(index, nullptr, &size), "Get debug message size");
      std::vector<unsigned char> storage(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      check(messages->GetMessage(index, message, &size), "Get debug message");
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
    std::fprintf(stderr, "Usage: taxi-camera-copy-validation.exe [--warp]\n");
    return 2;
  }
  try {
    const auto result = run(argc == 2);
    const bool passed = result.cases == 28 && result.debugErrors == 0 && result.drains == 15;
    std::printf(
        "{\"passed\":%s,\"adapter\":\"%s\",\"debug_layer\":%s,\"pixel_cases\":%llu,\"checked_pixels\":%llu,"
        "\"preserved_lower_pixels\":%llu,\"queues\":[\"direct\",\"copy\"],\"formats\":6,\"immutable_frames\":2,"
        "\"pool_bounds_passed\":true,\"debug_errors\":%llu,\"checked_queue_drains\":%llu}\n",
        passed ? "true" : "false", result.warp ? "warp" : "hardware", result.debugLayer ? "true" : "false",
        static_cast<unsigned long long>(result.cases), static_cast<unsigned long long>(result.checkedPixels),
        static_cast<unsigned long long>(result.lowerPixels), static_cast<unsigned long long>(result.debugErrors),
        static_cast<unsigned long long>(result.drains));
    return passed ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Copy validation failed: %s\n", error.what());
    return 1;
  }
}
