#include "../../src/graphics/native_device_identity.hpp"

#include <dxgi1_4.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
using namespace taxi_camera;
unsigned checks = 0;

void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}

// Stack-owned IUnknown objects model only the optional COM protocol. They never
// pretend to implement ID3D12Device or any other larger interface.
struct Unknown final : IUnknown {
  ULONG references = 1;
  unsigned additions = 0, releases = 0, unwrap_queries = 0;
  IUnknown* next = nullptr;
  IUnknown* canonical = nullptr;
  HRESULT unwrap_result = E_NOINTERFACE;
  HRESULT identity_result = S_OK;
  bool null_identity = false;
  bool value_on_failure = false;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
    if (!result)
      return E_POINTER;
    *result = nullptr;
    HRESULT response = S_OK;
    if (iid == IID_IUnknown) {
      response = identity_result;
      if ((FAILED(identity_result) && !value_on_failure) || null_identity)
        return identity_result;
      *result = canonical ? canonical : static_cast<IUnknown*>(this);
    } else if (iid == UnwrappedObjectId) {
      ++unwrap_queries;
      response = unwrap_result;
      if ((FAILED(unwrap_result) && !value_on_failure) || !next)
        return unwrap_result;
      *result = next;
    } else {
      return E_NOINTERFACE;
    }
    static_cast<IUnknown*>(*result)->AddRef();
    return response;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    ++additions;
    return ++references;
  }
  ULONG STDMETHODCALLTYPE Release() override {
    require(references > 1, "Resolver released the caller's last reference");
    ++releases;
    return --references;
  }
  void wrap(IUnknown& target) {
    next = &target;
    unwrap_result = S_OK;
  }
  void balanced() const { require(references == 1 && additions == releases, "Resolver leaked or consumed a COM reference"); }
};

void native_and_alias_identity() {
  Unknown native, alias;
  IUnknown* resolved = nullptr;
  require(resolve_unwrapped_object(&native, &resolved) && resolved == &native, "An ordinary E_NOINTERFACE endpoint was not returned");
  require(native.references == 2, "Returned endpoint did not own its reference");
  resolved->Release();
  native.balanced();

  alias.canonical = &native;
  resolved = nullptr;
  require(resolve_unwrapped_object(&alias, &resolved) && resolved == &native, "Resolver did not return canonical IUnknown identity");
  resolved->Release();
  native.balanced();
  alias.balanced();
}

void chains_and_bounds() {
  for (unsigned hops = 1; hops <= MaxDeviceProxyHops + 1; ++hops) {
    std::array<Unknown, MaxDeviceProxyHops + 2> chain;
    for (unsigned index = 0; index < hops; ++index)
      chain[index].wrap(chain[index + 1]);
    IUnknown* resolved = nullptr;
    const bool success = resolve_unwrapped_object(&chain[0], &resolved);
    if (hops <= MaxDeviceProxyHops) {
      require(success && resolved == &chain[hops], "An allowed proxy chain failed to reach its native identity");
      require(chain[hops].references == 2, "Resolved proxy endpoint did not retain ownership");
      resolved->Release();
    } else {
      require(!success && resolved == nullptr, "A chain beyond the fixed proxy bound was accepted");
    }
    unsigned queries = 0;
    for (const auto& item : chain) {
      queries += item.unwrap_queries;
      item.balanced();
    }
    require(queries <= MaxDeviceProxyHops + 1, "Optional COM queries exceeded the fixed chain bound");
  }
}

void cycles() {
  Unknown self;
  self.wrap(self);
  IUnknown* resolved = nullptr;
  require(!resolve_unwrapped_object(&self, &resolved) && !resolved, "A self-returning proxy was accepted");
  require(self.unwrap_queries <= MaxDeviceProxyHops + 1, "A self-cycle escaped the fixed query bound");
  self.balanced();

  Unknown first, second;
  first.wrap(second);
  second.wrap(first);
  require(!resolve_unwrapped_object(&first, &resolved) && !resolved, "A multi-object cycle was accepted");
  require(first.unwrap_queries + second.unwrap_queries <= MaxDeviceProxyHops + 1, "A proxy cycle escaped the fixed query bound");
  first.balanced();
  second.balanced();

  Unknown alias;
  alias.canonical = &first;
  first.wrap(alias);
  require(!resolve_unwrapped_object(&first, &resolved) && !resolved, "An alias to an earlier canonical identity bypassed cycle refusal");
  first.balanced();
  alias.balanced();
}

