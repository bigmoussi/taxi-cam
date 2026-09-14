$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$nativeRoot = Split-Path -Parent $PSScriptRoot
$compiler = Join-Path $nativeRoot 'build/deps/llvm-mingw/llvm-mingw-20260908-ucrt-x86_64/bin/clang++.exe'
$outputDirectory = Join-Path $nativeRoot 'build'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$output = Join-Path $outputDirectory 'scene-source-state-test.exe'
& $compiler -std=c++20 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static -mno-avx -mno-avx2 -mno-avx512f `
    (Join-Path $nativeRoot 'src/scene_source_state.cpp') (Join-Path $nativeRoot 'src/scene_source_state_test.cpp') -o $output
if ($LASTEXITCODE -ne 0) { throw 'Source-state test compilation failed.' }
& $output
if ($LASTEXITCODE -ne 0) { throw 'Source-state tests failed.' }
