param(
  [string]$Compiler = '',
  [switch]$Run,
  [ValidateRange(1, 120)][int]$Seconds = 30,
  [string]$Receipt = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$nativeRoot = Split-Path $PSScriptRoot -Parent
if (-not $Compiler) {
  $Compiler = Join-Path $nativeRoot 'build/deps/llvm-mingw/llvm-mingw-20260908-ucrt-x86_64/bin/clang++.exe'
}
if (-not (Test-Path -LiteralPath $Compiler -PathType Leaf)) {
  throw 'Pinned compiler unavailable; run the native build bootstrap first.'
}
$outputDirectory = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$testExecutable = Join-Path $outputDirectory 'taxi-provider-test.exe'
$liveExecutable = Join-Path $outputDirectory 'taxi-buttons-live.exe'
$flags = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-fms-extensions', '-mno-avx', '-mno-avx2',
  '-DNOMINMAX', '-DTAXI_BODY_POSE_PROVIDER_VALIDATION')
$provider = Join-Path $nativeRoot 'native-camera/body_pose_provider.cpp'
& $Compiler @flags '-DTAXI_BODY_POSE_PROVIDER_TESTING' $provider (Join-Path $nativeRoot 'native-camera/body_pose_provider_test.cpp') '-o' $testExecutable
if ($LASTEXITCODE -ne 0) { throw 'Provider regression build failed.' }
& $testExecutable '--self-test'
if ($LASTEXITCODE -ne 0) { throw 'Provider regression tests failed.' }
& $Compiler @flags $provider (Join-Path $PSScriptRoot 'taxi_buttons_live.cpp') '-o' $liveExecutable
if ($LASTEXITCODE -ne 0) { throw 'Read-only TAXI validator build failed.' }
Write-Output "Built $liveExecutable"
if ($Run) {
  if (-not $Receipt) { $Receipt = Join-Path $outputDirectory 'taxi-buttons-live.jsonl' }
  & $liveExecutable $Seconds | Tee-Object -FilePath $Receipt
  if ($LASTEXITCODE -ne 0) { throw "Live TAXI telemetry unavailable; inspect $Receipt" }
}
