#pragma once

#include "memory_reader.hpp"

#include <array>
#include <cstdint>

namespace taxi_camera::engine_camera {

enum class ViewPoolStatus {
  not_inspected,
  complete,
  invalid_renderer,
  invalid_array,
  invalid_view,
  duplicate_view,
  invalid_control,
  read_failed,
  changed,
  read_budget_exhausted
};

enum class ViewAssociation { unobserved, null_control, stale_generation, null_payload, occupied };

struct ViewSlot {
  std::uint32_t index = 0;
  // Internal addresses only. Do not serialize these into UI/log/JSON output.
  std::uint64_t view_address = 0;
  ViewAssociation association = ViewAssociation::unobserved;
  bool free = false;
  bool association_valid = false;
};

struct ViewPoolSnapshot {
  bool valid = false;
  ViewPoolStatus status = ViewPoolStatus::not_inspected;
  // Internal pointer; never a lifetime token or a reservation.
  std::uint64_t array_address = 0;
  std::uint32_t slots_examined = 0;
  std::int32_t failure_slot = -1;
  // Attempted bytes, including failed reads, capped at 8192. The current
  // eight-slot walk plus complete trace reread uses at most 592 bytes.
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
  std::array<ViewSlot, 8> slots{};
  // Published only after all slots and their consistency rechecks succeed.
  std::uint32_t free_count = 0;
  std::array<std::int32_t, 2> first_free_indices{-1, -1};
};

// Reproduce the captured 66864720 availability predicate over eight EXISTING
// views. renderer+2752 contains a pointer to an array, not an inline array.
// Read only its eight view pointers, each P+72 sixteen-byte association, and
// nonnull control+28 generation / matching control+0 payload. Null/stale/null-
// payload handles are free; a null or duplicate P is an error. Reread every
// observed field at its exact address before publishing complete capacity.
// Control records may be byte-aligned; renderer, array and
// view pointers still require eight-byte alignment. No pointer bits are masked.
// Never call an engine function, reserve
// a slot, mutate memory, query the cached global or follow an association payload.
// The adapter owns readable-region checks and cached-renderer/build validation.
// Call in the same engine phase as prospective creation: this detects observed
// changes but is not atomic, a lifetime guarantee or a capacity reservation.
// The reader must not throw; report unavailable memory by returning false.
// This helper is not an SEH fault barrier.
ViewPoolSnapshot inspect_view_pool(MemoryReader& reader, std::uint64_t renderer) noexcept;

}  // namespace taxi_camera::engine_camera
