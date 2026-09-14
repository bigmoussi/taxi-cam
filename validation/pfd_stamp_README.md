# PFD image stamp validation

Run `./validation/pfd_stamp_build.ps1`. Its optional `-ReShadeDll` argument defaults to the already installed MSFS2024 ReShade6.8.0 DLL. The script copies that DLL only into the ignored validation directory. It neither opens MSFS nor changes its files.

The script builds and runs:

- 8,215 native observer tests: exact forwarding, failed/successful Reset results, partial installation, protection restoration, disabled callbacks, unknown objects, incarnation reuse, an in-flight stale callback, the8,192-list cap, and removal. The observer retains no native command-list references.
- Standalone hardware and WARP pixel tests. Two changing camera inputs are generated on the GPU, composed, copied into immutable GPU buffers, and stamped into a768×1024 target. Each run checks1,572,864 pixels, including400,896 lower-region pixels.
- The same pixel test through the actual ReShade6.8 proxy device and command list. The validation executable registers itself as an add-on through ReShade's registration API. Application root signatures, PSOs and state generate the real event metadata consumed by `pfd_state_adapter`; native heap/CBV/Reset observations fill the missing state. There are no validation exports in the production add-on.

The application draw following each stamp receives **no new graphics bindings**. Its output depends on restored root constants, CBV, SRV, UAV, a texture descriptor table and sampler table in both original descriptor heaps, PSO, topology, viewport and scissor. Its lower-region patch is erased after the stamp, so the following draw must recreate that patch. The rest of the lower261 rows remains byte-for-byte unchanged. Final results and exact executable/DLL SHA256 hashes are written to `build/pfd-stamp-validation/result.json`.

These tests exposed two issues that have been corrected: ReShade's resource `GetDevice` returns its proxy even for native-created resources, and ReShade emits a synthetic reset event after command-list creation. Native device identity is now normalized using the pinned ReShade COM unwrap contract; recording retirement uses only the actual native Reset result.

The stamp changes only its graphics PSO, root signature/arguments, primitive topology, viewport and scissor, and then restores them. It does not change descriptor heaps, RTV/DSV bindings, compute state or simulator-resource barriers. It samples an owned GPU buffer. Complete current state and live PSO/root generations are required; unknown state refuses the stamp. Bundle/indirect state, native render passes, depth attachments and other unsupported target configurations remain excluded by the adapter/caller contract.

The caller must establish the selected sole RTV, exact bound mip dimensions and format, and register the consuming recording on the shared queue timeline before stamping. All producers, replayable consumers, GPU resources, root signatures and PSOs must remain valid through their fence/recording lifetimes. The test debug layer was unavailable on this host; hardware/WARP pixel and callback results are reported separately. Generated inputs prove GPU delivery and state restoration, not MSFS scene capture or live frame rate. Repainting after every PFD draw preserves coverage; its in-simulator performance still needs measurement.
