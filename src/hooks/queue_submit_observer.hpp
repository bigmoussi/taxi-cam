#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

#include <cstdint>

namespace taxi_camera::engine_hook::queue_submit {

inline constexpr unsigned kMaximumQueues = 8;
inline constexpr unsigned kMaximumCommandLists = 256;

enum class Refusal { oversized_batch, invalid_batch, reentrant_submission };

struct Callbacks {
  void* context = nullptr;
  // Snapshot recording generations and retain their capture state here. Return
  // an opaque nonzero receipt for owned capture work, or zero for no such work.
  std::uint64_t (*before)(void*, ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept = nullptr;
  // Called only AFTER the exact original ExecuteCommandLists returns. It may
  // enqueue Queue::Signal and publish that receipt's pending fence value. A
  // successful queue Signal is submission evidence, not GPU completion.
  // It may submit its own private compositor lists: nested Execute calls forward
  // directly without observation. A private tail capture may be submitted only
  // after attaching its ownership to this outer receipt; the outer Signal must
  // cover it. Nested submissions receive no independent capture receipt.
  void (*after)(void*, ID3D12CommandQueue*, std::uint64_t receipt) noexcept = nullptr;
  // No completion is reported on a refused observation. Invalidate outstanding
  // capture publication for this queue, retaining referenced GPU resources.
  void (*refused)(void*, ID3D12CommandQueue*, Refusal) noexcept = nullptr;
};

enum class Status {
  registered,
  already_registered,
  disabled,
  not_registered,
  removed,
  not_installed,
  invalid_argument,
  invalid_queue_memory,
  invalid_slot_memory,
  invalid_original,
  different_vtable,
  queue_limit,
  callback_mismatch,
  slot_changed,
  installation_consumed,
  pin_failed,
  protection_change_failed,
  protection_restore_failed,
  protection_restored,
  reentrant_operation,
};

struct Result {
  Status status = Status::not_installed;
  DWORD windows_error = ERROR_SUCCESS;
  bool hook_installed = false;
  bool queue_retained = false;
  bool protection_restored = true;
};

struct Statistics {
  std::uint64_t submissions = 0;
  std::uint64_t receipts = 0;
  std::uint64_t refusals = 0;
};

// Current-process public COM objects supplied by the caller only. Installs one
// shared native ID3D12CommandQueue vtable slot10, without copying a vtable or
// editing executable instructions. At most8 queue identities on that same
// vtable can be registered. Unknown queues sharing it forward unchanged.
//
// The queue MUST be a live, valid native COM interface, not an unverified interface wrapper.
// Registration keeps one AddRef permanently and pins this module, the original
// function's image and the vtable's image when applicable. Once queue_retained
// is true, including a partial failure, the callback/context association is
// immutable; the caller must retain its context until process exit. There is
// no live unloading, queue-reference release or executable-code allocation.
// Failed registration retains no queue reference unless queue_retained is true.
//
// Registered calls serialize before/original/after on that queue. The wrapper
// forwards the original queue/count/list-array exactly once, even when capture
// is refused. Notifications must not throw, call registration/removal APIs or
// take locks that can be held by their callers. Only after may submit private
// compositor or explicitly owned tail work as described above; a nested before/original submission
// quarantines the outer receipt through refused.
// The original public COM method uses its documented Win64 ABI. This is not an
// SEH fault barrier or a guarantee that invalid application submissions execute.
Result register_queue(ID3D12CommandQueue* queue, const Callbacks& callbacks) noexcept;

// Optional native bootstrap discovery. Called outside the submission/registry locks,
// before observation begins, with the application's live queue argument. The callback
// may register a DIRECT queue. No default means the original fail-closed behavior.
using UnknownQueueObserver = void (*)(ID3D12CommandQueue*) noexcept;
bool set_unknown_queue_observer(UnknownQueueObserver) noexcept;

// Disables notifications and waits only for an already-entered CPU wrapper on
// this queue to return. Does not wait for the GPU or release anything. Repeating
// registration with the identical callbacks may enable this retained queue.
Result disable_queue(ID3D12CommandQueue* queue) noexcept;

// Stops notifications and exchanges only our slot value for the saved original.
// A successful removal permanently consumes this installation. This does not
// prove quiescence: a thread may already have fetched the wrapper address.
// All retained modules, contexts and native queue references remain alive.
Result remove() noexcept;
Result restore_protection() noexcept;
Statistics statistics(ID3D12CommandQueue* queue) noexcept;
const char* status_name(Status status) noexcept;

}  // namespace taxi_camera::engine_hook::queue_submit