void query_refusals() {
  for (const auto failure : {E_FAIL, E_ACCESSDENIED, E_NOTIMPL, E_POINTER}) {
    Unknown proxy;
    proxy.unwrap_result = failure;
    IUnknown* resolved = nullptr;
    require(!resolve_unwrapped_object(&proxy, &resolved) && !resolved, "An optional-query error was treated as an absent extension");
    proxy.balanced();
  }
  {
    Unknown proxy;
    proxy.unwrap_result = S_OK;
    IUnknown* resolved = nullptr;
    require(!resolve_unwrapped_object(&proxy, &resolved) && !resolved, "A successful null unwrap result was accepted");
    proxy.balanced();
  }
  for (const auto failure : {E_NOINTERFACE, E_FAIL}) {
    Unknown proxy, endpoint;
    proxy.wrap(endpoint);
    proxy.unwrap_result = failure;
    proxy.value_on_failure = true;
    IUnknown* resolved = nullptr;
    require(!resolve_unwrapped_object(&proxy, &resolved) && !resolved, "A failed optional query with a nonnull result was accepted");
    proxy.balanced();
    endpoint.balanced();
  }
  for (const bool null_success : {false, true}) {
    Unknown proxy, endpoint;
    proxy.wrap(endpoint);
    endpoint.identity_result = null_success ? S_OK : E_FAIL;
    endpoint.null_identity = null_success;
    IUnknown* resolved = nullptr;
    require(!resolve_unwrapped_object(&proxy, &resolved) && !resolved, "An unavailable canonical identity was accepted");
    proxy.balanced();
    endpoint.balanced();
  }
  {
    Unknown proxy, endpoint;
    proxy.wrap(endpoint);
    endpoint.identity_result = E_FAIL;
    endpoint.value_on_failure = true;
    IUnknown* resolved = nullptr;
    require(!resolve_unwrapped_object(&proxy, &resolved) && !resolved, "A failed canonical query with a nonnull result was accepted");
    proxy.balanced();
    endpoint.balanced();
  }
  {
    Unknown native;
    IUnknown* resolved = &native;
    require(!resolve_unwrapped_object(nullptr, &resolved) && !resolved, "Null input was accepted or left a stale output");
    require(!resolve_unwrapped_object(&native, nullptr), "Null output pointer was accepted");
    native.balanced();
  }
}

void equality_and_typed_refusal() {
  Unknown native, unrelated, first, second;
  first.wrap(native);
  second.wrap(native);
  require(same_device_identity(&native, &native), "A native identity did not match itself");
  require(same_device_identity(&first, &native) && same_device_identity(&native, &first), "Proxy/native comparison was not symmetric");
  require(same_device_identity(&first, &second), "Two proxies over the same object did not agree");
  require(!same_device_identity(&first, &unrelated), "An unrelated object passed device identity comparison");
  require(!same_device_identity(nullptr, &native) && !same_device_identity(&native, nullptr), "Null identities were accepted");
  second.unwrap_result = E_FAIL;
  require(!same_device_identity(&first, &second) && !same_device_identity(&second, &first),
          "A failed resolution fell back to proxy identity equality");
  require(!same_device_identity(&second, &second), "Pointer equality bypassed a failed unwrap protocol");
  ID3D12Device* typed = nullptr;
  require(!resolve_native_device(&first, &typed) && !typed, "IUnknown-only endpoint was cast to ID3D12Device without public QI");
  require(!resolve_native_device(nullptr, &typed) && !typed, "Null device input was accepted");
  require(!resolve_native_device(&native, nullptr), "Null typed output pointer was accepted");
  for (const auto* item : {&native, &unrelated, &first, &second})
    item->balanced();
}

// Optional integration coverage uses actual WARP interfaces; the CPU harness
// above deliberately supplies no fake ID3D12Device implementation or casts.
void warp_device() {
  IDXGIFactory4* factory = nullptr;
  IDXGIAdapter* adapter = nullptr;
  ID3D12Device* device = nullptr;
  require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory, "Could not create WARP factory");
  require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) && adapter, "Could not select WARP adapter");
  require(SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) && device,
          "Could not create real WARP device");
  adapter->Release();
  factory->Release();

  Unknown proxy;
  proxy.wrap(*device);
  for (unsigned pass = 0; pass < 3; ++pass) {
    ID3D12Device* resolved = nullptr;
    require(resolve_native_device(&proxy, &resolved) && resolved, "Valid proxy did not return a genuine typed device");
    require(same_device_identity(device, resolved), "Genuine resolved device has a different COM identity");
    require(SUCCEEDED(resolved->GetDeviceRemovedReason()), "Resolved typed device could not execute its public method");
    resolved->Release();
    require(SUCCEEDED(device->GetDeviceRemovedReason()), "Resolver consumed the caller-owned device reference");
    proxy.balanced();
  }
  ID3D12CommandAllocator* allocator = nullptr;
  require(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) && allocator,
          "Could not create genuine device child");
  require(same_native_device(allocator, device), "A genuine child failed device identity verification");
  require(!same_native_device(nullptr, device) && !same_native_device(allocator, nullptr), "Null child/device ownership was accepted");
  allocator->Release();

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC description{};
  description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  description.Width = 256;
  description.Height = description.DepthOrArraySize = description.MipLevels = 1;
  description.SampleDesc.Count = 1;
  description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* resource = nullptr;
  require(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&resource))) &&
              resource,
          "Could not create genuine resource");
  require(same_native_device(resource, device), "A genuine resource's GetDevice identity was rejected");
  resource->Release();
  device->Release();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--warp"))) {
    std::fprintf(stderr, "Usage: native-device-identity-test [--warp]\n");
    return 2;
  }
  native_and_alias_identity();
  chains_and_bounds();
  cycles();
  query_refusals();
  equality_and_typed_refusal();
  if (argc == 2)
    warp_device();
  std::printf("PASS native device identity: %u checks, bounded COM unwrapping and reference ownership%s.\n", checks,
              argc == 2 ? ", genuine WARP device" : "");
}
