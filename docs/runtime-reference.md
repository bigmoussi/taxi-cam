# Runtime reference

Reference for the **0.8.0 native implementation**, inspected on **14 September 2026**. Read [working architecture](architecture.md) for ownership, rendering flow and validation limits. Values below describe the source defaults; saved user calibration can differ.

## Executables and local files

| Item | Default location or behaviour |
| --- | --- |
| Companion | `%LOCALAPPDATA%\380 Taxi Cam\app\380-taxi-cam.exe` |
| In-process bridge | Adjacent `taxi-camera-bridge.dll` |
| Saved A380 settings | `%LOCALAPPDATA%\380 Taxi Cam\profiles\fbw-a380x.ini` |
| Bridge diagnostic log | `%LOCALAPPDATA%\380 Taxi Cam\bridge.log` |
| Legacy mount import | `taxi-camera-mounts.cfg` beside the installed companion |
| Build output | `build/native/` |
| Validation receipt | `build/native/validation.json` |
| Versioned package | ZIP under `build/packages/` |

The installer discovers the appropriate simulator `exe.xml`; `-ExeXml` supplies an explicit path when necessary. The launch entry uses an absolute companion path and `--background --simulator "<absolute MSFS executable path>"`.

Normal launch shows settings. `--background` hides startup UI and exits after the attached simulator exits. `--preview` uses isolated `preview-settings` beside the executable and never attaches to MSFS; it is for UI verification.

Settings load order is saved INI, otherwise bounded legacy mount import, otherwise compiled defaults. Saving validates the complete settings object, writes a UTF-16 temporary file, flushes it and atomically replaces the INI. Live edits and persistent Save are separate operations.

## Settings contract

Source: [Settings and validation](../standalone/protocol.hpp), [INI storage](../standalone/settings_store.hpp).

| Field | Default | Allowed values / meaning | Persisted |
| --- | --- | --- | --- |
| `enabled` | 1 | Boolean service enable | Yes |
| `profile` | 1 | Known catalog ID; only A380 currently exists | Profile key selects file |
| `follow_taxi` | 1 | Use aircraft buttons; 0 uses `manual_mask` | Yes |
| `auto_detect` | 1 | Enable initial PFD activity detection | Yes |
| `camera_rate` | 15 | Integer 15–60; per-camera activation limit | Yes |
| `single_camera` | 0 | Nose-only scene performance test | Yes |
| `automatic_exposure` | 1 | Enable ambient-light exposure adaptation | Yes |
| `exposure` | −8.8 | Finite EV, −16 to +4 | Yes |
| `night_boost` | 4 | Maximum automatic boost, 0–8 EV | Yes |
| `mounts[2][6]` | Profile defaults | Nose/tail transforms, validated below | Yes |
| `calibration_budget` | 4096 | Integer 64–16384 batches per accounting window | Yes |
| `manual_mask` | 0 | 0 off, 1 left, 2 right, 3 both | No |
| `calibration_mask` | 0 | Same side bits, animated target identification | No |
| `scene_test` | 0 | Request scenes without needing a PFD target | No |
| `left_id`, `right_id` | 0 | Distinct valid live resource generations | No |
| `route_request` | 0 | Changed request number for explicit assignment | No |

A single-camera test cannot produce the normal dual-pane composition: the output stage requires fresh snapshots from both feeds. Calibration proves a writable target; it does not prove scene capture.

The calibration budget is an allowance for batches of overlay rectangles, not a camera frame-rate control. The native adapter supplies no Present counter, so it uses the **50 ms fallback accounting window**. Reaching the limit skips further calibration writes in that window; it does not turn off calibration. See [write budget](../src/write_budget.hpp).

The saved sections are `[service]`, `[display]`, `[nose]` and `[tail]`. Diagnostic activation masks, scene-test state and resource identities restart cleared. Profile selection is not yet a runtime aircraft detector; `profiles::active()` remains A380.

### Mounts

Field order is **right, up, forward, pitch, yaw, lens**.

| View | Right m | Up m | Forward m | Pitch ° | Yaw ° | Lens rad |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Nose | 0 | −1.75 | 26.950668984 | −17.5 | 0 | 1.24 |
| Tail | 0 | 18 | −25 | −32 | 0 | 1.02 |

