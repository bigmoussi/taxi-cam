#include "../../src/bridge/source_invalidation_policy.hpp"

#include <cstdint>
#include <cstdio>

int main() {
  using namespace taxi_camera::standalone;
  namespace boundary = taxi_camera::engine_hook::render_boundary;

  constexpr std::uint32_t strong = boundary::InvalidationBarrierBatch | boundary::InvalidationUnobservedWork |
                                   boundary::InvalidationObserverDisabled | boundary::InvalidationResetFailed;
  unsigned checked = 0;
  for (std::uint32_t reasons = 0; reasons <= 0xff; ++reasons) {
    const bool expected = reasons != 0 && (reasons & strong) == 0;
    if (source_invalidation::can_scope_to_exact_targets(reasons) != expected) {
      std::fprintf(stderr, "policy mismatch for reasons=0x%02x\n", static_cast<unsigned>(reasons));
      return 1;
    }
    ++checked;
  }
  std::printf("{\"checked\":%u,\"passStateExactTarget\":true,\"strongReasonsConservative\":true}\n", checked);
  return 0;
}
