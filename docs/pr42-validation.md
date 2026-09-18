# PR 42 validation — 18 September 2026

The user confirmed that the revised camera rendering works in MSFS after the late-Connect fixes. The A350 had also been verified earlier in this testing session. Automatic display assignment is **not validated**: both iniBuilds and FlyByWire A380 selected a different instrument when the camera was activated during display boot. Manual selection allowed correction. A subsequent same-aircraft airport change caused an A350-1000 CTD and a reported system freeze; reload stability is also unresolved.

## Earlier rendering-validation build

The test pair used for the rendering observations above was version **0.9.35**, build 0, IPC protocol 9:

| File | SHA-256 |
| --- | --- |
| `taxi-cam.exe` | `ED38650E555484C5B74AC3C72B1997BF89123C63FCC9571A068D94C0643DD029` |
| `taxi-camera-bridge.dll` | `A2D325881BD2029A2225227A3F2DD1730829363F1F686BEC2D3FCDA77D3D6DEF` |

This pair was built and tested before committing the source changes. A subsequent CI build has its own version metadata, hashes and validation receipt. Historical build and delivery receipts remain unchanged; simulator observations are recorded here separately.

- Pinned `build.ps1 -Validate`: passed with hardware and WARP GPU validation.
- `smoke-test.ps1` against the exact pair: **148 checks passed**.
- Display submission matrix: **36 cases passed**, covering ini A380 and FBW two/three-candidate layouts, SRV/COMMON exits, barrier-only and mixed lists, first-list insertion, automatic mapping in the fixtures, camera/calibration pixels, replay and later native overwrite.
- Submission proof: **427 checks passed**. Actual bridge handler/metadata regression: **191 checks passed**.
- The mixed-list tests include a timestamp query and buffer copy before display exits, with both buffer and display pixel checks.
- A separate diagnostic D3D11On12 reproduction passed **10 hardware/WARP cases** for baseline, timestamp, occlusion, pipeline statistics and event queries. The previous build refused the query-before-exit cases; the revised build produced the expected display pixels while preserving query results. This fixture is retained as local evidence, not part of the standard CI matrix.
- Exact binary audits reject thread-suspension dependencies and a production D3D11On12 bootstrap. Lifecycle, native identity, render-pass, resource-state and GPU-fence guards remain in force.
- The D3D12 debug layer was unavailable. Passing pixel/query checks do not claim debug-layer coverage or prove the absence of every simulator stall.

Installation verified byte-identical copies and preserved all 12 protected settings, calibration and other files. No simulator process was stopped by the deployment.

## Simulator findings and remaining work

The failed ini A380 session had current camera output and fully observed closed command lists, but no accepted submission copies. The revised prefix proof allows commands that cannot access a display's base mip in render-target state before its first explicit RT exit. Actual query-before-exit sequences reproduce the old failure locally. The first blocking opcode in the original failing simulator session was not retained, so the reproduction is not presented as an observed live opcode.

The subsequent live session produced camera output. Its ini A380 group contained resource IDs 45, 46, 47, 48, 49, 50, 52 and 53. Automatic selection chose 53/50; manual selection later used 48 for the left display. The user reported a different instrument, not merely reversed PFD sides, and reported the same class of selection error on FBW A380.

Boot-time selection is a plausible contributing condition: FBW stops activity ranking while both routes are populated, surviving routes retain their identities, and ini A380 can adopt a complete group before activity settles. These rules do not inspect texture contents. Capturing original candidate images and evaluating PFD layout recognition remains follow-up work; no image classifier, thumbnail export or mandatory left/right confirmation was added.

Rendering success does not establish correct automatic cockpit identity, all reload/power-up sequences, aircraft variants, lighting, motion or graphics-setting combinations. Existing unresolved issues in those areas remain separate from this validation.

### Same-aircraft airport-change crash

At 13:14:14 UTC the simulator's crash report recorded an access violation in `RenderThreadProc`. Windows Error Reporting and the bridge's bounded register record agree on MSFS 1.8.16.0, PID 45404, exception `0xc0000005` at executable RVA `0x3d0ffe4`, reading address `0x10`. The active profile was A350-1000. A native-module stack does not exclude an earlier bridge lifecycle error.

