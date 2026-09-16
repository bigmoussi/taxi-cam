[CmdletBinding()]
param(
    [string]$Installation = (Join-Path $env:LOCALAPPDATA 'Taxi Cam/app'),
    [switch]$RestoreLegacy,
    [switch]$RemoveSettings
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'exe_xml.ps1')
. (Join-Path $PSScriptRoot 'settings.ps1')
$record = Get-Content -Raw -LiteralPath (Join-Path $Installation 'installation.json') | ConvertFrom-Json
if (Get-Process -Name FlightSimulator2024 -ErrorAction SilentlyContinue) { throw 'Close MSFS before disabling or restoring a camera bridge.' }
$settingsSnapshot = @()
$settingsBackup = $null
$settingsComplete = $false
$xmlWrittenHash = ''
$backup = $null
if ($RemoveSettings) {
    Assert-TaxiSettingsClosed
    [void]@(Get-TaxiSettingsTargets -Installation $Installation -IncludeMount)
    $settingsBackup = Join-Path ([IO.Path]::GetTempPath()) ('taxi-cam-settings-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $settingsBackup | Out-Null
    $settingsSnapshot = @(New-TaxiSettingsSnapshot -Installation $Installation -BackupDirectory $settingsBackup -IncludeMount)
}
try {
$document = Read-TaxiLaunchXml $record.exeXml
$hash = (Get-FileHash -LiteralPath $record.exeXml).Hash
Set-TaxiStartupEntry $document '' '' -Remove
$backup = Save-TaxiLaunchXml $document $record.exeXml $hash
if ($RemoveSettings) {
    $xmlWrittenHash = (Get-FileHash -LiteralPath $record.exeXml).Hash
    Remove-TaxiSettingsSnapshot $settingsSnapshot
}
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
$settingsComplete = $true
} catch {
    $failure = $_
    if ($RemoveSettings) {
        try {
            Restore-TaxiSettingsSnapshot $settingsSnapshot
            if ($xmlWrittenHash) {
                if (-not (Test-Path -LiteralPath $record.exeXml -PathType Leaf) -or (Get-FileHash -LiteralPath $record.exeXml).Hash -ne $xmlWrittenHash) {
                    throw 'exe.xml changed during uninstall; the newer contents were preserved.'
                }
                if ($backup) { Copy-Item -LiteralPath $backup -Destination $record.exeXml -Force }
            }
            $settingsComplete = $true
        }
        catch { throw "Uninstall settings rollback needs attention. Recovery files: $settingsBackup. $($_.Exception.Message)" }
    }
    throw $failure
} finally {
    if ($settingsComplete -and $settingsBackup) {
        foreach ($entry in $settingsSnapshot) { if (Test-Path -LiteralPath $entry.backup -PathType Leaf) { Remove-Item -LiteralPath $entry.backup } }
        Remove-Item -LiteralPath $settingsBackup
    }
}
Write-Output "Automatic startup removed. exe.xml backup: $backup"
if ($RemoveSettings) { Write-Output 'Known saved settings, legacy profile imports and installed camera calibration were removed. Logs and unknown files were retained.' }
else { Write-Output 'Exit the running companion through its tray menu. Application files, logs and aircraft settings are retained.' }
