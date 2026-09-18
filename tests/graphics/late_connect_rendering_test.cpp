#include <atomic>
#include <memory>
#include <thread>
#include "../../src/bridge/d3d12_bridge.hpp"
#include "../support/graphics_fixture.hpp"

using namespace taxi_camera::testing;
namespace bridge = taxi_camera::standalone;
namespace {
constexpr unsigned Width = 1644, Height = 1024, Targets = 3, RenderWorkers = 2;
std::atomic<bool> running{true}, failed{};
std::array<std::atomic<std::uint64_t>, RenderWorkers> frames{};
std::atomic<std::uint64_t> cpu_cycles{}, allocations{};
std::atomic<std::uint64_t> presents{};

void report(const char* phase, std::uint64_t elapsed = 0) {
  std::printf("phase=%s elapsed_ms=%llu frames=%llu/%llu cpu=%llu allocations=%llu presents=%llu\n", phase,
              static_cast<unsigned long long>(elapsed), static_cast<unsigned long long>(frames[0].load()),
              static_cast<unsigned long long>(frames[1].load()), static_cast<unsigned long long>(cpu_cycles.load()),
              static_cast<unsigned long long>(allocations.load()), static_cast<unsigned long long>(presents.load()));
  std::fflush(stdout);
}
void require_live(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s\n", message);
    std::fflush(stderr);
    ExitProcess(1);
  }
}

struct RenderWorker {
  ID3D12Device* device;
  GradientGenerator* generator;
  unsigned index;
  Reference<ID3D12CommandQueue> queue;
  Reference<ID3D12CommandAllocator> allocator;
  Reference<ID3D12GraphicsCommandList> list;
  Reference<ID3D12DescriptorHeap> heap;
  Reference<ID3D12Fence> fence;
  std::array<Reference<ID3D12Resource>, Targets> targets;
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, Targets> views{};
  HANDLE event{};
  std::uint64_t fence_value{};