During the transition, invalid camera world data preceded the public session-change notification. The old camera pair disappeared; warmup later created entries 1003/1004, raising the created-view count from two to four shortly before the crash. The final bridge samples show output disabled and both gates closed. This narrows investigation to transition/removal/recreation ordering but does not prove the faulting object's identity or the cause of the subsequent system freeze. GPU process-memory telemetry timed out; no claim of GPU release or retained allocations is made.

The full runtime reset below was implemented after this crash. It was not present in the rendering-validation snapshot. The later End Flight/runway test below reproduced the CTD with that reset present.

## Full flight-session reset test build

Version **0.9.36**, build 0, IPC protocol 9 was installed at **13:40:22 UTC** after local validation. All 12 protected settings, calibration and other files retained their exact hashes. MSFS was already closed; only the companion was gracefully restarted.

| File | SHA-256 |
| --- | --- |
| `taxi-cam.exe` | `5EA94531043CAFE32AB3D80C53612F126F7F37005FF92894AE2E0B07FDD53E76` |
| `taxi-camera-bridge.dll` | `DC3F2BC2A026E3EB9BC8877C9A12B16D06FE11ECF47E4416225B5C202E4DFE4A` |

Aircraft/profile changes and same-aircraft airport loads/teleports now invalidate the flight session. Native retirement stays on the verified observer; old IDs must be confirmed absent before a fresh pair can start. Loading or invalid WORLD data blocks creation, recovery and rendering. Completion needs fresh supported identity, body and WORLD telemetry. The telemetry adapter is selected once per transition so waiting for fresh samples cannot repeatedly stop its worker.

Routes, activity, recovered bindings, previous recording admission and completed output are reset. Old GPU packets and replayable recordings retain their lifetime and fence obligations, while old frames cannot publish into the new session. Resume requires the exact new generation. Saved user calibration and ordinary TAXI OFF/ON parking remain intact.

- Pinned `build.ps1 -Validate`: passed on hardware and WARP, including the 36 display-submission cases.
- Exact installed-pair smoke: **148 checks passed**.
- Public telemetry/session tests: **1,355 checks passed**; native reset policy: **35 checks passed**; bridge metadata: **196 checks passed**.
- Hardware/WARP reset tests cover an actual blocked producer queue, old-work replay, source destruction only after fence completion, stale-frame rejection, calibration suppression and exact-generation resume.
- Exact binary audits still reject thread-suspending dependencies and production D3D11On12 bootstrap. No GPU wait was added. The debug layer was unavailable.

These are local results. Actual flow-event delivery, loaded-flight Connect after this change, aircraft swapping and same-aircraft airport reload still need simulator verification. The A380 wrong-instrument selection and cause of the CTD/system freeze remain unresolved. The simulator process had exited and the companion was responsive during investigation. No display-driver reset was recorded in the checked Windows System event interval, but GPU-memory telemetry timed out; that does not establish whether the bridge caused or contributed to the freeze.

## A350 cold-and-dark automatic assignment correction

The subsequent A350-1000 session (PID44364) discovered six textures but never assigned them automatically. Native draw counts were zero. Submitted RT-exit counts for auxiliary IDs43/44/46 tied above the active EFIS group45/47/75, so ranking all six always returned `ambiguous_activity`. Read-only IPC identified the auxiliaries as five-mip UNORM and the EFIS group as one-mip RGBA8 typeless. The user confirmed47 as the working left display;45/right remains inferred from matched activity and the existing side rule.

The A350 submission-only fallback now ranks the complete three-member EFIS group, retaining the existing activity margin, three stable windows and side ordering. Partial idle pairs cannot bypass this through the bridge's two-candidate shortcut. Native-draw ranking, A380 selection and the full manual inventory remain unchanged.

Version **0.9.37**, build 0, was installed at **14:08:10 UTC**. All 12 protected settings/calibration files retained their hashes; MSFS was already closed and only the companion was restarted.

| File | SHA-256 |
| --- | --- |
| `taxi-cam.exe` | `643800CB2722830CC97CB8EF10C7A12982F575CB749F34D4CD190EA3E9055DE0` |
| `taxi-camera-bridge.dll` | `26FFACC681CAE74C3C7F0B4B6A74FCC42874775268E097F074E708DDDE946F68` |

The captured detector regression fails against90ae339 and passes with this correction. Actual bridge discovery/routing checks pass **129 cases**, including partial power-up, reset and manual selection. Pinned `build.ps1 -Validate` passed on hardware/WARP; exact-pair smoke passed **148 checks**. The A350 graphics fixture now rejects its incomplete idle automatic pair, then verifies manual camera/calibration output with the same pixel oracles. The debug layer was unavailable.

