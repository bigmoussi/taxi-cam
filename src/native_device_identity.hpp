#pragma once
#include <d3d12.h>

namespace taxi_camera {
// ReShade 6.8 substitutes its proxy in ID3D12Resource::GetDevice even for
// resources created through the native device. Its checked-in COM escape IID
// returns an AddRef'd original object (source/com_utils.hpp:137 and
// source/d3d12/d3d12_device.cpp:117, pinned commit18deaa52).
// All arguments here are actual live public COM interfaces, never engine-read
// addresses. Unknown native implementations simply return E_NOINTERFACE.
inline constexpr GUID ReShadeUnwrappedObject = {0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};

inline bool same_native_device(ID3D12DeviceChild* child, ID3D12Device* expected_native) noexcept {
  if (!child || !expected_native)
    return false;
  ID3D12Device* reported = nullptr;
  if (FAILED(child->GetDevice(IID_PPV_ARGS(&reported))) || !reported)
    return false;
  IUnknown* unwrapped = nullptr;
  const auto result = reported->QueryInterface(ReShadeUnwrappedObject, reinterpret_cast<void**>(&unwrapped));
  if ((FAILED(result) && result != E_NOINTERFACE) || (SUCCEEDED(result) && !unwrapped)) {
    if (unwrapped)
      unwrapped->Release();
    reported->Release();
    return false;
  }
  IUnknown* actual_identity = nullptr;
  IUnknown* expected_identity = nullptr;
  auto* actual = unwrapped ? unwrapped : static_cast<IUnknown*>(reported);
  const bool equal = SUCCEEDED(actual->QueryInterface(IID_PPV_ARGS(&actual_identity))) &&
                     SUCCEEDED(expected_native->QueryInterface(IID_PPV_ARGS(&expected_identity))) && actual_identity &&
                     actual_identity == expected_identity;
  if (actual_identity)
    actual_identity->Release();
  if (expected_identity)
    expected_identity->Release();
  if (unwrapped)
    unwrapped->Release();
  reported->Release();
  return equal;
}
}  // namespace taxi_camera
