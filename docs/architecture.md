# Working architecture

This document describes the implemented **0.8.0 native architecture**, inspected on **14 September 2026**. It is intended for developers maintaining the renderer or adding aircraft support.

The system consists of a Windows tray application and a small DLL inside Microsoft Flight Simulator 2024. The DLL creates two aircraft-relative simulator scenes, captures their GPU output, combines them and draws the result into selected PFD textures. Both PFDs consume the same nose/tail composition; their TAXI buttons independently control delivery to each display.

**Validation boundary:** the native build has passed local GPU, lifecycle, IPC, installer and UI checks. Its validation receipt records `simulatorVerified: false`. The preceding ReShade prototype produced live PFD camera images; that does not establish that the replacement native startup and graphics hooks work end to end in MSFS. See [validation and known limits](#validation-and-known-limits).

## Contents

- [System boundaries](#system-boundaries)
- [Startup and shutdown](#startup-and-shutdown)
- [Aircraft controls and camera poses](#aircraft-controls-and-camera-poses)
- [GPU frame delivery](#gpu-frame-delivery)
- [PFD identification and routing](#pfd-identification-and-routing)
- [Display presentation](#display-presentation)
- [Recovery and lifetime rules](#recovery-and-lifetime-rules)
- [Aircraft extension boundary](#aircraft-extension-boundary)
- [Source map](#source-map)
- [Validation and known limits](#validation-and-known-limits)

The [runtime reference](runtime-reference.md) contains settings, IPC fields, intervals and diagnostic interpretation. The [aircraft adapter guide](aircraft-profiles.md) describes requirements for additional aircraft.

## System boundaries

~~~mermaid
flowchart LR
    Startup["MSFS exe.xml"] --> App
    subgraph Windows["External Windows process"]
        App["380-taxi-cam.exe<br/>Tray and settings UI"]
        Worker["Connection worker"]
        Settings["Per-aircraft INI"]
        App <--> Worker
        App <--> Settings
    end
    Worker <-->|"Settings, heartbeat and status"| IPC["Per-MSFS-process<br/>memory-mapped mailbox"]
    Worker -->|"Verified DLL load and start"| Bridge
    subgraph Simulator["FlightSimulator2024.exe"]
        Bridge["taxi-camera-bridge.dll<br/>Control loop"]
        Telemetry["SimConnect telemetry worker"]
        Engine["Observed engine update<br/>Owned nose and tail scenes"]
        Graphics["Native D3D12 observers<br/>Resource and recording identity"]
        Bridge <--> Telemetry
        Bridge -->|"Queued requests"| Engine
        Engine -->|"Scene/resource identity"| Graphics
    end
    IPC <--> Bridge
    subgraph GPU["GPU on the simulator device"]
        SceneTextures["Nose and tail render targets"]
        Snapshots["Owned captured textures"]
        Compose["Composition and stable output buffer"]
        PFDs["Left and/or right upper PFD"]
        SceneTextures --> Snapshots --> Compose --> PFDs
    end
    Engine --> SceneTextures
    Graphics -->|"Capture, ordering and PFD draws"| GPU
~~~

The external application carries control data. Camera pixels remain on the GPU in the simulator process: there is no frame readback, CPU video encoding or image transport through IPC.

The native path builds without ReShade or ImGui. A compatibility identity check can recognize an existing ReShade resource proxy when another add-on uses it; this is not a runtime dependency. The old ReShade adapter remains in the repository for rollback and is excluded from the default native build.

The settings interface uses Win32/GDI, double buffering, DPI awareness and a dark window theme. It does not host a browser or managed UI runtime. Implementation: [companion](../standalone/companion.cpp), [bridge entry point](../standalone/bridge_main.cpp), [native D3D12 adapter](../standalone/d3d12_bridge.cpp).

### Execution contexts

| Context | Responsibility |
| --- | --- |
| Companion UI thread | Tray/window messages, settings edits, explicit Save, Hide and Exit |
| Companion connection worker | Locate MSFS, create IPC, load/start the bridge, publish settings and read status |
| Bridge worker | Reconcile controls and targets, submit camera requests, service completed GPU captures, publish status |
| SimConnect worker | Receive aircraft/camera telemetry, GS, lighting and TAXI state; issue requested button events |
| Observed simulator manager update | Validate and operate private camera objects; apply poses, dimensions and activation gates |
| Simulator graphics recording/submission threads | Observe actual D3D12 calls and record/order capture and PFD work |

UI and IPC threads do not directly call private camera creation functions. Graphics callbacks do not call the private camera engine. A thread-local `OwnedWork` guard prevents the bridge's own GPU operations from recursively entering ordinary application observation.

## Startup and shutdown

1. [The installer](../install-native.ps1) checks the native binaries against a successful validation receipt, installs the companion and DLL, and adds one launch entry to `exe.xml`. It backs up the XML and preserves unrelated entries. A previous taxi ReShade add-on is retained under a disabled name; unrelated ReShade files remain untouched.
2. MSFS launches `380-taxi-cam.exe --background --simulator "<absolute executable path>"`. Background mode starts without displaying settings.
3. [The launcher](../standalone/launcher.hpp) finds the exact executable in the same Windows user/session. An explicit executable path narrows the match; ambiguity is refused. It checks the supported x64 image identity before attaching.
4. The companion creates a mailbox named for the simulator PID. It resolves the remote Windows loader using the owning module's identity and RVA, writes the absolute DLL path into remote memory and invokes `LoadLibraryW`. It then resolves and invokes the bridge's `TaxiCameraStart` export.
5. `DllMain` only disables thread notifications. `TaxiCameraStart` rejects a wrong host or an already loaded legacy taxi add-on, pins the DLL and starts its worker outside the loader lock.
6. The bridge opens the existing mailbox, initializes native graphics observation and starts public telemetry. Camera scene creation is requested when a routed TAXI side or an explicit scene test requires it.

The launcher has a bounded loader wait. If a remote load remains pending, it does not repeatedly inject into that simulator session or free the path while the loader might still read it.

There is one companion instance per Windows session. A second normal launch opens the existing settings window. Closing the window hides it; **Exit** disables delivery and stops the companion. Background mode exits after the simulator it attached to exits.

The bridge, installed hook code and resources referenced by executable command lists remain resident until MSFS exits. Exit is not a live DLL unload. Stopping future camera writes also cannot erase work already recorded in an application command list; normal application redraw replaces the prior image.

## Aircraft controls and camera poses

### A380 control contract

[The compiled profile](../profiles/catalog.hpp) supplies these exact external identifiers:

| Side | Read TAXI state | Request button push |
| --- | --- | --- |
| Left | `L:A32NX_FCU_EFIS_L_TAXI_LIGHT_ON` | `A32NX.FCU_EFIS_L_TAXI_PUSH` |
| Right | `L:A32NX_FCU_EFIS_R_TAXI_LIGHT_ON` | `A32NX.FCU_EFIS_R_TAXI_PUSH` |

The bridge derives a requested side mask from fresh button state, or from the manual mask when automatic TAXI control is disabled. Delivery also requires a fresh companion heartbeat, an enabled service, graphics readiness, no speed inhibition and a live target for that side.

Missing button telemetry is not treated as a fresh OFF event. [TaxiButtonIntent](../src/taxi_button_routes.hpp) holds previously accepted intent for up to two seconds after the first observed invalid gap. Fresh OFF acts immediately. Camera pose freshness is enforced separately, so held button intent does not authorize rendering with a stale pose.

Above **60 knots**, [TaxiSpeedCutoff](../native-camera/taxi_speed_cutoff.hpp) inhibits delivery and requests the aircraft's actual TAXI push event for an observed ON side. It does not write the annunciator Lvar. An accepted push waits for a newer OFF sample instead of repeatedly toggling the button. Failed sends can retry after one second. A crossing remains pending until OFF is acknowledged, even if speed subsequently falls.

### Telemetry and body frame

[BodyPoseProvider](../native-camera/body_pose_provider.cpp) uses SimConnect exports from the simulator's installed `SimConnect_internal.dll`. Aircraft latitude, longitude, altitude, pitch, bank, true heading and ground velocity arrive through a continuous `SIM_FRAME` data request. Camera telemetry supplies the calibration between the public aircraft frame and the private scene coordinate system.

The provider caches samples. Consumers read that cache rather than making synchronous SimConnect calls from the rendering callback. Packet receipt times establish freshness; they are not simulation timestamps or motion prediction. Changing the camera activation rate therefore cannot, by itself, remove telemetry-to-animation timing differences.

[Body-pose math](../native-camera/body_pose_math.hpp) and [mount transforms](../native-camera/aircraft_mounts.hpp) place each scene relative to the aircraft datum:

| Parameter | Meaning |
| --- | --- |
| Right / up / forward | Metres in the aircraft body frame |
| Pitch / yaw | Degrees; positive pitch looks up, positive yaw turns right |
| Lens | Field-of-view parameter in radians |

The current calibrated defaults are recorded in the runtime reference. Saved user calibration takes precedence. The two views follow the aircraft; the pilot camera is not switched between nose and tail every frame.

### Owned scene lifecycle

[The native probe](../native-camera/probe.cpp) consumes requests on the observed simulator manager update. [Code contracts](../native-camera/code_contract.cpp), [local memory checks](../native-camera/local_memory.cpp) and the [verified profile](../native-camera/verified_profile.cpp) validate the inspected engine build, code and object relationships before private calls.

[The engine-camera layer](../engine-camera/README.md) tracks owned entries and their IDs. It creates and removes only its own scene pair, verifies manager/pool/view identity, applies independent dimensions and poses, and controls activation. A non-null output pointer is only an intermediate observation.

[RenderSchedule](../native-camera/render_schedule.hpp) alternates activation opportunities between the feeds. Every enabled observer interval is followed by an interval with both gates closed. Per-feed deadlines survive settings changes; a delayed update does not trigger a catch-up burst. The configurable **15–60** rate is an upper limit on opportunities per camera. Simulator update cadence, GPU work and the mandatory closed interval can reduce the actual rate.

## GPU frame delivery

### Dimensions and ownership

The camera render sizes match their logical panes from the start:

| Surface | Dimensions | Owner |
| --- | --- | --- |
| Nose scene | 768 × 255 | MSFS scene renderer |
| Tail scene | 768 × 504 | MSFS scene renderer |
| Captured snapshots | Corresponding source dimensions/compatible format | Capture manager |
| Composed texture | 768 × 763, RGBA8 | Bridge compositor |
| Stable output buffer | 2,343,936 bytes; row pitch 3,072 | Bridge output stage |
| PFD destination | 768 × 1024, five mips in the A380 contract | Aircraft/simulator |

This avoids rendering a full main-window image and then shrinking it. It still creates two simulator scenes and performs GPU snapshot, composition and output copies; small dimensions do not eliminate scene setup or engine rendering cost.

### Identity before capture

[SceneHandoff](../src/scene_handoff.hpp) connects inspected scene outputs to resources observed through real graphics API arguments. Matches include device/resource generation, manager identity and scene epoch. An engine-read pointer is not blindly cast into a usable COM resource.

The native adapter tracks relevant resource creation, RTV/DSV descriptors and descriptor copies, graphics state, render boundaries, command-list Reset and DIRECT queue submission. Generation IDs distinguish an object from a later allocation at the same address. Private-data lifetime sentinels retire resource, root-signature and command-list metadata without retaining their parent objects in a reference cycle.

Graphics objects may already exist when the bridge starts. Descriptor use can discover resources; submission can discover command lists. An existing list's recording is unknown until a successful observed Reset establishes a new recording. Root-state tracking also supports root signatures created before bridge startup.

### Capture paths

[SceneCaptureManager](../src/scene_capture_manager.hpp) supports three forms of evidence:

| Path | Required evidence and action |
| --- | --- |
| Whole-texture copy | A real compatible `CopyResource` or whole subresource-zero `CopyTextureRegion` is forwarded once, then a snapshot is recorded from the proven image |
| Render-target exit | A complete observed transition from the exact render-target state, with valid render-pass context, permits a temporary transition/copy/restore before the original barrier |
| Queue-tail capture | After application submission, accumulated draw and state evidence proves a current source is still a render target; a private capture list is appended on that queue |

Legacy and enhanced barriers have separate state models. The code does not substitute a legacy barrier for an enhanced layout. Unknown state, partial copies, unsupported pass semantics, identity changes and unobserved recordings cause refusal rather than a guessed copy.

The queue-tail path is important when the engine renders directly into a texture without a useful whole-texture copy or target-exit event. It remains rate-limited and requires an owned source lease plus a current scene match. “Source draws increased” alone is insufficient.

Capture storage is bounded: the shared manager admits four device identities, 4,096 lists, 16 packets and 256 MiB of packet allocations. The current native adapter initializes one device. Repeated eligible writes to the same unsubmitted recording can update its retained snapshot rather than allocate per draw. Detailed contracts: [capture-manager integration](../src/scene_capture_manager.md) and [source-state tracker](../src/scene_source_state.hpp).

### Queue ordering and replay

~~~mermaid
sequenceDiagram
    participant App as MSFS recording / DIRECT queue
    participant Capture as Capture manager
    participant GPU as Device fence timeline
    participant Compose as Private compositor queue
    participant PFD as MSFS PFD recording / queue

    App->>Capture: Observed draw, barrier/copy and current source identity
    Capture->>App: Record snapshot, or append proven queue-tail capture
    App->>GPU: Submit work and signal producer timeline point
    Note over App,Capture: Application-owned recordings retire on Reset or destruction.<br/>Private tail recordings are submitted once.
    Capture->>GPU: Poll producer completion and recording/lease eligibility
    Capture->>Compose: Lease current nose and tail snapshots
    Compose->>GPU: Wait for prior timeline, compose, update stable buffer and signal
    PFD->>Capture: Register recording as a stable-output consumer
    PFD->>GPU: Ordered submission reads stable buffer and stamps upper PFD
    Note over Compose,PFD: Future output writes and every observed consumer replay<br/>remain on the same device timeline.
~~~

One per-device fence timeline orders capture producers, private composition and application PFD consumers, including submissions on different DIRECT queues. Queue waits establish GPU ordering without waiting for GPU completion on the CPU. CPU submission locks still serialize the relevant submission receipts.

A fence completing once does not mean an application command list cannot replay. Captures recorded in application lists become publishable only after the recording retires, all submission receipts return and its producer fence completes. Private queue-tail recordings are single-use. Input snapshots remain leased until their compositor consumption is ordered.

The compositor queue is not registered for ordinary application queue callbacks. Explicit `begin_private_submission` / `end_private_submission` calls put it on the same timeline. Queue observation cannot be disabled while recorded references remain executable. Refused observation quarantines publication.

### Composition and stable output

[SceneRuntime](../src/scene_runtime.cpp) polls completed captures, keeps the newest pending snapshot per feed and rejects obsolete identities. A composition requires both feeds from the same manager and scene epoch. Their capture times can differ; the pair is not a synchronized stereo frame.

[The D3D12 compositor](../src/camera_compositor_d3d12.hpp) draws a full-screen triangle using the two source textures, then [SceneFrameOutput](../src/scene_frame_output.hpp) copies the result into one stable GPU buffer. The buffer's GPU virtual address remains unchanged because application PFD command lists may record that address and replay later.

The compositor initializes shaders, pipelines and output resources once. Input descriptors change when resource/format identity changes. It does not compile shaders, upload frames or read pixels back to the CPU for each composition.

### Writing the PFD

[The stamp renderer](../src/pfd_stamp_d3d12.hpp) reads the stable buffer through a root shader-resource view and draws only the upper 763 rows. The native adapter records it after eligible aircraft PFD draws so later instrument drawing can be covered again. This modifies the display texture; it is not a Windows overlay or a window z-order operation.

Before stamping, the adapter requires the selected live target, supported RTV/DSV combination, mip zero, appropriate render-pass context and sufficient known graphics state. [PfdStampState](../src/pfd_stamp_state.hpp) restores pipeline, root signature and observed root arguments, topology, viewport and scissor. The root-SRV delivery path avoids replacing the application's descriptor heap.

Sparse root replay preserves only arguments actually established since Reset/signature changes. Heap changes invalidate affected tables. Unsupported indirect/bundle/predication paths invalidate the recording for stamping until Reset. A list discovered midway through recording is not assumed restorable.

## PFD identification and routing

[The detector](../src/pfd_target_detector.hpp) filters the complete live inventory for the current A380 destination contract: **768 × 1024, five mips, DXGI format 28**. It ranks draw-count increments over one-second windows, requires comparable activity from the leading pair and separation from the third candidate, and requires three stable windows.

For initial assignment, the higher ID of the detected pair is treated as left, based on previously observed sessions. This is an empirical ordering heuristic, not a material-name guarantee. The catalog's `SCREEN_DU_PFDL` and `SCREEN_DU_PFDR` strings are hints; the native detection path does not establish semantic labels for every runtime texture.

[Routes](../src/taxi_button_routes.hpp) retain surviving side identity when one texture disappears. Once both previously assigned identities are lost, the code refuses to silently reapply initial ID ordering. Manual assignment may be required. Settings provide per-side calibration, assignment and swap. IDs are session-local and are never saved.

The UI's candidate shortlist is limited to 16 and sorted by cumulative draw count. Detection uses the complete eligible inventory and interval deltas, so shortlist order is not the detector's side-assignment algorithm.

## Display presentation

Composition uses top-left pixel coordinates with these row ranges:

| Rows, inclusive | Presentation |
| --- | --- |
| 0–254 | Logical nose pane |
| 255–258 | Logical four-row separator |
| 259–762 | Logical tail pane |
| 245–268 | Actual visible black divider, painted over the pane edges |
| 763–1023 | Untouched lower PFD/trim region |

The visible divider is **24 pixels high**. Its extra ten pixels on either side obscure existing pane pixels; they do not change the render sizes or tail sampling origin.

The shader draws two magenta dots in the nose view and mirrored angled brackets in the tail view. These are fixed screen-space references fitted to the supplied real-aircraft photograph, not projected ground-clearance measurements. Changing camera geometry does not recalibrate them.

The top-left **140 × 48** area is an opaque GS panel rendered by the compositor. It contains white “GS” and green rounded knots from SimConnect, or `--` when unavailable. It does not reveal the aircraft's original GS layer.

### Exposure and colour

[DisplayExposureController](../src/display_exposure.hpp) starts from the calibrated **−8.8 EV** baseline. Automatic mode derives a bounded positive boost from the ambient-light sample and slews toward the target at one EV per second. The default maximum boost is four EV; stale lighting returns the target to the manual baseline.

For `R11G11B10_FLOAT` input, the shader applies exposure, per-channel Reinhard tone mapping and sRGB encoding. Other supported typed RGBA/BGRA and float inputs follow their sampled-colour path; the EV control does not apply identically to every accepted source format.

This is display processing of the scene output. It cannot add runway/taxiway lights or other contributions that the private scene renderer did not render.

## Recovery and lifetime rules

| Event | Implemented response |
| --- | --- |
| Companion stops or heartbeat expires | Suppress delivery and request scene stop; retain process-lifetime graphics/hook objects |
| Fresh TAXI OFF | Clear that requested side; stop owned scenes when no side/test requires them |
| Brief missing TAXI sample | Hold accepted intent for a bounded gap; separately enforce fresh camera pose |
| Current scene/resource identity changes | Invalidate committed output and wait for a fresh matched pair |
| Camera capture stalls with continuing source draws and unknown source state | Request bounded owned-scene recovery |
| One PFD target disappears | Forget that incarnation and preserve the surviving side |
| Both established PFD identities disappear | Require a proven reassignment rather than infer sides again |
| Device/queue observation or composition fails | Refuse publication and retain resources still referenced by recordings |
| Unsupported simulator identity/private code contract | Refuse private camera activation |

[CaptureProgress](../src/capture_progress.hpp) watches for at least two seconds without composed-frame progress while source draws continue and the queue-tail status is `unknown_source_state`. The bridge checks this every 250 ms, only for an eligible active two-feed scene with fresh pose.

[SceneRecovery](../native-camera/scene_recovery.hpp) allows three retries, spaced by two seconds, for specified transient inspection, entry-loss, resolution-change or capture-stall reasons. Retry requires previous owned entries to be gone and fresh pose to be available. Ten seconds of healthy continued capture progress can replenish the retry budget. Identity refusal, creation failure and arbitrary exceptions are not treated as generic retryable events.

These mechanisms support recovery from engine changes such as time-of-day reconfiguration, but their existence is not evidence that every such transition has passed a live test.

## Aircraft extension boundary

The project is independent of an aircraft source checkout. Windows delivery, mailbox transport, guarded MSFS engine integration and GPU synchronization are shared.

The catalog is currently a compiled **single A380 profile**: `profiles::active()` returns A380. There is no implemented runtime aircraft/livery identity selector, dynamic aircraft plug-in loader or A350 adapter.

Several presentation and native target-filtering contracts remain A380-specific in shared source. Adding an aircraft therefore requires:

1. Reliable aircraft/version identity and lifecycle handling.
2. Validated button state, input events, side mapping and automatic OFF acknowledgement.
3. Display identity, dimensions, formats, layers, preserved regions and guides.
4. Body-relative camera geometry and exterior-model visibility.
5. Aircraft operating policy and focused GPU/live regression checks.

A catalog entry alone cannot implement a different PFD. See [aircraft adapters](aircraft-profiles.md) for the extension contract.

## Source map

| Area | Entry points |
| --- | --- |
| User interface and persistence | [companion.cpp](../standalone/companion.cpp), [settings_store.hpp](../standalone/settings_store.hpp) |
| Attachment and IPC | [launcher.hpp](../standalone/launcher.hpp), [protocol.hpp](../standalone/protocol.hpp) |
| Runtime coordination | [bridge_main.cpp](../standalone/bridge_main.cpp) |
| Aircraft contract | [catalog.hpp](../profiles/catalog.hpp) |
| Telemetry and camera transforms | [body_pose_provider.hpp](../native-camera/body_pose_provider.hpp), [aircraft_mounts.hpp](../native-camera/aircraft_mounts.hpp) |
| Private camera requests and verification | [probe.hpp](../native-camera/probe.hpp), [code_contract.hpp](../native-camera/code_contract.hpp), [profile.hpp](../native-camera/profile.hpp) |
| Owned scene lifecycle | [engine-camera README](../engine-camera/README.md) |
| Native hooks | [native_hooks.hpp](../standalone/native_hooks.hpp), [engine-hook README](../engine-hook/README.md) |
| Graphics object observation | [d3d12_bridge.cpp](../standalone/d3d12_bridge.cpp) |
| Scene identity and capture | [scene_handoff.hpp](../src/scene_handoff.hpp), [scene_capture_manager.hpp](../src/scene_capture_manager.hpp) |
| Composition and output | [scene_runtime.cpp](../src/scene_runtime.cpp), [scene_frame_output.hpp](../src/scene_frame_output.hpp), [camera_compositor_d3d12.hpp](../src/camera_compositor_d3d12.hpp) |
| PFD state and delivery | [pfd_stamp_state.hpp](../src/pfd_stamp_state.hpp), [pfd_stamp_d3d12.hpp](../src/pfd_stamp_d3d12.hpp) |
| Detection, exposure and recovery | [pfd_target_detector.hpp](../src/pfd_target_detector.hpp), [display_exposure.hpp](../src/display_exposure.hpp), [scene_recovery.hpp](../native-camera/scene_recovery.hpp) |
| Legacy delivery only | [taxi_camera_addon.cpp](../src/taxi_camera_addon.cpp) |

## Validation and known limits

The local 0.8.0 receipt was created at **2026-09-14 03:56:30 UTC**. It records successful local validation and explicitly leaves simulator verification false. Generated receipts and release archives are under ignored `build/`; they are not prerequisites for reading this documentation.

| Evidence | What it establishes |
| --- | --- |
| Native hardware and WARP GPU tests | Real GPU source images reach two synthetic PFD targets through the native adapter; lower rows and application graphics state are preserved |
| Pre-existing-object and root-state tests | Objects created before bridge startup and partial root updates are handled by the tested paths |
| Smoke, IPC and settings tests | Exact DLL loading/wrong-host refusal, mailbox validation and persistence behaviour |
| Engine, source-state, schedule and recovery tests | Guarded logic and calling contracts in the isolated cases covered |
| Installer/XML fixture tests | Backups, preservation of unrelated entries, duplicate prevention, update and rollback behaviour |
| Import/dependency audit | Default native binaries do not require ReShade/ImGui or missing dynamic C++ runtimes |
| Local settings UI exercise | Save, Hide, single-instance reopen and shared Settings/Exit menu behaviour; physical tray-icon interaction was not exercised |

Primary native tests are [graphics_validation.cpp](../standalone/graphics_validation.cpp), [smoke_validation.cpp](../standalone/smoke_validation.cpp), [root_state_test.cpp](../standalone/root_state_test.cpp) and [native_slots_test.cpp](../standalone/native_slots_test.cpp). [Build validation](../build.ps1) and [smoke-test.ps1](../smoke-test.ps1) orchestrate the relevant suites. The machine's D3D12 debug layer was unavailable; hardware/WARP success is not a debug-layer-clean claim.

Remaining live checks include native `exe.xml` startup, both TAXI buttons and OFF feedback, long-running capture/PFD delivery, time-of-day changes, aircraft/texture recreation, speed cutoff and simulator frame-time impact. The private camera contract currently targets inspected MSFS **1.8.16.0**.

Known prototype follow-ups include occasional nose-wheel motion artifacts and missing runway/taxiway lights in the custom scene. The activity-based PFD detector and fixed screen-space guides remain limitations. Do not interpret a ready flag, capture counter, composition counter or stamp counter alone as proof of a visibly correct live PFD.
