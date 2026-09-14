#include "queue_submit_observer.hpp"

#include <dxgi1_4.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
namespace qs = taxi_camera::engine_hook::queue_submit;
unsigned checks = 0;
unsigned fail_readonly = 0;
unsigned fail_writable = 0;
void** replace_slot = nullptr;
void* replacement = nullptr;

void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

void success(HRESULT value, const char* message) {
  require(SUCCEEDED(value), message);
}

using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
struct ExpectedCall {
  ID3D12CommandQueue* queue = nullptr;
  UINT count = 0;
  ID3D12CommandList* const* lists = nullptr;
};
thread_local ExpectedCall expected;
thread_local std::uint64_t expected_receipt = 0;

void* slot_value(void** slot) noexcept {
  // The C++ optimizer may otherwise fold a known class's constant vtable entry.
  void* value = nullptr;
  SIZE_T copied = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), slot, &value, sizeof(value), &copied) || copied != sizeof(value))
    std::terminate();
  return value;
}

void invoke(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
  const auto saved = expected;
  expected = {queue, count, lists};
  const auto table = *reinterpret_cast<void***>(queue);
  reinterpret_cast<Execute>(slot_value(table + 10))(queue, count, lists);
  expected = saved;
}

struct Context {
  std::atomic<unsigned> before{0}, after{0}, refused{0}, bad{0}, phase{0};
  std::atomic<std::uint64_t> sequence{0};
  std::atomic<bool> recurse_before{false}, recurse_after{false};
  bool receipt_enabled = true;
};

class MockQueue : public ID3D12CommandQueue {
 public:
  std::atomic<ULONG> references{1};
  std::atomic<unsigned> calls{0}, bad{0};
  Context* context = nullptr;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { return --references; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOINTERFACE; }
  void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource*,
                                            UINT,
                                            const D3D12_TILED_RESOURCE_COORDINATE*,
                                            const D3D12_TILE_REGION_SIZE*,
                                            ID3D12Heap*,
                                            UINT,
                                            const D3D12_TILE_RANGE_FLAGS*,
                                            const UINT*,
                                            const UINT*,
                                            D3D12_TILE_MAPPING_FLAGS) override {}
  void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource*,
                                          const D3D12_TILED_RESOURCE_COORDINATE*,
                                          ID3D12Resource*,
                                          const D3D12_TILED_RESOURCE_COORDINATE*,
                                          const D3D12_TILE_REGION_SIZE*,
                                          D3D12_TILE_MAPPING_FLAGS) override {}
  void STDMETHODCALLTYPE ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) override {
    ++calls;
    if (expected.queue != this || expected.count != count || expected.lists != lists)
      ++bad;
    if (context) {
      unsigned phase = 1;
      context->phase.compare_exchange_strong(phase, 2);
    }
    SwitchToThread();
  }
  void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
  void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
  void STDMETHODCALLTYPE EndEvent() override {}
  HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence*, UINT64) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence*, UINT64) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64*, UINT64*) override { return E_NOTIMPL; }
  D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE GetDesc(D3D12_COMMAND_QUEUE_DESC* output) override {
    *output = {};
    return output;
  }
  // A later interface extension must survive untouched. Production never copies
  // this or any other vtable into a shorter synthetic interface.
  virtual unsigned extension() { return 0x76543210u; }
};

class OtherQueue final : public MockQueue {
 public:
  unsigned extension() override { return 0x12345678u; }
};

