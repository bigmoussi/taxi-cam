param([Parameter(Mandatory = $true)][string]$ReShadeDll)
$ErrorActionPreference = 'Stop'
$pipelineCompiler = Join-Path $PSScriptRoot 'build/deps/llvm-mingw/llvm-mingw-20260908-ucrt-x86_64/bin/clang++.exe'
$pipelineFlags = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-static',
                   '-mno-avx', '-mno-avx2', '-mno-avx512f')
$pipelineHandoff = Join-Path $PSScriptRoot 'build/scene-handoff-test.exe'
& $pipelineCompiler @pipelineFlags (Join-Path $PSScriptRoot 'src/scene_handoff.cpp') `
    (Join-Path $PSScriptRoot 'tests/scene_handoff_test.cpp') -o $pipelineHandoff
if ($LASTEXITCODE -ne 0) { throw 'Scene identity handoff test compilation failed.' }
& $pipelineHandoff
if ($LASTEXITCODE -ne 0) { throw 'Scene identity handoff validation failed.' }
$pipelineQueueTest = Join-Path $PSScriptRoot 'build/queue-submit-observer-test.exe'
& $pipelineCompiler @pipelineFlags '-DTAXI_QUEUE_SUBMIT_VALIDATION' `
    (Join-Path $PSScriptRoot 'engine-hook/queue_submit_observer.cpp') `
    (Join-Path $PSScriptRoot 'engine-hook/queue_submit_observer_test.cpp') '-ld3d12' '-ldxgi' '-ldxguid' -o $pipelineQueueTest
if ($LASTEXITCODE -ne 0) { throw 'Native queue observer test compilation failed.' }
foreach ($pipelineCase in @('mock', 'fail-writable', 'fail-install-restore', 'fail-cas-restore', 'fail-remove-restore', 'hardware', 'warp')) {
    & $pipelineQueueTest $pipelineCase
    if ($LASTEXITCODE -ne 0) { throw "Native queue observer validation failed: $pipelineCase" }
}
& (Join-Path $PSScriptRoot 'validation/scene_capture_build.ps1')
& (Join-Path $PSScriptRoot 'validation/render_boundary_test.ps1')
& (Join-Path $PSScriptRoot 'engine-hook/resource_creation_test.ps1') -Compiler $pipelineCompiler
& (Join-Path $PSScriptRoot 'tests/test-source-state.ps1')
& (Join-Path $PSScriptRoot 'src/scene_capture_manager_test.ps1')
& (Join-Path $PSScriptRoot 'src/scene_frame_output_test.ps1')
& (Join-Path $PSScriptRoot 'validation/pfd_stamp_build.ps1') -ReShadeDll $ReShadeDll
& (Join-Path $PSScriptRoot 'validation/source_glue_build.ps1') -ReShadeDll $ReShadeDll
