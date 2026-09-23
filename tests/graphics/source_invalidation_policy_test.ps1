[CmdletBinding()]
param([string]$Compiler)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$dependencies = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'dependencies.json') | ConvertFrom-Json
if (-not $Compiler) { $Compiler = Join-Path $repoRoot ('build/deps/' + $dependencies.'llvm-mingw'.directory + '/bin/clang++.exe') }
$output = Join-Path $repoRoot 'build/source-invalidation-policy'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$executable = Join-Path $output 'source-invalidation-policy-test.exe'
& $Compiler '-std=c++20' '-O2' '-Wall' '-Wextra' '-Werror' '-mno-avx' '-mno-avx2' '-mno-avx512f' `
  (Join-Path $repoRoot 'tests/graphics/source_invalidation_policy_test.cpp') '-static' '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'Source invalidation policy strict compilation failed.' }
$result = & $executable
if ($LASTEXITCODE -ne 0) { throw 'Source invalidation policy exhaustive test failed.' }
$decoded = $result | ConvertFrom-Json
if ($decoded.checked -ne 256 -or -not $decoded.passStateExactTarget -or -not $decoded.strongReasonsConservative) {
  throw 'Source invalidation policy result was incomplete.'
}
$result | Write-Output
