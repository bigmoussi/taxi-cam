$ErrorActionPreference = 'Stop'
$frameRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$frameBuild = Join-Path $frameRoot 'build'
$frameCompiler = Join-Path $frameBuild 'deps/llvm-mingw/llvm-mingw-20260908-ucrt-x86_64/bin/clang++.exe'
$frameFlags = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-mno-avx', '-mno-avx2', '-mno-avx512f')
$frameObjects = @()
foreach ($frameSource in @('scene_frame_output', 'scene_capture_manager', 'scene_capture_d3d12', 'scene_handoff', 'scene_source_state')) {
  $frameObject = Join-Path $frameBuild "$frameSource-frame-validation.o"
  $frameExceptionFlags = @('-fno-exceptions')
  if ($frameSource -eq 'scene_handoff') { $frameExceptionFlags = @() }
  & $frameCompiler @frameFlags @frameExceptionFlags -c (Join-Path $PSScriptRoot "$frameSource.cpp") -o $frameObject
  if ($LASTEXITCODE -ne 0) { throw "$frameSource compile failed." }
  $frameObjects += $frameObject
}
$frameBoundary = Join-Path $frameBuild 'render-boundary-frame-validation.o'
& $frameCompiler @frameFlags -c (Join-Path $frameRoot 'engine-hook/render_boundary_observer.cpp') -o $frameBoundary
if ($LASTEXITCODE -ne 0) { throw 'Frame output native observer compilation failed.' }
$frameObjects += $frameBoundary
$frameExecutable = Join-Path $frameBuild 'scene-frame-output-test.exe'
& $frameCompiler @frameFlags -static (Join-Path $PSScriptRoot 'scene_frame_output_test.cpp') @frameObjects -ld3d12 -ld3dcompiler -ldxgi -ldxguid -o $frameExecutable
if ($LASTEXITCODE -ne 0) { throw 'Frame output test compile failed.' }
foreach ($frameAdapter in @('hardware', 'warp')) {
  $frameArguments = @()
  if ($frameAdapter -eq 'warp') { $frameArguments = @('--warp') }
  $frameOutput = & $frameExecutable @frameArguments
  if ($LASTEXITCODE -ne 0) { throw "Frame output validation failed on $frameAdapter." }
  $frameResult = $frameOutput | ConvertFrom-Json
  if ($frameResult.passed -ne $true -or $frameResult.checked_pixels -ne 1171968) { throw 'Invalid frame output receipt.' }
  $frameOutput | Set-Content -LiteralPath (Join-Path $frameBuild "scene-frame-output-$frameAdapter.json") -Encoding utf8
  Write-Output $frameOutput
}
