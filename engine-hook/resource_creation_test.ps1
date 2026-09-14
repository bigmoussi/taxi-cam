[CmdletBinding()]
param([string]$Compiler)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$nativeRoot = Split-Path -Parent $PSScriptRoot
if (-not $Compiler) {
    $deps = Get-Content -Raw -LiteralPath (Join-Path $nativeRoot 'dependencies.json') | ConvertFrom-Json
    $Compiler = Join-Path $nativeRoot ('build/deps/' + $deps.'llvm-mingw'.directory + '/bin/clang++.exe')
}
$outputDirectory = Join-Path $nativeRoot 'build/resource-creation-validation'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$exe = Join-Path $outputDirectory 'resource-creation-observer-test.exe'
& $Compiler '-std=c++20' '-O2' '-Wall' '-Wextra' '-Werror' '-fno-exceptions' '-fno-rtti' '-static' '-mno-avx' '-mno-avx2' '-mno-avx512f' `
    (Join-Path $PSScriptRoot 'resource_creation_observer_test.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Resource creation observer focused compilation failed.' }
$line = & $exe
if ($LASTEXITCODE -ne 0) { throw 'Resource creation observer focused tests failed.' }
$line | Write-Output
$result = $line | ConvertFrom-Json
if (-not $result.passed -or $result.signatures -ne 10 -or $result.exactForwards -ne 12 -or -not $result.nestedScopes) {
    throw 'Creation scope/signature evidence is incomplete.'
}
[ordered]@{
    binarySha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    focused = $result
    limitation = 'Exact local wrapper/context tests; actual vtable registration and GPU initial states are covered separately by the real ReShade source-glue host.'
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputDirectory 'result.json') -Encoding utf8
