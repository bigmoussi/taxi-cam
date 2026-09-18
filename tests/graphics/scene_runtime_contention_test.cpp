#include "../support/graphics_fixture.hpp"
// Exercise service itself and inspect its retained leases. Link the ordinary
// graphics objects except scene_runtime.cpp, which is compiled here.
#include "../../src/graphics/scene_runtime.cpp"

#include <chrono>
#include <future>

namespace {
using namespace taxi_camera;
using namespace taxi_camera::testing;
namespace runtime = taxi_camera::scene_runtime;
using Output = SceneFrameOutput;

void run(bool warp) {
  Reference<ID3D12Debug> debug;
  const bool debug_enabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())));
  if (debug_enabled)
    debug->EnableDebugLayer();
  Reference<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "Contention factory");
  Reference<IDXGIAdapter> adapter;
  if (warp)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "Contention WARP");
  Reference<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put())), "Contention device");
  Reference<ID3D12InfoQueue> messages;
  if (debug_enabled)
    check(device->QueryInterface(IID_PPV_ARGS(messages.put())), "Contention debug messages");
  constexpr std::uint64_t key = 9201;
  require(runtime::init_device(key, device.get()) && runtime::prepare(key), "Actual compositor runtime");
  require(runtime::set_display_exposure(key, 0), "Unmodified validation colours");
  auto& manager = runtime::manager();
  auto& item = *runtime::find(key);
  auto& handoff = scene_handoff();
  require(handoff.register_device(key) != 0, "Handoff device");
  Reference<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC qd{};
  check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.put())), "Application queue");
  Reference<ID3D12DescriptorHeap> heap;
  const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(heap.put())), "Source RTVs");
  std::array<Reference<ID3D12Resource>, 2> sources;
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> rtvs{};
  std::array<std::uint64_t, 2> handles{};
  for (UINT feed = 0; feed < 2; ++feed) {
    const auto pane = profiles::A380.camera_panes[feed];
    create_texture(device.get(), texture_description(pane[0], pane[1], DXGI_FORMAT_R8G8B8A8_UNORM), sources[feed].put());
    handles[feed] = reinterpret_cast<std::uint64_t>(sources[feed].get());
    require(handoff.register_resource(key, handles[feed], 501 + feed), "Source identity");
    rtvs[feed] = {heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{feed} * device->GetDescriptorHandleIncrementSize(hd.Type)};
    device->CreateRenderTargetView(sources[feed].get(), nullptr, rtvs[feed]);
  }
  handoff.begin_scene();
  require(handoff.publish(handoff.begin_capture(), {73, 1}, {101, 102}, handles), "Camera pair publication");
  struct Recording {
    Reference<ID3D12CommandAllocator> allocator;
    Reference<ID3D12GraphicsCommandList> list;
  };
  std::array<Recording, 4> recordings;
  using Colours = std::array<std::array<unsigned char, 4>, 2>;
  const Colours first{{{51, 102, 153, 255}, {153, 51, 102, 255}}};
  const Colours second{{{204, 153, 51, 255}, {51, 204, 153, 255}}};
  const auto capture = [&](unsigned round, const Colours& colours) {
    std::array<ID3D12CommandList*, 2> lists{};
    for (UINT feed = 0; feed < 2; ++feed) {
      const auto index = round * 2 + feed;
      auto& recording = recordings[index];
      check(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(recording.allocator.put())), "Capture allocator");
      check(device->CreateCommandList(0, qd.Type, recording.allocator.get(), nullptr, IID_PPV_ARGS(recording.list.put())), "Capture list");
      require(manager.register_command_list(recording.list.get(), key, index + 1), "Capture registration");
      std::array<float, 4> colour{};
      for (unsigned channel = 0; channel < 4; ++channel)
        colour[channel] = colours[feed][channel] / 255.f;
      recording.list->ClearRenderTargetView(rtvs[feed], colour.data(), 0, nullptr);
      require(manager.record_render_target_before_transition(recording.list.get(), sources[feed].get(), true, index + 1),
              "Real source capture");
      transition(recording.list.get(), sources[feed].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      transition(recording.list.get(), sources[feed].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      check(recording.list->Close(), "Close capture");
      lists[feed] = recording.list.get();
    }
    const auto receipt = manager.before_submission(queue.get(), static_cast<UINT>(lists.size()), lists.data());
    require(receipt != 0, "Capture submission receipt");
    queue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
    manager.after_submission(queue.get(), receipt);
    for (UINT feed = 0; feed < 2; ++feed) {
      const auto index = round * 2 + feed;
      manager.destroy_command_list(recordings[index].list.get(), index + 1);
    }
    require(drain_copy_queue(queue.get(), device.get()), "Captures and retirement fences complete");
  };
  capture(0, first);
  runtime::service();
  require(drain_copy_queue(item.output.queue(), device.get()), "Initial composition completion");
  require(runtime::snapshot(key).output && item.status.frames == 1 && !item.status.failed, "Initial healthy camera output");

  Reference<ID3D12Resource> readback;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = Output::BufferBytes;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const auto read_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
  check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(readback.put())),
        "Output readback");
  Recording consumer;
  check(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(consumer.allocator.put())), "Consumer allocator");
  check(device->CreateCommandList(0, qd.Type, consumer.allocator.get(), nullptr, IID_PPV_ARGS(consumer.list.put())), "Consumer list");
  require(manager.register_command_list(consumer.list.get(), key, 20) && manager.register_consumer_recording(consumer.list.get()),
          "Application consumer participates in the real timeline");
  consumer.list->CopyBufferRegion(readback.get(), 0, item.output.buffer(), 0, Output::BufferBytes);
  check(consumer.list->Close(), "Close consumer");
  ID3D12CommandList* executable = consumer.list.get();
  const auto finish_consumer = [&](std::uint64_t receipt) {
    queue->ExecuteCommandLists(1, &executable);
    manager.after_submission(queue.get(), receipt);
    require(drain_copy_queue(queue.get(), device.get()), "Application consumer completed");
  };
  unsigned pixels = 0;
  const auto verify_pixels = [&](const Colours& colours) {
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(Output::BufferBytes)};
    check(readback->Map(0, &range, &mapped), "Map composed output");
    bool correct = true;
    for (UINT feed = 0; feed < 2; ++feed)
      for (UINT dy = 0; dy < 4; ++dy)
        for (UINT dx = 0; dx < 4; ++dx) {
          const UINT x = 380 + dx, y = (feed ? 500 : 100) + dy;
          const auto* pixel = static_cast<const unsigned char*>(mapped) + SIZE_T{y} * Output::RowPitch + x * 4;
          correct &= std::memcmp(pixel, colours[feed].data(), 4) == 0;
          ++pixels;
        }
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    require(correct, "Both camera feeds reach output pixels without changing the last good image during contention");
  };

  capture(1, second);
  // Hold a genuine application consumer's before/after receipt on this thread.
  // service runs on its own thread as in the bridge, so TLS cannot bypass the
  // contention. Always complete the receipt before asserting, even on failure.
  const auto held = manager.before_submission(queue.get(), 1, &executable);
  require(held != 0, "Held application submission receipt");
  auto serviced = std::async(std::launch::async, [&] {
    runtime::service();
    const std::array tokens{item.pending[0].token, item.pending[1].token};
    runtime::service();
    bool leased = tokens[0] && tokens[1] && item.pending[0].token == tokens[0] && item.pending[1].token == tokens[1];
    {
      const std::lock_guard lock(manager.mutex_);
      for (const auto token : tokens) {
        bool retained = false;
        for (const auto& packet : manager.packets_)
          retained |= packet.token == token && packet.assigned && packet.leased && !packet.quarantined &&
                      packet.gpu.state() == SceneCaptureD3D12::State::ready;
        leased &= retained;
      }
    }
    const auto status = runtime::snapshot(key);
    return leased && !status.failed && status.output && status.frames == 1 && status.capture.completed == 4 &&
           !status.capture.quarantined && item.output.submissions() == 1 && !item.output.prepared_;
  });
  const bool prompt = serviced.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
  finish_consumer(held);
  const bool preserved = serviced.get();
  require(prompt, "Runtime must return while application submission serialization remains held");
  require(preserved, "Repeated deferral must preserve both leases, device health and last good output without GPU submission");
  verify_pixels(first);

  runtime::service();
  require(drain_copy_queue(item.output.queue(), device.get()), "Retried composition completes");
  const auto status = runtime::snapshot(key);
  require(!status.failed && status.output && status.frames == 2 && status.completed_frames == 2 && !status.capture.quarantined &&
              !item.pending[0].token && !item.pending[1].token,
          "Releasing contention composes the retained pair exactly once and returns its leases");
  const auto receipt = manager.before_submission(queue.get(), 1, &executable);
  require(receipt != 0, "Retried output consumer receipt");
  finish_consumer(receipt);
  verify_pixels(second);
  manager.destroy_command_list(consumer.list.get(), 20);
  handoff.stop_scene();
  runtime::reset_feed(key);
  UINT64 errors = 0;
  if (messages.get())
    for (UINT64 index = 0; index < messages->GetNumStoredMessages(); ++index) {
      SIZE_T size = 0;
      check(messages->GetMessage(index, nullptr, &size), "Debug message size");
      std::vector<unsigned char> bytes(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
      check(messages->GetMessage(index, message, &size), "Debug message");
      if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
        ++errors;
        std::fprintf(stderr, "D3D12 error %u: %s\n", message->ID, message->pDescription);
      }
    }
  require(errors == 0, "Contention GPU debug validation");
  std::printf("PASS runtime contention %s: prompt retry, two retained leases, old/new output %u pixels; debug=%d errors=%llu.\n",
              warp ? "WARP" : "hardware", pixels, debug_enabled, errors);
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--warp") != 0))
      return 2;
    run(argc == 2);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL runtime contention: %s\n", error.what());
    return 1;
  }
}
