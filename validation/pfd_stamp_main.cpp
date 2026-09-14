// Reuse the independently tested GPU gradient producer, pixel oracle and local
// COM helpers. Its entry point is not called; this remains a separate process.
#define wmain compositor_validation_entry_not_called
#include "compositor_main.cpp"
#undef wmain
#include "../engine-hook/pfd_state_observer.hpp"
#include "../src/pfd_stamp_d3d12.hpp"
#ifdef TAXI_PFD_REAL_ADAPTER
#include <reshade.hpp>
#include "../src/pfd_state_adapter.hpp"
#endif

namespace {
using taxi_camera::PfdGraphicsState;
using taxi_camera::PfdRootKind;
namespace Observer = taxi_camera::engine_hook::pfd_state;
#ifdef TAXI_PFD_REAL_ADAPTER
namespace Adapter = taxi_camera::pfd_adapter;
auto* live_handoff = new taxi_camera::SceneHandoff;
auto* live_manager = new taxi_camera::SceneCaptureManager(*live_handoff);
reshade::api::device* live_device = nullptr;
ID3D12Device* live_native_device = nullptr;
reshade::api::command_list* live_last_list = nullptr;
std::uint64_t live_object_generation = 0;
std::unordered_map<reshade::api::command_list*, std::uint64_t> live_lists;
std::string live_error;
void live_init_device(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d12)
    return;
  live_device = device;
  live_native_device = reinterpret_cast<ID3D12Device*>(device->get_native());
  if (!live_manager->register_device(reinterpret_cast<std::uint64_t>(device), live_native_device))
    live_error = "manager device registration";
}
void live_init_list(reshade::api::command_list* list) {
  if (list->get_device() != live_device)
    return;
  auto* native = reinterpret_cast<ID3D12GraphicsCommandList*>(list->get_native());
  if (native->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return;
  const auto generation = ++live_object_generation;
  live_lists.emplace(list, generation);
  live_last_list = list;
  if (!live_manager->register_command_list(native, reinterpret_cast<std::uint64_t>(live_device), generation) ||
      !Adapter::init_list(list, reinterpret_cast<std::uint64_t>(live_device), generation))
    live_error = "adapter list registration";
}
void live_destroy_list(reshade::api::command_list* list) {
  const auto found = live_lists.find(list);
  if (found == live_lists.end())
    return;
  Adapter::destroy_list(list, found->second);
  live_manager->destroy_command_list(reinterpret_cast<ID3D12GraphicsCommandList*>(list->get_native()), found->second);
  live_lists.erase(found);
}
void live_init_queue(reshade::api::command_queue* queue) {
  if (queue->get_device() != live_device)
    return;
  auto* native = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
  if (native->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return;
  const auto result = taxi_camera::engine_hook::queue_submit::register_queue(native, live_manager->callbacks());
  if (!result.hook_installed || !result.queue_retained || !result.protection_restored)
    live_error = "queue observation registration";
}
void register_live_adapter() {
  require(reshade::register_addon(GetModuleHandleW(nullptr)), "Register validation executable with real ReShade");
  require(Adapter::initialize(*live_manager), "Initialize actual PFD adapter");
  Adapter::register_events();
  reshade::register_event<reshade::addon_event::init_device>(live_init_device);
  reshade::register_event<reshade::addon_event::init_command_list>(live_init_list);
  reshade::register_event<reshade::addon_event::destroy_command_list>(live_destroy_list);
  reshade::register_event<reshade::addon_event::init_command_queue>(live_init_queue);
}
#endif
struct ObservationContext {
  PfdGraphicsState state;
  taxi_camera::PfdRootLayout metadata;
  std::uint64_t object_generation = 1, recording = 1, heaps = 0, cbv = 0, resets = 0;
} observer;
#ifndef TAXI_PFD_REAL_ADAPTER
void observe_heaps(void*, ID3D12GraphicsCommandList*, std::uint64_t generation, UINT count, ID3D12DescriptorHeap* const* heaps) noexcept {
  if (generation != observer.object_generation)
    return;
  ++observer.heaps;
  observer.state.descriptor_heaps(count, heaps);
}
void observe_cbv(void*, ID3D12GraphicsCommandList*, std::uint64_t generation, UINT index, UINT64 address) noexcept {
  if (generation != observer.object_generation)
    return;
  ++observer.cbv;
  observer.state.descriptor(index, PfdRootKind::cbv, address);
}
void observe_root(void*, ID3D12GraphicsCommandList*, std::uint64_t generation, ID3D12RootSignature* root) noexcept {
  if (generation == observer.object_generation)
    observer.state.bind_root(root, 1, observer.metadata, true);
}
void observe_table(void*, ID3D12GraphicsCommandList*, std::uint64_t generation, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) noexcept {
  if (generation == observer.object_generation)
    observer.state.table(index, table.ptr);
}
void observe_reset(void*, ID3D12GraphicsCommandList*, std::uint64_t generation, HRESULT hr, ID3D12PipelineState* initial) noexcept {
  if (generation != observer.object_generation || FAILED(hr))
    return;
  ++observer.resets;
  observer.state.reset(++observer.recording, true);
  observer.state.bind_pipeline(initial);
}
#endif

void create_buffer(ID3D12Device* device,
                   UINT64 bytes,
                   D3D12_HEAP_TYPE heap_type,
                   D3D12_RESOURCE_STATES state,
                   D3D12_RESOURCE_FLAGS flags,
                   ID3D12Resource** result) {
  auto heap = heap_properties(heap_type);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = desc.MipLevels = desc.DepthOrArraySize = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(result)),
        "Create stamp test buffer");
}
void fill_upload(ID3D12Resource* buffer, UINT value) {
  void* data = nullptr;
  const D3D12_RANGE none{0, 0};
  check(buffer->Map(0, &none, &data), "Map scalar upload");
  std::memset(data, 0, 256);
  std::memcpy(data, &value, 4);
  const D3D12_RANGE written{0, 256};
  buffer->Unmap(0, &written);
}

