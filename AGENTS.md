# 380 Taxi Cam development

This is an independent Windows native project. Keep project source, builds and documentation in this repository; do not edit or depend on an aircraft source checkout.

- Keep external aircraft Lvar and material identifiers exact where required for compatibility. They are not project branding.
- Do not add inherited organization copyright or SPDX headers to original project files. Preserve upstream notices on third-party code and dependencies.
- Build with the pinned toolchain in `dependencies.json` through `build.ps1`; use the root `.clang-format` for C++.
- Run the smallest relevant checks first. Native delivery changes require `build.ps1 -Validate -ReShadeDll <path>` and `smoke-test.ps1 -ReShadeDll <path>` for the exact binary before installation.
- Build products, downloaded dependencies, process captures, logs and historical binary archives belong in ignored `build/` directories. Never rewrite historical release receipts to describe a different source tree or binary.
- Separate local test results from in-simulator observations. Keep unresolved rendering, targeting, lighting and motion issues explicit.
- Preserve native identity, memory bounds, lifecycle and GPU synchronization guards. Graphics resources and public SimConnect telemetry have different ownership and update contracts.
- The installed DLL is locked while MSFS runs. Do not stop the simulator or overwrite user calibration without authorization; build and validate first.
- The current ReShade prototype and the planned external Windows companion are different packaging stages. Do not describe the companion as implemented.