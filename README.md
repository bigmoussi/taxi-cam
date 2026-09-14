# 380 Taxi Cam

Nose-wheel and tail cameras for Microsoft Flight Simulator 2024, controlled by the aircraft's EFIS **TAXI** buttons.

The camera image occupies the upper part of the Primary Flight Display (PFD): nose-wheel view above, tail view below, with ground speed, magenta reference marks and a black divider. The lower trim display remains visible.

- **Aircraft:** FlyByWire A380X
- **Platform:** Windows x64, MSFS 2024 **1.8.16.0**
- **Delivery:** Windows tray application and an in-simulator DLL. ReShade is not required.

## How it works

380 Taxi Cam asks MSFS to render two additional views of the aircraft and its surroundings. It then draws those images into the texture that the cockpit model uses for its PFD screen.

1. **MSFS starts the tray app.** An `exe.xml` entry launches `380-taxi-cam.exe`, which loads `taxi-camera-bridge.dll` into the simulator.
2. **The bridge reads the aircraft.** SimConnect supplies TAXI-button state, aircraft position and orientation, ground speed and ambient lighting.
3. **MSFS renders the cameras.** The bridge calls the simulator's internal camera functions to position independent nose and tail views relative to the aircraft.
4. **The GPU combines the images.** The bridge copies the rendered views, adds the divider, reference marks and ground speed, then draws the result into the enabled PFD texture.

Left TAXI enables the left PFD; right TAXI enables the right PFD. Both displays share the same pair of cameras. Images remain on the GPU throughout capture and display.

See [How the camera reaches the PFD](docs/architecture.md) for the complete explanation.

## Install

Download the Windows x64 ZIP from [Releases](https://github.com/rthomson83/380-taxi-cam/releases/latest) and extract it. Close MSFS and exit any running 380 Taxi Cam instance, then run:

~~~powershell
.\install-native.ps1 -SimulatorDirectory 'C:\XboxGames\Microsoft Flight Simulator 2024\Content'
~~~

Set `-SimulatorDirectory` to the folder containing `FlightSimulator2024.exe`. If the installer cannot select the launch configuration, add `-ExeXml '<absolute path to exe.xml>'`.

The installer adds automatic startup to `exe.xml` and creates a Start menu shortcut. It backs up the launch configuration and preserves other add-ons.

## Use

1. Start MSFS and load the A380X.
2. Allow a few seconds for the app to identify the PFD textures.
3. Press the left or right EFIS **TAXI** button to enable that display. Press it again to switch the camera off.

Above **60 knots**, camera delivery is inhibited and active TAXI buttons are commanded off.

Right-click the tray icon and choose **Settings** to adjust the cameras. Closing the settings window leaves the app running. **Exit** stops delivery; the DLL remains loaded until MSFS exits.

If the camera appears on the wrong display, open **PFD routing** to identify, assign or swap the left and right targets. Automatic detection uses display draw activity, so it may require manual correction.

## Settings

| Page | Controls |
| --- | --- |
| Overview | Camera service, TAXI-button control and camera rate |
| Camera views | Independent position, pitch, yaw and field of view for each camera |
| Display | Manual exposure and automatic night adjustment |
| PFD routing | Left/right display assignment, preview and target identification |
| Diagnostics | Scene test, single-camera test and runtime counters |

Select **Save changes** to keep adjustments. Settings are stored in `%LOCALAPPDATA%\380 Taxi Cam\profiles\fbw-a380x.ini`.

The camera rate can be set from **15 to 60**. It limits how often each camera is requested to render; achieved frame rate depends on simulator updates and rendering load. Camera size is **768 × 255** for the nose and **768 × 504** for the tail.

## Remove automatic startup

To remove automatic startup, with MSFS closed:

~~~powershell
.\uninstall-native.ps1
~~~

Application files, settings and logs are retained.

## Build and releases

To build locally with the pinned compiler and run validation:

~~~powershell
.\build.ps1 -Bootstrap -Validate
.\smoke-test.ps1
.\package-native.ps1
~~~

Every push to `main` runs the [Windows release workflow](.github/workflows/release.yml). A successful run publishes a ZIP, checksums and release notes. Hosted checks use software D3D12 (WARP); live simulator verification is a separate check.

## Documentation

| Guide | What it explains |
| --- | --- |
| [Architecture](docs/architecture.md) | How the app creates camera views and puts them on the PFD |
| [Runtime reference](docs/runtime-reference.md) | Setting values, timing, IPC and diagnostics |
| [Aircraft integration](docs/aircraft-profiles.md) | The A380 controls, display identification and camera geometry |
| [Releases](docs/releases.md) | Automated builds, package contents and publication |

This project is maintained independently of the aircraft package. Dependency notices are in [Third-party notices](THIRD_PARTY_NOTICES.md).
