#pragma once

#include <cstdint>

#include "../hooks/render_boundary_observer.hpp"

namespace taxi_camera::standalone::source_invalidation {

namespace boundary = engine_hook::render_boundary;

// These reasons are local to the render pass or exact resource set already
// identified by the bridge. Any other uncertainty must stay on the
// recording/global fallback path.
inline constexpr std::uint32_t kExactTargetReasons =
    boundary::InvalidationPassBegin | boundary::InvalidationPassState | boundary::InvalidationSplitBarrier |
    boundary::InvalidationAliasOrDiscard;

constexpr bool can_scope_to_exact_targets(std::uint32_t reasons) noexcept {
  return reasons != 0 && (reasons & ~kExactTargetReasons) == 0;
}

static_assert(can_scope_to_exact_targets(boundary::InvalidationPassState));
static_assert(can_scope_to_exact_targets(boundary::InvalidationPassBegin | boundary::InvalidationPassState));
static_assert(can_scope_to_exact_targets(boundary::InvalidationPassState | boundary::InvalidationSplitBarrier));
static_assert(!can_scope_to_exact_targets(boundary::InvalidationPassState | boundary::InvalidationBarrierBatch));
static_assert(!can_scope_to_exact_targets(boundary::InvalidationPassState | boundary::InvalidationUnobservedWork));
static_assert(!can_scope_to_exact_targets(boundary::InvalidationPassState | boundary::InvalidationObserverDisabled));
static_assert(!can_scope_to_exact_targets(boundary::InvalidationPassState | boundary::InvalidationResetFailed));

}  // namespace taxi_camera::standalone::source_invalidation
