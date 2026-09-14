[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SimulatorDirectory,
    [string]$ExeXml,
    [switch]$LegacyReShade
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $LegacyReShade) {
    & (Join-Path $PSScriptRoot 'install-native.ps1') -SimulatorDirectory $SimulatorDirectory -ExeXml $ExeXml
    return
}
$directory = (Resolve-Path -LiteralPath $SimulatorDirectory).Path
if (-not (Test-Path -LiteralPath (Join-Path $directory 'FlightSimulator2024.exe'))) {
    throw 'The target must be the MSFS 2024 Content directory containing FlightSimulator2024.exe.'
}
$simulatorRunning = $null -ne (Get-Process -Name FlightSimulator2024 -ErrorAction SilentlyContinue)
$buildDirectory = Join-Path $PSScriptRoot 'build'
$source = Join-Path $buildDirectory 'taxi-camera-native.addon64'
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
$smoke = Get-Content -Raw -LiteralPath (Join-Path $buildDirectory 'smoke-result.json') | ConvertFrom-Json
if (-not $smoke.passed -or $smoke.addonSha256 -ne $sourceHash) {
    throw 'Run the isolated smoke test for this exact add-on build before installing.'
}
$reshade = Join-Path $directory 'dxgi.dll'
if ((Get-FileHash -LiteralPath $reshade -Algorithm SHA256).Hash -ne $smoke.reshadeSha256) {
    throw 'The installed ReShade binary differs from the one used in the smoke test.'
}
$destination = Join-Path $directory 'taxi-camera-native.addon64'
if ($simulatorRunning) {
    throw 'Close MSFS and leave it closed until installation finishes; Windows locks the loaded probe DLL.'
}
if (Test-Path -LiteralPath $destination) {
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -eq $sourceHash) {
        Write-Output "Already installed: $destination"
        return
    }
    $backup = $destination + '.backup-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
    Copy-Item -LiteralPath $destination -Destination $backup
    Write-Output "Previous probe preserved: $backup"
}
Copy-Item -LiteralPath $source -Destination $destination -Force
if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $sourceHash) {
    throw 'Installed add-on hash does not match the validated build.'
}
Write-Output "Installed: $destination"
Write-Output 'EFIS TAXI control and automatic A380 PFD detection are enabled. Cameras follow the TAXI buttons; manual diagnostics remain in Taxi Camera Native Probe.'
