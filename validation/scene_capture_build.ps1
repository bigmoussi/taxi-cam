$ErrorActionPreference = 'Stop'
$captureRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$captureBuild = Join-Path $captureRoot 'build'
$captureCompiler = Join-Path $captureBuild 'deps/llvm-mingw/llvm-mingw-20260908-ucrt-x86_64/bin/clang++.exe'
if (-not (Test-Path -LiteralPath $captureCompiler -PathType Leaf)) {
  throw "Pinned compiler is missing: $captureCompiler"
}
$captureObject = Join-Path $captureBuild 'scene-capture.o'
$captureExecutable = Join-Path $captureBuild 'scene-capture-validation.exe'
$captureFlags = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-mno-avx', '-mno-avx2', '-mno-avx512f')
& $captureCompiler @captureFlags -fno-exceptions -c (Join-Path $captureRoot 'src/scene_capture_d3d12.cpp') -o $captureObject
if ($LASTEXITCODE -ne 0) { throw 'Capture production compilation failed.' }
& $captureCompiler @captureFlags -static (Join-Path $PSScriptRoot 'scene_capture_main.cpp') $captureObject -ld3d12 -ldxgi -ldxguid -o $captureExecutable
if ($LASTEXITCODE -ne 0) { throw 'Capture validation compilation failed.' }
foreach ($captureCase in @(
  @{ Adapter = 'hardware'; Format = 'rgba8' }, @{ Adapter = 'warp'; Format = 'rgba8' },
  @{ Adapter = 'hardware'; Format = 'r11g11b10' }, @{ Adapter = 'warp'; Format = 'r11g11b10' }
)) {
  $captureAdapter = $captureCase.Adapter
  $captureArguments = @()
  if ($captureAdapter -eq 'warp') { $captureArguments += '--warp' }
  if ($captureCase.Format -eq 'r11g11b10') { $captureArguments += '--r11g11b10' }
  $captureOutput = & $captureExecutable @captureArguments
  if ($LASTEXITCODE -ne 0) { throw "Capture validation failed on $captureAdapter." }
  $captureResult = $captureOutput | ConvertFrom-Json
  if ($captureResult.passed -ne $true -or $captureResult.checked_pixels -ne 24576 -or
      $captureResult.recordings -ne 4 -or $captureResult.allocations -ne 1 -or
      $captureResult.r11g11b10 -ne ($captureCase.Format -eq 'r11g11b10')) {
    throw "Capture validation receipt is invalid on $captureAdapter."
  }
  $captureName = if ($captureCase.Format -eq 'rgba8') { "scene-capture-$captureAdapter.json" } else { "scene-capture-r11g11b10-$captureAdapter.json" }
  $captureOutput | Set-Content -LiteralPath (Join-Path $captureBuild $captureName) -Encoding utf8
  Write-Output $captureOutput
}
