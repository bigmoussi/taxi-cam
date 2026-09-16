[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ReShadeDll,
    [switch]$WarpOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$native = Join-Path $taskRoot 'build/native'
. (Join-Path $taskRoot 'installer/validation_receipt.ps1')
$validated = Assert-TaxiNativeReceipt $native
$source = Get-Item -LiteralPath $ReShadeDll
if ($source.VersionInfo.ProductName -ne 'ReShade') { throw 'Supply an existing trusted ReShade DLL.' }
$sourceHash = (Get-FileHash -LiteralPath $source.FullName -Algorithm SHA256).Hash
$harness = Join-Path $native 'native-graphics-validation.exe'
$harnessHash = (Get-FileHash -LiteralPath $harness -Algorithm SHA256).Hash
$runRoot = Join-Path $taskRoot ('build/reshade-validation/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot | Out-Null
$results = @()
foreach ($adapter in @('hardware', 'warp')) {
    if ($WarpOnly -and $adapter -eq 'hardware') { continue }
    foreach ($scenario in @('textured-gray', 'query-fallback', 'prefer-copy')) {
        # Each process loads ReShade beside the isolated harness, with fresh local
        # settings. Never copy simulator presets/add-ons or touch its installation.
        $directory = Join-Path $runRoot "$adapter-$scenario"
        New-Item -ItemType Directory -Path $directory | Out-Null
        $copiedDll = Join-Path $directory 'dxgi.dll'
        $copiedHarness = Join-Path $directory 'native-graphics-validation.exe'
        Copy-Item -LiteralPath $source.FullName -Destination $copiedDll
        Copy-Item -LiteralPath $harness -Destination $copiedHarness
        $copiedDllHash = (Get-FileHash -LiteralPath $copiedDll -Algorithm SHA256).Hash
        $copiedHarnessHash = (Get-FileHash -LiteralPath $copiedHarness -Algorithm SHA256).Hash
        if ($copiedDllHash -ne $sourceHash) {
            throw 'ReShade copy differs from the supplied binary.'
        }
        if ($copiedHarnessHash -ne $harnessHash) {
            throw 'Graphics harness changed while preparing validation cases.'
        }
        $arguments = @('--require-proxy', "--$scenario")
        if ($scenario -ne 'textured-gray') { $arguments += '--a350' }
        if ($adapter -eq 'warp') { $arguments += '--warp' }
        Push-Location -LiteralPath $directory
        try {
            & ./native-graphics-validation.exe @arguments *> result.log
            $result = $LASTEXITCODE
        } finally { Pop-Location }
        Get-Content -LiteralPath (Join-Path $directory 'result.log') | Select-Object -Last 3
        if ($result -ne 0) { throw "ReShade validation failed: $adapter / $scenario; see $directory" }
        if ((Get-FileHash -LiteralPath $copiedDll -Algorithm SHA256).Hash -ne $copiedDllHash -or
            (Get-FileHash -LiteralPath $copiedHarness -Algorithm SHA256).Hash -ne $copiedHarnessHash) {
            throw "Validation binaries changed during execution: $directory"
        }
        $results += [ordered]@{
            adapter=$adapter; scenario=$scenario; passed=$true; directory=$directory
            reshadeSha256=$copiedDllHash; graphicsHarnessSha256=$copiedHarnessHash
        }
    }
}
[ordered]@{
    passed=$true
    scope='Isolated GPU fixtures; not live simulator or overlay validation'
    reshadeVersion=$source.VersionInfo.FileVersion
    reshadeSha256=$sourceHash
    # Every executed copy matched these fixed hashes; a concurrent rebuild must
    # never replace this receipt's identity with a different source executable.
    graphicsHarnessSha256=$harnessHash
    nativeFiles=$validated.files
    cases=$results
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runRoot 'validation.json') -Encoding utf8
Write-Output "ReShade validation passed: $runRoot"