Automatic assignment in the new binary still needs a live cold-and-dark retest. These results do not establish right45's cockpit identity or resolve A380 boot-time targeting or airport-change crash/freeze behaviour.

## End Flight / new runway flight crash and lifecycle correction

The user reproduced the CTD on **0.9.37** by starting cold and dark, leaving Taxi Cam on the PFD, ending the flight, then starting a new flight on the runway. At **14:16:41 UTC**, Asobo reported `RenderThreadProc` with `0xc0000005`. WER and the bounded bridge record agree on PID44028, MSFS1.8.16.0, RVA`0x3d0ffe4`, read address`0x10`, matching the earlier airport-change fault. The old pair1001/1002 was absent before new entries1003/1004 were created during background warmup. Capture/composition/stamp totals did not advance after recreation. The reconnect in the launcher log occurred after the crash timestamp.

The previous lifecycle policy incorrectly let FLT_LOADED clear an observed End Flight/menu state. A frozen-source regression against`a5a3618` supplies valid A35K identity, body and WORLD telemetry after End Flight → FLT_LOAD → FLT_LOADED and reproduces premature readiness. The correction stays blocked until the separate FLIGHT_START event and new telemetry. Stopped/loading sessions retain their event subscription; same-profile setup clears cached samples/calibration without closing a healthy worker. Native work also requires the exact startup flight epoch, including valid epoch zero for Connect in an already loaded flight. Successful-transition logs now include public readiness/loading and the last observed flow event.

Version **0.9.38**, build 0, was installed at **14:32:58 UTC**, preserving all 12 protected settings/calibration files. MSFS was already closed; no simulator process was stopped.

| File | SHA-256 |
| --- | --- |
| `taxi-cam.exe` | `DC2D0B6D3A2DF1B35EDAB4868AF9E510ECED204380E46D9A0E406758B2FF4EFD` |
| `taxi-camera-bridge.dll` | `FCDBFE6C3AAA6FDE56AE686BFE51E100FDFF3EC11CCB23CDD73BF831A361184D` |

- Pinned `build.ps1 -Validate`: hardware/WARP passed; exact-pair smoke: **148 checks passed**.
- Public telemetry/session: **1,405 checks**; native reset/admission: **44**; production wrong-host runtime: **1,407**.
- End/load regression fails against the frozen previous provider and passes with the correction. Both event orders, early Sim-stop, subscription preservation, fresh telemetry and late Connect are covered locally.
- GPU fence/replay ownership remains unchanged. Existing blocked-producer reset and replay cases passed in full validation. The debug layer was unavailable.

The logs retain sampled flow state rather than every event, and WER had already deleted the temporary dump. Register patterns match the earlier crash, but the faulting object's ownership is not established. Native/GPU audits found no demonstrated stale descriptor or resource-reuse defect to justify speculative cleanup. **The patch fixes demonstrated admission defects; the exact CTD cause and resolution still require the identical simulator reproduction.** System-freeze causality and A380 boot-time target identity also remain unresolved.

## Repeated 0.9.38 failures and deferred native retirement

The user reproduced the cold-and-dark → End Flight → same aircraft on runway CTD repeatedly on **0.9.38**, and reported no crash with the companion closed. The previous flight-start patch did not resolve it. Three retained failure records (PIDs39324,25560 and the later27428 session) repeat `0xc0000005` at MSFS RVA`0x3d0ffe4`, reading`0x10` with RDI zero. Each run holds the loading latch, then creates replacement entries1003/1004 after public readiness returns. Background preparation has `intent_mask=0`; no completed prewarm pair follows. This evidence does not support another change to the public readiness latch.

Read-only native-code capture establishes a separate allocation hazard. Entry removal detaches a view's associations and queues it for deferred cleanup. The renderer uses two alternating queues; its later consumer clears the associations again, clears the pending-release marker, and retains the pooled view object for slots0..7. The native allocator chooses the first association-free slot without consulting those queues. Our previous capacity check mirrored that availability predicate, so entry absence and a free-slot count did not establish safe reuse. A queued view could be rebound before the consumer cleared its new associations.