class StampApplication {
 public:
  explicit StampApplication(ID3D12Device* device, bool depth_stencil = false) {
    constexpr char shader[] = R"(
cbuffer Constants : register(b0) { uint4 Constant; };
cbuffer BufferValues : register(b1) { uint4 BufferValue; };
ByteAddressBuffer Raw : register(t0);
RWByteAddressBuffer Writable : register(u1);
Texture2D<float4> Image : register(t1);
SamplerState PointSampler : register(s0);
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2(id & 1, id >> 1);
  return float4(uv.x * 2 - 1, 1 - uv.y * 2, 0.375, 1);
}
float4 ps_main() : SV_Target {
  float4 texel = Image.SampleLevel(PointSampler, float2(0.5,0.5),0);
  return float4((Constant.x + BufferValue.x + Raw.Load(0))/255.0,
                Writable.Load(0)/255.0 + texel.r, Constant.y/255.0 + texel.b, 1);
})";
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0};
    ranges[2] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 99, 0, 0};
    D3D12_ROOT_PARAMETER params[7]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[3].Descriptor.ShaderRegister = 1;
    params[4].ParameterType = params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable = {1, &ranges[0]};
    params[5].DescriptorTable = {1, &ranges[1]};
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[6].DescriptorTable = {1, &ranges[2]};  // Not used by either shader and never assigned.
    for (auto& p : params)
      p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = 7;
    signature.pParameters = params;
    Reference<ID3DBlob> serialized, vs, ps;
    check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), nullptr), "Serialize app root");
    check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(root_.put())),
          "Create app root");
    signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    Reference<ID3DBlob> alternate;
    check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, alternate.put(), nullptr), "Serialize alternate root");
    check(device->CreateRootSignature(0, alternate->GetBufferPointer(), alternate->GetBufferSize(), IID_PPV_ARGS(alternate_root_.put())),
          "Create alternate app root");
    check(D3DCompile(shader, sizeof(shader) - 1, "stamp_app", nullptr, nullptr, "vs_main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                     vs.put(), nullptr),
          "Compile app VS");
    Reference<ID3DBlob> errors;
    const auto pixel_hr = D3DCompile(shader, sizeof(shader) - 1, "stamp_app", nullptr, nullptr, "ps_main", "ps_5_0",
                                     D3DCOMPILE_ENABLE_STRICTNESS, 0, ps.put(), errors.put());
    if (FAILED(pixel_hr) && errors.get())
      std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    check(pixel_hr, "Compile app PS");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root_.get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& b = p.BlendState.RenderTarget[0];
    b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_ONE;
    b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_ZERO;
    b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.LogicOp = D3D12_LOGIC_OP_NOOP;
    b.RenderTargetWriteMask = 15;
    p.SampleMask = UINT_MAX;
    p.SampleDesc.Count = 1;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.DepthStencilState.FrontFace =
        p.DepthStencilState.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    if (depth_stencil) {
      p.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
      p.DepthStencilState.DepthEnable = TRUE;
      p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
      p.DepthStencilState.StencilEnable = TRUE;
      p.DepthStencilState.StencilReadMask = p.DepthStencilState.StencilWriteMask = 0xff;
      p.DepthStencilState.FrontFace.StencilPassOp = p.DepthStencilState.BackFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    }
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    check(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(pipeline_.put())), "Create app pipeline");
    create_buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, cbv_.put());
    fill_upload(cbv_.get(), 19);
    create_buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, srv_.put());
    fill_upload(srv_.get(), 23);
    create_buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, upload_.put());
    fill_upload(upload_.get(), 29);
    create_buffer(device, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  uav_.put());
    const auto desc = texture_description(1, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
    create_texture(device, desc, texture_.put());
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 1;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(srv_heap_.put())), "Create app SRV heap");
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(sampler_heap_.put())), "Create app sampler heap");
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = desc.Format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(texture_.get(), &view, srv_heap_->GetCPUDescriptorHandleForHeapStart());
    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    device->CreateSampler(&sampler, sampler_heap_->GetCPUDescriptorHandleForHeapStart());
    layout_.valid = true;
    layout_.count = 7;
    layout_.parameters[0] = {PfdRootKind::constants, 4};
    layout_.parameters[1] = {PfdRootKind::cbv, 1};
    layout_.parameters[2] = {PfdRootKind::srv, 1};
    layout_.parameters[3] = {PfdRootKind::uav, 1};
    layout_.parameters[4] = layout_.parameters[5] = {PfdRootKind::table, 1};
    layout_.parameters[6] = {PfdRootKind::table, 1};
  }
  void prepare(ID3D12Device* device, ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    list->CopyBufferRegion(uav_.get(), 0, upload_.get(), 0, 256);
    transition(list, uav_.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    device->CreateRenderTargetView(texture_.get(), nullptr, handle);
    const float color[]{31.f / 255, 37.f / 255, 41.f / 255, 1};
    list->ClearRenderTargetView(handle, color, 0, nullptr);
    transition(list, texture_.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  void bind(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv = nullptr) {
    auto& state = observer.state;
    list->SetPipelineState(pipeline_.get());
    state.bind_pipeline(pipeline_.get());
    observer.metadata = layout_;
    list->SetGraphicsRootSignature(alternate_root_.get());
    list->SetGraphicsRootSignature(root_.get());
    state.bind_root(root_.get(), 1, layout_, true);  // Actual native setter above; independent manual oracle.
    ID3D12DescriptorHeap* heaps[]{srv_heap_.get(), sampler_heap_.get()};
    list->SetDescriptorHeaps(2, heaps);
#ifdef TAXI_PFD_REAL_ADAPTER
    state.descriptor_heaps(2, heaps);  // separate manual oracle
#endif
    const UINT values[]{17, 43, 101, 103};
    list->SetGraphicsRoot32BitConstants(0, 2, values, 0);
    state.constants(0, 0, 2, values);
    list->SetGraphicsRoot32BitConstants(0, 2, values + 2, 2);
    state.constants(0, 2, 2, values + 2);
    list->SetGraphicsRootConstantBufferView(1, cbv_->GetGPUVirtualAddress());  // native observer ONLY
#ifdef TAXI_PFD_REAL_ADAPTER
    state.descriptor(1, PfdRootKind::cbv, cbv_->GetGPUVirtualAddress());  // manual oracle is separate from real adapter
#endif
    list->SetGraphicsRootShaderResourceView(2, srv_->GetGPUVirtualAddress());
    state.descriptor(2, PfdRootKind::srv, srv_->GetGPUVirtualAddress());
    list->SetGraphicsRootUnorderedAccessView(3, uav_->GetGPUVirtualAddress());
    state.descriptor(3, PfdRootKind::uav, uav_->GetGPUVirtualAddress());
    for (UINT index = 0; index < 2; ++index) {
      const auto gpu = heaps[index]->GetGPUDescriptorHandleForHeapStart();
      list->SetGraphicsRootDescriptorTable(4 + index, gpu);
      state.table(4 + index, gpu.ptr);
    }
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    state.topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    const D3D12_VIEWPORT viewport{0, 0, 768, 1024, 0, 1};
    list->RSSetViewports(1, &viewport);
    state.viewports(0, 1, &viewport);
    const D3D12_RECT scissor{20, 800, 44, 824};
    list->RSSetScissorRects(1, &scissor);
    state.scissors(0, 1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, dsv);
    if (dsv)
      list->OMSetStencilRef(0x5a);
    require(state.complete(), "Complete app root state refused");
    list->SetDescriptorHeaps(2, heaps);
#ifdef TAXI_PFD_REAL_ADAPTER
    state.descriptor_heaps(2, heaps);
#endif
    require(state.complete(), "Redundant native heap binding invalidated valid tables");
    list->SetDescriptorHeaps(1, heaps);
#ifdef TAXI_PFD_REAL_ADAPTER
    state.descriptor_heaps(1, heaps);
#endif
    require(state.undefined_table_count() == 3, "Removing a native heap retained defined table values");
    list->SetDescriptorHeaps(2, heaps);
#ifdef TAXI_PFD_REAL_ADAPTER
    state.descriptor_heaps(2, heaps);
#endif
    require(state.undefined_table_count() == 3, "Restoring heaps without tables falsely defined values");
    for (UINT index = 0; index < 2; ++index) {
      const auto gpu = heaps[index]->GetGPUDescriptorHandleForHeapStart();
      list->SetGraphicsRootDescriptorTable(4 + index, gpu);
      state.table(4 + index, gpu.ptr);
    }
    require(state.complete(), "Rebound tables did not recover state");
    require(state.undefined_table_count() == 1, "Unused unassigned table lost exact undefined provenance");
    // No subsequent table setters: the actual adapter must preserve these
    // bindings across both a redundant call and a reordered identical set.
    list->SetDescriptorHeaps(2, heaps);
    ID3D12DescriptorHeap* reversed[]{heaps[1], heaps[0]};
    list->SetDescriptorHeaps(2, reversed);
  }
  ID3D12PipelineState* pipeline() const { return pipeline_.get(); }

 private:
  Reference<ID3D12RootSignature> root_;
  Reference<ID3D12RootSignature> alternate_root_;
  Reference<ID3D12PipelineState> pipeline_;
  Reference<ID3D12Resource> cbv_, srv_, uav_, upload_, texture_;
  Reference<ID3D12DescriptorHeap> srv_heap_, sampler_heap_;
  taxi_camera::PfdRootLayout layout_;
};

class DepthStencilFixture {
 public:
  void initialize(ID3D12Device* device) {
    auto desc = texture_description(768, 1024, DXGI_FORMAT_D24_UNORM_S8_UINT);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    const auto heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = desc.Format;
    clear.DepthStencil = {0.75f, 0xa5};
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                          IID_PPV_ARGS(texture_.put())),
          "Create application depth/stencil texture");
    D3D12_DESCRIPTOR_HEAP_DESC views{};
    views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    views.NumDescriptors = 1;
    check(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(heap_.put())), "Create application DSV heap");
    handle_ = heap_->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(texture_.get(), nullptr, handle_);
    D3D12_FEATURE_DATA_FORMAT_INFO format{desc.Format, 0};
    check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format, sizeof(format)), "Depth/stencil plane count");
    require(format.PlaneCount == 2, "Depth/stencil fixture requires both planes");
    device->GetCopyableFootprints(&desc, 0, 2, 0, footprints_.data(), rows_.data(), row_bytes_.data(), &bytes_);
    require(bytes_ > 0 && bytes_ < 32 * 1024 * 1024, "Depth/stencil readback bound");
    for (UINT plane = 0; plane < 2; ++plane)
      require(rows_[plane] == 1024 && row_bytes_[plane] >= 768 && row_bytes_[plane] % 768 == 0 && row_bytes_[plane] / 768 <= 8 &&
                  footprints_[plane].Footprint.RowPitch >= row_bytes_[plane],
              "Depth/stencil plane footprint");
    for (auto& output : readbacks_)
      create_buffer(device, bytes_, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, output.put());
  }
  const D3D12_CPU_DESCRIPTOR_HANDLE* handle() const { return texture_.get() ? &handle_ : nullptr; }
  void clear(ID3D12GraphicsCommandList* list) const {
    if (texture_.get())
      list->ClearDepthStencilView(handle_, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.75f, 0xa5, 0, nullptr);
  }
  void snapshot(ID3D12GraphicsCommandList* list, UINT index) const {
    if (!texture_.get())
      return;
    // Validation-only image readback. Copy both native planes using reported
    // footprints; the byte oracle does not guess depth/stencil packing.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {texture_.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                          D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    for (UINT plane = 0; plane < 2; ++plane) {
      D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
      source.pResource = texture_.get();
      source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      source.SubresourceIndex = plane;
      destination.pResource = readbacks_[index].get();
      destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      destination.PlacedFootprint = footprints_[plane];
      list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
  }
  std::uint64_t verify_unchanged() const {
    if (!texture_.get())
      return 0;
    std::array<unsigned char*, 2> data{};
    const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes_)};
    for (UINT index = 0; index < 2; ++index)
      check(readbacks_[index]->Map(0, &range, reinterpret_cast<void**>(&data[index])), "Map depth/stencil oracle");
    std::uint64_t checked = 0;
    for (UINT plane = 0; plane < 2; ++plane) {
      const auto& footprint = footprints_[plane];
      const auto pixel_bytes = static_cast<SIZE_T>(row_bytes_[plane] / 768);
      const auto* background = data[0] + footprint.Offset;
      const auto* app_patch = background + UINT64{800} * footprint.Footprint.RowPitch + 20 * pixel_bytes;
      require(std::memcmp(background, app_patch, pixel_bytes) != 0,
              "The original app draw must modify both depth and stencil in its lower patch");
      for (UINT row = 0; row < rows_[plane]; ++row) {
        const auto offset = footprint.Offset + UINT64{row} * footprint.Footprint.RowPitch;
        require(std::memcmp(data[0] + offset, data[1] + offset, static_cast<SIZE_T>(row_bytes_[plane])) == 0,
                "Stamp or restored app draw changed depth/stencil contents");
        checked += row_bytes_[plane];
      }
    }
    const D3D12_RANGE no_writes{0, 0};
    for (auto& output : readbacks_)
      output->Unmap(0, &no_writes);
    return checked;
  }

 private:
  Reference<ID3D12Resource> texture_;
  Reference<ID3D12DescriptorHeap> heap_;
  std::array<Reference<ID3D12Resource>, 2> readbacks_;
  D3D12_CPU_DESCRIPTOR_HANDLE handle_{};
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints_{};
  std::array<UINT, 2> rows_{};
  std::array<UINT64, 2> row_bytes_{};
  UINT64 bytes_ = 0;
};

