#pragma once
#include <d3d12.h>

namespace taxi_camera {
// Compare canonical COM identities of live D3D12 interfaces. Never treat a
// resource address or a foreign interface wrapper as proof of device ownership.

inline bool same_native_device(ID3D12DeviceChild* child, ID3D12Device* expected_native) noexcept {
  if (!child || !expected_native)
    return false;
  ID3D12Device* reported = nullptr;
  if (FAILED(child->GetDevice(IID_PPV_ARGS(&reported))) || !reported)
    return false;
  IUnknown* actual_identity = nullptr;
  IUnknown* expected_identity = nullptr;
  const bool equal = SUCCEEDED(reported->QueryInterface(IID_PPV_ARGS(&actual_identity))) &&
                     SUCCEEDED(expected_native->QueryInterface(IID_PPV_ARGS(&expected_identity))) && actual_identity &&
                     actual_identity == expected_identity;
  if (actual_identity)
    actual_identity->Release();
  if (expected_identity)
    expected_identity->Release();
  reported->Release();
  return equal;
}
}  // namespace taxi_camera
