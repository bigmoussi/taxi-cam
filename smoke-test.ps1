[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ReShadeDll
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$buildDirectory = Join-Path $PSScriptRoot 'build'
$addon = Join-Path $buildDirectory 'taxi-camera-native.addon64'
$validation = Get-Content -Raw -LiteralPath (Join-Path $buildDirectory 'validation-result.json') | ConvertFrom-Json
$addonHash = (Get-FileHash -LiteralPath $addon -Algorithm SHA256).Hash
if ($validation.addonSha256 -ne $addonHash -or -not $validation.abiGatePassed -or
    -not $validation.nativeCameraTestsPassed -or -not $validation.engineHookTestsPassed -or
    -not $validation.pfdPipelineTestsPassed -or
    -not $validation.calibrationTestsPassed -or -not $validation.observationTestsPassed -or -not $validation.writeBudgetTestsPassed -or
    -not $validation.defaultDevice.passed -or -not $validation.warp.passed -or
    -not $validation.copyDefaultDevice.passed -or -not $validation.copyWarp.passed -or
    -not $validation.mipDefaultDevice.passed -or -not $validation.mipWarp.passed) {
    throw 'Run build.ps1 -Validate successfully for this exact add-on binary first.'
}
$sourceDll = Get-Item -LiteralPath $ReShadeDll
if ($sourceDll.VersionInfo.ProductName -notmatch 'ReShade' -or $sourceDll.VersionInfo.FileVersion -notmatch '^6\.8\.0\.') {
    throw 'This probe is pinned to ReShade 6.8.0. Supply its existing dxgi.dll.'
}

$testDirectory = Join-Path $buildDirectory 'reshade-smoke'
New-Item -ItemType Directory -Path $testDirectory -Force | Out-Null
Copy-Item -LiteralPath $sourceDll.FullName -Destination (Join-Path $testDirectory 'dxgi.dll') -Force
Copy-Item -LiteralPath $addon -Destination (Join-Path $testDirectory 'taxi-camera-native.addon64') -Force
$testExecutable = Join-Path $testDirectory 'taxi-camera-gpu-validation.exe'
Copy-Item -LiteralPath (Join-Path $buildDirectory 'gpu-validation/taxi-camera-gpu-validation.exe') -Destination $testExecutable -Force
$started = [DateTime]::UtcNow
$gpuResult = & $testExecutable --warp
$gpuResult | Write-Output
if ($LASTEXITCODE -ne 0) { throw 'Isolated ReShade validation host failed.' }
$copyExecutable = Join-Path $testDirectory 'taxi-camera-copy-validation.exe'
Copy-Item -LiteralPath (Join-Path $buildDirectory 'gpu-validation/taxi-camera-copy-validation.exe') -Destination $copyExecutable -Force
$copyResult = & $copyExecutable --warp
$copyResult | Write-Output
if ($LASTEXITCODE -ne 0) { throw 'Isolated ReShade copy validation host failed.' }
$mipExecutable = Join-Path $testDirectory 'taxi-camera-mip-validation.exe'
Copy-Item -LiteralPath (Join-Path $buildDirectory 'gpu-validation/taxi-camera-mip-validation.exe') -Destination $mipExecutable -Force
$mipResult = & $mipExecutable --warp
$mipResult | Write-Output
if ($LASTEXITCODE -ne 0) { throw 'Isolated ReShade mip validation host failed.' }
$logPath = Join-Path $testDirectory 'ReShade.log'
$logFile = Get-Item -LiteralPath $logPath
if ($logFile.LastWriteTimeUtc -lt $started) { throw 'No fresh ReShade log was produced.' }
$log = Get-Content -Raw -LiteralPath $logPath
foreach ($required in @(
    'Registered add-on "Taxi Camera Native Probe"',
    'Taxi Camera Native Probe: D3D12 device initialized; calibration disabled.',
    'Taxi Camera Native Probe: device destroyed;',
    'Unregistered add-on "Taxi Camera Native Probe"'
)) {
    if (-not $log.Contains($required)) { throw "ReShade lifecycle evidence missing: $required" }
}
if ($log -match '\| ERROR\s*\|') { throw 'ReShade reported an error; inspect build/reshade-smoke/ReShade.log.' }
[ordered]@{
    passed = $true
    addonSha256 = $addonHash
    reshadeSha256 = (Get-FileHash -LiteralPath $sourceDll.FullName -Algorithm SHA256).Hash
    reshadeVersion = $sourceDll.VersionInfo.FileVersion
    gpu = ($gpuResult | ConvertFrom-Json)
    copyGpu = ($copyResult | ConvertFrom-Json)
    mipGpu = ($mipResult | ConvertFrom-Json)
    limitation = 'Registration and device lifecycle passed in an isolated host. In-simulator draw selection and camera rendering are unverified.'
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $buildDirectory 'smoke-result.json') -Encoding utf8
Write-Output 'PASS: existing ReShade loaded the native add-on, initialized and destroyed a D3D12 device, and unloaded it cleanly.'
