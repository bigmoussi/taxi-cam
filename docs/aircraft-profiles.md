# Aircraft integration

**Auto aircraft** on Overview selects a supported profile from public SimConnect metadata. The matcher requires an add-on path component in the `AircraftLoaded` response. The FBW A380's own aircraft path identifies that integration; its `ATC TYPE` can be the Airbus brand string `ATCCOM.ATC_NAME AIRBUS.0.text`, not the ICAO code `A388`. The A350 profiles additionally require the variant's type. Type and path must come from the same metadata poll, and two distinct samples must agree before switching. A missing response does not select a default aircraft. Manual selection uses the same aircraft identity checks and turns automatic selection off.

Each profile owns its camera calibration, display colour and exposure settings. Before switching, the companion saves edits to the departing profile and loads the arriving profile's own file. Invalid unfinished input delays a switch rather than discarding edits.

## Profiles

| Profile | Settings key | Display texture | Nose / tail render sizes |
| --- | --- | --- | --- |
| FlyByWire A380X | `fbw-a380x` | 768 x 1024, RGBA8, five mips | 736 x 251 / 736 x 496 |
| iniBuilds A350-900 / ULR | `ini-a350-900` | 1644 x 1024 EFIS surface | 774 x 251 / 774 x 496 |
| iniBuilds A350-1000 | `ini-a350-1000` | 1644 x 1024 EFIS surface | 774 x 251 / 774 x 496 |
| iniBuilds A380 | `ini-a380` | 768 x 1024, one mip; supported RGBA/BGRA views | 736 x 251 / 736 x 496 |
| PMDG 777 | `pmdg-777` | Shared `DUS`, 2048 x 2048; gauges `DU_LeftInboard` and `DU_RightInboard` | 736 x 251 / 736 x 496 |

A350 package identifiers and geometry were inspected in iniBuilds version 1.2.6. The user has verified A350 rendering in the simulator. This does not establish every aircraft variant, framing, graphics mode or automatic target ordering; see [PR 42 validation](pr42-validation.md).

## Controls

| Aircraft | Left / right state | Automatic OFF |
| --- | --- | --- |
| FBW A380 | `L:A32NX_FCU_EFIS_L_TAXI_LIGHT_ON`, `L:A32NX_FCU_EFIS_R_TAXI_LIGHT_ON` | Corresponding `A32NX.FCU_EFIS_L_TAXI_PUSH` / `R_TAXI_PUSH` event |
| A350 | `L:INI_TAXI_LEFT`, `L:INI_TAXI_RIGHT` | Write zero to the selected latch through public SimConnect |
| iniBuilds A380 | Manual previews or configurable camera hotkeys; cockpit buttons marked INOP | Suppress camera output above the speed limit; no aircraft-variable writes |
| PMDG 777 | Manual preview or Ctrl + Shift + L / R / B; no cockpit TAXI or CAM binding | Suppress camera output above the speed limit; no aircraft-variable writes |

The installed iniBuilds A350 behavior XML uses each TAXI latch for its button state and lamp. Its input-event setter toggles that latch. An idempotent zero write makes cutoff independent of toggle timing. The other side is not written. The FBW A380 and A350 adapters wait for a fresh OFF acknowledgement. The manual-only iniBuilds A380 suppresses output without waiting for a cockpit-button acknowledgement. The catalog supplies the speed limit, currently 60 knots for all profiles.

## Display placement