std::uint64_t state_tests() {
  std::uint64_t checks = 0;
  auto expect = [&](bool value, const char* message) {
    require(value, message);
    ++checks;
  };
  using namespace reshade::api;
  pipeline_layout_param params[7]{};
  params[0].type = pipeline_layout_param_type::push_constants;
  params[0].push_constants.count = 4;
  params[1].type = pipeline_layout_param_type::push_descriptors;
  params[1].push_descriptors.count = 1;
  params[1].push_descriptors.type = descriptor_type::constant_buffer;
  params[2] = params[1];
  params[2].push_descriptors.type = descriptor_type::buffer_shader_resource_view;
  params[3] = params[1];
  params[3].push_descriptors.type = descriptor_type::buffer_unordered_access_view;
  descriptor_range range{};
  range.count = 1;
  params[4].type = params[5].type = pipeline_layout_param_type::descriptor_table;
  params[4].descriptor_table = params[5].descriptor_table = {1, &range};
  descriptor_range_with_flags static_range{};
  sampler_desc static_sampler{};
  static_range.type = descriptor_type::sampler;
  static_range.count = 1;
  static_range.static_samplers = &static_sampler;
  params[6].type = pipeline_layout_param_type::push_descriptors_with_ranges_and_flags;
  params[6].descriptor_table_with_flags = {1, &static_range};
  const auto layout = taxi_camera::parse_pfd_root_layout(7, params);
  expect(layout.valid && layout.count == 6, "Static sampler pseudo-param was not excluded");
  expect(!taxi_camera::parse_pfd_root_layout(66, params).valid, "Layout cap accepted");
  for (UINT n = 0; n < 6; ++n)
    expect(layout.parameters[n].count == (n ? 1u : 4u), "Root metadata count mismatch");
  for (UINT n = 0; n < 65; ++n) {
    params[0].push_constants.count = n;
    const auto parsed = taxi_camera::parse_pfd_root_layout(1, params);
    expect(parsed.valid == (n > 0), "Constant bound mismatch");
  }
  params[0].push_constants.count = 65;
  expect(!taxi_camera::parse_pfd_root_layout(1, params).valid, "Oversized constants accepted");
  PfdGraphicsState state;
  state.reset(1, true);
  state.bind_pipeline(reinterpret_cast<ID3D12PipelineState*>(0x1000));
  state.bind_root(reinterpret_cast<ID3D12RootSignature*>(0x2000), 1, layout);
  const D3D12_VIEWPORT viewport{0, 0, 768, 1024, 0, 1};
  const D3D12_RECT scissor{0, 0, 768, 1024};
  state.viewports(0, 1, &viewport);
  state.scissors(0, 1, &scissor);
  state.topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  UINT values[4]{};
  for (UINT n = 0; n < 4; ++n) {
    expect(!state.complete(), "Partial root constants accepted");
    state.constants(0, n, 1, values + n);
  }
  state.descriptor(1, PfdRootKind::cbv, 0);
  state.descriptor(2, PfdRootKind::srv, 0);
  state.descriptor(3, PfdRootKind::uav, 0);
  ID3D12DescriptorHeap* heaps[]{reinterpret_cast<ID3D12DescriptorHeap*>(0x3000), reinterpret_cast<ID3D12DescriptorHeap*>(0x4000)};
  state.descriptor_heaps(2, heaps);
  state.table(4, 0);
  state.table(5, 0);
  expect(state.complete(), "Known null root values wrongly unknown");
  state.descriptor_heaps(2, heaps);
  expect(state.complete(), "Identical heap set invalidated tables");
  ID3D12DescriptorHeap* reversed[]{heaps[1], heaps[0]};
  state.descriptor_heaps(2, reversed);
  expect(state.complete(), "Reordered identical heap set invalidated tables");
  auto one_heap = state;
  one_heap.descriptor_heaps(1, heaps);
  expect(!one_heap.complete(), "Removed heap retained tables");
  one_heap.table(4, 0);
  one_heap.table(5, 0);
  one_heap.descriptor_heaps(1, heaps);
  expect(one_heap.complete(), "Identical single heap invalidated tables");
  auto replacement = state;
  ID3D12DescriptorHeap* other[]{heaps[0], reinterpret_cast<ID3D12DescriptorHeap*>(0x5000)};
  replacement.descriptor_heaps(2, other);
  expect(!replacement.complete(), "Changed heap retained tables");
  replacement.descriptor_heaps(2, heaps);
  expect(!replacement.complete(), "Returning old heap set restored undefined tables");
  for (UINT bad = 0; bad < 4; ++bad) {
    auto malformed = state;
    ID3D12DescriptorHeap* invalid[]{bad == 2 ? nullptr : heaps[0], heaps[0]};
    malformed.descriptor_heaps(bad == 0 ? 3 : bad == 1 ? 1 : 2, bad == 1 ? nullptr : invalid);
    expect(!malformed.complete(), "Malformed heap arguments accepted");
  }
  state.descriptor_heaps(0, nullptr);
  expect(!state.complete(), "Unbinding all heaps retained tables");
  state.descriptor_heaps(2, heaps);
  state.table(4, 0);
  state.table(5, 0);
  expect(state.complete(), "Heap recovery failed");
  state.bind_root(reinterpret_cast<ID3D12RootSignature*>(0x2000), 2, layout);
  expect(!state.complete(), "Root address reuse retained old arguments");
  state.reset(2, false);
  expect(!state.complete(), "Missing native observations accepted");
  auto exact = replacement;
  exact.bind_root(reinterpret_cast<ID3D12RootSignature*>(0x6000), 3, layout, true);
  exact.constants(0, 0, 4, values);
  exact.descriptor(1, PfdRootKind::cbv, 0);
  exact.descriptor(2, PfdRootKind::srv, 0);
  exact.descriptor(3, PfdRootKind::uav, 0);
  expect(exact.complete() && exact.undefined_table_count() == 2, "Exact native root change failed to establish undefined tables");
  exact.table(4, 0);
  expect(exact.complete() && exact.undefined_table_count() == 1, "Known table did not replace undefined state");
  exact.bind_root(reinterpret_cast<ID3D12RootSignature*>(0x6000), 3, layout, true);
  expect(exact.undefined_table_count() == 1, "Same native root rebind erased known arguments");
  exact.descriptor_heaps_changed();
  expect(!exact.complete() && exact.undefined_table_count() == 0, "Unobserved invalidation was mistaken for undefined state");
  exact.descriptor_heaps(1, heaps);
  expect(exact.complete() && exact.undefined_table_count() == 2, "Observed native heap change did not establish undefined state");
  auto public_only = exact;
  public_only.bind_root(reinterpret_cast<ID3D12RootSignature*>(0x7000), 4, layout);
  public_only.constants(0, 0, 4, values);
  public_only.descriptor(1, PfdRootKind::cbv, 0);
  public_only.descriptor(2, PfdRootKind::srv, 0);
  public_only.descriptor(3, PfdRootKind::uav, 0);
  public_only.descriptor_heaps(0, nullptr);
  expect(!public_only.complete() && public_only.undefined_table_count() == 0, "Missing native root history admitted tables");
  return checks;
}

