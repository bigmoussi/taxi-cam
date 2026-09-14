# Aircraft adapters

The native companion separates Windows UI and saved settings from the camera renderer and aircraft controls. Version 0.8.0 ships one adapter: **FlyByWire A380X**. A350 support is not implemented.

`profiles/catalog.hpp` is the adapter catalog. The A380 entry supplies its stable profile key, display layout kind, actual TAXI-light Lvars, push events, material-label hints, calibrated mounts and pane dimensions. The telemetry provider and camera defaults consume this catalog. Settings are saved under the profile key; resource IDs and diagnostic activation never persist.

The native engine contract is independent of aircraft profiles. A profile cannot supply process addresses, machine code or unchecked hooks. MSFS build guards, resource lifetimes, queue synchronization and scene ownership remain shared.

## Contracts for another aircraft

| Adapter responsibility | Required evidence |
| --- | --- |
| Aircraft identity | A reliable installed-aircraft/version match and explicit handling of aircraft changes |
| Controls | Light/state variables, input events, left/right mapping and acknowledgement of automatic OFF |
| Display identification | Verified labels or another demonstrated identity strategy; transient IDs are insufficient |
| Display layout | Texture format, dimensions, mips, pane placement, reference guides and areas to preserve |
| Camera geometry | Body-relative datum, transforms, lenses and exterior-model availability |
| Operating policy | Speed cutoff, power/activation conditions and telemetry freshness |

The current GPU presentation, native texture observation and PFD detector intentionally implement the A380 layout: 768 × 1024, five mips, RGBA8 destination, 768 × 255 nose and 768 × 504 tail. The catalog records that contract; adding an entry alone does not implement a different display. Extend the layout/identification adapter and its pixel tests together.

The initial A380 detector uses the previously tested high-activity pair ordering. The material strings are hints, not proof that every runtime resource has a label. Surviving target identity retains its side. Losing both established textures can require explicit reassignment; the UI provides per-side selection, calibration and swap.

## Adding the requested A350

Inspect its actual display layers, control signals/events and exterior model. Implement and validate its identity and layout adapter, add a catalog entry and give it an independent settings file. Verify camera perspectives, both buttons, OFF feedback, speed policy, aircraft reload, time-of-day changes and lost telemetry. Measure completed frames and simulator frame-time at the actual pane sizes.

No A350 Lvar, material name, input event or mount transform is assumed here. The shared rendering and native camera lifecycle can be reused once those contracts are demonstrated.

See the [working architecture](architecture.md) for the shared rendering and lifecycle contracts, and the [runtime reference](runtime-reference.md) for current profile values and settings.
