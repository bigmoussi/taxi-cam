#pragma once

#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "calibration.hpp"

namespace taxi_camera {

// Shutdown only: the caller must prevent further submissions to this queue.
// Every fence/event failure is observable. On a
// failed or timed-out wait, retain the native fence and event until process exit:
// the GPU/driver may still reference both. The caller must then abandon the pool.
inline bool drain_copy_queue(ID3D12CommandQueue* queue, ID3D12Device* device) noexcept {
  if (!queue || !device || FAILED(device->GetDeviceRemovedReason())) {
    return false;
  }
  ID3D12Fence* fence = nullptr;
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
    return false;
  }
  const HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!completed) {
    fence->Release();
    return false;
  }
  if (FAILED(queue->Signal(fence, 1))) {
    // No event registration was made, but conservatively keep the fence alive
    // because the failed native call cannot establish a completed submission.
    CloseHandle(completed);
    return false;
  }
  if (FAILED(fence->SetEventOnCompletion(1, completed))) {
    return false;
  }
  const DWORD waited = WaitForSingleObject(completed, 30000);
  const UINT64 value = fence->GetCompletedValue();
  if (waited != WAIT_OBJECT_0 || value < 1 || value == UINT64_MAX || FAILED(device->GetDeviceRemovedReason())) {
    return false;
  }
  CloseHandle(completed);
  fence->Release();
  return true;
}

// Copies an explicit diagnostic pattern into a texture already in COPY_DEST.
// This does not create a camera feed or infer/modify application resource states.
// The caller must serialize record/release, keep the destination alive, and call
// only outside render passes after a known application copy to this subresource.
// Upload buffers are immutable after creation, so recorded lists can execute in
// any order or on different queues without an upload ring or per-frame fences.
// release/destruction requires completion of ALL lists that reference this pool.
class CopyCalibration {
 public:
  static constexpr std::uint64_t MaxBytes = 128ull * 1024 * 1024;
  static constexpr std::size_t MaxEntries = 16;

  CopyCalibration() = default;
  CopyCalibration(const CopyCalibration&) = delete;
  CopyCalibration& operator=(const CopyCalibration&) = delete;
  ~CopyCalibration() { release(); }

  std::uint64_t bytes_allocated() const noexcept { return bytes_; }
  std::size_t entry_count() const noexcept { return count_; }

  // Invalid/unsupported dimensions or formats return zero. A valid result may
  // exceed MaxBytes; callers can explain this refusal without allocating a GPU
  // resource. Includes the committed buffer's 64 KiB allocation granularity.
  static std::uint64_t required_bytes(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format) noexcept {
    Entry candidate{};
    return describe(candidate, width, height, format) ? candidate.bytes : 0;
  }

  void release() noexcept {
    for (std::size_t index = 0; index < count_; ++index) {
      entries_[index].upload->Release();
      entries_[index] = {};
    }
    count_ = 0;
    bytes_ = 0;
    device_ = nullptr;
  }

  // Exceptional shutdown only, after GPU completion could not be established.
  // Native references are intentionally quarantined until process termination;
  // releasing them here could free an upload buffer still used by the GPU.
  void abandon() noexcept {
    entries_ = {};
    count_ = 0;
    bytes_ = 0;
    device_ = nullptr;
  }

