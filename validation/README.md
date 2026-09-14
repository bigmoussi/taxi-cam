# Native GPU validation

`main.cpp` is a windowless Windows D3D12 host for the exact `record_calibration`
helper used by the diagnostic add-on. It requires no simulator process, ReShade
installation, MSFS SDK, or changes outside the build output directory.

The host creates a real 768 × 1024 RGBA8 render target, clears it to a known color,
then records the production calibration operation on an open direct command list
while the texture is in `RENDER_TARGET` state. A test-only copy to a readback
buffer and a GPU fence let it check every pixel. The test passes only if all
585,984 upper pixels changed and all 200,448 lower pixels remained identical.
The helper and this host do not attempt to infer simulator texture states or
transition resources from unknown states.

Run the compiled executable normally to use the default hardware adapter with a
WARP fallback. Run it with `--warp` to require the Windows software D3D12 adapter.
Each run prints one JSON result and returns zero only on success. No window is
created. GPU completion has a 30-second deadline.

The optional Windows graphics debug layer is enabled when available. D3D12 error
or corruption messages fail the test. `debug_layer: false` means validation did
not include that additional check; the host does not install it.

With LLVM-MinGW, the required link libraries are `d3d12`, `dxgi`, and `dxguid`;
use `-municode` for the `wmain` entry point. The source includes only Windows
headers and the repository's calibration helper.

This proves the native GPU calibration writes and lower-display preservation.
It does **not** prove ReShade callback timing, simulator texture identification,
camera rendering, camera offsets, airframe visibility, or simulator performance.
Readback occurs only in this validation executable, never in the production
calibration helper.

`copy_main.cpp` additionally exercises the production `CopyCalibration` helper
against textures without render-target usage. It checks 28 pixel cases on direct
and copy queues, using six RGBA/BGRA UNORM, sRGB and typeless formats, two
independently recorded animation frames and an odd-sized surface. Every pixel
must match the expected diagnostic or the unchanged lower display. It checks
immutable storage reuse and the 16-entry allocation cap. The shutdown fence
helper is also exercised against real submitted GPU work. Run the executable
normally or with `--warp`, just like the RTV host.

These cases validate the GPU helpers and the preservation boundary. They do not
identify MSFS's final PFD texture or prove the simulator camera-render interface.

`mip_main.cpp` matches the five-mip 768 × 1024 RGBA8 resources observed in MSFS.
It creates an RTV for each mip and calls the production rectangle helper with
that mip's dimensions. All 1,047,552 pixels are checked, including all 267,408
lower-region pixels. The checked upper boundaries are 763, 381, 190, 95 and 47
for levels 0 through 4. This catches the error of applying base-level dimensions
or the wrong RTV to a smaller mip. The test does not generate mips: subsequent
application filtering can blend across the display divider even though the
calibration writes themselves preserve each mip's lower rows.
