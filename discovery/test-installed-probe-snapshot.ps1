# Builds/tests only; never discovers, launches or reads a simulator process.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$nativeRoot = Split-Path -Parent $PSScriptRoot
$compilerRoot = Join-Path $nativeRoot 'build/deps/llvm-mingw/llvm-mingw-20260908-ucrt-x86_64/bin'
$release = Join-Path $nativeRoot 'build/releases/0.7.2/taxi-camera-native.addon64'
$expected = '6A6AF4EF6848458268DF6535FC1C5F44E9040B2D3CBA8A89CB2CF0F01EE60080'
if ((Get-FileHash -LiteralPath $release -Algorithm SHA256).Hash -ne $expected) {
    throw 'Archived 0.7.2 DLL hash does not match the diagnostic profile.'
}
$symbols = & (Join-Path $compilerRoot 'llvm-nm.exe') -C $release
if ($LASTEXITCODE -ne 0 -or -not ($symbols -cmatch '^180096768 d taxi_camera::scene_handoff\(\)::instance$')) {
    throw 'The exact archived DLL does not contain the expected static instance symbol.'
}
$outputDirectory = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$executable = Join-Path $outputDirectory 'installed-probe-snapshot.exe'
& (Join-Path $compilerRoot 'clang++.exe') -std=c++20 -O2 -Wall -Wextra -Werror -fno-access-control `
    -DNOMINMAX -D_WIN32_WINNT=0x0A00 -mno-avx -mno-avx2 -mno-avx512f -static `
    (Join-Path $PSScriptRoot 'installed_probe_snapshot.cpp') -lbcrypt -ladvapi32 -lpsapi -o $executable
if ($LASTEXITCODE -ne 0) { throw 'Snapshot helper compilation failed.' }
$selfText = & $executable --self-test
if ($LASTEXITCODE -ne 0) { throw 'Snapshot synthetic fixtures failed.' }
$self = $selfText | ConvertFrom-Json
if (-not $self.self_test_passed -or $self.publication_size -ne 128 -or $self.object_budget -ne 1024) {
    throw 'Snapshot layout/self-test result did not match the profile.'
}
$wrongText = & $executable --pid $PID
if ($LASTEXITCODE -ne 1) { throw 'Snapshot helper did not refuse the PowerShell host.' }
$wrong = $wrongText | ConvertFrom-Json
if ($wrong.valid -or $wrong.verified -or $wrong.error -ne 'wrong_process' -or $wrong.object_bytes -ne 0 -or $wrong.pointer_bytes -ne 0) {
    throw 'Wrong-host validation reached target memory or passed.'
}
foreach ($arguments in @(@('--pid','0'), @('--pid','-1'), @('--pid','23068junk'), @('--pid','4294967296'), @('--pid'), @('--all'))) {
    $null = & $executable @arguments
    if ($LASTEXITCODE -ne 2) { throw 'Invalid arguments were accepted.' }
}
$selfText
Write-Output 'PASS: exact archived hash/symbol, synthetic snapshots, wrong host and malformed CLI. No simulator read.'
