#include "resource_creation_observer.hpp"
#include "../src/native_device_identity.hpp"

#include <array>
#include <atomic>
#include <limits>
#include <tuple>
#include <type_traits>

namespace taxi_camera::engine_hook::resource_creation {
namespace {
using Model = source_state::Model;
constexpr std::array<unsigned, 10> Slots{27, 29, 30, 53, 55, 69, 70, 76, 77, 78};
constexpr std::array<unsigned, 10> DescArgs{2, 2, 0, 2, 0, 2, 2, 2, 2, 0};
constexpr std::array<unsigned, 10> StateArgs{3, 3, 1, 3, 1, 3, 3, 3, 3, 1};
struct Device {
  std::atomic<ID3D12Device*> proxy{nullptr};
  std::atomic<std::uint64_t> key{0};
};
struct Method {
  std::atomic<void*> original{nullptr};
  void** slot = nullptr;
  DWORD pending_protection = 0;
  bool installed = false;
};
std::array<Device, 4> devices;
std::array<Method, 10> methods;
SRWLOCK control = SRWLOCK_INIT;
std::atomic<bool> enabled{false};
void* saved_table = nullptr;
bool removed = false;
struct Lock {
  Lock() noexcept { AcquireSRWLockExclusive(&control); }
  ~Lock() { ReleaseSRWLockExclusive(&control); }
};
bool readable(DWORD p) noexcept {
  if (p & (PAGE_GUARD | PAGE_NOACCESS))
    return false;
  p &= 255;
  return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
         p == PAGE_EXECUTE_WRITECOPY;
}
bool region(const void* address, std::size_t size, MEMORY_BASIC_INFORMATION& info) noexcept {
  if (!address || VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT || !readable(info.Protect))
    return false;
  const auto first = reinterpret_cast<std::uintptr_t>(info.BaseAddress), at = reinterpret_cast<std::uintptr_t>(address);
  return info.RegionSize <= UINTPTR_MAX - first && at >= first && at <= first + info.RegionSize && size <= first + info.RegionSize - at;
}
bool read(const void* address, void* out, std::size_t size) noexcept {
  MEMORY_BASIC_INFORMATION info{};
  SIZE_T copied = 0;
  return region(address, size, info) && ReadProcessMemory(GetCurrentProcess(), address, out, size, &copied) && copied == size;
}
bool pin(const void* address, bool code) noexcept {
  MEMORY_BASIC_INFORMATION info{};
  if (!region(address, 1, info) || info.Type != MEM_IMAGE)
    return false;
  const auto p = info.Protect & 255;
  if (code && p != PAGE_EXECUTE_READ && p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY)
    return false;
  HMODULE module = nullptr;
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(address),
                            &module) != FALSE;
}
bool restore() noexcept {
  bool success = true;
  for (auto& method : methods) {
    if (!method.pending_protection)
      continue;
    DWORD ignored = 0;
    if (VirtualProtect(method.slot, sizeof(void*), method.pending_protection, &ignored))
      method.pending_protection = 0;
    else
      success = false;
  }
  return success;
}
Result result(const char* status, bool ready = false) noexcept {
  Result value;
  value.status = status;
  value.ready = ready;
  for (const auto& method : methods) {
    value.hooked_methods += method.installed;
    value.protection_restored &= method.pending_protection == 0;
  }
  return value;
}
std::uint64_t key_for(ID3D12Device* proxy) noexcept {
  for (const auto& device : devices)
    if (device.proxy.load(std::memory_order_acquire) == proxy) {
      const auto key = device.key.load(std::memory_order_acquire);
      if (device.proxy.load(std::memory_order_acquire) == proxy)
        return key;
    }
  return 0;
}
bool description(const D3D12_RESOURCE_DESC* input, D3D12_RESOURCE_DESC& output) noexcept {
  return read(input, &output, sizeof(output));
}
bool description(const D3D12_RESOURCE_DESC1* input, D3D12_RESOURCE_DESC& output) noexcept {
  D3D12_RESOURCE_DESC1 value{};
  if (!read(input, &value, sizeof(value)))
    return false;
  output = {value.Dimension, value.Alignment, value.Width,      value.Height, value.DepthOrArraySize,
            value.MipLevels, value.Format,    value.SampleDesc, value.Layout, value.Flags};
  return true;
}
bool equal(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) noexcept {
  // D3D12_RESOURCE_DESC documents zero as the runtime default. GetDesc may
  // materialize 64KiB for a single-sample texture; admit only that exact case.
  // https://learn.microsoft.com/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc
  const bool alignment =
      a.Alignment == b.Alignment || (a.Alignment == 0 && a.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && a.SampleDesc.Count == 1 &&
                                     b.Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
  return a.Dimension == b.Dimension && alignment && a.Width == b.Width && a.Height == b.Height &&
         a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels && a.Format == b.Format &&
         a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality && a.Layout == b.Layout &&
         a.Flags == b.Flags;
}
Model model(D3D12_RESOURCE_STATES state) noexcept {
  return state == D3D12_RESOURCE_STATE_RENDER_TARGET ? Model::legacy_rt : Model::unknown;
}
Model model(D3D12_BARRIER_LAYOUT layout) noexcept {
  return layout == D3D12_BARRIER_LAYOUT_RENDER_TARGET ? Model::enhanced_rt : Model::unknown;
}
struct Context {
  ID3D12Device* proxy = nullptr;
  std::uint64_t key = 0;
  void** output = nullptr;
  D3D12_RESOURCE_DESC desc{};
  Model initial = Model::unknown;
};
thread_local const Context* current = nullptr;
struct Scope {
  Context value;
  const Context* previous = current;
  template <class Desc, class State>
  Scope(ID3D12Device* proxy, const Desc* desc, State state, void** output) noexcept {
    if (enabled.load(std::memory_order_acquire) && output && description(desc, value.desc)) {
      value.proxy = proxy;
      value.key = key_for(proxy);
      value.output = output;
      value.initial = model(state);
    }
    // Even an unknown nested creation hides its caller's context until return.
    current = &value;
  }
  ~Scope() { current = previous; }
};
template <unsigned Index, class Signature>
struct Wrapper;
template <unsigned Index, class C, class... Args>
struct Wrapper<Index, HRESULT (STDMETHODCALLTYPE C::*)(Args...)> {
  using Function = HRESULT(STDMETHODCALLTYPE*)(C*, Args...);
  static HRESULT STDMETHODCALLTYPE call(C* self, Args... args) noexcept {
    const auto tuple = std::forward_as_tuple(args...);
    Scope scope(reinterpret_cast<ID3D12Device*>(self), std::get<DescArgs[Index]>(tuple), std::get<StateArgs[Index]>(tuple),
                std::get<sizeof...(Args) - 1>(tuple));
    return reinterpret_cast<Function>(methods[Index].original.load(std::memory_order_acquire))(self, args...);
  }
};
// The compiler derives every complete parameter list from the public COM
// declaration, including REFIID/reference and extended castable-format arguments.
const std::array<void*, 10> Wrappers{reinterpret_cast<void*>(&Wrapper<0, decltype(&ID3D12Device::CreateCommittedResource)>::call),
                                     reinterpret_cast<void*>(&Wrapper<1, decltype(&ID3D12Device::CreatePlacedResource)>::call),
                                     reinterpret_cast<void*>(&Wrapper<2, decltype(&ID3D12Device::CreateReservedResource)>::call),
                                     reinterpret_cast<void*>(&Wrapper<3, decltype(&ID3D12Device4::CreateCommittedResource1)>::call),
                                     reinterpret_cast<void*>(&Wrapper<4, decltype(&ID3D12Device4::CreateReservedResource1)>::call),
                                     reinterpret_cast<void*>(&Wrapper<5, decltype(&ID3D12Device8::CreateCommittedResource2)>::call),
                                     reinterpret_cast<void*>(&Wrapper<6, decltype(&ID3D12Device8::CreatePlacedResource1)>::call),
                                     reinterpret_cast<void*>(&Wrapper<7, decltype(&ID3D12Device10::CreateCommittedResource3)>::call),
                                     reinterpret_cast<void*>(&Wrapper<8, decltype(&ID3D12Device10::CreatePlacedResource2)>::call),
                                     reinterpret_cast<void*>(&Wrapper<9, decltype(&ID3D12Device10::CreateReservedResource2)>::call)};
bool interface_is_same(ID3D12Device* proxy, REFIID iid, bool& supported) noexcept {
  IUnknown* object = nullptr;
  const auto hr = proxy->QueryInterface(iid, reinterpret_cast<void**>(&object));
  supported = SUCCEEDED(hr) && object;
  const bool valid = supported ? reinterpret_cast<void*>(object) == proxy : hr == E_NOINTERFACE && !object;
  if (object)
    object->Release();
  return valid;
}
bool native_identity(ID3D12Device* proxy, ID3D12Device* expected) noexcept {
  if (!proxy || !expected || proxy == expected)
    return false;
  IUnknown *unwrapped = nullptr, *actual_id = nullptr, *expected_id = nullptr;
  const bool ok = SUCCEEDED(proxy->QueryInterface(ReShadeUnwrappedObject, reinterpret_cast<void**>(&unwrapped))) && unwrapped &&
                  SUCCEEDED(unwrapped->QueryInterface(IID_PPV_ARGS(&actual_id))) && actual_id &&
                  SUCCEEDED(expected->QueryInterface(IID_PPV_ARGS(&expected_id))) && expected_id && actual_id == expected_id;
  if (expected_id)
    expected_id->Release();
  if (actual_id)
    actual_id->Release();
  if (unwrapped)
    unwrapped->Release();
  return ok;
}
}  // namespace

Result register_device(std::uint64_t key, ID3D12Device* proxy, ID3D12Device* expected_native) noexcept {
  const Lock lock;
  if (!restore())
    return result("protection_restore_failed");
  if (removed)
    return result("installation_consumed");
  if (!key || !native_identity(proxy, expected_native))
    return result("proxy_identity_mismatch");
  bool base = false, device4 = false, device8 = false, device10 = false;
  if (!interface_is_same(proxy, __uuidof(ID3D12Device), base) || !base || !interface_is_same(proxy, __uuidof(ID3D12Device4), device4) ||
      !interface_is_same(proxy, __uuidof(ID3D12Device8), device8) || !interface_is_same(proxy, __uuidof(ID3D12Device10), device10))
    return result("interface_pointer_mismatch");
  if ((device10 && (!device8 || !device4)) || (device8 && !device4))
    return result("interface_support_mismatch");
  Device* selected = nullptr;
  for (auto& device : devices) {
    const auto old_key = device.key.load(std::memory_order_acquire);
    const auto old_proxy = device.proxy.load(std::memory_order_acquire);
    if (old_key == key || old_proxy == proxy) {
      if (old_key != key || old_proxy != proxy)
        return result("device_registration_mismatch");
      selected = &device;
      break;
    }
    if (!old_key && !old_proxy && !selected)
      selected = &device;
  }
  if (!selected)
    return result("device_limit");
  void* table = nullptr;
  if (!read(proxy, &table, sizeof(table)) || !table || reinterpret_cast<std::uintptr_t>(table) % 8 ||
      reinterpret_cast<std::uintptr_t>(table) > UINTPTR_MAX - (Slots.back() + 1) * sizeof(void*) || !pin(table, false))
    return result("invalid_vtable");
  if (saved_table && saved_table != table)
    return result("different_vtable");
  const unsigned count = device10 ? 10 : device8 ? 7 : device4 ? 5 : 3;
  enabled.store(false, std::memory_order_release);
  for (unsigned i = 0; i < count; ++i) {
    auto& method = methods[i];
    auto* slot = reinterpret_cast<void**>(table) + Slots[i];
    MEMORY_BASIC_INFORMATION info{};
    void* before = nullptr;
    if (!region(slot, sizeof(void*), info) || info.Type != MEM_IMAGE ||
        ((info.Protect & 255) != PAGE_READONLY && (info.Protect & 255) != PAGE_READWRITE && (info.Protect & 255) != PAGE_WRITECOPY) ||
        !read(slot, &before, sizeof(before)))
      return result("invalid_slot_memory");
    if (method.installed) {
      if (method.slot != slot || before != Wrappers[i])
        return result("slot_changed");
      continue;
    }
    if (method.slot && (method.slot != slot || before != method.original.load(std::memory_order_acquire)))
      return result("slot_changed");
    if (!pin(before, true) || !pin(Wrappers[i], true))
      return result("code_pin_failed");
    saved_table = table;
    method.slot = slot;
    method.original.store(before, std::memory_order_release);
    DWORD prior = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prior))
      return result("protection_change_failed");
    method.pending_protection = prior;
    const auto observed = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), Wrappers[i], before);
    method.installed = observed == before;
    if (!restore())
      return result("protection_restore_failed");
    if (!method.installed)
      return result("slot_changed");
  }
  selected->key.store(key, std::memory_order_release);
  selected->proxy.store(proxy, std::memory_order_release);
  enabled.store(true, std::memory_order_release);
  return result("ready", true);
}

