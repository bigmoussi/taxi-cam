# Third-party notices

Original project files are maintained separately from downloaded dependencies. The following upstream notices are retained verbatim; they apply to their respective components, not as a project-wide license declaration.

| Component | Use | Notice |
| --- | --- | --- |
| ReShade 6.8.0 | Add-on API headers and separately installed runtime | [ReShade notice](licenses/ReShade.txt) |
| Dear ImGui 1.92.5 | Headers for the ReShade overlay integration | [Dear ImGui notice](licenses/Dear-ImGui.txt) |
| LLVM toolchain | Compiler and runtime supplied by the pinned llvm-mingw distribution | [LLVM license](licenses/LLVM.txt) |

Exact versions, source locations and archive hashes are in `dependencies.json`. Dependency archives and their original notices remain intact under `build/deps/`; the repository does not vendor their source trees or ship the simulator/ReShade runtime. Additional notices in a toolchain or runtime distribution must remain with that distribution.