The native contract and source establish this hazard. The recorded failures did not capture queue contents at replacement creation, so they do not independently prove that this hazard caused the observed CTD. A later read-only active ini A380 snapshot found clean free slots and no pending releases; manager/entry mapping confirmed our entries1001/1002 in occupied slots1/2. No simulator thread was suspended or process memory modified by those diagnostics.

The correction verifies the complete native release and consumer instruction contracts at startup, then uses bounded, reread observations of both queue buffers and each view's pending marker at creation boundaries. It checks the first native-free slots rather than counting later clean slots. Old view identities remain tracked after entry removal until native retirement is observed. A refused allocation preserves the current request through partial-pair cleanup; Stop or a newer request cancels it. Native creation failures retain their terminal handling. No release marker is written, no queue is drained by the bridge, and no blocking GPU wait or thread suspension is added. The existing rotating log records cumulative retirement deferrals and the latest queue counts/status.

- Pool regression: **81,112 checks**, including the old association-only predicate accepting a queued first-free slot, both release buffers, the marker-cleared/queue-not-yet-cleared interval, retained queue capacity, unavailable reads and changed snapshots.
- Deferred-start/prewarm regression: **376 checks** against the actual pair controller. Partial ownership remains tracked through cleanup; temporary suspension does not consume Start; empty deferral is cleared before publication so prewarm cannot mistake it for a permanent failure.
- Native contract regression: **153,765 checks**, including changed queue/marker displacements and renderer endpoint refusal. Existing native reset tests: **44 checks**.
- The complete **126-byte release and 948-byte consumer** templates were independently compared with read-only live captures, including **18 decoded address operands**, with no mismatches. The production resolver accepted **43 ranges** using a local reader overlay of only the known original manager-vtable slot; the raw already-hooked image correctly refused that identity. The overlay did not modify simulator memory and is not part of production.

Version **0.9.39**, build 0, IPC protocol 9 passed pinned `build.ps1 -Validate` on hardware and WARP, followed by **148 exact-binary smoke checks**. The debug layer was unavailable. Its renderer endpoints were also checked against the actual current renderer vtable without an overlay. This build closes the demonstrated retirement-admission gap; the identical simulator reload reproduction remains the acceptance test for the reported CTD.

| File | SHA-256 |
| --- | --- |
| `taxi-cam.exe` | `E08CE11A5C7B44DCD9EA227735BAD1E078BE00770EE47391A35872EA9BFD8F3B` |
| `taxi-camera-bridge.dll` | `14F8663CDAF453B0919146DB3C4DD8C1AF641810532AF4E7D646FCFF5D58F6F3` |

The pair was validated before committing these source changes. No simulator process was stopped for validation. Its binary hashes and validation receipt identify this local test build independently of later CI version metadata.

## Local evidence

Logs, GPU reproductions, process snapshots, binaries and receipts remain in ignored build directories:

- `build/pr42-review/prefix-still-broken-20260918-1159/`: full validation, exact smoke, before/after query reproduction and frozen binaries.
- `build/pr42-review/delivery-20260918-121628-952/`: immutable installation receipt and rollback pair.
- `build/pr42-review/texture-detection-20260918/`: subsequent live logs and display-identity investigation.
- `build/pr42-review/airport-change-ctd-20260918-1314/`: bridge/launcher logs, Windows events, simulator crash report and bounded exception record.
- `build/pr42-review/flight-session-reset/`: full validation, exact smoke, focused GPU/lifecycle tests and deployment scripts for the new reset build.
- `build/pr42-review/delivery-20260918-134022-210/`: immutable reset-build installation receipt, validated pair and rollback pair.
- `build/pr42-review/a350-cold-dark-assignment/`: captured failing log/IPC metadata, old-policy failure, corrected regressions, full validation and exact smoke.
- `build/pr42-review/delivery-20260918-140810-116/`: immutable A350-assignment installation receipt, validated pair and rollback pair.
- `build/pr42-review/flight-reload-ctd-20260918-1416/`: 0.9.38 latch regression, validation, crash evidence and deployment scripts.
- `build/pr42-review/delivery-20260918-143257-579/`: immutable 0.9.38 installation receipt and rollback pair.
- `build/pr42-review/repeated-reload-ctd-20260918-1618/`: repeated failure logs/records, native fault and release-consumer captures, bounded renderer snapshots and retirement investigation.
- `build/pr42-review/retirement-validated-20260918-1545/`: immutable 0.9.39 validated executable/DLL pair and receipts.