void stamp_run(bool warp, bool depth_stencil, bool unknown_depth_pso) {
  const auto checks = state_tests();
#ifdef TAXI_PFD_REAL_ADAPTER
  register_live_adapter();
#endif
  Reference<ID3D12Debug> debug;
  bool debug_enabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())));
  if (debug_enabled)
    debug->EnableDebugLayer();
  Reference<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "Create factory");
  Reference<IDXGIAdapter> adapter;
  if (warp)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "Get WARP");
  Reference<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "Create device");
  ID3D12Device* owned_device = device.get();
#ifdef TAXI_PFD_REAL_ADAPTER
  require(live_device && live_native_device && live_error.empty(), "Real ReShade device callback missing");
  owned_device = live_native_device;
#endif
  Reference<ID3D12InfoQueue> messages;
  if (debug_enabled)
    device->QueryInterface(IID_PPV_ARGS(messages.put()));
  Compositor compositor;
  check(compositor.initialize(owned_device), "Initialize compositor");
  GradientGenerator generator(owned_device);
  taxi_camera::PfdStampD3D12 stamp;
  check(stamp.initialize(owned_device, DXGI_FORMAT_R8G8B8A8_UNORM,
                         depth_stencil && !unknown_depth_pso ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_UNKNOWN),
        "Initialize PFD stamp");
  StampApplication app(device.get(), depth_stencil);
  DepthStencilFixture depth;
  if (depth_stencil)
    depth.initialize(device.get());
  Reference<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap.NumDescriptors = 4;
  check(owned_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(rtv_heap.put())), "Create private RTVs");
  const auto first = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  const auto stride = owned_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> rtvs{};
  for (UINT i = 0; i < 4; ++i)
    rtvs[i].ptr = first.ptr + i * stride;
  Reference<ID3D12DescriptorHeap> pfd_rtv_heap;
  heap.NumDescriptors = 1;
  check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(pfd_rtv_heap.put())), "Create application RTV");
  rtvs[2] = pfd_rtv_heap->GetCPUDescriptorHandleForHeapStart();
  Reference<ID3D12Resource> nose, tail, pfd;
  create_texture(owned_device, texture_description(17, 11, DXGI_FORMAT_R8G8B8A8_UNORM), nose.put());
  create_texture(owned_device, texture_description(29, 19, DXGI_FORMAT_R8G8B8A8_UNORM), tail.put());
  const auto pfd_desc = texture_description(768, 1024, DXGI_FORMAT_R8G8B8A8_UNORM);
  create_texture(device.get(), pfd_desc, pfd.put());
  owned_device->CreateRenderTargetView(nose.get(), nullptr, rtvs[0]);
  owned_device->CreateRenderTargetView(tail.get(), nullptr, rtvs[1]);
  device->CreateRenderTargetView(pfd.get(), nullptr, rtvs[2]);
  const auto inputs_hr = compositor.set_inputs(nose.get(), DXGI_FORMAT_R8G8B8A8_UNORM, tail.get(), DXGI_FORMAT_R8G8B8A8_UNORM);
  check(inputs_hr, compositor.last_error());
  std::array<taxi_camera::PfdStampFrame, 2> frames;
  for (UINT frame = 0; frame < 2; ++frame) {
    check(frames[frame].initialize(owned_device), "Initialize immutable stamp frame");
    PrivateSubmission producer(owned_device);
    if (!frame)
      app.prepare(owned_device, producer.list(), rtvs[3]);
    generator.record(producer.list(), rtvs[0], 17, 11, false, frame, 0);
    generator.record(producer.list(), rtvs[1], 29, 19, false, frame, 1);
    check(compositor.record(producer.list(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET),
          "Compose GPU sources");
    require(frames[frame].record_copy(producer.list(), compositor.output()), "Record GPU-only stamp buffer copy");
    require(!frames[frame].publish(false), "Premature frame publication");
    producer.finish(compositor);
    require(frames[frame].publish(true), "Publish completed immutable frame");
  }
  Reference<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.put())), "Create consumer queue");
  Reference<ID3D12CommandAllocator> allocator;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())), "Create consumer allocator");
  Reference<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put())),
        "Create consumer list");
  observer.state.reset(1, true);
