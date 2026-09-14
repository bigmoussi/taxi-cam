$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'ci/toolchain.ps1')
Set-StrictMode -Version Latest
$nativeRoot = $repoRoot
$compiler = Join-Path (Get-TaxiToolchain $repoRoot) 'clang++.exe'
$outputDirectory = Join-Path $nativeRoot 'build'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$output = Join-Path $outputDirectory 'scene-source-state-test.exe'
& $compiler -std=c++20 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static -mno-avx -mno-avx2 -mno-avx512f `
    (Join-Path $nativeRoot 'src/graphics/scene_source_state.cpp') (Join-Path $nativeRoot 'tests/graphics/scene_source_state_test.cpp') -o $output
if ($LASTEXITCODE -ne 0) { throw 'Source-state test compilation failed.' }
& $output
if ($LASTEXITCODE -ne 0) { throw 'Source-state tests failed.' }
