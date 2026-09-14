[CmdletBinding()]
param(
    [switch]$Bootstrap,
    [switch]$Validate,
    [string]$ReShadeDll
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Bootstrap) {
    & (Join-Path $PSScriptRoot 'bootstrap.ps1')
}

$dependencies = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') | ConvertFrom-Json
$dependencyRoot = Join-Path $PSScriptRoot 'build/deps'
$compilerRoot = Join-Path $dependencyRoot $dependencies.'llvm-mingw'.directory
$compiler = Join-Path $compilerRoot 'bin/clang++.exe'
$reshade = Join-Path $dependencyRoot $dependencies.reshade.directory
$imgui = Join-Path $dependencyRoot $dependencies.imgui.directory
foreach ($required in @($compiler, "$reshade/include/reshade.hpp", "$imgui/imgui.h")) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing $required. Run build.ps1 -Bootstrap first."
    }
}

$outputDirectory = Join-Path $PSScriptRoot 'build'
$output = Join-Path $outputDirectory 'taxi-camera-native.addon64'
& (Join-Path $PSScriptRoot 'abi/validate.ps1') -Compiler $compiler -ReShadeInclude "$reshade/include" -ImGuiInclude $imgui
& (Join-Path $PSScriptRoot 'engine-hook/build.ps1')
& (Join-Path $PSScriptRoot 'engine-camera/build.ps1')
& (Join-Path $PSScriptRoot 'native-camera/build.ps1')
$common = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-fms-extensions', '-static', '-mno-avx', '-mno-avx2', '-mno-avx512f')
$addonArguments = $common + @(
    '-shared', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN',
    '-isystem', "$reshade/include", '-isystem', $imgui,
    (Join-Path $PSScriptRoot 'src/taxi_camera_addon.cpp'),
    (Join-Path $PSScriptRoot 'src/scene_handoff.cpp'),
    (Join-Path $PSScriptRoot 'src/scene_capture_d3d12.cpp'),
    (Join-Path $PSScriptRoot 'src/scene_capture_manager.cpp'),
    (Join-Path $PSScriptRoot 'src/scene_source_state.cpp'),
    (Join-Path $PSScriptRoot 'src/scene_frame_output.cpp'),
    (Join-Path $PSScriptRoot 'src/scene_runtime.cpp'),
    (Join-Path $PSScriptRoot 'src/pfd_stamp_state.cpp'),
    (Join-Path $PSScriptRoot 'src/pfd_stamp_d3d12.cpp'),
    (Join-Path $PSScriptRoot 'src/pfd_state_adapter.cpp'),
    (Join-Path $PSScriptRoot 'engine-hook/queue_submit_observer.cpp'),
    (Join-Path $PSScriptRoot 'engine-hook/pfd_state_observer.cpp'),
    (Join-Path $PSScriptRoot 'engine-hook/render_boundary_observer.cpp'),
    (Join-Path $PSScriptRoot 'engine-hook/resource_creation_observer.cpp'),
    (Join-Path $PSScriptRoot 'native-camera/probe.cpp'),
    (Join-Path $PSScriptRoot 'native-camera/local_memory.cpp'),
    (Join-Path $PSScriptRoot 'native-camera/code_contract.cpp'),
    (Join-Path $PSScriptRoot 'native-camera/activation_mask.cpp'),
    (Join-Path $PSScriptRoot 'native-camera/view_resize.cpp'),
    (Join-Path $PSScriptRoot 'native-camera/source_view.cpp'),
    (Join-Path $PSScriptRoot 'native-camera/body_pose_provider.cpp'),
    (Join-Path $PSScriptRoot 'native-camera/mount_config.cpp'),
    (Join-Path $PSScriptRoot 'native-camera/verified_profile.cpp'),
    (Join-Path $PSScriptRoot 'discovery/aircraft_inventory.cpp'),
    (Join-Path $PSScriptRoot 'engine-camera/entry_pair.cpp'),
    (Join-Path $PSScriptRoot 'engine-camera/owned_entry_inventory.cpp'),
    (Join-Path $PSScriptRoot 'engine-camera/owned_view.cpp'),
    (Join-Path $PSScriptRoot 'engine-camera/view_pool.cpp'),
    (Join-Path $PSScriptRoot 'engine-hook/build/engine-hook.a'),
    (Join-Path $outputDirectory 'abi/native_bridge.obj'),
    '-o', $output, '-Wl,--no-insert-timestamp', '-ld3d12', '-ldxgi', '-ldxguid', '-ld3dcompiler'
)
& $compiler @addonArguments
if ($LASTEXITCODE -ne 0) { throw 'Native add-on compilation failed.' }

