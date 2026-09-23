# TaxiCam architecture review — 2026-09-22

## Scope

This review starts from `fix/scoped-pass-state-recovery` at `7536ea9b7365f09e47cfcd35a7f2529075aea940` after its Windows PR validation completed successfully. It focuses on the frozen-camera failure mode around render-pass invalidation, especially with a D3D12 proxy/add-on such as ReShade in the chain.

## Current validated fix

The PR changes the source-state fallback so a pass-state refusal does not automatically erase every published camera source. A recording that can name sources retires the named source generations; malformed/overflowing recordings and stronger invalidation reasons still fall back to global invalidation.

This is a useful safety net, but it is intentionally conservative: the set of sources named by a command-list recording can be broader than the render targets attached to the render pass that triggered `PassState`.

## Stronger evidence already available in the bridge

The native observer invokes `pass_targets` before it publishes render-pass invalidation. `pass_targets` resolves the native `D3D12_RENDER_PASS_RENDER_TARGET_DESC::cpuDescriptor` handles through the bridge RTV registry and stores the lifetime-qualified resources in `List::targets`.

For a valid descriptor set this gives the bridge a stronger fact than recording history:

- resource identity is exact (`ID3D12Resource*` plus bridge generation),
- the base-mip view is known when the descriptor metadata is known,
- the target belongs to the render pass that is causing the pass-state refusal,
- the bindings remain fixed for the duration of a D3D12 render pass.

The existing `invalidate` callback already uses these exact targets for the target-scoped `PassBegin`/split/alias path, through `SceneCaptureManager::invalidate_source_targets`. `PassState` currently falls through to `invalidate_source_recording`, so it loses this stronger evidence.

## Proposed runtime change

### Phase A — exact-target PassState scoping

Extend the bridge's target-scopable invalidation class to include `InvalidationPassState` **only when `List::targets` contains resolved live target generations**.

Expected behavior:

1. `BeginRenderPass` calls `pass_targets` first.
2. The bridge resolves and stores the actual pass RTV resources.
3. The observer reports `PassBegin | PassState` for suspended/resuming/malformed pass state.
4. The bridge keeps all PFD/submission safety invalidation exactly as today.
5. For camera-source state only, it calls `invalidate_source_targets` for the resolved current pass targets.
6. If no exact target is resolved, it keeps the PR fallback (`invalidate_source_recording`).
7. Any barrier-batch, observer-disabled, reset-failed or unobserved-work reason still takes the conservative global/fallback route.

This is narrower than the current PR fallback. A command list that rendered camera A earlier and later enters an unrelated bad pass on target B no longer retires camera A merely because A appears in that recording's historical effects.

### Safety argument

This change does **not** make an unsafe recording valid again.

- `submission_proof` and `copy_proof` invalidations stay unchanged.
- A `source_effects` recording already marked invalid by earlier global uncertainty remains invalid; appending a target-local effect cannot revive it.
- Exact target invalidation uses the existing generation-qualified manager API.
- Missing/malformed target metadata keeps the conservative fallback.
- `pass_ended` clears the pass targets only after the End callback; therefore an End-time pass-state refusal still sees the exact attachments.

### Required regression cases

Add production-bridge tests for:

1. previous camera A in recording + bad pass on exact unrelated B => A remains valid;
2. bad pass whose exact target is camera A => A loses RT evidence;
3. `PassBegin | PassState` with no resolved target => PR fallback remains active;
4. prior global invalidation + later exact-target PassState => recording remains globally invalid;
5. mixed `PassState | ResetFailed`, `ObserverDisabled`, `BarrierBatch` or `UnobservedWork` => never narrowed;
6. stale resource pointer with mismatched generation => never invalidates a replacement incarnation;
7. suspended/resuming pass End path sees targets until `pass_ended` and cannot restore capture permission.

## ReShade-specific direction

ReShade's public add-on event API exposes semantic D3D12 events for barriers, `begin_render_pass`, `end_render_pass`, and render-target bindings. When ReShade is present, an optional provider could use those semantic events to supplement the native observer and reduce ambiguity caused by proxy vtable chains.

It should **not** become TaxiCam's mandatory graphics backend:

- TaxiCam must continue to work without ReShade;
- a ReShade dependency would couple core camera delivery to another add-on's version/lifecycle;
- the current native bridge needs to remain the authority for exact application queue submission, resource lifetime and owned copy/composition work.

Recommended future shape: `NativeD3D12EvidenceProvider` remains mandatory; an optional `ReShadeEvidenceProvider` may contribute corroborating semantic metadata, never weaker evidence and never ownership of camera/PFD resources.

## MSFS 2024 Camera API direction

The public SimConnect Camera API is valuable for a user-facing add-on camera and should be monitored as it leaves beta. It does not currently document a facility for two simultaneous independent off-screen camera render targets. TaxiCam's two PFD camera feeds therefore cannot be migrated to it without losing the core product behavior.

Keep the existing private/internal off-screen camera creation behind the current capability/evidence gates. If the SDK later exposes independent off-screen camera instances or render surfaces, reevaluate this boundary first: replacing the private camera creation path would remove more maintenance risk than replacing the D3D12 composition path.

## Performance/maintenance observations

The code already contains good bounded policies: fixed-capacity state proofs, generation-qualified identities, nonblocking queue planning, and late-attach bounds. Preserve those.

The highest-value simplification is not a broad rewrite. It is to move source invalidation toward **event-local evidence**:

`exact current target > exact resource transition > recording-local named source > global invalidation`

That hierarchy minimizes false camera stalls while keeping uncertainty conservative.

Longer term, separate evidence collection from policy:

- `RenderEvidence`: immutable facts (target identity, generation, state/layout, pass scope, source operation);
- `CapturePolicy`: pure decision on whether to keep/retire/global-invalidate source state;
- `Delivery`: GPU copy/composition only after both evidence and policy admit it.

This makes the unsafe cases testable without COM/GPU objects and reduces the amount of policy embedded in hook callbacks.

## Validation gate

No simulator claim is made by this document. Before replacing the PR fallback with Phase A:

- portable logic tests must pass;
- Windows pinned build/`-Validate` must pass;
- D3D12 WARP smoke test must pass;
- ReShade-loaded simulator test must show continuous nose/tail motion, not merely `connected=1`;
- test must include camera toggle off/on and aircraft/session reconnect;
- log should preserve invalidation reason counters so any fallback/global path is observable.