Each side has an explicit destination rectangle. The A380 covers rows 0 through 762 across the PFD. The A350 captain uses columns 0 through 805 of the combined EFIS surface; the first officer uses columns 838 through 1643. Both outer camera regions are 806 pixels wide, leaving a 32-pixel central gap for the grey separator and its edge padding. The adjacent navigation display and rows 763 through 1023 are preserved. The gap is based on the installed divider artwork; exact cockpit alignment remains subject to live verification. The display arrangement follows the [Airbus ETACS diagram, section 4-1-0](https://www.aircraft.airbus.com/sites/g/files/jlcbta126/files/2024-06/AC_A350_0524.pdf).

Inside each outer region, both aircraft draw a black border 16 target pixels wide on the left and right and 12 pixels high at the top. The complete camera image fits within that border, including its GS panel and reference marks. Content is 736 x 751 pixels on A380 and 774 x 751 on A350. This is distinct from the preserved central grey separator.

The profile defines accepted texture dimensions, mip policy and formats. Resource IDs identify an allocation lifetime, not an aircraft material. FBW A380 and A350 detection ranks activity across three one-second windows and requires a clear pair above other candidates. The iniBuilds A380 rule instead checks the complete display group described below. The side-order rule is profile data; it must be verified in the simulator. `$EFIS_LEFT` / `$EFIS_RIGHT` and A380 material hints are reference labels, not proof of GPU identity. **PFD routing** supports explicit assignment and correction when the heuristic is ambiguous.

## Shared rendering contract

The renderer captures two independently sized scene textures. It composes them into one bounded **768 x 763 working image**, then maps that image into the profile's inner content rectangle. This stable GPU buffer is shared infrastructure, not a request to render a full-size simulator view. Native sources match the inner pane sizes: 736 pixels wide on A380 and 774 on A350, with nose and tail heights of 251 and 496 pixels. The visible divider covers working rows 251 through 262, a 12-pixel band; reducing this band leaves the source dimensions and pane positions unchanged. The border is drawn by the existing PFD shader, without an additional GPU pass.

Profiles supply the pane division, visible separator, reference dot/bracket coordinates and default colour. Both A380 and A350 use 14-by-14-pixel nose squares and default to magenta markings. Both retain their existing tail brackets. **Marking colour** on **Reference guides** changes the nose squares and tail brackets together, with a separate saved RGB colour for each aircraft profile. On both A380 and A350, the ground-speed panel is inset from the camera edges, with internal padding and a width that fits the current value. The same layout applies to both PFDs. Panel layout belongs to the aircraft profile. The ground-speed text has its own saved RGB colour, editable on **Display**; changing it does not change exposure or the reference marks. Marks are visual references; adjusting mounts or field of view does not calibrate metric clearance.

## Geometry and settings

Each camera mount supplies right/up/forward metres, pitch/yaw degrees and lens radians. A350-900 and -1000 have separate mount presets and settings files. The presets start from the exterior model's camera-mesh locations, with a forward adjustment for the belly camera. **Camera views** adjusts each profile separately.

The A350-900 defaults are nose **right/up/forward 0 / -2 / 16 m, pitch/yaw -15 / 0 degrees, lens 0.55 rad**; tail **0 / 10 / -33 m, -15 / 0 degrees, 0.62 rad**. Its tail guide points are `(0.29, 0.76)`, `(0.26, 0.87)` and `(0.31, 0.87)` for the left upper point, lower corner and inner endpoint, mirrored on the right. All aircraft share the default GS colour RGB **22 / 109 / 19** (`#166D13`) and marking colour RGB **255 / 0 / 255** (`#FF00FF`).

The A350-1000 retains the same height, pitch, yaw and lens for each camera, with nose forward position **19.81 m** and tail **-36.17 m** for its longer fuselage. Its tail guide points are `(0.31, 0.69)`, `(0.29, 0.85)` and `(0.37, 0.855)`. The FBW A380 uses tail guide points `(0.33, 0.64)`, `(0.305, 0.75)` and `(0.365, 0.758)`.

All profiles use a daytime exposure default of **-11.5 EV**, with automatic night adjustment enabled. Existing saved settings take precedence. FBW A380 and A350 profiles start with TAXI-button control enabled; iniBuilds A380 starts in manual control with both displays off. Guide points are fixed image references; changing camera settings can move the gear relative to them. The geometry check projects the installed iniBuilds A350 1.2.6 `flight_model.cfg` gear contact points; it does not replace a cockpit comparison or establish metric clearance.

Changing profiles hides output and closes the owned views through the engine update callback. After validating the same manager, entry IDs and output resources, it retains that pair while changing the control subscription and camera mounts. Render dimensions stay fixed at the pair's original allocation sizes; the compositor scales into the selected profile's PFD rectangle. The bridge clears texture routes, button intent, pose calibration and capture history. A new profile cannot inherit another aircraft's saved mounts or stale ON state. The application stores the selected profile separately from each profile's INI file.

Flight/aircraft load notifications and changes in simulation running state also trigger this suspended transition, including reloads of the same aircraft. Confirming the current profile again in the dropdown explicitly retries its connection and display discovery. Manual texture IDs and preview/calibration requests belong to one flight and are cleared at the transition; saved mounts, guides and exposure remain intact. Native identity and GPU lifetime checks still apply. If the retained manager, IDs or output allocations cannot be verified, the cameras remain unavailable; a flight-load notification does not authorize abandoning or replacing those objects.

## Adding an aircraft

### iniBuilds A380 configuration

The iniBuilds A380 reports `ATC TYPE=Airbus` and an `AircraftLoaded` path under `SimObjects/Airplanes/inibuilds-a380/presets/inibuilds/a380-800_rr_basic/config/aircraft.CFG`. Matching requires its exact product path component; a generic Airbus type or iniBuilds vendor folder alone cannot select this profile. The FBW A380 keeps its separate identity and settings.

The cockpit marks both TAXI buttons INOP, and no supported public control contract is available for them. The profile therefore neither reads nor writes guessed TAXI variables. Use the companion's left/right preview controls or camera hotkeys. Manual control still obeys aircraft identity, session, service, GPU and speed guards.

The public input inventory contains 1,000 descriptors. Subscriptions expose the captain's PFD brightness adjustment (`AIRLINER_MIP_SIDE_PFD_LEFT`) but no input-value notification for either TAXI button. The clickable cursor therefore does not provide a supported public camera toggle. This does not rule out a private aircraft interaction.

The iniBuilds A380 defaults are: nose **right/up/forward 0 / 2.2 / 16 m, pitch/yaw -17.5 / 0 degrees, lens 1 rad**; tail **0 / 18 / -34 m, -32 / 0 degrees, 1 rad**. The left tail guide points are `(0.34, 0.52)`, `(0.305, 0.65)` and `(0.355, 0.65)`, mirrored on the right. Nose markers are `(0.14, 0.48)`. Daytime exposure defaults to **-11.5 EV**, with automatic night adjustment enabled. Settings remain independent in `ini-a380.ini`, and saved calibration takes precedence over defaults. These are visual alignments, not a metric clearance calibration.

Texture admission requires 768 x 1024, exactly one mip and supported RGBA/BGRA views. The ini A380 detector uses eight RGBA8 typeless resources (format 27), assigning the highest resource ID left and third-highest right. It checks unchanged membership and activity on every member across three one-second windows; discovery also has a complete-idle-group fallback. IDs may have gaps and are not fixed constants. Missing, extra, changed or incompletely tracked resources prevent the complete-group assignment. Structural changes withdraw automatically assigned sides while preserving explicit selections; loss of both identities can require reselecting the aircraft profile or assigning the PFDs manually. Pauses alone do not remove existing identities. FBW A380 and A350 use activity ranking instead.

The user confirmed camera rendering with the revised bridge, then reported that both A380 integrations selected another instrument when the camera was requested during display boot. Automatic display identity is therefore a known unresolved issue. PFD routing permits manual correction. The dropdown sorts by draw count, so its visual position is not resource-ID order. No texture-content classifier or thumbnail export is implemented. Own-device GPU fixtures check display regions and preserved lower rows; they do not establish cockpit identity, hotkeys, AA, turns, cutoff or reload behavior. See [PR 42 validation](pr42-validation.md) for the exact tested build and scope.

### PMDG 777 configuration

One profile matches the 777-200ER, 777-300ER and 777F from the airplane folder in the `AircraftLoaded` path. The open 777-200ER RR reported `SimObjects\Airplanes\PMDG 777-200ER\presets\pmdg\PMDG 777-200ER RR\config\aircraft.CFG` and `ATC TYPE` `ATCCOM.ATC_NAME BOEING.0.text`. That path has no `pmdg-aircraft-77er` component, so a package-folder match does not select it. The matching component is `PMDG 777-200ER`. The 300ER and 777F use the same kind of component, `PMDG 777-300ER` and `PMDG 777F`; those two paths were not in this log. ATC TYPE is the Boeing brand string and is not required. A `-copy` folder does not match.

The picture is the navigation display, not a flight PFD. A live scan of the open 777-200ER RR found no separate taxi-camera texture. Both inboard gauges, `DU_LeftInboard` and `DU_RightInboard`, draw one shared 2048 x 2048 texture named `DUS`. The navigation display is the whole inboard gauge. `panel.cfg` gives the same `htmlgauge` x, y, width, height on the 777-200ER, 777-300ER, and 777F: `DU_LeftInboard` is 1058, 33, 958, 971 and `DU_RightInboard` is 30, 1058, 958, 971. Taxi Cam stamps the composed page only inside those two rectangles. The rest of `DUS`, including the outboard PFDs, stays clear.

Taxi Cam does not copy a camera page the simulator already drew. It keeps the existing nose and tail viewpoints — two cameras, not three — and composes them into the shared 768 x 763 working image, then stamps that image into each inboard gauge rectangle on `DUS`. The top band stays one full-width forward view. The tail image is split into left and right panes with a 32-pixel centered gap in that working image, measured from the reference photo (about 4.4 percent of the navigation-display content width). The left pane samples the left half of the tail view and the right pane samples the right half. Other profiles leave this split off, so their full-width tail is unchanged.

The display list only offers GPU resources that match the selected profile's size. Until this profile is selected, the bridge keeps the previous profile's filter. The default is the FBW A380 at 768 x 1024, five mips, format 28. A 2048 x 2048 `DUS` texture is not a candidate for that filter, so the list stays empty and detection reports `no_candidates`. The log does not print gauge names. After this profile is selected, a 2048 x 2048 resource with any non-zero format and 1–12 mips is a candidate. Mip count and DXGI format were not in the scan. The texture list and the display-routing dropdown use resource-id order, not draw count. When more than one candidate matches, automatic selection takes the last entry, which is the highest resource id, and assigns that one texture to both inboard rectangles. It does not take the first entry, and it does not use a texture name. Display routing can still assign a texture by hand. GPU targeting cannot read gauge or material names; `DU_LeftInboard` and `DU_RightInboard` are the rectangles above on the shared `DUS` texture.

The cockpit has no CAM button for this profile. The profile does not read or write a TAXI Lvar, a CAM event, or any other aircraft variable. Camera on/off is the same Left, Right and Both shortcuts as the other aircraft (Ctrl + Shift + L / R / B) and the same left/right preview controls. There is no extra Camera shortcut. On this profile those requests are manual: they do not write an aircraft variable.

Nose and tail mounts are unverified starting points (nose **0 / -1.5 / 18 m, pitch/yaw -15 / 0 degrees, lens 1 rad**; tail **0 / 12 / -30 m, -25 / 0 degrees, lens 0.9 rad**). They are not a measured 777 clearance calibration. This profile does not require calibration or alignment markers: reference guides stay off, and no 777 marker geometry is added. Daytime exposure stays the shared default of **-11.5 EV**. Settings are stored in `pmdg-777.ini`. PFD refresh is not measured (`pfd_refresh_hz` 0). Live framing and lighting still need an in-simulator check.

### Integration requirements

Define an `AircraftProfile` in [the catalog](../src/profiles/catalog.hpp): accepted aircraft types and add-on path markers, control strategy and variables, texture constraints, side-order rule, destination rectangles and border insets, camera dimensions, mounts, composition, speed limit and the measured PFD refresh (`pfd_refresh_hz`: bridge `stamps`/s ÷ 2; 0 until measured; iniBuilds A380 16 from 0.9.11, A350 80 from the 0.9.35 sweep under frame generation, FBW A380 not yet measured). Rendering, GPU synchronization, exposure and native camera ownership consume these values without aircraft-name branches.

Add profile/settings tests and a GPU fixture that checks both camera regions and every preserved display region. Then verify actual cockpit buttons, texture identity, framing, cutoff and aircraft reload. An aircraft with different control semantics needs a control adapter; an aircraft without two camera views needs a different composition contract.
