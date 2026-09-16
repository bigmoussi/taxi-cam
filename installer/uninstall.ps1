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
$startupPaths = if ($record.PSObject.Properties['startupPaths']) { @($record.startupPaths) }
    elseif ($record.exeXml) { @($record.exeXml) } else { @() }
$xmlWrites = @()
$legacyRestored = $false
$restoredLegacyHash = ''
if ($RemoveSettings) {
    Assert-TaxiSettingsClosed
    [void]@(Get-TaxiSettingsTargets -Installation $Installation -IncludeMount)
    $settingsBackup = Join-Path ([IO.Path]::GetTempPath()) ('taxi-cam-settings-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $settingsBackup | Out-Null
    $settingsSnapshot = @(New-TaxiSettingsSnapshot -Installation $Installation -BackupDirectory $settingsBackup -IncludeMount)
}
try {
foreach ($startupPath in $startupPaths) {
    if (-not (Test-Path -LiteralPath $startupPath -PathType Leaf)) { continue }
    $hash = (Get-FileHash -LiteralPath $startupPath).Hash
    $document = Read-TaxiLaunchXml $startupPath
    $entries = @($document.DocumentElement.SelectNodes('Launch.Addon') | Where-Object {
        $name = $_.SelectSingleNode('Name'); $entryPath = $_.SelectSingleNode('Path')
        $null -ne $name -and $name.InnerText -in @('Taxi Cam','380 Taxi Cam') -and
        $null -ne $entryPath -and $entryPath.InnerText -in @((Join-Path $record.destination 'taxi-cam.exe'), (Join-Path $record.destination '380-taxi-cam.exe'))
    })
    if (-not $entries.Count) { continue }
    # Remove only entries still pointing to this installation, including the former name.
    foreach ($entry in $entries) { [void]$document.DocumentElement.RemoveChild($entry) }
    $xmlWrittenHash = ''
    $backup = Save-TaxiLaunchXml $document $startupPath $hash ([ref]$xmlWrittenHash)
    $xmlWrites += [pscustomobject]@{path=$startupPath;backup=$backup;priorHash=$hash;installedHash=$xmlWrittenHash}
}
if ($RemoveSettings) { Remove-TaxiSettingsSnapshot $settingsSnapshot }
if ($RestoreLegacy -and $record.legacyBackup) {
    $legacy = Join-Path (Split-Path -Parent $record.simulator) 'taxi-camera-native.addon64'
    if (Test-Path -LiteralPath $legacy) { throw 'An active legacy add-on already exists; it was not overwritten.' }
    if (-not (Test-Path -LiteralPath $record.legacyBackup -PathType Leaf)) { throw 'The recorded legacy backup is missing.' }
    $restoredLegacyHash = (Get-FileHash -LiteralPath $record.legacyBackup).Hash
    Move-Item -LiteralPath $record.legacyBackup -Destination $legacy
    $legacyRestored = $true
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
    $rollbackErrors = @()
    if ($RemoveSettings) {
        try { Restore-TaxiSettingsSnapshot $settingsSnapshot }
        catch { $rollbackErrors += $_.Exception.Message }
    }
    if ($legacyRestored) {
        try {
            if (-not (Test-Path -LiteralPath $legacy -PathType Leaf) -or
                (Get-FileHash -LiteralPath $legacy).Hash -ne $restoredLegacyHash -or
                (Test-Path -LiteralPath $record.legacyBackup)) { throw 'Restored legacy add-on changed during uninstall; the newer state was preserved.' }
            Move-Item -LiteralPath $legacy -Destination $record.legacyBackup
        } catch { $rollbackErrors += $_.Exception.Message }
    }
    # Every successful XML mutation is tracked immediately with its prepared
    # digest. A later XML/settings failure must restore all earlier owned writes.
    for ($index = $xmlWrites.Count - 1; $index -ge 0; $index--) {
        $write = $xmlWrites[$index]
        try {
            if (-not (Test-Path -LiteralPath $write.path -PathType Leaf) -or (Get-FileHash -LiteralPath $write.path).Hash -ne $write.installedHash) {
                throw "Startup file changed during uninstall; newer contents were preserved: $($write.path)"
            }
            if (-not (Test-Path -LiteralPath $write.backup -PathType Leaf) -or (Get-FileHash -LiteralPath $write.backup).Hash -ne $write.priorHash) {
                throw "Startup recovery backup is missing or changed: $($write.backup)"
            }
            Copy-Item -LiteralPath $write.backup -Destination $write.path -Force
        } catch { $rollbackErrors += $_.Exception.Message }
    }
    if ($rollbackErrors.Count) {
        $recoveryPaths = (@($settingsBackup) + @($xmlWrites | ForEach-Object backup) | Where-Object { $_ }) -join ', '
        throw "Uninstall rollback needs attention. Recovery files: $recoveryPaths. $($rollbackErrors -join ' ')"
    }
    $settingsComplete = $true
    throw $failure
} finally {
    if ($settingsComplete -and $settingsBackup) {
        try {
            foreach ($entry in $settingsSnapshot) { if (Test-Path -LiteralPath $entry.backup -PathType Leaf) { Remove-Item -LiteralPath $entry.backup } }
            Remove-Item -LiteralPath $settingsBackup
        } catch { Write-Warning "Uninstall settings staging cleanup could not finish: $settingsBackup" }
    }
}
Write-Output "Owned automatic startup entries removed. exe.xml backups: $((@($xmlWrites | ForEach-Object backup)) -join ', ')"
if ($RemoveSettings) { Write-Output 'Known saved settings, legacy profile imports and installed camera calibration were removed. Logs and unknown files were retained.' }
else { Write-Output 'Exit the running companion through its tray menu. Application files, logs and aircraft settings are retained.' }
