# 380 Taxi Cam

An independent native Windows taxi-camera project for Microsoft Flight Simulator 2024. It renders separate nose-wheel and tail views into the upper PFD, with magenta reference guides, a black divider and ground speed from SimConnect.

The current prototype is a ReShade add-on. A native Windows companion with a small in-simulator bridge is the intended packaging direction; removing the ReShade dependency is still future work. This repository builds independently and contains no aircraft package or aircraft build system.

## Current state

The current source version is **0.7.12**, migrated from the 0.7.11 baseline. Earlier live testing confirmed colour camera feeds on both PFDs through their EFIS TAXI buttons. The latest changes add recovery after temporary telemetry/scene loss, preserve surviving left/right target assignments, stream aircraft pose per simulation frame and adjust exposure for ambient light. Version 0.7.12 additionally detects the observed capture-state stall and retires/recreates the camera pair through the existing guarded recovery path. Above 60 knots ground speed, it inhibits camera delivery and requests OFF through each active aircraft TAXI input, awaiting the actual light feedback. Local validation is separate from live confirmation; the new recovery and cutoff still need an in-simulator check.

- Each camera renders at its pane dimensions, with a selectable 15-60 fps activation/capture limit. This limit is not a measurement of completed frames.
- Camera capture, composition and PFD delivery stay on the GPU; pixel readback is used only in validation.
- The lower PFD trim area is preserved. Mounts are editable through `taxi-camera-mounts.cfg`.
- The current integration reads the A380X variables `L:A32NX_FCU_EFIS_L_TAXI_LIGHT_ON` and `L:A32NX_FCU_EFIS_R_TAXI_LIGHT_ON`. These are external aircraft compatibility identifiers. The add-on does not write those output variables. For the speed cutoff it sends the corresponding `A32NX.FCU_EFIS_L_TAXI_PUSH` / `A32NX.FCU_EFIS_R_TAXI_PUSH` custom events to the aircraft controller.
- Private camera calls are guarded against the inspected MSFS **1.8.16.0** build. Unexpected code or object identities refuse startup.

The user reports improved but occasional nose-wheel motion artifacts with 0.7.11. Remaining live checks are automatic recovery after a time-of-day change, the 60-knot cutoff and button feedback, nose-wheel motion stability, continuous flicker and lower-camera framing. Missing runway/taxiway lights remain unresolved. Initial PFD selection without debug labels uses an activity heuristic; losing both established targets can require manual assignment.

## Build and validate

Run PowerShell from this repository on Windows x64. Dependencies are pinned by commit/version and SHA256 in `dependencies.json`. Bootstrap downloads them into the ignored `build/deps/` directory; no aircraft checkout, Node.js, Docker or aircraft SDK build is required.

```powershell
.\build.ps1 -Bootstrap
```

For full validation, supply an existing ReShade 6.8.0 DLL with add-on support:

```powershell
$reshadeDll = 'C:\XboxGames\Microsoft Flight Simulator 2024\Content\dxgi.dll'
.\build.ps1 -Validate -ReShadeDll $reshadeDll
.\smoke-test.ps1 -ReShadeDll $reshadeDll
```

The build runs strict C++ compilation and ABI checks. Full validation adds lifecycle, telemetry, recovery, routing and GPU pipeline checks on hardware and WARP, then the smoke test loads the exact built add-on with ReShade in an isolated process. Results are written under `build/`. These checks do not replace live simulator verification.

Output: `build/taxi-camera-native.addon64`.

## Install and run the prototype

Close MSFS before replacing a loaded add-on. Use the same ReShade binary that passed validation:

```powershell
.\install.ps1 -SimulatorDirectory 'C:\XboxGames\Microsoft Flight Simulator 2024\Content'
```

The installer verifies the exact smoke-tested binary and backs up the preceding installed add-on. On first setup, copy `taxi-camera-mounts.cfg` beside it; retain an existing configuration if you have tuned the mounts. Load the supported aircraft and allow its pose to settle while parked. EFIS TAXI buttons control the two PFD feeds. Press Home to open the ReShade overlay and use **Taxi Camera Native Probe** for diagnostics, manual PFD assignment, camera rate and exposure controls.

## Multiple aircraft

The renderer is a shared MSFS integration, but current display dimensions, target detection, controls and mounts are A380X-specific. The intended package separates these into aircraft profiles. An iniBuilds A350 profile is a requested future target, not verified support. See the [aircraft profile design](docs/aircraft-profiles.md) for the required boundaries and validation.

## Source map

| Directory | Responsibility |
| --- | --- |
| `src/` | ReShade integration, resource tracking, camera capture/composition, PFD delivery and controls |
| `native-camera/` | Guarded native camera calls, aircraft telemetry, mount configuration and recovery |
| `engine-camera/` | Camera-pair ownership and lifecycle logic |
| `engine-hook/` | Native update/graphics observers and call-state preservation |
| `abi/` | Public-header ABI checks and the Microsoft-ABI bridge |
| `tests/`, `validation/` | Logic regressions and isolated GPU/loader validation |
| `discovery/` | Build-specific investigation tools and evidence notes |

See [development history](DEVELOPMENT_HISTORY.md), [historical SDK research](docs/taxi-camera-sdk-research.md) and [native interface findings](discovery/findings.md). Historical binary archives and local process evidence stay in ignored build directories and are not distributed by this repository.

Third-party dependency notices are retained in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `licenses/`.