std::uint64_t before(void* opaque, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept {
  auto& context = *static_cast<Context*>(opaque);
  ++context.before;
  if (context.phase.exchange(1) != 0 || expected.queue != queue || expected.count != count || expected.lists != lists)
    ++context.bad;
  expected_receipt = ++context.sequence;
  if (context.recurse_before.exchange(false))
    invoke(queue, count, lists);
  if (qs::disable_queue(queue).status != qs::Status::reentrant_operation)
    ++context.bad;
  return context.receipt_enabled ? expected_receipt : 0;
}

void after(void* opaque, ID3D12CommandQueue* queue, std::uint64_t receipt) noexcept {
  auto& context = *static_cast<Context*>(opaque);
  ++context.after;
  if (context.phase.exchange(0) != 2 || receipt != expected_receipt)
    ++context.bad;
  if (context.recurse_after.exchange(false)) {
    ID3D12CommandList* nested[] = {reinterpret_cast<ID3D12CommandList*>(0x12340)};
    invoke(queue, 1, nested);
  }
}

void refused(void* opaque, ID3D12CommandQueue*, qs::Refusal) noexcept {
  auto& context = *static_cast<Context*>(opaque);
  ++context.refused;
  context.phase = 0;
}

qs::Callbacks callbacks(Context* context) {
  return {context, before, after, refused};
}

DWORD protection(void* address) {
  MEMORY_BASIC_INFORMATION region{};
  require(VirtualQuery(address, &region, sizeof(region)) == sizeof(region), "Cannot query the test slot page");
  return region.Protect;
}

void replace(void** slot, void* value) {
  DWORD prior = 0, discarded = 0;
  require(VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prior) != FALSE, "Cannot edit this process's own test table");
  InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), value);
  require(VirtualProtect(slot, sizeof(void*), prior, &discarded) != FALSE, "Cannot restore the test table protection");
}