All values must be finite. Position components are bounded to ±500 m, pitch to ±89°, yaw to ±180°, and lens to 0.05–1.55 radians. Position is relative to the aircraft datum, not geographic coordinates. Increasing the lens parameter widens the field of view.

Source: [profile catalog](../profiles/catalog.hpp), [mount implementation](../native-camera/aircraft_mounts.hpp).

## IPC contract

[Mailbox](../standalone/protocol.hpp) is a local Windows memory-mapped native struct protected by a named mutex:

| Object | Name / value |
| --- | --- |
| Mutex | `Local\380TaxiCamera.Control.<MSFS_PID>` |
| Mapping | `Local\380TaxiCamera.Data.<MSFS_PID>` |
| Magic | `0x54415849` |
| Protocol version | `1` |
| Header | `magic`, `version`, `bytes`, `owner_pid`, `owner_heartbeat` |
| Payload | `Settings`, then `Status` |

`bytes` must equal `sizeof(Shared)`. This is a matched-build C++ ABI with compiler layout/padding, not a portable wire serialization. Change protocol version/layout validation when changing compatibility; ship the companion and bridge together.

The companion owns settings and its heartbeat; the bridge owns status. Normal mailbox operations try the mutex without blocking. Startup/status initialization has bounded waits. If the mutex is abandoned, the mailbox clears enable/heartbeat and refuses that potentially partial message.

The bridge accepts a heartbeat only when nonzero, not in the future and at most **5,000 ms** old according to `GetTickCount64`. Invalid settings or failed heartbeat acceptance suppress activation. The mapping contains scalars, arrays, IDs and bounded text, never engine pointers, COM pointers or image pixels.

`route_request` changes identify new manual assignments. The bridge acknowledges internally by remembering the request after a valid pair is assigned; current `left_id` and `right_id` in status provide the resulting mapping. Side masks consistently use bit 0 for left and bit 1 for right.

## Cadences and freshness

These are scheduling intervals or validation bounds, not promises about completed GPU frames.

| Operation | Source value |
| --- | --- |
| Companion connection/settings/status worker | 200 ms loop delay |
| Bridge control/service loop | 25 ms loop delay |
| Telemetry initialization/retry | Every 2,000 ms |
| Aircraft pose and GS stream | SimConnect `SIM_FRAME` |
| GS and TAXI sample freshness | 500 ms |
| Ambient-light sampling | Every 500 ms |
| Ambient-light freshness | 1,500 ms |
| Held TAXI intent after first invalid gap | Less than 2,000 ms |
| PFD discovery | Every 1,000 ms |
| Detector confirmation | Three qualifying windows after baseline |
| Detector stale-window reset | Gap greater than 5,000 ms |
| Capture-progress/recovery check | Every 250 ms |
| Stalled-capture threshold | At least 2,000 ms without progress, with continuing source draws and qualifying state |
| Retry spacing / budget | 2,000 ms / three retries |
| Healthy progress to replenish retries | 10,000 ms, with no qualifying progress gap over 2,000 ms |
| Bridge log snapshot | Every 5,000 ms |
| Automatic exposure slew | One EV per second; elapsed step capped at 1,000 ms |

Camera scheduling additionally requires a closed observer interval after every activation pulse. Queue-tail sampling has its own per-feed interval derived from the configured rate. Raising the UI limit does not increase the 25 ms output-service cadence or guarantee matched pairs at that rate.

## Presentation constants

Source: [compositor](../src/camera_compositor_d3d12.hpp), [stable output](../src/scene_frame_output.hpp).

- PFD destination: 768 × 1024; composed upper image: 768 × 763.
- Nose scene: 768 × 255; tail scene: 768 × 504; tail logical origin: row 259.
- Visible black divider: rows 245–268 inclusive, wider than the logical four-row gap.
- Lower 261 rows are preserved.
- GS panel: top-left 140 × 48 pixels, opaque black; white label and green value/`--`.
- Nose dots: mirrored at 14%/86% of width and 48% of nose height, radius 4.5 pixels.
- Tail brackets: mirrored segments through local normalized points `(0.33, 0.625)`, `(0.305, 0.75)`, `(0.365, 0.758)`; distance threshold 2 pixels.
- Guides are fixed screen-space graphics enabled in the compositor. They are not a user-calibrated ground projection.

Automatic exposure uses the following source formula, with a valid ambient sample:

