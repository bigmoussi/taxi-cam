# Render-thread freeze guards

The bridge runs on the simulator's threads: command-list recording hooks, `Close`, `Reset`, device creation hooks, the `ExecuteCommandLists` wrapper (application render thread, Present and frame-generation helpers, other injectors' helpers) and the D3D12 private-data release that retires our metadata. None of those threads may be parked on a bridge lock. A bridge worker may hold a lock for milliseconds (discovery, private compose submission, pipeline preparation); the same lock taken blocking on a simulator thread turns that into a visible stall, and a lock cycle through a driver-internal lock turns it into the soft freeze reported in issues 27, 53 and 69 (frames and telemetry stop, bridge worker keeps logging).

## Rule

Simulator threads acquire bridge locks through `BoundedLock` (`src/shared/bounded_lock.hpp`) with a budget and skip their work for the current frame when it expires. Budgets: 100 us per-command recording hooks, 500 us Close-time PFD delivery, 1 ms `ExecuteCommandLists` ordering, 5 ms creation and destruction paths. Bridge-owned threads keep ordinary blocking locks.

A skip always leaves the bridge in a state that refuses injection rather than guessing:

| Path | On an expired wait |
| --- | --- |
| `find_list` registry miss | Null result; the next successful lookup of that list on the same thread invalidates the recording once (state between miss and hit was untracked). |
| `OMSetRenderTargets`, render-pass targets, clears | Targets cleared, `raw_om_known=false`, submission proof `contended`, copy proof invalidated. |
| `SetGraphicsRootSignature` | Observed root cleared; the stamp restore refuses an incomplete state. |
| `stage_pfd`, `drain_pfds`, barrier copy, `copy_patch`, `stamp_at_recording_end` | No PFD write this recording (`skipped_pfd_writes`, `runtime_writes`). |
| Device hooks (resource, RTV/DSV, descriptor copy, root creation) | Entry stays unrecorded; a skipped descriptor copy or RTV marks the view maps stale so no write trusts them until `discover_pfds` clears and relearns them. Missed source candidates are registered by the worker. |
| Capture-manager evidence callbacks | Publish `deferred_sources_` for every device; the next transaction invalidates the source model (existing contended-path mechanism). |
| `successful_reset`, `destroy_command_list` (lifetime release) | Deferred through a ring drained under the manager lock by the next transaction or collect. Overflow fails the devices. |
| `before_submission` ordering fallbacks | Escape exactly like the PR 31 contended path: owned recordings marked before the forward, source invalidation published after it through `forwarded_unordered`. No transaction, so no unpaired `queue->Wait`. |
| `Resource::retire` -> handoff | Deferred to the worker; ring overflow raises the handoff's global lifecycle event. |

`finish_transaction` remains blocking: it runs on the thread that already owns `submission_mutex_` and must `Signal` the timeline it queued a `Wait` on. Manager `mutex_` sections are CPU-only plus D3D12 calls on that same thread, so this wait is bounded by our own work.

## Presentation watchdog

`src/bridge/freeze_watchdog.hpp`, hosted on a dedicated bridge thread that reads atomics only. The pulse counts hooked `ExecuteCommandLists` on registered direct queues plus hooked `Close`. If the pulse stops for 3 s while the bridge is connected, the watchdog closes the graphics gate (`observation_enabled()` false, PFD plans `not_ready`, capture manager escapes every submission) with an atomic store, the worker disarms cameras and observation on its next iteration, and one `Presentation watchdog:` line is logged with the contention counters. After 5 s of frames it re-arms and logs again. SIM_FRAME telemetry stalling while frames continue (pause, menu) is logged once and does not trip; it substitutes for the pulse only before the first submit has been observed.

The gate cannot wake a thread already parked inside a driver call or a GPU `Wait`. Its job, after the bounded waits above, is to stop re-entry into contended work and make the degraded state visible.

## Log counters

`Render-thread contention:` every 5 s: `armed`, `pulse`, `queue_calls`, `queue_contended` (submit lock busy: other threads on the same queue), `manager_evidence`/`manager_lifecycle`/`manager_submit`, `unordered`, `gated`, `deferred_retirements`, `deferred_lifecycle`, `runtime_writes`, `watchdog_trips`, `last_stall_ms`, `sim_messages`, then the bridge sites `registry_recording`, `registry_close`, `registry_creation`, `registry_lifecycle`, `observation_recording`, `handoff_lifecycle`, `skipped_pfd_writes`. High `queue_contended`/`unordered` with low bridge skips means another injector's pressure on the queue, not bridge lock contention.

## In-simulator messages

`src/shared/sim_messages.hpp` maps events to text; the SimConnect telemetry worker (`body_pose_provider.cpp`), which owns the only SimConnect connection, sends them with `SimConnect_Text` (MESSAGE_WINDOW, PRINT fallback after a refused packet). Setting `settings.ini` `[messages] in_simulator` (default on) and the tray toggle "Show messages in simulator". Events: connected, cameras ready, connection stopped, simulator unsupported, presentation stalled (watchdog), cameras disarmed, presentation resumed, speed cutoff, aircraft mismatch, camera startup failed, capture paused. Per-event repeat interval and a 4-per-20 s budget prevent spam. Nothing is sent from a render thread; the queue shares no state with graphics resources.

## Validation

`freeze-guards`, `sim-messages` and the extended `queue-submit` tests run in `build.ps1 -Validate`. Local validation does not establish live behaviour under DLSS Frame Generation; see the PR for the live check that still has to be done.
