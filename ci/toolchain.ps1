function Get-TaxiToolchain([string]$Repository) {
    $dependency = (Get-Content -Raw -LiteralPath (Join-Path $Repository 'dependencies.json') | ConvertFrom-Json).'llvm-mingw'
    $directory = Join-Path $Repository ('build/deps/' + $dependency.directory + '/bin')
    if (-not (Test-Path -LiteralPath (Join-Path $directory 'clang++.exe') -PathType Leaf)) {
        throw 'Pinned compiler missing. Run ./bootstrap.ps1 first.'
    }
    return $directory
}

function Write-TaxiLlvmConfig([string]$Repository, [string]$OutputDirectory) {
    $libraries = @(Get-ChildItem -LiteralPath (Get-TaxiToolchain $Repository) -Filter 'libLLVM-*.dll' -File)
    if ($libraries.Count -ne 1) { throw 'Expected one LLVM library in the pinned toolchain.' }
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $library = $libraries[0].FullName.Replace('\', '/').Replace('"', '\"')
    @('#pragma once', ('#define TAXI_LLVM_LIBRARY_PATH L"' + $library + '"')) |
        Set-Content -LiteralPath (Join-Path $OutputDirectory 'llvm_library.hpp') -Encoding utf8
}