if ($Validate) {
    if (-not $ReShadeDll) { throw '-Validate requires -ReShadeDll pointing to the existing ReShade 6.8 DLL for the real PFD adapter test.' }
    & (Join-Path $PSScriptRoot 'validate-pfd-pipeline.ps1') -ReShadeDll $ReShadeDll
    $validationDirectory = Join-Path $outputDirectory 'gpu-validation'
    New-Item -ItemType Directory -Path $validationDirectory -Force | Out-Null
    $unitTests = Join-Path $validationDirectory 'calibration-tests.exe'
    & $compiler @common (Join-Path $PSScriptRoot 'tests/calibration_test.cpp') -o $unitTests
    if ($LASTEXITCODE -ne 0) { throw 'Calibration test compilation failed.' }
    & $unitTests
    if ($LASTEXITCODE -ne 0) { throw 'Calibration boundary tests failed.' }
    $writeBudgetTests = Join-Path $validationDirectory 'write-budget-tests.exe'
    & $compiler @common (Join-Path $PSScriptRoot 'tests/write_budget_test.cpp') -o $writeBudgetTests
    if ($LASTEXITCODE -ne 0) { throw 'Write budget test compilation failed.' }
    & $writeBudgetTests
    if ($LASTEXITCODE -ne 0) { throw 'Write budget tests failed.' }
    $taxiButtonRouteTests = Join-Path $validationDirectory 'taxi-button-routes-tests.exe'
    & $compiler @common (Join-Path $PSScriptRoot 'tests/taxi_button_routes_test.cpp') -o $taxiButtonRouteTests
    if ($LASTEXITCODE -ne 0) { throw 'Taxi button route test compilation failed.' }
    & $taxiButtonRouteTests
    if ($LASTEXITCODE -ne 0) { throw 'Taxi button route tests failed.' }
    $pfdTargetDetectorTests = Join-Path $validationDirectory 'pfd-target-detector-tests.exe'
    & $compiler @common (Join-Path $PSScriptRoot 'tests/pfd_target_detector_test.cpp') -o $pfdTargetDetectorTests
    if ($LASTEXITCODE -ne 0) { throw 'PFD target detector test compilation failed.' }
    & $pfdTargetDetectorTests
    if ($LASTEXITCODE -ne 0) { throw 'PFD target detector tests failed.' }
    $observationTests = Join-Path $validationDirectory 'resource-observation-tests.exe'
    & $compiler @common '-isystem' "$reshade/include" (Join-Path $PSScriptRoot 'tests/resource_observation_test.cpp') -o $observationTests
    if ($LASTEXITCODE -ne 0) { throw 'Resource observation test compilation failed.' }
    & $observationTests
    if ($LASTEXITCODE -ne 0) { throw 'Resource observation tests failed.' }
    $displayExposureTests = Join-Path $validationDirectory 'display-exposure-tests.exe'
    & $compiler @common (Join-Path $PSScriptRoot 'tests/display_exposure_test.cpp') -o $displayExposureTests
    if ($LASTEXITCODE -ne 0) { throw 'Display exposure test compilation failed.' }
    & $displayExposureTests
    if ($LASTEXITCODE -ne 0) { throw 'Display exposure tests failed.' }
    $validator = Join-Path $validationDirectory 'taxi-camera-gpu-validation.exe'
    $validationArguments = $common + @(
        '-municode', (Join-Path $PSScriptRoot 'validation/main.cpp'),
        '-o', $validator, '-Wl,--no-insert-timestamp', '-ld3d12', '-ldxgi', '-ldxguid'
    )
    & $compiler @validationArguments
    if ($LASTEXITCODE -ne 0) { throw 'GPU validation host compilation failed.' }
    $hardwareResult = & $validator
    $hardwareResult | Write-Output
    if ($LASTEXITCODE -ne 0) { throw 'Default-device GPU validation failed.' }
    $warpResult = & $validator --warp
    $warpResult | Write-Output
    if ($LASTEXITCODE -ne 0) { throw 'WARP GPU validation failed.' }
    $copyValidator = Join-Path $validationDirectory 'taxi-camera-copy-validation.exe'
    & $compiler @common '-municode' (Join-Path $PSScriptRoot 'validation/copy_main.cpp') '-o' $copyValidator `
        '-Wl,--no-insert-timestamp' '-ld3d12' '-ldxgi' '-ldxguid'
    if ($LASTEXITCODE -ne 0) { throw 'Copy calibration validation compilation failed.' }
    $copyHardwareResult = & $copyValidator
    $copyHardwareResult | Write-Output
    if ($LASTEXITCODE -ne 0) { throw 'Default-device copy calibration failed.' }
    $copyWarpResult = & $copyValidator --warp
    $copyWarpResult | Write-Output
    if ($LASTEXITCODE -ne 0) { throw 'WARP copy calibration failed.' }
    $mipValidator = Join-Path $validationDirectory 'taxi-camera-mip-validation.exe'
    & $compiler @common '-municode' (Join-Path $PSScriptRoot 'validation/mip_main.cpp') '-o' $mipValidator `
        '-Wl,--no-insert-timestamp' '-ld3d12' '-ldxgi' '-ldxguid'
    if ($LASTEXITCODE -ne 0) { throw 'Mip calibration validation compilation failed.' }
    $mipHardwareResult = & $mipValidator
    $mipHardwareResult | Write-Output
    if ($LASTEXITCODE -ne 0) { throw 'Default-device mip calibration failed.' }
    $mipWarpResult = & $mipValidator --warp
    $mipWarpResult | Write-Output
    if ($LASTEXITCODE -ne 0) { throw 'WARP mip calibration failed.' }
    $compositorValidator = Join-Path $validationDirectory 'compositor-validation.exe'
    & $compiler @common '-municode' (Join-Path $PSScriptRoot 'validation/compositor_main.cpp') '-o' $compositorValidator `
        '-Wl,--no-insert-timestamp' '-ld3d12' '-ldxgi' '-ldxguid' '-ld3dcompiler'
    if ($LASTEXITCODE -ne 0) { throw 'Two-source compositor validation compilation failed.' }
    $compositorHardwareResult = & $compositorValidator
    $compositorHardwareResult | Write-Output
    if ($LASTEXITCODE -ne 0) { throw 'Default-device two-source compositor validation failed.' }
    $compositorWarpResult = & $compositorValidator --warp
    $compositorWarpResult | Write-Output
    if ($LASTEXITCODE -ne 0) { throw 'WARP two-source compositor validation failed.' }
    [ordered]@{
        addonSha256 = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash
        abiGatePassed = $true
        calibrationTestsPassed = $true
        writeBudgetTestsPassed = $true
        taxiButtonRouteTestsPassed = $true
        pfdTargetDetectorTestsPassed = $true
        observationTestsPassed = $true
        displayExposureTestsPassed = $true
        nativeCameraTestsPassed = $true
        engineHookTestsPassed = $true
        pfdPipelineTestsPassed = $true
        defaultDevice = ($hardwareResult | ConvertFrom-Json)
        warp = ($warpResult | ConvertFrom-Json)
        copyDefaultDevice = ($copyHardwareResult | ConvertFrom-Json)
        copyWarp = ($copyWarpResult | ConvertFrom-Json)
        mipDefaultDevice = ($mipHardwareResult | ConvertFrom-Json)
        mipWarp = ($mipWarpResult | ConvertFrom-Json)
        compositorDefaultDevice = ($compositorHardwareResult | ConvertFrom-Json)
        compositorWarp = ($compositorWarpResult | ConvertFrom-Json)
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputDirectory 'validation-result.json') -Encoding utf8
}

Get-FileHash -LiteralPath $output -Algorithm SHA256 | Select-Object Path, Hash
