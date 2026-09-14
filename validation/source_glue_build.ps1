[CmdletBinding()]
param(
    [string]$ReShadeDll = 'C:/XboxGames/Microsoft Flight Simulator 2024/Content/dxgi.dll',
    [ValidateSet('baseline', 'immediate', 'unrelated-bundle', 'unknown-native', 'native-reset', 'all')][string]$Scenario = 'immediate'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$nativeRoot = Split-Path -Parent $PSScriptRoot
$deps = Get-Content -Raw -LiteralPath (Join-Path $nativeRoot 'dependencies.json') | ConvertFrom-Json
$compiler = Join-Path $nativeRoot ('build/deps/' + $deps.'llvm-mingw'.directory + '/bin/clang++.exe')
$reshade = Join-Path $nativeRoot ('build/deps/' + $deps.reshade.directory + '/include')
$imgui = Join-Path $nativeRoot ('build/deps/' + $deps.imgui.directory)
$outputDirectory = Join-Path $nativeRoot 'build/source-glue-validation'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$exe = Join-Path $outputDirectory 'source-glue-validation.exe'
# Use the actual production dependency list. The main TU includes the add-on,
# so its source is intentionally omitted from the separate linker inputs.
$build = Get-Content -Raw -LiteralPath (Join-Path $nativeRoot 'build.ps1')
$segment = [regex]::Match($build, '(?s)\$addonArguments =.*?(?=& \$compiler @addonArguments)').Value
if (-not $segment) { throw 'Production add-on source list was not found.' }
$sources = @([regex]::Matches($segment, "Join-Path \`$PSScriptRoot '([^']+)'" ) | ForEach-Object {
    $path = $_.Groups[1].Value
    if ($path -ne 'src/taxi_camera_addon.cpp') { Join-Path $nativeRoot $path }
})
if ($sources.Count -lt 20) { throw 'Incomplete production dependency list.' }
$sources += Join-Path $nativeRoot 'build/abi/native_bridge.obj'
$dll = Get-Item -LiteralPath $ReShadeDll
if ($dll.VersionInfo.ProductName -notmatch 'ReShade' -or $dll.VersionInfo.FileVersion -notmatch '^6\.8\.0\.') {
    throw 'The isolated host requires the existing pinned ReShade 6.8.0 DLL.'
}
& $compiler '-std=c++20' '-O2' '-Wall' '-Wextra' '-Werror' '-fms-extensions' '-static' '-mno-avx' '-mno-avx2' '-mno-avx512f' `
    '-DNOMINMAX' '-DWIN32_LEAN_AND_MEAN' '-isystem' $reshade '-isystem' $imgui `
    (Join-Path $nativeRoot 'validation/source_glue_main.cpp') @sources '-ld3d12' '-ldxgi' '-ldxguid' '-ld3dcompiler' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Production source-glue host compilation failed.' }
Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $outputDirectory 'dxgi.dll') -Force
$results = [ordered]@{ binarySha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash; reshadeSha256 = (Get-FileHash -LiteralPath $dll.FullName -Algorithm SHA256).Hash }
$scenarios = @($Scenario)
if ($Scenario -eq 'immediate') { $scenarios = @('immediate', 'unrelated-bundle', 'unknown-native', 'native-reset') }
if ($Scenario -eq 'all') { $scenarios = @('baseline', 'immediate', 'unrelated-bundle', 'unknown-native', 'native-reset') }
$failed = $false
foreach ($testScenario in $scenarios) {
foreach ($warp in @($false, $true)) {
    $arguments = @(); if ($warp) { $arguments += '--warp' }
    $arguments += '--' + $testScenario
    $lines = @(& $exe @arguments)
    $exitCode = $LASTEXITCODE
    $lines | Write-Output
    if (-not $lines.Count) { throw 'Production source-glue GPU validation returned no evidence.' }
    $result = $lines | ConvertFrom-Json
    $backend = $(if ($warp) { 'warp' } else { 'hardware' })
    $key = $(if ($testScenario -eq $Scenario -or $testScenario -eq 'immediate') { $backend } else { $testScenario + '-' + $backend })
    $results[$key] = $result
    if ($exitCode -ne 0 -or -not $result.passed) {
        $failed = $true
        continue
    }
    if ($testScenario -eq 'unknown-native') {
        if (-not $result.expectedRefusal -or $result.tailCaptures -ne 0 -or $result.sourceDraws -ne 4 -or
            $result.status -ne 'unknown_source_state') { throw 'Unknown native work was not conservatively refused.' }
    } elseif (-not $result.productionCallbacks -or $result.tailCaptures -ne 4 -or $result.pixels -ne 1165824 -or
        -not $result.legacyBornRT -or -not $result.enhancedBornRT -or $result.initialSourceBarriers -ne 0) {
        throw 'Production callback/pixel evidence is incomplete.'
    }
    if ($testScenario -eq 'immediate' -and ($result.immediateFlushes -ne 2 -or $result.immediateResets -ne 2 -or
        -not $result.immediateRegistered)) { throw 'Actual ReShade immediate lifecycle was not exercised.' }
    if ($testScenario -eq 'native-reset' -and ($result.nativeUnobservedSubmissions -ne 2 -or $result.nativeResets -ne 1 -or
        $result.nativeCleanSubmissions -ne 2 -or -not $result.preResetCaptureRefused -or $result.preResetSourceDraws -ne 2 -or
        $result.totalSourceDraws -ne 10)) {
        throw 'Actual native list adoption, unobserved replay refusal and successful Reset were not exercised.'
    }
    $log = Get-Content -Raw -LiteralPath (Join-Path $outputDirectory 'ReShade.log')
    if ($log -notmatch 'Taxi Camera Native Probe' -or $log -match '\| ERROR\s*\|') { throw 'Actual ReShade registration/error check failed.' }
}
}
$results.passed = -not $failed
$results.limitation = 'Isolated generated GPU scene: actual creation-state seeding, ReShade immediate flushes, unrelated bundles and native-only lists after observed successful Reset. Unobserved recordings and replay still refuse capture; fresh sources are required after state loss. No simulator private calls; readback is test-only.'
$resultName = $(if ($Scenario -eq 'immediate') { 'result.json' } else { 'result-' + $Scenario + '.json' })
$results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outputDirectory $resultName) -Encoding utf8
if ($failed) { throw 'Production source-glue GPU regression failed; per-case evidence was saved.' }
