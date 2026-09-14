# Aircraft integration

Taxi Cam uses an aircraft profile to connect cockpit controls, camera geometry and display layout. The active profile is **FlyByWire A380X**, defined in [profiles/catalog.hpp](../profiles/catalog.hpp).

## A380 controls

The bridge reads TAXI-light state through SimConnect. Automatic speed cutoff uses the corresponding push event and waits for the light to report OFF.

| Side | State variable | Push event |
| --- | --- | --- |
| Left | `L:A32NX_FCU_EFIS_L_TAXI_LIGHT_ON` | `A32NX.FCU_EFIS_L_TAXI_PUSH` |
| Right | `L:A32NX_FCU_EFIS_R_TAXI_LIGHT_ON` | `A32NX.FCU_EFIS_R_TAXI_PUSH` |

The profile key is `fbw-a380x` and its numeric ID is `1`. Settings use that key as the INI filename.

## Display and camera geometry

The A380 destination is a **768 × 1024 RGBA8 texture with five mips**. The camera image covers the upper 763 rows and preserves the lower 261 rows. Nose and tail scenes render at 768 × 255 and 768 × 504 respectively.

The profile supplies two body-relative mounts, each containing right/up/forward position, pitch, yaw and field of view. Exact defaults and limits are in [Camera mounts](runtime-reference.md#camera-mounts).

`SCREEN_DU_PFDL` and `SCREEN_DU_PFDR` are material-name hints in the catalog. Native PFD detection uses texture dimensions, format and draw activity. Initial side assignment uses the higher resource ID for left, and the UI allows manual correction.

## Shared components and aircraft-specific code

| Shared across integrations | Specific to the aircraft |
| --- | --- |
| Tray app and settings transport | Cockpit state variables and input events |
| MSFS camera function validation | Camera mounts and exterior-model framing |
| Owned camera lifecycle | Display texture identification |
| GPU capture and queue ordering | Pane layout, guides and preserved display area |
| Exposure controller | Operating rules, including speed cutoff |

The current catalog contains one profile, and `profiles::active()` returns A380. Display filtering and composition also contain A380-specific dimensions. There is no automatic aircraft selector or dynamic profile plug-in loader.

## Implementing another aircraft

An additional integration must define the following contracts:

1. **Identity:** determine which aircraft/version is loaded and handle aircraft changes.
2. **Controls:** map left/right state, activation events and OFF acknowledgement.
3. **Display:** identify the correct texture and layer, including format, dimensions and preserved regions.
4. **Geometry:** establish body-relative camera positions, direction, field of view and exterior-model visibility.
5. **Policy:** define speed, power and telemetry conditions for camera use.

Implement those contracts in the profile and the relevant display/control code. Validate both sides, camera framing, display restoration, aircraft reload and operating limits. A new catalog entry alone does not implement a new aircraft.

The process and rendering interfaces are explained in [Architecture](architecture.md).
