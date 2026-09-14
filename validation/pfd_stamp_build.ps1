[CmdletBinding()]
param([string]$Compiler, [string]$ReShadeDll = 'C:/XboxGames/Microsoft Flight Simulator 2024/Content/dxgi.dll')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskDependencies = Get-Content -Raw -LiteralPath (Join-Path $taskRoot 'dependencies.json') | ConvertFrom-Json
if (-not $Compiler) { $Compiler = Join-Path $taskRoot ('build/deps/' + $taskDependencies.'llvm-mingw'.directory + '/bin/clang++.exe') }
$taskInclude = Join-Path $taskRoot ('build/deps/' + $taskDependencies.reshade.directory + '/include')
$taskOutput = Join-Path $taskRoot 'build/pfd-stamp-validation'
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskCommon = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-mno-avx', '-mno-avx2', '-mno-avx512f', '-fms-extensions', '-isystem', $taskInclude)
$taskSources = @('validation/pfd_stamp_main.cpp', 'src/pfd_stamp_state.cpp', 'src/pfd_stamp_d3d12.cpp', 'engine-hook/pfd_state_observer.cpp') | ForEach-Object { Join-Path $taskRoot $_ }
$taskLibraries = @('-static', '-municode', '-ld3d12', '-ldxgi', '-ldxguid', '-ld3dcompiler')
$taskExecutable = Join-Path $taskOutput 'pfd-stamp-validation.exe'
& $Compiler @taskCommon @taskSources @taskLibraries '-o' $taskExecutable
if ($LASTEXITCODE -ne 0) { throw 'PFD stamp strict compilation failed.' }
$taskResults = [ordered]@{ standaloneBinarySha256 = (Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash }
$taskObserverExecutable = Join-Path $taskOutput 'pfd-state-observer-test.exe'
& $Compiler @taskCommon '-DTAXI_PFD_STATE_OBSERVER_VALIDATION' (Join-Path $taskRoot 'validation/pfd_state_observer_test.cpp') `
  (Join-Path $taskRoot 'engine-hook/pfd_state_observer.cpp') '-static' '-o' $taskObserverExecutable
if ($LASTEXITCODE -ne 0) { throw 'PFD state observer strict compilation failed.' }
$taskObserverResult = & $taskObserverExecutable
if ($LASTEXITCODE -ne 0) { throw 'PFD state observer lifecycle/refusal tests failed.' }
$taskObserverResult | Write-Output
$taskResults.observer = $taskObserverResult | ConvertFrom-Json
$taskObserverOptional = & $taskObserverExecutable '--optional'
if ($LASTEXITCODE -ne 0) { throw 'PFD state observer optional callback tests failed.' }
$taskResults.observerOptional = $taskObserverOptional | ConvertFrom-Json
$taskResults.observerBinarySha256 = (Get-FileHash -LiteralPath $taskObserverExecutable -Algorithm SHA256).Hash
foreach ($taskDepth in @($false, $true)) {
  foreach ($taskWarp in @($false, $true)) {
    $taskArgs = @()
    if ($taskWarp) { $taskArgs += '--warp' }
    if ($taskDepth) { $taskArgs += '--depth-stencil' }
    $taskResult = & $taskExecutable @taskArgs
    if ($LASTEXITCODE -ne 0) { throw 'PFD stamp GPU validation failed.' }
    $taskResult | Write-Output
    $taskValue = $taskResult | ConvertFrom-Json
    if (-not $taskValue.passed -or ($taskDepth -and (-not $taskValue.boundDepthStencil -or $taskValue.unchangedDepthStencilBytes -eq 0))) {
      throw 'PFD depth/stencil preservation evidence is missing.'
    }
    $taskKey = if ($taskWarp) { 'warp' } else { 'hardware' }
    if ($taskDepth) { $taskKey = 'depthStencil-' + $taskKey }
    $taskResults[$taskKey] = $taskValue
  }
}
if ($ReShadeDll) {
  $taskDll = Get-Item -LiteralPath $ReShadeDll
  if ($taskDll.VersionInfo.ProductName -notmatch 'ReShade' -or $taskDll.VersionInfo.FileVersion -notmatch '^6\.8\.0\.') {
    throw 'Real adapter validation requires the existing pinned ReShade6.8.0 DLL.'
  }
  $taskRealDirectory = Join-Path $taskOutput 'real-reshade'
  New-Item -ItemType Directory -Path $taskRealDirectory -Force | Out-Null
  $taskRealExecutable = Join-Path $taskRealDirectory 'pfd-real-adapter-validation.exe'
  $taskAdapterSources = @('src/pfd_state_adapter.cpp', 'src/scene_capture_manager.cpp', 'src/scene_source_state.cpp', 'src/scene_capture_d3d12.cpp', 'src/scene_handoff.cpp', 'engine-hook/queue_submit_observer.cpp', 'engine-hook/render_boundary_observer.cpp') |
    ForEach-Object { Join-Path $taskRoot $_ }
  & $Compiler @taskCommon '-DTAXI_PFD_REAL_ADAPTER' @taskSources @taskAdapterSources @taskLibraries '-o' $taskRealExecutable
  if ($LASTEXITCODE -ne 0) { throw 'Actual ReShade PFD adapter host compilation failed.' }
  $taskResults.realAdapterBinarySha256 = (Get-FileHash -LiteralPath $taskRealExecutable -Algorithm SHA256).Hash
  Copy-Item -LiteralPath $taskDll.FullName -Destination (Join-Path $taskRealDirectory 'dxgi.dll') -Force
  foreach ($taskDepth in @($false, $true)) {
    foreach ($taskWarp in @($false, $true)) {
      $taskArgs = @()
      if ($taskWarp) { $taskArgs += '--warp' }
      if ($taskDepth) { $taskArgs += '--depth-stencil' }
      $taskLines = @(& $taskRealExecutable @taskArgs)
      if ($LASTEXITCODE -ne 0) { throw 'Actual ReShade PFD adapter GPU validation failed.' }
      $taskLines | Write-Output
      $taskValues = @($taskLines | ForEach-Object { $_ | ConvertFrom-Json })
      if ($taskValues.Count -ne 2 -or -not $taskValues[0].realReShadeAdapter -or $taskValues[0].stamped -ne 4 -or $taskValues[0].undefinedTableStamps -ne 4 -or -not $taskValues[1].passed) {
        throw 'Actual ReShade adapter evidence is missing.'
      }
      if ($taskDepth -and (-not $taskValues[1].boundDepthStencil -or $taskValues[1].unchangedDepthStencilBytes -eq 0)) {
        throw 'Actual ReShade depth/stencil preservation evidence is missing.'
      }
      $taskLog = Get-Content -Raw -LiteralPath (Join-Path $taskRealDirectory 'ReShade.log')
      if ($taskLog -notmatch 'Registered add-on "pfd-real-adapter-validation"' -or $taskLog -match '\| ERROR\s*\|') {
        throw 'Actual ReShade registration/error log check failed.'
      }
      $taskKey = if ($taskWarp) { 'realReShadeWarp' } else { 'realReShadeHardware' }
      if ($taskDepth) { $taskKey = 'depthStencil-' + $taskKey }
      $taskResults[$taskKey] = [ordered]@{ adapter = $taskValues[0]; pixels = $taskValues[1] }
    }
  }
  $taskResults.reshadeSha256 = (Get-FileHash -LiteralPath $taskDll.FullName -Algorithm SHA256).Hash
}
$taskResults.limitation = 'Generated GPU camera inputs in isolated hosts; no simulator scene access. Debug-layer availability is reported per run. The caller must validate selected RTV scope and serialize owned buffer producers and all replayable consumers.'
$taskResults | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $taskOutput 'result.json') -Encoding utf8
