# PR 42 validation — 18 September 2026

The user confirmed that the revised camera rendering works in MSFS after the late-Connect fixes. The A350 had also been verified earlier in this testing session. Automatic display assignment is **not validated**: both iniBuilds and FlyByWire A380 selected a different instrument when the camera was activated during display boot. Manual selection allowed correction. A subsequent same-aircraft airport change caused an A350-1000 CTD and a reported system freeze; reload stability is also unresolved.

## Validated local build

The installed test pair is version **0.9.35**, build 0, IPC protocol 9:

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

The requested full runtime reset on aircraft or airport changes is follow-up work. It is not implemented or validated by this rendering snapshot.

## Local evidence

Logs, GPU reproductions, process snapshots, binaries and receipts remain in ignored build directories:

- `build/pr42-review/prefix-still-broken-20260918-1159/`: full validation, exact smoke, before/after query reproduction and frozen binaries.
- `build/pr42-review/delivery-20260918-121628-952/`: immutable installation receipt and rollback pair.
- `build/pr42-review/texture-detection-20260918/`: subsequent live logs and display-identity investigation.
- `build/pr42-review/airport-change-ctd-20260918-1314/`: bridge/launcher logs, Windows events, simulator crash report and bounded exception record.
