#include "../engine-hook/pfd_state_observer.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace obs = taxi_camera::engine_hook::pfd_state;
namespace {
std::atomic<unsigned> protect_calls{0};
unsigned fail_protect_call = 0;
std::uint64_t checks = 0;
void require(bool value, const char* text) {
  ++checks;
  if (!value)
    throw std::runtime_error(text);
}
struct Fake {
  void** table;
};
struct Statistics {
  UINT heaps = 0, cbvs = 0, resets = 0, heap_callbacks = 0, cbv_callbacks = 0, reset_callbacks = 0;
  UINT roots = 0, tables = 0, root_callbacks = 0, table_callbacks = 0;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12PipelineState* pipeline = nullptr;
  ID3D12RootSignature* root = nullptr;
  ID3D12DescriptorHeap* const* heap_array = nullptr;
  UINT count = 0, index = 0;
  UINT64 address = 0, generation = 0;
  HRESULT result = E_FAIL, observed_result = S_OK;
} stats;
std::atomic<bool> block_original{false}, original_entered{false}, allow_return{false};
bool nested_table_from_root = false, retire_from_root = false;
void wait_original() {
  if (block_original.load()) {
    original_entered.store(true);
    while (!allow_return.load())
      std::this_thread::yield();
  }
}
HRESULT STDMETHODCALLTYPE original_reset(ID3D12GraphicsCommandList* list,
                                         ID3D12CommandAllocator* allocator,
                                         ID3D12PipelineState* pipeline) {
  ++stats.resets;
  stats.list = list;
  stats.allocator = allocator;
  stats.pipeline = pipeline;
  return stats.result;
}
void STDMETHODCALLTYPE original_heaps(ID3D12GraphicsCommandList* list, UINT count, ID3D12DescriptorHeap* const* heaps) {
  ++stats.heaps;
  stats.list = list;
  stats.count = count;
  stats.heap_array = heaps;
  wait_original();
}
void STDMETHODCALLTYPE original_cbv(ID3D12GraphicsCommandList* list, UINT index, UINT64 address) {
  ++stats.cbvs;
  stats.list = list;
  stats.index = index;
  stats.address = address;
}
void STDMETHODCALLTYPE original_root(ID3D12GraphicsCommandList* list, ID3D12RootSignature* signature) {
  ++stats.roots;
  stats.list = list;
  stats.root = signature;
  wait_original();
}
void STDMETHODCALLTYPE original_table(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
  ++stats.tables;
  stats.list = list;
  stats.index = index;
  stats.address = handle.ptr;
  wait_original();
}
void callback_heaps(void*, ID3D12GraphicsCommandList* list, UINT64 generation, UINT count, ID3D12DescriptorHeap* const* heaps) noexcept {
  if (stats.list == list && stats.heaps && stats.count == count && stats.heap_array == heaps)
    ++stats.heap_callbacks;
  stats.generation = generation;
}
void callback_cbv(void*, ID3D12GraphicsCommandList* list, UINT64 generation, UINT index, UINT64 address) noexcept {
  if (stats.list == list && stats.cbvs && stats.index == index && stats.address == address)
    ++stats.cbv_callbacks;
  stats.generation = generation;
}
void callback_reset(void*, ID3D12GraphicsCommandList* list, UINT64 generation, HRESULT result, ID3D12PipelineState* pipeline) noexcept {
  if (stats.list == list && stats.resets && stats.pipeline == pipeline)
    ++stats.reset_callbacks;
  stats.generation = generation;
  stats.observed_result = result;
}
void callback_root(void*, ID3D12GraphicsCommandList* list, UINT64 generation, ID3D12RootSignature* signature) noexcept {
  if (stats.list == list && stats.roots && stats.root == signature)
    ++stats.root_callbacks;
  stats.generation = generation;
  if (nested_table_from_root) {
    using Table = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
    reinterpret_cast<Table>(reinterpret_cast<Fake*>(list)->table[32])(list, 12, {0xabcdefff12345678ull});
  }
  if (retire_from_root)
    obs::unregister_list(list, generation);
}
void callback_table(void*, ID3D12GraphicsCommandList* list, UINT64 generation, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) noexcept {
  if (stats.list == list && stats.tables && stats.index == index && stats.address == handle.ptr)
    ++stats.table_callbacks;
  stats.generation = generation;
}
const obs::Callbacks callbacks{nullptr, callback_heaps, callback_cbv, callback_reset, callback_root, callback_table};
using Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using Heaps = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using Cbv = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT64);
using Root = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using Table = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
}  // namespace
extern "C" BOOL taxi_pfd_test_virtual_protect(void* address, SIZE_T bytes, DWORD access, PDWORD previous) noexcept {
  const auto call = ++protect_calls;
  if (fail_protect_call && call == fail_protect_call) {
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  return VirtualProtect(address, bytes, access, previous);
}
int main(int argc, char** argv) {
  try {
    std::array<void*, 39> table{};
    table[10] = reinterpret_cast<void*>(&original_reset);
    table[28] = reinterpret_cast<void*>(&original_heaps);
    table[30] = reinterpret_cast<void*>(&original_root);
    table[32] = reinterpret_cast<void*>(&original_table);
    table[38] = reinterpret_cast<void*>(&original_cbv);
    Fake object{table.data()}, unknown{table.data()};
    auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(&object);
    if (argc == 2 && std::string(argv[1]) == "--optional") {
      const obs::Callbacks optional{nullptr, callback_heaps, callback_cbv, callback_reset};
      require(obs::register_list(list, 1, optional).ready, "Optional callbacks may be absent");
      reinterpret_cast<Root>(table[30])(list, nullptr);
      reinterpret_cast<Table>(table[32])(list, 63, {0xffffeeee11112222ull});
      require(stats.roots == 1 && stats.tables == 1 && stats.root_callbacks == 0 && stats.table_callbacks == 0,
              "Absent callbacks retain exact original forwarding");
      require(stats.root == nullptr && stats.index == 63 && stats.address == 0xffffeeee11112222ull, "Optional exact arguments");
      require(obs::remove().protection_restored, "Optional installation removal");
      std::printf("{\"passed\":true,\"optionalObserverChecks\":%llu}\n", static_cast<unsigned long long>(checks));
      return 0;
    }
    require(argc == 1, "Unknown test argument");
    require(!obs::register_list(nullptr, 1, callbacks).ready, "Null registration");
    require(!obs::register_list(list, 0, callbacks).ready, "Zero incarnation");
    fail_protect_call = 1;
    auto first = obs::register_list(list, 1, callbacks);
    require(!first.ready && first.protection_restored && !obs::operational(), "Protect failure did not refuse");
    fail_protect_call = protect_calls + 2;
    auto second = obs::register_list(list, 1, callbacks);
    require(!second.ready && !second.protection_restored && !obs::operational(), "Restore failure was lost");
    // Partial installation still forwards the exact original once with its HRESULT.
    auto* allocator = reinterpret_cast<ID3D12CommandAllocator*>(0x1000);
    auto* pso = reinterpret_cast<ID3D12PipelineState*>(0x2000);
    require(reinterpret_cast<Reset>(table[10])(list, allocator, pso) == E_FAIL && stats.resets == 1 && stats.reset_callbacks == 0,
            "Partial reset forwarding");
    fail_protect_call = protect_calls + 1;
    require(!obs::repair_protection().protection_restored, "Pending restore failure disappeared");
    fail_protect_call = 0;
    require(obs::repair_protection().protection_restored, "Repair protection");
    require(obs::register_list(list, 1, callbacks).ready, "Retry complete install");
    require(obs::operational(), "Complete install not operational");
    const auto reset = reinterpret_cast<Reset>(table[10]);
    const auto heaps = reinterpret_cast<Heaps>(table[28]);
    const auto cbv = reinterpret_cast<Cbv>(table[38]);
    const auto root = reinterpret_cast<Root>(table[30]);
    const auto descriptor_table = reinterpret_cast<Table>(table[32]);
    require(reset(list, allocator, pso) == E_FAIL && stats.observed_result == E_FAIL && stats.reset_callbacks == 1 &&
                stats.allocator == allocator,
            "Failed HRESULT post notification");
    stats.result = S_OK;
    require(reset(list, allocator, pso) == S_OK && stats.reset_callbacks == 2 && stats.observed_result == S_OK,
            "Successful HRESULT post notification");
    ID3D12DescriptorHeap* heap_values[]{reinterpret_cast<ID3D12DescriptorHeap*>(0x3000), reinterpret_cast<ID3D12DescriptorHeap*>(0x4000)};
    heaps(list, 2, heap_values);
    require(stats.heaps == 1 && stats.heap_callbacks == 1 && stats.heap_array == heap_values && stats.count == 2,
            "Exact heap forwarding/after callback");
    cbv(list, 61, 0xfedcba9876543200ull);
    require(stats.cbvs == 1 && stats.cbv_callbacks == 1 && stats.index == 61 && stats.address == 0xfedcba9876543200ull,
            "Exact64-bit CBV forwarding");
    auto* signature = reinterpret_cast<ID3D12RootSignature*>(0x5000);
    root(list, signature);
    require(stats.roots == 1 && stats.root_callbacks == 1 && stats.root == signature && stats.generation == 1,
            "Exact root signature post-original notification");
    root(list, nullptr);
    require(stats.roots == 2 && stats.root_callbacks == 2 && stats.root == nullptr, "Null root forwarded unchanged");
    descriptor_table(list, 63, {0xfedcba9876543210ull});
    require(stats.tables == 1 && stats.table_callbacks == 1 && stats.index == 63 && stats.address == 0xfedcba9876543210ull,
            "Exact 64-bit root table handle post-original notification");
    descriptor_table(list, 0, {});
    require(stats.tables == 2 && stats.table_callbacks == 2 && stats.address == 0, "Zero table handle forwarded unchanged");
    root(reinterpret_cast<ID3D12GraphicsCommandList*>(&unknown), signature);
    descriptor_table(reinterpret_cast<ID3D12GraphicsCommandList*>(&unknown), 60, {123});
    require(stats.roots == 3 && stats.tables == 3 && stats.root_callbacks == 2 && stats.table_callbacks == 2,
            "Unknown root/table list forwarded without notification");
    {
      const obs::ScopedBypass bypass;
      root(list, signature);
      descriptor_table(list, 61, {456});
    }
    require(stats.roots == 4 && stats.tables == 4 && stats.root_callbacks == 2 && stats.table_callbacks == 2,
            "Root/table private replay bypass");
    nested_table_from_root = true;
    root(list, signature);
    nested_table_from_root = false;
    require(stats.roots == 5 && stats.root_callbacks == 3 && stats.tables == 5 && stats.table_callbacks == 2 &&
                stats.address == 0xabcdefff12345678ull,
            "Nested table original forwarded exactly once without recursive notification");
    auto mismatch = callbacks;
    mismatch.graphics_root = nullptr;
    require(!obs::register_list(list, 1, mismatch).ready, "Root callback identity immutable");
    mismatch = callbacks;
    mismatch.graphics_table = nullptr;
    require(!obs::register_list(list, 1, mismatch).ready, "Table callback identity immutable");
    heaps(reinterpret_cast<ID3D12GraphicsCommandList*>(&unknown), 2, heap_values);
    require(stats.heaps == 2 && stats.heap_callbacks == 1, "Unknown list forwarded with notification");
    {
      const obs::ScopedBypass bypass;
      cbv(list, 62, 44);
    }
    require(stats.cbvs == 2 && stats.cbv_callbacks == 1, "Internal replay notification was not bypassed");
    require(!obs::register_list(list, 2, callbacks).ready, "Unretired incarnation changed");
    obs::unregister_list(list, 9);
    heaps(list, 0, nullptr);
    require(stats.heap_callbacks == 2, "Stale destroy retired current identity");
    block_original = true;
    original_entered = false;
    allow_return = false;
    std::thread concurrent([&] { heaps(list, 0, nullptr); });
    while (!original_entered.load())
      std::this_thread::yield();
    obs::unregister_list(list, 1);
    require(obs::register_list(list, 2, callbacks).ready, "Reused address registration");
    allow_return = true;
    concurrent.join();
    block_original = false;
    require(stats.heap_callbacks == 2, "Stale in-flight callback crossed incarnation");
    heaps(list, 0, nullptr);
    require(stats.generation == 2 && stats.heap_callbacks == 3, "New incarnation notification missing");
    for (bool use_root : {true, false}) {
      block_original = true;
      original_entered = false;
      allow_return = false;
      const auto old_root_callbacks = stats.root_callbacks, old_table_callbacks = stats.table_callbacks;
      std::thread pending([&] {
        if (use_root)
          root(list, signature);
        else
          descriptor_table(list, 7, {0xf0f0f0f0f0f0f0f0ull});
      });
      while (!original_entered.load())
        std::this_thread::yield();
      obs::unregister_list(list, 2);
      require(obs::register_list(list, 4, callbacks).ready, "Root/table reused-address registration");
      allow_return = true;
      pending.join();
      block_original = false;
      require(stats.root_callbacks == old_root_callbacks && stats.table_callbacks == old_table_callbacks,
              "Stale root/table original cannot publish into replacement incarnation");
      obs::unregister_list(list, 4);
      require(obs::register_list(list, 2, callbacks).ready, "Restore test incarnation");
    }
    retire_from_root = true;
    root(list, signature);
    retire_from_root = false;
    require(obs::register_list(list, 2, callbacks).ready, "Callback may retire identity without registry-lock deadlock");
    std::vector<Fake> more(8192, Fake{table.data()});
    for (std::size_t i = 0; i < 8191; ++i)
      require(obs::register_list(reinterpret_cast<ID3D12GraphicsCommandList*>(&more[i]), 3, callbacks).ready,
              "Bounded registry early refusal");
    require(!obs::register_list(reinterpret_cast<ID3D12GraphicsCommandList*>(&more.back()), 3, callbacks).ready,
            "Registry capacity not enforced");
    for (std::size_t i = 0; i < 8191; ++i)
      obs::unregister_list(reinterpret_cast<ID3D12GraphicsCommandList*>(&more[i]), 3);
    obs::unregister_list(list, 2);
    const auto removed = obs::remove();
    require(std::string(removed.status) == "removed" && removed.protection_restored && !obs::operational(), "Remove all5 slots");
    require(table[10] == reinterpret_cast<void*>(&original_reset) && table[28] == reinterpret_cast<void*>(&original_heaps) &&
                table[30] == reinterpret_cast<void*>(&original_root) && table[32] == reinterpret_cast<void*>(&original_table) &&
                table[38] == reinterpret_cast<void*>(&original_cbv),
            "Original slots not restored");
    require(!obs::register_list(list, 3, callbacks).ready, "Removed installation reused");
    std::printf("{\"passed\":true,\"observerChecks\":%llu,\"retainedCommandLists\":0}\n", static_cast<unsigned long long>(checks));
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
