# Copy calibration validation

`copy_main.cpp` runs the production `CopyCalibration` helper against a native D3D12 device. Build it with the same compiler and flags as `main.cpp`, then run the executable normally and with `--warp`. Each invocation prints a JSON result and returns a nonzero exit code on failure.

The checks cover six RGBA/BGRA 8-bit UNORM, sRGB and typeless formats on direct and copy queues. Targets have no render-target flag. Two distinct animation frames are recorded before submission and both are read back, checking that immutable upload storage preserves earlier commands. The 768×1024 cases compare every pixel and preserve every pixel from row 763 onward; odd dimensions exercise row-pitch padding and the scaled boundary. The validator also checks cache reuse, the 16-entry limit, rejected dimensions/formats and the 128 MiB allocation calculation. Fifteen native queue drains must complete successfully per adapter.

The normal Windows graphics debug layer is optional and its availability is reported explicitly. Pixel success with `debug_layer: false` does not claim debug-layer validation. The tests exercise successful queue draining; they do not manufacture GPU hangs or device removal.

## Integration contract

- This is a diagnostic color pattern, not a camera or a scene renderer.
- The caller must supply an open direct/copy command list outside a native render pass and a live, non-sparse, single-mip/layer/sample target already in `COPY_DEST`. The helper validates the native description but does not infer, track or change resource states.
- Append it after an exactly replayed application copy. Native texture-to-texture copy events preserve source/destination subresources, source box and destination coordinates. The ReShade buffer-to-texture event omits original row pitch and source-box origin, so it remains observation-only.
- Cached upload buffers are immutable and bounded to 16 shape/format entries and 128 MiB. They remain alive across target selection changes and can be used by multiple recorded command lists. Larger shapes can be refused by the byte budget even when their dimensions are otherwise supported.
- Serialize pool access. Prevent further queue submissions before calling `drain_copy_queue`; a successful checked fence permits cleanup after all relevant queues drain. The drain uses a 30-second timeout and reports native failures.
- If any drain fails, call `abandon()` at teardown instead of `release()`. This intentionally retains native allocations until process exit. A failed drain also retains any fence/event that could still be referenced by the driver. Hot unloading is not supported.
- Copies transfer encoded bytes without blending or sRGB conversion. No pixels below the upper camera area are written.

The state and format requirements follow Microsoft's [CopyTextureRegion contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion). Sparse resources are rejected using [GetHeapProperties](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-getheapproperties), which returns an error for reserved resources.