#ifndef TAXI_PFD_REAL_ADAPTER
  const Observer::Callbacks callbacks{nullptr, observe_heaps, observe_cbv, observe_reset, observe_root, observe_table};
  const auto installed = Observer::register_list(list.get(), 1, callbacks);
  require(installed.ready && installed.protection_restored, installed.status);
#else
  require(live_last_list && live_error.empty(), live_error.empty() ? "Real list callback missing" : live_error.c_str());
  auto* actual_api_list = live_last_list;
#endif
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes = 0;
  device->GetCopyableFootprints(&pfd_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  Reference<ID3D12Resource> readback;
  create_buffer(device.get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, readback.put());
  constexpr std::array<unsigned char, 4> sentinel{7, 13, 29, 211};
  const float clear[]{7.f / 255, 13.f / 255, 29.f / 255, 211.f / 255};
  std::uint64_t pixels = 0, lower = 0, depth_stencil_bytes = 0;
  for (UINT frame = 0; frame < 2; ++frame) {
    if (frame) {
      check(allocator->Reset(), "Reset consumer allocator");
      check(list->Reset(allocator.get(), app.pipeline()), "Reset consumer list");
#ifndef TAXI_PFD_REAL_ADAPTER
      require(observer.resets == 1, "Successful native Reset notification missing");
#else
      require(live_manager->statistics().resets >= 1, "Actual adapter native Reset did not retire manager recording");
#endif
      transition(list.get(), pfd.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    list->ClearRenderTargetView(rtvs[2], clear, 0, nullptr);
    depth.clear(list.get());
    app.bind(list.get(), rtvs[2], depth.handle());
    list->DrawInstanced(4, 1, 0, 0);
    depth.snapshot(list.get(), 0);
    const auto cbv_before = observer.cbv;
#ifndef TAXI_PFD_REAL_ADAPTER
    require(stamp.record(list.get(), observer.state, frames[frame], 768, 1024), "Stamp complete state");
#else
    require(live_manager->register_consumer_recording(reinterpret_cast<ID3D12GraphicsCommandList*>(actual_api_list->get_native())),
            "Register real consumer recording");
    const bool stamped = Adapter::record_stamp(actual_api_list, stamp, owned_device, frames[frame].address(), 768, 1024);
    require(stamped, Adapter::statistics().last_error);
#endif
    require(observer.cbv == cbv_before, "Internal native restore reentered observation callbacks");
    // Erase the preceding application's lower patch; the NEXT draw receives no
    // bindings whatsoever and must recreate it using all restored app state.
    const D3D12_RECT erase{20, 800, 44, 824};
    list->ClearRenderTargetView(rtvs[2], clear, 1, &erase);
    list->DrawInstanced(4, 1, 0, 0);
    depth.snapshot(list.get(), 1);
    // A legal final app draw deliberately overwrites the entire camera area.
    // Its unused t99 table remains positively undefined. A final stamp must
    // restore the camera image instead of leaving the application content.
    const D3D12_RECT upper{0, 0, 768, 763};
    list->RSSetScissorRects(1, &upper);
    observer.state.scissors(0, 1, &upper);
    list->DrawInstanced(4, 1, 0, 0);
#ifndef TAXI_PFD_REAL_ADAPTER
    require(stamp.record(list.get(), observer.state, frames[frame], 768, 1024), "Final writer stamp with undefined unused table");
#else
    require(Adapter::record_stamp(actual_api_list, stamp, owned_device, frames[frame].address(), 768, 1024),
            Adapter::statistics().last_error);
#endif
    transition(list.get(), pfd.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = pfd.get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    check(list->Close(), "Close consumer list");
    ID3D12CommandList* batch[]{list.get()};
    queue->ExecuteCommandLists(1, batch);
    if (!taxi_camera::drain_copy_queue(queue.get(), device.get()))
      ExitProcess(1);
    depth_stencil_bytes += depth.verify_unchanged();
    unsigned char* data = nullptr;
    const D3D12_RANGE read{0, static_cast<SIZE_T>(bytes)};
    check(readback->Map(0, &read, reinterpret_cast<void**>(&data)), "Read validation pixels");
    for (UINT y = 0; y < 1024; ++y)
      for (UINT x = 0; x < 768; ++x) {
        std::array<unsigned char, 4> expected{0, 0, 0, 255};
        int tolerance = 0;
        if (y >= 763) {
          expected = sentinel;
          ++lower;
          if (x >= 20 && x < 44 && y >= 800 && y < 824)
            expected = {59, 60, 84, 255};
        } else if (!reference_overlay_oracle::pixel(x, y, expected)) {
          const UINT source = y < 255 ? 0 : 1;
          const Source dimensions{source ? 29u : 17u,
                                  source ? 19u : 11u,
                                  DXGI_FORMAT_R8G8B8A8_UNORM,
                                  DXGI_FORMAT_R8G8B8A8_UNORM,
                                  false,
                                  false,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET};
          for (UINT c = 0; c < 3; ++c)
            expected[c] = expected_channel(dimensions, x, source ? y - 259 : y, source ? 504 : 255, c, frame, source);
          tolerance = 2;
        }
        const auto* actual = data + footprint.Offset + static_cast<UINT64>(y) * footprint.Footprint.RowPitch + x * 4;
        for (UINT c = 0; c < 4; ++c)
          if (std::abs(int(actual[c]) - int(expected[c])) > (c == 3 ? 0 : tolerance)) {
            char error[256]{};
            std::snprintf(error, sizeof(error), "PFD stamp mismatch frame%u (%u,%u) channel%u got%u expected%u", frame, x, y, c,
                          unsigned(actual[c]), unsigned(expected[c]));
            throw std::runtime_error(error);
          }
        ++pixels;
      }
    const D3D12_RANGE no_writes{0, 0};
    readback->Unmap(0, &no_writes);
  }
#ifndef TAXI_PFD_REAL_ADAPTER
  Observer::unregister_list(list.get(), 1);
  observer.object_generation = 2;
  require(Observer::register_list(list.get(), 2, callbacks).ready, "Retired list address could not register a new incarnation");
  require(!Observer::register_list(list.get(), 3, callbacks).ready, "Unretired list address generation accepted");
  Observer::unregister_list(list.get(), 2);
  const auto removed = Observer::remove();
  require(removed.protection_restored && std::string(removed.status) == "removed", "Remove observer slots");
#else
  require(Adapter::statistics().stamped == 4 && Adapter::statistics().stamps_with_undefined_tables == 4,
          "Actual ReShade adapter did not preserve undefined unused tables on every stamp");
  std::printf("{\"realReShadeAdapter\":true,\"stamped\":%llu,\"refused\":%llu,\"successfulResets\":%llu,\"undefinedTableStamps\":%llu}\n",
              static_cast<unsigned long long>(Adapter::statistics().stamped),
              static_cast<unsigned long long>(Adapter::statistics().refused),
              static_cast<unsigned long long>(live_manager->statistics().resets),
              static_cast<unsigned long long>(Adapter::statistics().stamps_with_undefined_tables));
#endif
  std::uint64_t debug_errors = 0;
  if (messages.get())
    for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
      SIZE_T size = 0;
      messages->GetMessage(i, nullptr, &size);
      std::vector<unsigned char> storage(size);
      auto* m = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      messages->GetMessage(i, m, &size);
      if (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
        ++debug_errors;
        std::fprintf(stderr, "D3D12: %s\n", m->pDescription);
      }
    }
  require(debug_errors == 0, "D3D12 validation errors");
  std::printf(
      "{\"passed\":true,\"warp\":%s,\"debugLayer\":%s,\"debugErrors\":%llu,\"frames\":2,\"checkedPixels\":%llu,\"lowerPixels\":%llu,"
      "\"stateChecks\":%llu,\"nativeHeapObservations\":%llu,\"nativeCbvObservations\":%llu,\"successfulResetObservations\":%llu,"
      "\"boundDepthStencil\":%s,\"unknownDepthPso\":%s,\"unchangedDepthStencilBytes\":%llu}\n",
      warp ? "true" : "false", debug_enabled ? "true" : "false", static_cast<unsigned long long>(debug_errors),
      static_cast<unsigned long long>(pixels), static_cast<unsigned long long>(lower), static_cast<unsigned long long>(checks),
      static_cast<unsigned long long>(observer.heaps), static_cast<unsigned long long>(observer.cbv),
      static_cast<unsigned long long>(observer.resets), depth_stencil ? "true" : "false", unknown_depth_pso ? "true" : "false",
      static_cast<unsigned long long>(depth_stencil_bytes));
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    bool warp = false, depth_stencil = false, unknown_depth_pso = false;
    for (int index = 1; index < argc; ++index) {
      const std::wstring argument(argv[index]);
      if (argument == L"--warp" && !warp)
        warp = true;
      else if (argument == L"--depth-stencil" && !depth_stencil)
        depth_stencil = true;
      else if (argument == L"--depth-stencil-unknown-pso" && !depth_stencil)
        depth_stencil = unknown_depth_pso = true;
      else
        require(false, "Usage: pfd-stamp-validation.exe [--warp] [--depth-stencil|--depth-stencil-unknown-pso]");
    }
    stamp_run(warp, depth_stencil, unknown_depth_pso);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
