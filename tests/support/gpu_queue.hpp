#pragma once

#include <d3d12.h>

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

}  // namespace taxi_camera
