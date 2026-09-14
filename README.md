# 380 Taxi Cam

A native Windows taxi-camera companion for Microsoft Flight Simulator 2024. It renders separate nose-wheel and tail views into the upper PFD, with magenta references, a black divider and ground speed from SimConnect.

**0.8.0 replaces the ReShade delivery path with a background tray application and a small in-simulator DLL.** The default build requires neither ReShade nor ImGui. The renderer still runs inside MSFS so it can use the simulator's scene and GPU resources; the Windows application owns settings, startup and lifecycle.

## Run

Install while MSFS is closed:

```powershell
.\install-native.ps1 -SimulatorDirectory 'C:\XboxGames\Microsoft Flight Simulator 2024\Content'
```

The installer accepts a built repository or an extracted native package. Use `-ExeXml <absolute path>` if both Store and Steam configurations exist. It backs up `exe.xml`, adds one **380 Taxi Cam** launch entry and preserves other add-ons. The previous taxi ReShade add-on is retained with a disabled filename. Other ReShade files remain untouched.

MSFS starts the companion in the background through `exe.xml`. Right-click its tray icon for **Settings** or **Exit**. Closing the settings window hides it to the tray. The Start menu shortcut also opens settings. Exit stops camera delivery; the bridge stays resident until MSFS exits.

Load the **FlyByWire A380X**, park and allow PFD detection to settle. Left and right EFIS TAXI buttons control their displays. First-install detection retains the tested activity heuristic; verify side assignment and use **PFD routing** to swap or explicitly assign targets when needed. Transient texture IDs are never saved across simulator sessions.

## Settings

- **Overview:** aircraft profile, camera service, automatic TAXI-button control and 15–60 fps per-camera activation limit.
- **Camera views:** independent right/up/forward offsets, pitch, yaw, lens and quick movement controls for nose and tail.
- **Display:** exposure, automatic night exposure and maximum night boost. The calibrated baseline is −8.8 EV.
- **PFD routing:** current targets, manual camera previews, left/right swap and animated target calibration.
- **Diagnostics:** independent scene test, single-camera performance test, calibration write budget, CPU timings, capture/composition/PFD counters and logs.

Save changes stores this aircraft's settings in `%LOCALAPPDATA%\380 Taxi Cam\profiles\fbw-a380x.ini`. The existing `taxi-camera-mounts.cfg` is imported on first use; an existing saved profile takes priority. Scene tests and manual texture IDs do not persist. Invalid values are rejected before reaching the bridge.

The nose renders at **768 × 255**, the tail at **768 × 504**. The lower 261 PFD rows remain available to the aircraft. The activation limit is not a measurement of completed frames. Above **60 knots** the existing A380 rule inhibits the cameras and requests that active TAXI buttons switch off.

## Build and validation

Windows x64, PowerShell and the pinned LLVM-MinGW toolchain:

```powershell
.\build.ps1 -Bootstrap -Validate
.\smoke-test.ps1
.\package-native.ps1
```

Outputs are `build/native/380-taxi-cam.exe` and `build/native/taxi-camera-bridge.dll`. The package script produces a versioned ZIP under `build/packages`. No aircraft checkout, Docker, Node, .NET, WebView or ReShade installation is required.

Validation covers exact-DLL loading, wrong-host refusal, settings and IPC, native COM slots, graphics-state restoration, pre-existing graphics objects, GPU capture and both PFDs on hardware and WARP, routing, telemetry, camera lifecycle and preservation of other `exe.xml` entries. The installer checks the exact executable/DLL hashes against the successful validation receipt. Runtime imports and native source dependencies are checked for ReShade/ImGui and missing dynamic C++ runtimes.

**Local validation is separate from a live MSFS test.** The native migration needs an in-simulator startup, button, camera and frame-time check. Private camera entry points remain guarded against the inspected MSFS **1.8.16.0** build; other builds refuse activation.

The 0.7.12 baseline remains archived separately. Known live follow-ups from that baseline include occasional nose-wheel motion artifacts, missing runway/taxiway lights, time-of-day recovery and cutoff confirmation. Initial texture targeting is a heuristic; losing both established PFD identities can require manual reassignment.

## Aircraft modules

The first supported integration is **FlyByWire A380X**. This is an independent project, not an aircraft package. Compatibility identifiers retain their actual aircraft names.

`profiles/catalog.hpp` owns the adapter identifier, Lvars, input events, camera defaults and display contract. The renderer, transport, native camera lifecycle and Windows UI are shared. Presentation and target detection currently implement the A380 layout. Adding an A350 or another aircraft requires its validated display/control adapter and camera geometry, not just two replacement strings. See [aircraft profiles](docs/aircraft-profiles.md).

## Rollback

With MSFS closed:

```powershell
.\uninstall-native.ps1 -RestoreLegacy
```

This removes only our startup entry and restores the recorded legacy taxi add-on. Exit the companion from its tray menu. Application files, settings and logs are retained. Omit `-RestoreLegacy` to disable automatic startup without restoring the old add-on.

Legacy development commands remain available:

```powershell
.\build.ps1 -LegacyReShade -Validate -ReShadeDll '<existing ReShade 6.8 DLL>'
.\smoke-test.ps1 -ReShadeDll '<existing ReShade 6.8 DLL>'
.\install.ps1 -LegacyReShade -SimulatorDirectory '<MSFS Content directory>'
```

## Technical documentation

- [Working architecture](docs/architecture.md): process boundaries, native camera lifecycle, GPU capture and PFD delivery, routing, recovery and validation limits.
- [Runtime reference](docs/runtime-reference.md): settings, IPC contract, timings, presentation constants, diagnostics and deployment.
- [Aircraft adapters](docs/aircraft-profiles.md): current A380 integration and the requirements for additional aircraft.

## Source map

| Directory | Responsibility |
| --- | --- |
| `standalone/` | Windows tray/settings app, IPC, native launcher, D3D12 adapter and native validation |
| `profiles/` | Aircraft adapter metadata and calibrated defaults |
| `src/` | Shared GPU capture, composition, PFD delivery, exposure and routing; legacy add-on |
| `native-camera/` | Guarded private camera integration, public telemetry and recovery |
| `engine-camera/` | Scene ownership and lifecycle |
| `engine-hook/` | Native camera and graphics observers |
| `licenses/` | Preserved upstream notices |

See [third-party notices](THIRD_PARTY_NOTICES.md). Downloaded toolchains and build products remain in ignored build directories.
