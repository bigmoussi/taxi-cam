[CmdletBinding()]
param(
    [string]$Installation = (Join-Path $env:LOCALAPPDATA 'Taxi Cam/app'),
    [switch]$RestoreLegacy
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'standalone/exe_xml.ps1')
$record = Get-Content -Raw -LiteralPath (Join-Path $Installation 'installation.json') | ConvertFrom-Json
if (Get-Process -Name FlightSimulator2024 -ErrorAction SilentlyContinue) { throw 'Close MSFS before disabling or restoring a camera bridge.' }
$document = Read-TaxiLaunchXml $record.exeXml
$hash = (Get-FileHash -LiteralPath $record.exeXml).Hash
Set-TaxiStartupEntry $document '' '' -Remove
$backup = Save-TaxiLaunchXml $document $record.exeXml $hash
if ($RestoreLegacy -and $record.legacyBackup) {
    $legacy = Join-Path (Split-Path -Parent $record.simulator) 'taxi-camera-native.addon64'
    if (Test-Path -LiteralPath $legacy) { throw 'An active legacy add-on already exists; it was not overwritten.' }
    if (-not (Test-Path -LiteralPath $record.legacyBackup -PathType Leaf)) { throw 'The recorded legacy backup is missing.' }
    Move-Item -LiteralPath $record.legacyBackup -Destination $legacy
}
if ($record.shortcut -and (Test-Path -LiteralPath $record.shortcut)) {
    $shell = New-Object -ComObject WScript.Shell
    if ($shell.CreateShortcut($record.shortcut).TargetPath -eq (Join-Path $record.destination 'taxi-cam.exe')) {
        Remove-Item -LiteralPath $record.shortcut
    }
}
Write-Output "Automatic startup removed. exe.xml backup: $backup"
Write-Output 'Exit the running companion through its tray menu. Application files, logs and aircraft settings are retained.'