void unregister_device(std::uint64_t key) noexcept {
  const Lock lock;
  for (auto& device : devices)
    if (device.key.load(std::memory_order_acquire) == key) {
      device.key.store(0, std::memory_order_release);
      device.proxy.store(nullptr, std::memory_order_release);
    }
}

Model initial_model(std::uint64_t key, ID3D12Resource* resource, const D3D12_RESOURCE_DESC& actual) noexcept {
  if (!enabled.load(std::memory_order_acquire) || !key || !resource || !current || current->key != key || key_for(current->proxy) != key ||
      !equal(current->desc, actual))
    return Model::unknown;
  void* output = nullptr;
  if (!read(current->output, &output, sizeof(output)) || output != resource)
    return Model::unknown;
  return current->initial;
}

Result remove() noexcept {
  const Lock lock;
  enabled.store(false, std::memory_order_release);
  removed = true;
  if (!restore())
    return result("protection_restore_failed");
  for (unsigned i = 0; i < methods.size(); ++i) {
    auto& method = methods[i];
    if (!method.installed)
      continue;
    DWORD prior = 0;
    if (!VirtualProtect(method.slot, sizeof(void*), PAGE_READWRITE, &prior))
      return result("protection_change_failed");
    method.pending_protection = prior;
    const auto observed = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(method.slot),
                                                            method.original.load(std::memory_order_acquire), Wrappers[i]);
    if (observed == Wrappers[i])
      method.installed = false;
    if (!restore())
      return result("protection_restore_failed");
    if (method.installed)
      return result("slot_changed");
  }
  return result("removed");
}
}  // namespace taxi_camera::engine_hook::resource_creation
