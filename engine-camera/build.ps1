param()
$ErrorActionPreference = 'Stop'
$cameraRoot = $PSScriptRoot
$nativeRoot = Split-Path -Parent $cameraRoot
$toolchain = Join-Path $nativeRoot 'build/deps/llvm-mingw/llvm-mingw-20260908-ucrt-x86_64/bin'
$compiler = Join-Path $toolchain 'clang++.exe'
$archiver = Join-Path $toolchain 'llvm-ar.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'The parent native probe pinned LLVM-MinGW compiler is required.' }
$outputDirectory = Join-Path $cameraRoot 'build'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$common = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-mno-avx', '-mno-avx2', '-mno-avx512f')
$components = @(
    @{ Source = 'entry_pair'; Test = 'engine-camera-test' }
    @{ Source = 'owned_entry_inventory'; Test = 'owned-entry-inventory-test' }
    @{ Source = 'view_pool'; Test = 'view-pool-test' }
    @{ Source = 'owned_view'; Test = 'owned-view-test' }
)
$cameraObjects = @()
foreach ($component in $components) {
    $cameraObject = Join-Path $outputDirectory ($component.Source + '.o')
    & $compiler @common '-fno-exceptions' '-c' (Join-Path $cameraRoot ($component.Source + '.cpp')) '-o' $cameraObject
    if ($LASTEXITCODE -ne 0) { throw "Camera component failed to compile: $($component.Source)" }
    $cameraObjects += $cameraObject
}
$library = Join-Path $outputDirectory 'engine-camera.a'
& $archiver 'rcs' $library @cameraObjects
if ($LASTEXITCODE -ne 0) { throw 'Camera components failed to archive.' }
foreach ($component in $components) {
    $testExecutable = Join-Path $outputDirectory ($component.Test + '.exe')
    & $compiler @common '-static' (Join-Path $cameraRoot ($component.Source + '_test.cpp')) $library '-o' $testExecutable
    if ($LASTEXITCODE -ne 0) { throw "Camera tests failed to compile: $($component.Source)" }
    & $testExecutable
    if ($LASTEXITCODE -ne 0) { throw "Camera tests failed: $($component.Source)" }
    Write-Output "Built: $testExecutable"
}
Write-Output "Built: $library"
