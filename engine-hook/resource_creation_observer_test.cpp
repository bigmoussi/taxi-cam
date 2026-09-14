// Isolated scope/ABI test: include the exact implementation, replace only saved
// originals with local typed fixtures. No simulator objects or vtable patches.
#include "resource_creation_observer.cpp"
#include <cstdio>
#include <cstdlib>

namespace taxi_camera::engine_hook::resource_creation {
namespace {
unsigned checks = 0, forwards = 0;
std::uint64_t expected_hash = 0;
ID3D12Device* expected_self = nullptr;
ID3D12Resource* const fixture_resource = reinterpret_cast<ID3D12Resource*>(0x40000);
constexpr std::uint64_t FixtureKey = 711;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}
template <class T>
std::uint64_t argument(T value) {
  if constexpr (std::is_pointer_v<T>)
    return reinterpret_cast<std::uint64_t>(value);
  else
    return static_cast<std::uint64_t>(value);
}
std::uint64_t argument(const GUID& value) {
  return value.Data1 ^ (std::uint64_t(value.Data2) << 32) ^ (std::uint64_t(value.Data3) << 48) ^ value.Data4[3];
}
template <class... Args>
std::uint64_t hash(Args... args) {
  std::uint64_t result = 0xcbf29ce484222325ull;
  ((result = (result ^ argument(args)) * 0x100000001b3ull), ...);
  return result;
}
template <unsigned Index, class Signature>
struct Probe;
template <unsigned Index, class C, class... Args>
struct Probe<Index, HRESULT (STDMETHODCALLTYPE C::*)(Args...)> {
  static HRESULT STDMETHODCALLTYPE original(C* self, Args... args) {
    ++forwards;
    require(reinterpret_cast<ID3D12Device*>(self) == expected_self && hash(args...) == expected_hash,
            "Wrapper changed documented COM arguments");
    const auto tuple = std::forward_as_tuple(args...);
    auto** output = std::get<sizeof...(Args) - 1>(tuple);
    D3D12_RESOURCE_DESC desc{};
    require(description(std::get<DescArgs[Index]>(tuple), desc), "Fixture description unreadable");
    const auto wanted = model(std::get<StateArgs[Index]>(tuple));
    require(initial_model(FixtureKey, fixture_resource, desc) == Model::unknown, "Unassigned output supplied creation authority");
    *output = fixture_resource;
    require(initial_model(FixtureKey, fixture_resource, desc) == wanted, "Actual init-event scope did not match exact creation");
    require(initial_model(FixtureKey + 1, fixture_resource, desc) == Model::unknown, "Wrong device key accepted");
    require(initial_model(FixtureKey, reinterpret_cast<ID3D12Resource*>(0x40008), desc) == Model::unknown, "Wrong output accepted");
    auto changed = desc;
    ++changed.Width;
    require(initial_model(FixtureKey, fixture_resource, changed) == Model::unknown, "Changed description accepted");
    void* nested_output = reinterpret_cast<void*>(0x50000);
    {
      Scope nested(reinterpret_cast<ID3D12Device*>(0x60000), &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &nested_output);
      require(initial_model(FixtureKey, fixture_resource, desc) == Model::unknown, "Unregistered nested creation leaked outer authority");
    }
    require(initial_model(FixtureKey, fixture_resource, desc) == wanted, "Nested scope failed to restore caller context");
    {
      void* inner_resource = reinterpret_cast<void*>(0x70000);
      Scope nested(expected_self, &desc, D3D12_BARRIER_LAYOUT_RENDER_TARGET, &inner_resource);
      require(initial_model(FixtureKey, reinterpret_cast<ID3D12Resource*>(0x70000), desc) == Model::enhanced_rt,
              "Registered nested creation was not independently scoped");
      require(initial_model(FixtureKey, fixture_resource, desc) == Model::unknown, "Nested call reused the outer output");
    }
    require(initial_model(FixtureKey, fixture_resource, desc) == wanted, "Registered nesting did not restore outer context");
    return static_cast<HRESULT>(0x00007123 + Index);
  }
  static void run(C* self, Args... args) {
    const auto tuple = std::forward_as_tuple(args...);
    *std::get<sizeof...(Args) - 1>(tuple) = nullptr;
    expected_self = reinterpret_cast<ID3D12Device*>(self);
    expected_hash = hash(args...);
    methods[Index].original.store(reinterpret_cast<void*>(&original), std::memory_order_release);
    const auto before = forwards;
    const auto hr = Wrapper<Index, HRESULT (STDMETHODCALLTYPE C::*)(Args...)>::call(self, args...);
    require(hr == static_cast<HRESULT>(0x00007123 + Index) && forwards == before + 1,
            "Original call/result was not forwarded exactly once");
    D3D12_RESOURCE_DESC desc{};
    require(description(std::get<DescArgs[Index]>(tuple), desc), "Postcall fixture description unreadable");
    require(initial_model(FixtureKey, fixture_resource, desc) == Model::unknown, "Completed creation left stale scope authority");
  }
};
void run_tests() {
  auto* proxy = reinterpret_cast<ID3D12Device*>(0x30000);
  devices[0].key.store(FixtureKey);
  devices[0].proxy.store(proxy);
  enabled.store(true);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 768;
  desc.Height = 255;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_RESOURCE_DESC1 desc1{desc.Dimension,  desc.Alignment, desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels, desc.Format,
                             desc.SampleDesc, desc.Layout,    desc.Flags, {0, 0, 0}};
  D3D12_HEAP_PROPERTIES properties{};
  D3D12_CLEAR_VALUE clear{};
  void* output = nullptr;
  auto* placed_heap = reinterpret_cast<ID3D12Heap*>(0x81200);
  auto* protected_session = reinterpret_cast<ID3D12ProtectedResourceSession*>(0x93400);
  DXGI_FORMAT formats[]{DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R32_UINT};
  constexpr auto flags = D3D12_HEAP_FLAG_SHARED;
  constexpr auto rt = D3D12_RESOURCE_STATE_RENDER_TARGET;
  constexpr auto enhanced_rt = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
  const auto& iid = __uuidof(ID3D12Resource);
#define CHECK_METHOD(N, TYPE, METHOD, ...) Probe<N, decltype(&TYPE::METHOD)>::run(reinterpret_cast<TYPE*>(proxy), __VA_ARGS__)
  CHECK_METHOD(0, ID3D12Device, CreateCommittedResource, &properties, flags, &desc, rt, &clear, iid, &output);
  CHECK_METHOD(1, ID3D12Device, CreatePlacedResource, placed_heap, 0x123456789ull, &desc, rt, &clear, iid, &output);
  CHECK_METHOD(2, ID3D12Device, CreateReservedResource, &desc, rt, &clear, iid, &output);
  CHECK_METHOD(3, ID3D12Device4, CreateCommittedResource1, &properties, flags, &desc, rt, &clear, protected_session, iid, &output);
  CHECK_METHOD(4, ID3D12Device4, CreateReservedResource1, &desc, rt, &clear, protected_session, iid, &output);
  CHECK_METHOD(5, ID3D12Device8, CreateCommittedResource2, &properties, flags, &desc1, rt, &clear, protected_session, iid, &output);
  CHECK_METHOD(6, ID3D12Device8, CreatePlacedResource1, placed_heap, 0xabcdeffffffffull, &desc1, rt, &clear, iid, &output);
  CHECK_METHOD(7, ID3D12Device10, CreateCommittedResource3, &properties, flags, &desc1, enhanced_rt, &clear, protected_session, 2, formats,
               iid, &output);
  CHECK_METHOD(8, ID3D12Device10, CreatePlacedResource2, placed_heap, 0xaabbccddeeffull, &desc1, enhanced_rt, &clear, 2, formats, iid,
               &output);
  CHECK_METHOD(9, ID3D12Device10, CreateReservedResource2, &desc, enhanced_rt, &clear, protected_session, 2, formats, iid, &output);
  CHECK_METHOD(0, ID3D12Device, CreateCommittedResource, &properties, flags, &desc, D3D12_RESOURCE_STATE_COMMON, &clear, iid, &output);
  CHECK_METHOD(7, ID3D12Device10, CreateCommittedResource3, &properties, flags, &desc1, D3D12_BARRIER_LAYOUT_COMMON, &clear,
               protected_session, 2, formats, iid, &output);
#undef CHECK_METHOD
  output = fixture_resource;
  {
    Scope scope(proxy, &desc, rt, &output);
    require(initial_model(FixtureKey, fixture_resource, desc) == Model::legacy_rt, "Lifetime fixture failed");
    auto normalized = desc;
    normalized.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    require(initial_model(FixtureKey, fixture_resource, normalized) == Model::legacy_rt,
            "Documented zero-to-default texture alignment normalization refused");
    normalized.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
    require(initial_model(FixtureKey, fixture_resource, normalized) == Model::unknown, "Undeclared small texture alignment accepted");
    normalized.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
    require(initial_model(FixtureKey, fixture_resource, normalized) == Model::unknown, "MSAA alignment inferred for ordinary texture");
    normalized = desc;
    normalized.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    ++normalized.Height;
    require(initial_model(FixtureKey, fixture_resource, normalized) == Model::unknown, "Alignment normalization relaxed other fields");
    unregister_device(FixtureKey);
    require(initial_model(FixtureKey, fixture_resource, desc) == Model::unknown, "Device unregister left creation authority");
  }
  require(register_device(0, nullptr, nullptr).ready == false, "Invalid registration accepted");
  require(register_device(1, proxy, proxy).ready == false, "Native interface was treated as distinct ReShade proxy");
  require(forwards == 12, "Not all ten public signatures and unknown-state cases were exercised");
  std::printf("{\"passed\":true,\"checks\":%u,\"signatures\":10,\"exactForwards\":12,\"nestedScopes\":true}\n", checks);
}
}  // namespace
}  // namespace taxi_camera::engine_hook::resource_creation
int main() {
  taxi_camera::engine_hook::resource_creation::run_tests();
}