  // The presenting worker owns and pumps its hidden window. Initialization on
  // another thread cannot accidentally prevent this window receiving messages.
  struct Presentation {
    HWND window{};
    Reference<IDXGISwapChain3> chain;
    std::array<Reference<ID3D12Resource>, 2> buffers;
    Reference<ID3D12DescriptorHeap> heap;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> views{};
    ~Presentation() {
      if (window)
        DestroyWindow(window);
    }
    void create(ID3D12Device* device, ID3D12CommandQueue* queue) {
      window = CreateWindowExW(0, L"STATIC", L"Taxi Cam active-Present validation", WS_OVERLAPPEDWINDOW, 0, 0, 320, 180, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
      require(window != nullptr, "Hidden presentation window");
      Reference<IDXGIFactory4> factory;
      check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "Presentation factory");
      DXGI_SWAP_CHAIN_DESC1 description{};
      description.Width = 320;
      description.Height = 180;
      description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      description.SampleDesc.Count = 1;
      description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      description.BufferCount = 2;
      description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      Reference<IDXGISwapChain1> created;
      check(factory->CreateSwapChainForHwnd(queue, window, &description, nullptr, nullptr, created.put()), "Presentation swap chain");
      check(created->QueryInterface(IID_PPV_ARGS(chain.put())), "Presentation chain3");
      D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
      descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      descriptors.NumDescriptors = 2;
      check(device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(heap.put())), "Presentation RTV heap");
      const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
      for (unsigned i = 0; i < 2; ++i) {
        check(chain->GetBuffer(i, IID_PPV_ARGS(buffers[i].put())), "Presentation buffer");
        views[i] = heap->GetCPUDescriptorHandleForHeapStart();
        views[i].ptr += i * increment;
        device->CreateRenderTargetView(buffers[i].get(), nullptr, views[i]);
      }
    }
    void record(ID3D12GraphicsCommandList* list, GradientGenerator* generator) {
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      const auto index = chain->GetCurrentBackBufferIndex();
      transition(list, buffers[index].get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
      generator->record(list, views[index], 320, 180, false, 0, 0);
      transition(list, buffers[index].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }
    void present() {
      check(chain->Present(0, 0), "Present during live initialization");
      ++presents;
    }
  };

  RenderWorker(ID3D12Device* native, GradientGenerator* draw, unsigned worker) : device(native), generator(draw), index(worker) {
    D3D12_COMMAND_QUEUE_DESC q{};
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(queue.put())), "Create worker queue");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())), "Worker allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put())), "Worker list");
    check(list->Close(), "Worker initial close");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "Worker fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "Worker fence event");
    D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
    descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    descriptors.NumDescriptors = Targets;
    check(device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(heap.put())), "Worker RTV heap");
    const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (unsigned i = 0; i < Targets; ++i) {
      auto desc = texture_description(Width, Height, worker ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_TYPELESS);
      desc.MipLevels = worker ? 5 : 1;
      create_texture(device, desc, targets[i].put());
      views[i] = heap->GetCPUDescriptorHandleForHeapStart();
      views[i].ptr += i * increment;
      D3D12_RENDER_TARGET_VIEW_DESC view{};
      view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      device->CreateRenderTargetView(targets[i].get(), &view, views[i]);
    }
  }
  ~RenderWorker() { CloseHandle(event); }
  void submit() {
    check(list->Close(), "Worker close");
    ID3D12CommandList* submitted[]{list.get()};
    queue->ExecuteCommandLists(1, submitted);
    check(queue->Signal(fence.get(), ++fence_value), "Worker signal");
    if (fence->GetCompletedValue() < fence_value) {
      check(fence->SetEventOnCompletion(fence_value, event), "Worker completion");
      require(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "Worker GPU timeout");
    }
    check(device->GetDeviceRemovedReason(), "Worker device health");
  }
  void render() {
    try {
      Presentation presentation;
      if (index == 0)
        presentation.create(device, queue.get());
      while (running.load(std::memory_order_relaxed)) {
        check(allocator->Reset(), "Worker allocator reset");
        check(list->Reset(allocator.get(), nullptr), "Worker list reset");
        for (unsigned i = 0; i < Targets; ++i) {
          // Entry/exit activity surrounds real draws while preserving a known
          // allocator/fence contract. These are pre-attachment native RTVs.
          transition(list.get(), targets[i].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
          transition(list.get(), targets[i].get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
          generator->record(list.get(), views[i], Width, Height, false, 0, 0);
        }
        if (index == 0)
          presentation.record(list.get(), generator);
        submit();
        if (index == 0)
          presentation.present();
        ++frames[index];
      }
    } catch (const std::exception& error) {
      std::fprintf(stderr, "render_worker=%u FAIL %s\n", index, error.what());
      failed = true;
      running = false;
    }
  }
  std::size_t verify_pixels() {
    constexpr UINT RowPitch = (Width * 4 + 255) & ~255u;
    Reference<ID3D12Resource> readback;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = RowPitch * Height;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto properties = heap_properties(D3D12_HEAP_TYPE_READBACK);
    check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(readback.put())),
          "Pixel readback");
    check(allocator->Reset(), "Readback allocator reset");
    check(list->Reset(allocator.get(), nullptr), "Readback reset");
    transition(list.get(), targets[0].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = targets[0].get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource = readback.get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, Width, Height, 1, RowPitch};
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(list.get(), targets[0].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    submit();
    void* pixels{};
    const D3D12_RANGE read{0, RowPitch * Height}, written{0, 0};
    check(readback->Map(0, &read, &pixels), "Map pixels");
    std::size_t checked{};
    for (unsigned y : {0u, Height / 2, Height - 1}) {
      for (unsigned x : {0u, Width / 2, Width - 1}) {
        const auto* pixel = static_cast<const unsigned char*>(pixels) + y * RowPitch + x * 4;
        const auto red = static_cast<int>(std::lround(255.0 * x / (Width - 1)));
        const auto green = static_cast<int>(std::lround(255.0 * y / (Height - 1)));
        require(std::abs(static_cast<int>(pixel[0]) - red) <= 1 && std::abs(static_cast<int>(pixel[1]) - green) <= 1 && pixel[2] == 51 &&
                    std::abs(static_cast<int>(pixel[3]) - 64) <= 1,
                "Native pixels changed during active initialization");
        ++checked;
      }
    }
    readback->Unmap(0, &written);
    return checked;
  }
};
}  // namespace

