[CmdletBinding()]
param([string] $Compiler, [string] $OutputDirectory)

$ErrorActionPreference = 'Stop'
$nativeRoot = Split-Path -Parent $PSScriptRoot
if (-not $Compiler) {
    $Compiler = Join-Path $nativeRoot 'build/deps/llvm-mingw/llvm-mingw-20260908-ucrt-x86_64/bin/clang++.exe'
}
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot 'build/call-abi' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$source = Join-Path $PSScriptRoot 'call_abi.cpp'
$targets = @(
    @{ Name = 'gnu'; Target = 'x86_64-w64-windows-gnu' }
    @{ Name = 'msvc'; Target = 'x86_64-pc-windows-msvc' }
)
foreach ($target in $targets) {
    & $Compiler -target $target.Target -S -O1 -std=c++20 -Wall -Wextra -Werror -fno-exceptions -ffreestanding `
        -mno-avx -mno-avx2 -mno-avx512f -nostdinc++ $source -o (Join-Path $OutputDirectory ($target.Name + '.s'))
    if ($LASTEXITCODE -ne 0) { throw "Native primitive ABI probe failed to compile for $($target.Target)." }
}

function Get-FunctionAssembly([string] $Assembly, [string] $Name) {
    $pattern = '(?ms)^' + [regex]::Escape($Name) + ':.*?(?=^\s*# -- End function|^\s*\.def\s|\z)'
    $match = [regex]::Match($Assembly, $pattern)
    if (-not $match.Success) { throw "Missing native call ABI function: $Name" }
    # Preserve instructions, return registers, stack slots and unwind directives.
    # Only comments and surrounding whitespace are semantically irrelevant.
    return (($match.Value -split '\r?\n' | ForEach-Object { ($_ -replace '#.*$', '').Trim() } |
                Where-Object { $_ }) -join "`n")
}

$gnu = Get-Content -Raw -LiteralPath (Join-Path $OutputDirectory 'gnu.s')
$msvc = Get-Content -Raw -LiteralPath (Join-Path $OutputDirectory 'msvc.s')
$names = @(
    'initialize', 'create', 'erase', 'activate', 'activate_true', 'vector', 'scalar', 'position',
    'position_checked', 'orientation', 'quaternion_rotate', 'view_update', 'view_output', 'view_output_checked'
)
foreach ($name in $names) {
    $symbol = 'call_abi_' + $name
    if ((Get-FunctionAssembly $gnu $symbol) -cne (Get-FunctionAssembly $msvc $symbol)) {
        throw "Native primitive call ABI mismatch for $symbol; inspect generated assembly before integration."
    }
}
$negative = 'call_abi_incompatible_return'
if ((Get-FunctionAssembly $gnu $negative) -ceq (Get-FunctionAssembly $msvc $negative)) {
    throw 'The intentionally incompatible aggregate-return control did not differ; the ABI comparison is not trustworthy.'
}

$result = [ordered]@{
    passed = $true
    primitiveSignatures = $names
    negativeControl = $negative
    compiler = (& $Compiler --version | Select-Object -First 1)
    limitation = 'Compile-only agreement for declared primitive/pointer calls; does not prove recovered signatures, private ABI safety, engine lifetime or thread phase.'
}
$result | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
Write-Output "Native call ABI gate passed: $($names.Count) primitive/pointer probes match; incompatible aggregate control differs. No engine calls."
