#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#ifndef TAXI_NATIVE_RUNTIME
#include <reshade_api.hpp>
#endif

#include <array>
#include <cstdint>
#include <cstring>

namespace taxi_camera {

enum class PfdRootKind : std::uint8_t { constants, table, cbv, srv, uav };
struct PfdRootParameter {
  PfdRootKind kind = PfdRootKind::constants;
  std::uint32_t count = 0;
};
struct PfdRootLayout {
  bool valid = false;
  std::uint32_t count = 0;
  std::array<PfdRootParameter, 64> parameters{};
};

// Pinned ReShade 6.8 D3D12 init_pipeline_layout metadata only. Static sampler
// metadata is appended by that implementation and is NOT a native root index.
#ifndef TAXI_NATIVE_RUNTIME
PfdRootLayout parse_pfd_root_layout(std::uint32_t count, const reshade::api::pipeline_layout_param* parameters) noexcept;
#endif

struct PfdRootValue {
  std::uint64_t known = 0;
  std::uint64_t address = 0;
};

// One live command-list recording. External adapter serializes this object with
// the API's command-list recording calls; D3D12 does not permit concurrent calls
// on a list. Never retain this snapshot beyond the intercepted draw. Pointers
// are borrowed current graphics bindings, not resource lifetime guarantees.
// Register BOTH native state observations before tracking; reset on every list
// reset, retire on destroy, invalidate after ExecuteBundle or unsupported state.
class PfdGraphicsState {
 public:
  void reset(std::uint64_t generation, bool native_observations) noexcept;
  void invalidate(const char* reason = "graphics_state_invalidated") noexcept {
    if (!invalid_reason_)
      invalid_reason_ = reason;
  }
  void bind_pipeline(ID3D12PipelineState* pipeline) noexcept { pipeline_ = pipeline; }
  void bind_root(ID3D12RootSignature* root,
                 std::uint64_t layout_generation,
                 const PfdRootLayout& layout,
                 bool exact_native_change = false) noexcept;
  // Native-only observations from the beginning of a recording also support
  // signatures created before the bridge loaded. Replay only arguments actually
  // set since Reset/signature change; all others were undefined in the app.
  void bind_observed_root(ID3D12RootSignature* root, std::uint64_t generation) noexcept;
  void constants(UINT index, UINT first, UINT count, const void* data) noexcept;
  void table(UINT index, UINT64 address) noexcept;
  void descriptor(UINT index, PfdRootKind kind, UINT64 address) noexcept;
  // Exact native SetDescriptorHeaps arguments. Rebinding the same complete set
  // preserves tables; an actual heap-set change makes them undefined.
  void descriptor_heaps(UINT count, ID3D12DescriptorHeap* const* heaps) noexcept;
  void descriptor_heaps_changed() noexcept;
  void topology(D3D12_PRIMITIVE_TOPOLOGY value) noexcept { topology_ = value; }
  void viewports(UINT first, UINT count, const D3D12_VIEWPORT* data) noexcept;
  void scissors(UINT first, UINT count, const D3D12_RECT* data) noexcept;
  bool complete() const noexcept;
  const char* incomplete_reason() const noexcept;
  UINT undefined_table_count() const noexcept;
  std::uint64_t generation() const noexcept { return generation_; }
  ID3D12RootSignature* root() const noexcept { return root_; }
  std::uint64_t layout_generation() const noexcept { return layout_generation_; }
  // Direct native replay is deliberate: the stamp bypasses ReShade shadow
  // state, so its public root setters may incorrectly optimize restoration out.
  void restore(ID3D12GraphicsCommandList* native) const noexcept;

 private:
  bool parameter(UINT index, PfdRootKind kind) noexcept;
  const char* invalid_reason_ = nullptr;
  bool observed_ = false;
  std::uint64_t generation_ = 0, layout_generation_ = 0;
  ID3D12PipelineState* pipeline_ = nullptr;
  ID3D12RootSignature* root_ = nullptr;
  PfdRootLayout layout_{};
  std::array<PfdRootValue, 64> values_{};
  // Only a native signature change or native heap change can establish this.
  // Missing public observations remain unknown and are never admitted.
  bool native_root_observed_ = false;
  std::array<bool, 64> undefined_tables_{};
  std::array<std::uint32_t, 64> constants_{};
  std::array<std::uint8_t, 64> constant_offsets_{};
  bool observed_arguments_ = false;
  UINT observed_word_count_ = 0;
  std::array<std::uint16_t, 64> observed_word_keys_{};
  bool heaps_known_ = false;
  UINT heap_count_ = 0;
  std::array<ID3D12DescriptorHeap*, 2> heaps_{};
  D3D12_PRIMITIVE_TOPOLOGY topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  UINT viewport_count_ = 0, scissor_count_ = 0;
  std::array<D3D12_VIEWPORT, 16> viewports_{};
  std::array<D3D12_RECT, 16> scissors_{};
};

}  // namespace taxi_camera
