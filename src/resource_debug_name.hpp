#pragma once

#include <d3d12.h>
#include <array>

namespace taxi_camera {

// WKPDID_D3DDebugObjectNameW is absent from the pinned MinGW d3dcommon.h.
// Exact declaration: Microsoft DirectX-Headers v1.4.9, include/directx/d3dcommon.h
// https://chromium.googlesource.com/external/github.com/microsoft/DirectX-Headers/+/refs/tags/upstream/v1.4.9/include/directx/d3dcommon.h
// SetName stores this key: https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12object-setname
inline constexpr GUID D3DDebugObjectNameWide = {0x4cca5fd8, 0x921f, 0x42c8, {0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41}};

// The caller supplies an actual live public COM object with a valid lifetime.
// Optional metadata only: at most two bounded reads, no resource-state changes.
inline bool read_resource_debug_name(ID3D12Object* object, std::array<char, 256>& name) noexcept {
  name.fill(0);
  if (!object)
    return false;
  UINT bytes = static_cast<UINT>(name.size() - 1);
  if (SUCCEEDED(object->GetPrivateData(WKPDID_D3DDebugObjectName, &bytes, name.data())) && bytes < name.size()) {
    name[bytes] = 0;
    if (name[0] != 0)
      return true;
  }
  name.fill(0);
  std::array<wchar_t, 256> wide{};
  bytes = static_cast<UINT>((wide.size() - 1) * sizeof(wchar_t));
  if (FAILED(object->GetPrivateData(D3DDebugObjectNameWide, &bytes, wide.data())) || bytes == 0 ||
      bytes > (wide.size() - 1) * sizeof(wchar_t) || bytes % sizeof(wchar_t) != 0)
    return false;
  const auto written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(bytes / sizeof(wchar_t)),
                                           name.data(), static_cast<int>(name.size() - 1), nullptr, nullptr);
  if (written <= 0) {
    name.fill(0);
    return false;
  }
  name[static_cast<std::size_t>(written)] = 0;
  return name[0] != 0;
}

}  // namespace taxi_camera
