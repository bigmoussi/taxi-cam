# Native GPU validation

Run `build.ps1 -Validate` from the repository root with the pinned toolchain. It builds the companion and bridge, runs the native graphics checks on hardware and WARP, and runs the state, lifecycle, telemetry, startup and update tests. `-WarpOnly` explicitly skips hardware checks for hosted CI. Run `smoke-test.ps1` afterward to verify the exact executable and DLL covered by the build receipt.

`standalone/graphics_validation.cpp` exercises the native bridge with independent pixel oracles from `compositor_main.cpp` and `reference_overlay_oracle.hpp`. It checks camera composition and lower-display preservation, native graphics state replay, pre-existing graphics objects, predicate guards and D3D11On12 coexistence. Readback is confined to validation. The optional D3D12 debug layer is used when available; a passing run without it does not establish debug-layer coverage.

Additional focused checks remain available:

- `validation/scene_capture_build.ps1`: capture packet pixels, producer/consumer fences and storage reuse; see [capture contracts](scene_capture_README.md).
- `src/scene_capture_manager_test.ps1`: recording lifetime, submission receipts and capture publication.
- `src/scene_frame_output_test.ps1`: stable output composition and synchronization.
- `validation/render_boundary_test.ps1`: native render-boundary observations.
- `standalone/install_test.ps1` and `installer/test-installer.ps1`: isolated installation, calibration preservation, startup registration and rollback.

These checks do not establish live simulator compatibility, texture targeting, camera placement, lighting, motion or performance. Simulator observations must be recorded separately from local GPU and UI results.
