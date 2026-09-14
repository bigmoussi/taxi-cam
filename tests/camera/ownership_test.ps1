param()
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'ci/toolchain.ps1')
$cameraRoot = $PSScriptRoot
$nativeRoot = $repoRoot
$toolchain = Get-TaxiToolchain $repoRoot
$compiler = Join-Path $toolchain 'clang++.exe'
$archiver = Join-Path $toolchain 'llvm-ar.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'Run ./bootstrap.ps1 to install the pinned compiler.' }
$outputDirectory = Join-Path $repoRoot 'build/tests/camera/ownership'
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
    & $compiler @common '-fno-exceptions' '-c' (Join-Path $repoRoot ('src/camera/' + $component.Source + '.cpp')) '-o' $cameraObject
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