void STDMETHODCALLTYPE foreign(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept {}

void mock() {
  auto* context = new Context;
  auto* queue = new MockQueue;
  queue->context = context;
  auto** table = *reinterpret_cast<void***>(queue);
  const auto original = slot_value(table + 10);
  const auto extension = slot_value(table + 19);
  const auto prior = protection(table + 10);
  require(qs::register_queue(nullptr, callbacks(context)).status == qs::Status::invalid_argument, "Null queue was accepted");
  require(qs::register_queue(queue, {}).status == qs::Status::invalid_argument, "Missing callbacks were accepted");
  require(qs::register_queue(reinterpret_cast<ID3D12CommandQueue*>(1), callbacks(context)).status == qs::Status::invalid_queue_memory,
          "Misaligned queue memory was accepted");
  const auto installed = qs::register_queue(queue, callbacks(context));
  require(
      installed.status == qs::Status::registered && installed.hook_installed && installed.queue_retained && installed.protection_restored,
      "Mock registration failed");
  require(queue->references == 2 && slot_value(table + 10) != original && slot_value(table + 19) == extension &&
              protection(table + 10) == prior,
          "Installation did not preserve table extension, reference count or protection");
  require(qs::register_queue(queue, callbacks(context)).status == qs::Status::already_registered && queue->references == 2,
          "Repeated registration changed ownership");
  require(qs::register_queue(queue, callbacks(new Context)).status == qs::Status::callback_mismatch, "Callback identity changed");
  require(qs::register_queue(new OtherQueue, callbacks(new Context)).status == qs::Status::different_vtable,
          "Another native vtable was admitted to this wrapper");
  ID3D12CommandList* lists[] = {reinterpret_cast<ID3D12CommandList*>(0x10000), reinterpret_cast<ID3D12CommandList*>(0x20000)};
  invoke(queue, 2, lists);
  require(queue->calls == 1 && queue->bad == 0 && context->before == 1 && context->after == 1 && context->bad == 0,
          "Typed Win64 arguments, call count or pre/post order changed");
  auto* unknown = new MockQueue;
  invoke(unknown, 0x87654321u, lists);
  require(unknown->calls == 1 && unknown->bad == 0 && context->before == 1, "Unknown queue was observed or altered");
  invoke(queue, qs::kMaximumCommandLists + 1, lists);
  invoke(queue, 0, lists);
  invoke(queue, 1, nullptr);
  require(queue->calls == 4 && context->before == 1 && context->refused == 3 && context->after == 1,
          "A refused batch was dropped, inspected beyond its bound or falsely completed");
  context->recurse_after = true;
  invoke(queue, 2, lists);
  require(queue->calls == 6 && context->before == 2 && context->after == 2 && context->refused == 3,
          "Private after-submit recursion was dropped, recursively observed or deadlocked");
  context->recurse_before = true;
  invoke(queue, 2, lists);
  require(queue->calls == 8 && context->before == 3 && context->after == 2 && context->refused == 4,
          "An ambiguous nested pre-submit batch published completion");

  std::array<std::thread, 4> workers;
  for (auto& worker : workers)
    worker = std::thread([&] {
      ID3D12CommandList* local[] = {reinterpret_cast<ID3D12CommandList*>(0x30000)};
      for (unsigned i = 0; i < 200; ++i)
        invoke(queue, 1, local);
    });
  for (auto& worker : workers)
    worker.join();
  require(queue->calls == 808 && queue->bad == 0 && context->after == 802 && context->bad == 0 && context->phase == 0,
          "Concurrent queue submissions did not preserve exact forwarding and receipt ordering");
  const auto stats = qs::statistics(queue);
  require(stats.submissions == 806 && stats.receipts == 802 && stats.refusals == 4, "Submission statistics are inconsistent");
  require(qs::disable_queue(queue).status == qs::Status::disabled, "Disabling the queue failed");
  invoke(queue, 2, lists);
  require(queue->calls == 809 && context->after == 802 && queue->references == 2, "Disable observed work or released the retained queue");
  require(qs::register_queue(queue, callbacks(context)).status == qs::Status::already_registered,
          "Identical registration could not resume");
  for (unsigned i = 1; i < qs::kMaximumQueues; ++i)
    require(qs::register_queue(new MockQueue, callbacks(new Context)).status == qs::Status::registered, "Bounded queue slot failed");
  auto* overflow = new MockQueue;
  require(qs::register_queue(overflow, callbacks(new Context)).status == qs::Status::queue_limit && overflow->references == 1,
          "Queue limit consumed an unowned reference");
  const auto wrapper = slot_value(table + 10);
  replace(table + 10, reinterpret_cast<void*>(&foreign));
  require(qs::remove().status == qs::Status::slot_changed && slot_value(table + 10) == reinterpret_cast<void*>(&foreign),
          "Removal overwrote a foreign replacement");
  replace(table + 10, wrapper);
  const auto removed = qs::remove();
  require(removed.status == qs::Status::removed && !removed.hook_installed && removed.protection_restored &&
              slot_value(table + 10) == original && protection(table + 10) == prior,
          "Owned-slot removal did not restore the original and page protection");
  require(qs::register_queue(queue, callbacks(context)).status == qs::Status::installation_consumed, "Removed installation was reused");
  invoke(queue, 2, lists);
  require(queue->calls == 810 && queue->bad == 0 && context->after == 802, "Removed hook still observed or omitted an original call");
}

void protection_failure(const std::string& mode) {
  auto* queue = new MockQueue;
  auto* context = new Context;
  queue->context = context;
  auto** slot = *reinterpret_cast<void***>(queue) + 10;
  const auto original = slot_value(slot);
  const auto prior = protection(slot);
  if (mode == "fail-writable") {
    fail_writable = 1;
    const auto failed = qs::register_queue(queue, callbacks(context));
    require(failed.status == qs::Status::protection_change_failed && !failed.hook_installed && failed.queue_retained &&
                failed.protection_restored && slot_value(slot) == original && queue->references == 2,
            "Initial protection failure did not leave the queue untouched");
  } else if (mode == "fail-install-restore") {
    fail_readonly = 2;
    const auto failed = qs::register_queue(queue, callbacks(context));
    require(failed.status == qs::Status::protection_restore_failed && failed.hook_installed && failed.queue_retained &&
                !failed.protection_restored && queue->references == 2,
            "Installed-but-unrestored hook was not explicitly reported");
    ID3D12CommandList* lists[] = {reinterpret_cast<ID3D12CommandList*>(0x10000)};
    invoke(queue, 1, lists);
    require(queue->calls == 1 && context->before == 0, "Unregistered partial installation did not forward unchanged");
    require(qs::restore_protection().status == qs::Status::protection_restore_failed,
            "A repeated protection restoration failure was forgotten");
    require(qs::restore_protection().status == qs::Status::protection_restored && protection(slot) == prior,
            "Saved original protection was not recovered");
  } else if (mode == "fail-cas-restore") {
    replace_slot = slot;
    replacement = reinterpret_cast<void*>(&foreign);
    fail_readonly = 1;
    const auto failed = qs::register_queue(queue, callbacks(context));
    require(failed.status == qs::Status::protection_restore_failed && !failed.hook_installed && failed.queue_retained &&
                !failed.protection_restored && slot_value(slot) == replacement,
            "CAS mismatch and failed restoration lost their separate state");
    require(
        qs::restore_protection().status == qs::Status::protection_restored && protection(slot) == prior && slot_value(slot) == replacement,
        "CAS mismatch recovery modified a foreign slot");
    replace(slot, original);
  }
  const auto registered = qs::register_queue(queue, callbacks(context));
  require((registered.status == qs::Status::registered || registered.status == qs::Status::already_registered) &&
              registered.queue_retained && registered.protection_restored && queue->references == 2,
          "Registration did not recover after the controlled failure");
  if (mode == "fail-remove-restore") {
    fail_readonly = 2;
    const auto failed = qs::remove();
    require(failed.status == qs::Status::protection_restore_failed && !failed.hook_installed && !failed.protection_restored &&
                slot_value(slot) == original,
            "Removal's failed protection restoration was not retained");
    require(qs::remove().status == qs::Status::protection_restore_failed, "Removal discarded its pending restoration");
    require(qs::remove().status == qs::Status::not_installed && protection(slot) == prior, "Removal retry did not repair the page");
  } else {
    require(qs::remove().status == qs::Status::removed && protection(slot) == prior, "Recovered registration could not be removed");
  }
}

struct GpuContext {
  ID3D12CommandList* expected = nullptr;
  ID3D12CommandList* private_list = nullptr;
  ID3D12Fence* fence = nullptr;
  unsigned before_count = 0, after_count = 0, refused_count = 0;
  HRESULT signal_result = E_FAIL;
};

std::uint64_t gpu_before(void* opaque, ID3D12CommandQueue*, UINT count, ID3D12CommandList* const* lists) noexcept {
  auto& context = *static_cast<GpuContext*>(opaque);
  ++context.before_count;
  return count == 1 && lists[0] == context.expected ? 1 : 0;
}

void gpu_after(void* opaque, ID3D12CommandQueue* queue, std::uint64_t receipt) noexcept {
  auto& context = *static_cast<GpuContext*>(opaque);
  ++context.after_count;
  // Required integration behavior: own compositor work bypasses observation,
  // then a native queue fence certifies both original and private submissions.
  queue->ExecuteCommandLists(1, &context.private_list);
  context.signal_result = queue->Signal(context.fence, receipt);
}

void gpu_refused(void* opaque, ID3D12CommandQueue*, qs::Refusal) noexcept {
  ++static_cast<GpuContext*>(opaque)->refused_count;
}

void gpu(bool warp) {
  IDXGIFactory4* factory = nullptr;
  IDXGIAdapter* adapter = nullptr;
  ID3D12Device* device = nullptr;
  success(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory creation failed");
  if (warp)
    success(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP enumeration failed");
  success(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12 device creation failed");
  ID3D12CommandQueue* queue = nullptr;
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  success(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "Native queue creation failed");
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12CommandAllocator* private_allocator = nullptr;
  ID3D12GraphicsCommandList* first = nullptr;
  ID3D12GraphicsCommandList* second = nullptr;
  success(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Allocator creation failed");
  success(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&private_allocator)),
          "Private allocator creation failed");
  success(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&first)),
          "First list creation failed");
  success(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, private_allocator, nullptr, IID_PPV_ARGS(&second)),
          "Second list creation failed");
  ID3D12Resource* upload = nullptr;
  ID3D12Resource* readback = nullptr;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 256;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  success(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&upload)),
          "Upload creation failed");
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  success(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&readback)),
          "Readback creation failed");
  std::uint8_t* bytes = nullptr;
  D3D12_RANGE none{0, 0};
  success(upload->Map(0, &none, reinterpret_cast<void**>(&bytes)), "Upload Map failed");
  for (unsigned i = 0; i < 256; ++i)
    bytes[i] = static_cast<std::uint8_t>(i ^ 0xa5);
  upload->Unmap(0, nullptr);
  first->CopyBufferRegion(readback, 0, upload, 0, 128);
  second->CopyBufferRegion(readback, 128, upload, 128, 128);
  success(first->Close(), "First Close failed");
  success(second->Close(), "Second Close failed");
  auto* context = new GpuContext;
  context->expected = first;
  context->private_list = second;
  success(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context->fence)), "Completion fence creation failed");
  ID3D12Fence* blocker = nullptr;
  success(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker)), "Blocker creation failed");
  const auto registered = qs::register_queue(queue, {context, gpu_before, gpu_after, gpu_refused});
  require(registered.status == qs::Status::registered && registered.hook_installed && registered.queue_retained &&
              registered.protection_restored,
          "Real native queue registration failed");
  success(queue->Wait(blocker, 1), "GPU blocker enqueue failed");
  queue->ExecuteCommandLists(1, &context->expected);
  require(context->before_count == 1 && context->after_count == 1 && context->refused_count == 0,
          "Native callbacks or recursion are incorrect");
  success(context->signal_result, "Post-submit queue Signal failed");
  require(context->fence->GetCompletedValue() == 0, "Completion was fabricated while the GPU submission was still blocked");
  success(blocker->Signal(1), "Test-only CPU unblock failed");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "Completion event creation failed");
  success(context->fence->SetEventOnCompletion(1, event), "Completion event setup failed");
  require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0 && context->fence->GetCompletedValue() == 1, "GPU completion timed out");
  D3D12_RANGE written{0, 256};
  success(readback->Map(0, &written, reinterpret_cast<void**>(&bytes)), "Readback Map failed");
  for (unsigned i = 0; i < 256; ++i)
    require(bytes[i] == static_cast<std::uint8_t>(i ^ 0xa5),
            "Fence completed before original/private GPU copies produced the expected bytes");
  readback->Unmap(0, &none);
  require(qs::statistics(queue).submissions == 1 && qs::statistics(queue).receipts == 1,
          "Nested native private submission was observed twice");
  require(qs::remove().status == qs::Status::removed, "Real native queue hook removal failed");
  success(device->GetDeviceRemovedReason(), "D3D12 device was removed");
  CloseHandle(event);
  blocker->Release();
  first->Release();
  second->Release();
  allocator->Release();
  private_allocator->Release();
  upload->Release();
  readback->Release();
  queue->Release();  // Registration's separate reference stays retained.
  device->Release();
  if (adapter)
    adapter->Release();
  factory->Release();
}

}  // namespace

extern "C" BOOL taxi_queue_test_virtual_protect(void* address, SIZE_T size, DWORD access, DWORD* prior) noexcept {
  if ((access == PAGE_READONLY && fail_readonly != 0) || (access == PAGE_READWRITE && fail_writable != 0)) {
    if (access == PAGE_READONLY)
      --fail_readonly;
    else
      --fail_writable;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  const auto result = VirtualProtect(address, size, access, prior);
  if (result && access == PAGE_READWRITE && replace_slot) {
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(replace_slot), replacement);
    replace_slot = nullptr;
  }
  return result;
}

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "mock";
  try {
    if (mode == "mock")
      mock();
    else if (mode == "hardware" || mode == "warp")
      gpu(mode == "warp");
    else if (mode == "fail-writable" || mode == "fail-install-restore" || mode == "fail-cas-restore" || mode == "fail-remove-restore")
      protection_failure(mode);
    else
      throw std::runtime_error("Unknown standalone validation mode");
    std::printf("PASS: %u queue-submit observer checks (%s); this process only.\n", checks, mode.c_str());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks (%s): %s\n", checks, mode.c_str(), error.what());
    return 1;
  }
}