~~~text
darkness = clamp(log2(4000 / max(ambient, 1)) / log2(4000), 0, 1)
targetEV = clamp(manualEV + nightBoost * darkness, -16, 4)
~~~

Ambient values use the SimConnect variable's numeric scale; the code does not treat them as lux. Missing/stale lighting sets the target back to manual EV. The shader's EV/tone-map path is selected for `R11G11B10_FLOAT` inputs; consult the architecture before assuming it applies to every source format.

## Status and diagnosis

The mailbox reports a bounded UTF-8 message plus these diagnostic groups:

| Field | Meaning / limit of inference |
| --- | --- |
| `graphics_ready` | Native graphics initialization accepted |
| `scene_ready` | Both inspected scene-ready flags are set; no guarantee of captured pixels |
| `taxi_mask` | Routed requested sides; it can remain nonzero while rendering is unavailable |
| `speed_inhibited` | Over-speed or pending OFF acknowledgement; can persist after speed falls |
| `left_id`, `right_id` | Current session's assigned resource identities |
| `captures` | Recorded snapshot capture operations, not necessarily completed/displayed frames |
| `composed` | Accepted paired composition submissions, not presentation count |
| `stamps` | Recorded PFD overlay operations, not completed presentation count |
| `hook_failures` | Native hook failure count |
| `speed` | Current valid knots; −1 means unavailable |
| `exposure` | Applied exposure-controller EV |
| `probe_cpu_ms`, `probe_max_ms` | Private camera observer CPU time; excludes engine rendering and GPU time |
| `stage_ms[10]` | Manager, pool, lifecycle, entries, view 1, view 2, handoff, pose, activation, publication |
| `candidates[16]` | At most 16 PFD candidates with resource ID and cumulative draw count |
| `heartbeat` | Bridge status timestamp |

The periodic bridge log includes readiness, masks, target IDs, capture/composition/stamp counters, hook failures, cutoff and message. It is diagnostic metadata, not a frame recording.

### Locating a delivery failure

| Observation | Inspect next |
| --- | --- |
| No bridge heartbeat | Companion connection status, executable identity, loader result and native startup |
| Graphics refused | Native initialization error and hook failures; scene settings cannot repair hook initialization |
| Graphics ready, scenes not ready | Supported private-engine profile, fresh body/camera telemetry and owned-scene lifecycle |
| Scenes ready, captures stay zero | Current scene-to-resource handoff, actual source draws/state, queue observation and capture refusal reason |
| Captures increase, compositions do not | Producer completion/recording retirement, both feeds, epoch match and output queue ordering |
| Compositions increase, stamps do not | Target assignment, requested side, valid PFD recording state and supported render context |
| Stamps increase, wrong/unchanged visible display | Actual aircraft texture/layer, side assignment, executed/replayed draw ordering; use target calibration and visual confirmation |
| Controls cease after a time/scene change | Heartbeat and telemetry freshness, cutoff pending state, source identity replacement and bounded recovery |
| Night view remains dark or lacks lights | Source format and exposure path, then whether the engine rendered those lights at all |

Internal source-state and queue refusal details remain available in the capture manager's statistics/source. The companion status is a summary and does not expose every old ReShade diagnostic row. The status message follows a priority order and can describe missing button telemetry or targets before a lower-level rendering issue.

## Build, delivery and evidence

From the repository root:

~~~powershell
.\build.ps1 -Bootstrap -Validate
.\smoke-test.ps1
.\package-native.ps1
~~~

Install or update with MSFS closed:

~~~powershell
.\install-native.ps1 -SimulatorDirectory '<absolute MSFS Content directory>'
~~~

The installer verifies exact binary hashes against the successful receipt. A build without its validation evidence is not equivalent to the validated package. Keep existing calibration and historical receipts intact.

Rollback, with MSFS closed:

~~~powershell
.\uninstall-native.ps1 -RestoreLegacy
~~~

This removes the native startup entry and restores the recorded legacy taxi add-on. Settings/logs/application files remain. The native DLL cannot be replaced or unloaded safely by closing only the settings window.

Local tests do not replace a live startup and camera check. The 0.8.0 native receipt inspected for this document has `passed: true` and `simulatorVerified: false`; the D3D12 debug layer was unavailable. See [validation and known limits](architecture.md#validation-and-known-limits) for the evidence boundary and outstanding live checks.
