# Multiple-aircraft package design

This is the intended next architecture, not a claim that profile loading or A350 support is already implemented. The project is independently built and maintained. The current runtime still hardcodes the A380X adapter in several places.

The shared core owns native camera lifecycle and MSFS build guards, GPU capture and synchronization, composition, telemetry transport, recovery and diagnostics. Each aircraft adapter supplies the controls, geometry and display contracts. The planned Windows companion owns configuration and profile selection, with a small bridge inside the simulator; the present prototype uses ReShade for that bridge.

## Profile contract

| Profile data | Why it is needed |
| --- | --- |
| Aircraft/package identity and supported versions | Select the correct adapter on load; stop and release the previous adapter when aircraft changes. A title substring alone may match liveries or unrelated variants. |
| Display binding strategy | Exact runtime labels where present, or an independently validated binding/identification method. Never save transient texture IDs or assume the current activity heuristic works for another aircraft. |
| Texture and delivery contract | Dimensions, format, mip/sample layout, upper/lower region boundaries and any instrument layers that redraw over the image. The destination might not be called a PFD. |
| Controls | State variables, side mapping, ON/OFF or toggle input events, acknowledgement behavior and power conditions. A lamp/output variable alone is insufficient to change the aircraft controller's latch. |
| Cameras | Aircraft-relative positions, orientation, FOV, reference origin and available exterior-model geometry for each view. Camera coordinate conventions must be explicit. |
| Presentation | Pane sizes, divider, reference guides, ground-speed position, colour/exposure settings and the regions to preserve. |
| Operating rules | Speed cutoff, activation conditions and automatic/manual behavior appropriate to the supported aircraft. The current requested A380 cutoff is strictly GS > 60 knots. |

The profile schema must be validated before allocation or activation. Configuration can describe known adapters; it must not supply unchecked process addresses, arbitrary hooks or executable code.

## Current A380-specific seams to extract

- `src/pfd_target_detector.hpp`: 768x1024, five-mip RGBA8 target filtering and the initial observed ordering heuristic.
- `src/calibration.hpp`, `src/camera_compositor_d3d12.hpp` and the capture/resize filters: the 763-row upper region and 768x255 / 768x504 source contract, guides and layout.
- `native-camera/body_pose_provider.cpp`: two light Lvars and corresponding aircraft push events.
- `native-camera/aircraft_mounts.hpp`, `taxi-camera-mounts.cfg`: current nose and tail transforms and FOV.
- `native-camera/taxi_speed_cutoff.hpp`: the current speed rule and toggle acknowledgement policy.

Extract and validate these contracts together. Changing two strings while leaving the render-target filters, geometry or aircraft latch handling unchanged is not sufficient.

## Adding the requested iniBuilds A350 adapter

First inspect the installed aircraft's actual button signals/input events, destination displays, rendering layers and exterior model. Verify resource identity, both side mappings and the camera perspectives while parked. Then test flight/aircraft reload, time-of-day changes, temporary telemetry loss, target replacement and the aircraft's intended automatic-off behavior. Measure completed camera frames and simulator frame-time impact at the actual pane sizes.

No A350 Lvar, label, event name or camera transform is assumed here. A separate adapter can reuse the rendering core once those contracts have been demonstrated. It remains possible that a different display implementation will need an additional delivery adapter.