  bool record(ID3D12GraphicsCommandList* list,
              ID3D12Device* device,
              ID3D12Resource* destination,
              std::uint32_t width,
              std::uint32_t height,
              DXGI_FORMAT format,
              std::uint64_t frame) noexcept {
    if (!list || !device || !destination || (device_ && device_ != device)) {
      return false;
    }
    const auto list_type = list->GetType();
    if (list_type != D3D12_COMMAND_LIST_TYPE_DIRECT && list_type != D3D12_COMMAND_LIST_TYPE_COPY) {
      return false;
    }
    Entry description{};
    if (!describe(description, width, height, format) || description.bytes > MaxBytes) {
      return false;
    }
    // MinGW's COM headers explicitly implement Microsoft's aggregate-return
    // ABI for GetDesc, including its native COM struct-return convention.
    D3D12_RESOURCE_DESC actual{};
#if defined(__MINGW32__)
#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#error This MinGW build requires explicit Microsoft COM aggregate-return wrappers.
#endif
    destination->GetDesc(&actual);
#else
    actual = destination->GetDesc();
#endif
    if (actual.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || actual.Width != width || actual.Height != height ||
        actual.DepthOrArraySize != 1 || actual.MipLevels != 1 || actual.SampleDesc.Count != 1 || actual.SampleDesc.Quality != 0 ||
        actual.Format != format || (actual.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
      return false;
    }
    // Reserved (sparse) resources have no heap and fail GetHeapProperties.
    D3D12_HEAP_PROPERTIES target_heap{};
    D3D12_HEAP_FLAGS target_heap_flags{};
    if (FAILED(destination->GetHeapProperties(&target_heap, &target_heap_flags))) {
      return false;
    }

    Entry* entry = nullptr;
    for (std::size_t index = 0; index < count_; ++index) {
      if (entries_[index].width == width && entries_[index].height == height && entries_[index].format == description.format) {
        entry = &entries_[index];
        break;
      }
    }
    if (!entry) {
      if (count_ == MaxEntries || description.bytes > MaxBytes - bytes_ || !initialize(description, device)) {
        return false;
      }
      entries_[count_] = description;
      entry = &entries_[count_++];
      bytes_ += description.bytes;
      device_ = device;
    }

    const auto rectangles = calibration_rectangles(width, height, frame);
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = destination;
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.SubresourceIndex = 0;
    for (std::size_t index = 0; index < rectangles.size(); ++index) {
      const auto& rectangle = rectangles[index];
      D3D12_TEXTURE_COPY_LOCATION source{};
      source.pResource = entry->upload;
      source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      source.PlacedFootprint = entry->tiles[index];
      const D3D12_BOX box{0, 0, 0, static_cast<UINT>(rectangle.right - rectangle.left), static_cast<UINT>(rectangle.bottom - rectangle.top),
                          1};
      list->CopyTextureRegion(&target, static_cast<UINT>(rectangle.left), static_cast<UINT>(rectangle.top), 0, &source, &box);
    }
    return true;
  }

 private:
  struct Entry {
    ID3D12Resource* upload = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t bytes = 0;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 5> tiles{};
  };

  static constexpr std::uint64_t align(std::uint64_t value, std::uint64_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
  }

  static bool describe(Entry& entry, std::uint32_t width, std::uint32_t height, DXGI_FORMAT format) noexcept {
    if (width < 32 || width > 16384 || height < 32 || height > 16384) {
      return false;
    }
    switch (format) {
      case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        entry.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
      case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        entry.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
      default:
        return false;
    }
    entry.width = width;
    entry.height = height;
    const auto rectangles = calibration_rectangles(width, height, 0);
    std::uint64_t offset = 0;
    for (std::size_t index = 0; index < rectangles.size(); ++index) {
      const auto& rectangle = rectangles[index];
      auto& tile = entry.tiles[index];
      tile.Offset = align(offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      tile.Footprint.Format = entry.format;
      tile.Footprint.Width = static_cast<UINT>(rectangle.right - rectangle.left);
      tile.Footprint.Height = static_cast<UINT>(rectangle.bottom - rectangle.top);
      tile.Footprint.Depth = 1;
      tile.Footprint.RowPitch = static_cast<UINT>(align(std::uint64_t(tile.Footprint.Width) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
      offset = tile.Offset + std::uint64_t(tile.Footprint.RowPitch) * tile.Footprint.Height;
    }
    entry.bytes = align(offset, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    return true;
  }

  static bool initialize(Entry& entry, ID3D12Device* device) noexcept {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = entry.bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&entry.upload)))) {
      return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE no_reads{0, 0};
    if (FAILED(entry.upload->Map(0, &no_reads, &mapped))) {
      entry.upload->Release();
      entry.upload = nullptr;
      return false;
    }
    const auto rectangles = calibration_rectangles(entry.width, entry.height, 0);
    const bool bgra = entry.format == DXGI_FORMAT_B8G8R8A8_UNORM;
    for (std::size_t index = 0; index < rectangles.size(); ++index) {
      const auto& tile = entry.tiles[index];
      std::array<unsigned char, 4> pixel{};
      for (std::size_t channel = 0; channel < 4; ++channel) {
        pixel[channel] = static_cast<unsigned char>(rectangles[index].color[bgra && channel < 3 ? 2 - channel : channel] * 255.0f + 0.5f);
      }
      // Copies transfer encoded bytes; no shader or sRGB conversion is applied.
      for (UINT y = 0; y < tile.Footprint.Height; ++y) {
        auto* row = static_cast<unsigned char*>(mapped) + tile.Offset + std::uint64_t(y) * tile.Footprint.RowPitch;
        for (UINT x = 0; x < tile.Footprint.Width; ++x) {
          for (std::size_t channel = 0; channel < 4; ++channel) {
            row[std::size_t(x) * 4 + channel] = pixel[channel];
          }
        }
      }
    }
    const D3D12_RANGE writes{0, static_cast<SIZE_T>(entry.bytes)};
    entry.upload->Unmap(0, &writes);
    return true;
  }

  std::array<Entry, MaxEntries> entries_{};
  ID3D12Device* device_ = nullptr;
  std::uint64_t bytes_ = 0;
  std::size_t count_ = 0;
};

}  // namespace taxi_camera