int main(int argc, char** argv) {
  try {
    bool warp = false, proxy = false;
    for (int i = 1; i < argc; ++i) {
      warp |= std::strcmp(argv[i], "--warp") == 0;
      proxy |= std::strcmp(argv[i], "--expect-proxy") == 0;
    }
    if (proxy) {
      wchar_t module[32768]{}, executable[32768]{};
      require(GetModuleFileNameW(GetModuleHandleW(L"dxgi.dll"), module, 32768) != 0, "Loaded proxy module path");
      require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Fixture executable path");
      const std::wstring location(executable);
      const auto expected = location.substr(0, location.find_last_of(L"\\/")) + L"\\dxgi.dll";
      require(_wcsicmp(module, expected.c_str()) == 0, "Explicit proxy fixture must load its local DXGI proxy");
      std::printf("proxy=local_dxgi\n");
    }
    Reference<IDXGIFactory4> factory;
    Reference<IDXGIAdapter> adapter;
    check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "Factory");
    if (warp)
      check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "WARP");
    Reference<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put())), "Device");
    GradientGenerator generator(device.get());
    const auto finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(finished != nullptr, "Create watchdog event");
    std::thread watchdog([finished] {
      if (WaitForSingleObject(finished, 20000) != WAIT_OBJECT_0) {
        std::fputs("FAIL active initialization exceeded20s fixture budget\n", stderr);
        std::fflush(stderr);
        ExitProcess(1);
      }
    });
    std::array<std::unique_ptr<RenderWorker>, RenderWorkers> workers;
    std::array<std::thread, RenderWorkers> rendering;
    for (unsigned i = 0; i < RenderWorkers; ++i)
      workers[i] = std::make_unique<RenderWorker>(device.get(), &generator, i);
    for (unsigned i = 0; i < RenderWorkers; ++i)
      rendering[i] = std::thread([&, i] { workers[i]->render(); });
    std::thread cpu([] {
      std::uint64_t value = 17;
      while (running.load(std::memory_order_relaxed)) {
        std::vector<std::uint64_t> scratch(1024);
        for (auto& item : scratch) {
          value = (value ^ (value >> 11)) * 0x9e3779b97f4a7c15ull;
          item = value;
        }
        if (scratch[0] != scratch[1])
          ++cpu_cycles;
        SwitchToThread();
      }
    });
    std::thread allocation([&] {
      try {
        while (running.load(std::memory_order_relaxed)) {
          Reference<ID3D12Resource> resource;
          create_texture(device.get(), texture_description(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM), resource.put());
          ++allocations;
          SwitchToThread();
        }
      } catch (const std::exception& error) {
        std::fprintf(stderr, "allocation_worker FAIL %s\n", error.what());
        failed = true;
        running = false;
      }
    });
    const auto began = GetTickCount64();
    while (frames[0] < 8 || frames[1] < 8 || presents < 8 || cpu_cycles < 100 || allocations < 20) {
      require_live(!failed && GetTickCount64() - began < 10000, "Workers did not become active");
      Sleep(1);
    }
    report("initialize_begin");
    const auto before0 = frames[0].load(), before1 = frames[1].load();
    const auto before_cpu = cpu_cycles.load(), before_allocations = allocations.load();
    const auto initialize_begin = GetTickCount64();
    const bool initialized = bridge::initialize_graphics(device.get());
    report(initialized ? "initialize_complete" : "initialize_failed", GetTickCount64() - initialize_begin);
    require_live(initialized, "Bridge graphics initialization failed");
    // Preserve native drawing and presentation while attachment begins, then
    // enable normal observation using the actual control-thread entry points.
    bridge::set_aircraft_profile(2);
    bridge::set_graphics_observation_demand(true);
    const auto after0 = frames[0].load(), after1 = frames[1].load();
    const auto after_cpu = cpu_cycles.load(), after_allocations = allocations.load();
    const auto after_present = presents.load();
    const auto observation_begin = GetTickCount64();
    while (GetTickCount64() - observation_begin < 1000 || frames[0] < after0 + 8 || frames[1] < after1 + 8) {
      require_live(!failed && GetTickCount64() - observation_begin < 10000, "Workers stopped after initialization");
      Sleep(1);
    }
    running = false;
    for (auto& thread : rendering)
      thread.join();
    allocation.join();
    cpu.join();
    require_live(!failed && cpu_cycles > after_cpu && allocations > after_allocations && presents > after_present,
                 "CPU/allocation/Present work did not survive initialization");
    std::size_t pixels{};
    for (auto& worker : workers)
      pixels += worker->verify_pixels();
    report("complete", GetTickCount64() - began);
    std::printf("PASS backend=%s proxy=%u init_progress=%llu/%llu cpu=%llu allocations=%llu sampled_pixels=%zu graphics=%s\n",
                warp ? "WARP" : "hardware", proxy ? 1u : 0u, static_cast<unsigned long long>(after0 - before0),
                static_cast<unsigned long long>(after1 - before1), static_cast<unsigned long long>(after_cpu - before_cpu),
                static_cast<unsigned long long>(after_allocations - before_allocations), pixels, bridge::graphics_status().error);
    SetEvent(finished);
    watchdog.join();
    CloseHandle(finished);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    std::fflush(stderr);
    // The isolated fixture may have submitted GPU work or live worker threads.
    // End this test process without destroying resources after an unsafe drain.
    ExitProcess(1);
  }
}
