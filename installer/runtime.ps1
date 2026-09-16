[CmdletBinding()]
param(
    [ValidateSet('Discover','Install','Rollback','CheckClosed','Uninstall')][string]$Mode,
    [Parameter(Mandatory=$true)][string]$Destination,
    [string]$SimulatorDirectory, [string]$ExeXml, [string]$PayloadDirectory,
    [Parameter(Mandatory=$true)][string]$StateDirectory,
    [ValidateRange(0,2147483647)][int]$UpdateFromPid = 0,
    [ValidateSet('Automatic','Manual')][string]$StartupMode = 'Automatic',
    [switch]$ResetSettings,
    [switch]$RemoveSettings
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Setup may inherit PSModulePath from PowerShell 7; load the Windows PowerShell
# utility module by its own absolute path for Get-FileHash and JSON operations.
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
. (Join-Path $PSScriptRoot 'settings.ps1')
New-Item -ItemType Directory -Force -Path $StateDirectory | Out-Null
$statePath = Join-Path $StateDirectory 'transaction.json'
$operation = "Starting $Mode"
function Restore-Transaction($TransactionState = $null) {
    if ($null -eq $TransactionState -and -not (Test-Path -LiteralPath $statePath)) { return }
    try {
    $state = if ($null -ne $TransactionState) { $TransactionState } else { Get-Content -Raw -LiteralPath $statePath | ConvertFrom-Json }
    $settingsEntries = @($state.files | Where-Object { $_.owned -and $_.PSObject.Properties['root'] -and $_.root })
    if ($settingsEntries.Count) {
        Assert-TaxiSettingsClosed
        foreach ($entry in $settingsEntries) { Assert-TaxiSettingsPath $entry.path $entry.root }
    }
    $conflicts = @()
    foreach ($entry in $state.files) {
        if (-not $entry.owned) { continue }
        $currentHash = if (Test-Path -LiteralPath $entry.path -PathType Leaf) { (Get-FileHash -LiteralPath $entry.path).Hash } else { '' }
        if ($currentHash -ne $entry.installedHash) { $conflicts += $entry.path; continue }
        if ($entry.existed -and $entry.PSObject.Properties['root'] -and $entry.root) { Set-TaxiSettingsFile $entry $entry.backup }
        elseif ($entry.existed) { Copy-Item -LiteralPath $entry.backup -Destination $entry.path -Force }
        elseif (Test-Path -LiteralPath $entry.path -PathType Leaf) { Remove-Item -LiteralPath $entry.path }
    }
    if ($state.createdLegacyBackup -and (Test-Path -LiteralPath $state.createdLegacyBackup)) {
        if ((Get-FileHash -LiteralPath $state.createdLegacyBackup).Hash -eq $state.createdLegacyHash) { Remove-Item -LiteralPath $state.createdLegacyBackup }
        else { $conflicts += $state.createdLegacyBackup }
    }
    if ($conflicts.Count) {
        throw ('Rollback preserved files changed by another writer: ' + ($conflicts -join ', '))
    }
    Remove-Item -LiteralPath $statePath
    } catch {
        $rollbackError = $_.Exception.Message
        $recovery = Join-Path ([IO.Path]::GetTempPath()) ('taxi-cam-recovery-' + [Guid]::NewGuid().ToString('N'))
        Copy-Item -LiteralPath $StateDirectory -Destination $recovery -Recurse
        $recoveryStatePath = Join-Path $recovery 'transaction.json'
        # The newest ownership can be in memory if writing the journal failed.
        if ($null -ne $TransactionState) { $TransactionState | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $recoveryStatePath -Encoding utf8 }
        $recoveryState = Get-Content -Raw -LiteralPath $recoveryStatePath | ConvertFrom-Json
        $statePrefix = [IO.Path]::GetFullPath($StateDirectory).TrimEnd('\') + '\'
        foreach ($entry in $recoveryState.files) {
            # XML backups stay beside the original, including their EFS protection.
            if ($entry.backup -and [IO.Path]::GetFullPath($entry.backup).StartsWith($statePrefix, [StringComparison]::OrdinalIgnoreCase)) {
                $entry.backup = Join-Path $recovery ([IO.Path]::GetFileName($entry.backup))
            }
        }
        $recoveryState | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $recoveryStatePath -Encoding utf8
        throw ($rollbackError + '. Recovery snapshot: ' + $recovery)
    }
}
try {
    if ($Mode -eq 'Discover') {
        $recordPath = Join-Path $Destination 'installation.json'
        if (Test-Path -LiteralPath $recordPath) {
            $record = Get-Content -Raw -LiteralPath $recordPath | ConvertFrom-Json
            $SimulatorDirectory = Split-Path -Parent $record.simulator
            $ExeXml = $record.exeXml
            if ($record.PSObject.Properties['startupRequested']) { $StartupMode = $record.startupRequested }
        } else {
            $simChoices = @('C:\XboxGames\Microsoft Flight Simulator 2024\Content', 'C:\Program Files (x86)\Steam\steamapps\common\Limitless')
            $found = @($simChoices | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'FlightSimulator2024.exe') })
            if ($found.Count -eq 1) { $SimulatorDirectory = $found[0] }
            $xmlChoices = @((Join-Path $env:LOCALAPPDATA 'Packages/Microsoft.Limitless_8wekyb3d8bbwe/LocalCache/exe.xml'), (Join-Path $env:APPDATA 'Microsoft Flight Simulator 2024/exe.xml'))
            $foundXml = @($xmlChoices | Where-Object { Test-Path -LiteralPath $_ })
            if ($foundXml.Count -eq 1) { $ExeXml = $foundXml[0] }
        }
        @('[Paths]', "Simulator=$SimulatorDirectory", "ExeXml=$ExeXml", "Startup=$($StartupMode.ToLowerInvariant())") | Set-Content -LiteralPath (Join-Path $StateDirectory 'choices.ini') -Encoding Unicode
        exit 0
    }
    if ($Mode -eq 'Rollback') { Restore-Transaction; exit 0 }
    if ($UpdateFromPid) {
        $updater = Get-Process -Id $UpdateFromPid -ErrorAction SilentlyContinue
        if ($updater) {
            if ($updater.Path -notin @((Join-Path $Destination 'taxi-cam.exe'),(Join-Path $Destination '380-taxi-cam.exe'))) { throw 'UPDATEFROMPID does not identify the installed companion.' }
            if (-not $updater.WaitForExit(30000)) { throw 'The companion is still exiting. Close it and retry Setup.' }
        }
    }
    if (Get-Process -Name FlightSimulator2024 -ErrorAction SilentlyContinue) { throw 'Close MSFS 2024 before installing or uninstalling Taxi Cam.' }
    foreach ($process in @(Get-Process -Name taxi-cam,380-taxi-cam -ErrorAction SilentlyContinue)) {
        if ($process.Path -in @((Join-Path $Destination 'taxi-cam.exe'),(Join-Path $Destination '380-taxi-cam.exe'))) { throw 'Exit the taxi camera app from its tray menu, then retry.' }
    }
    if ($ResetSettings -or $RemoveSettings) { Assert-TaxiSettingsClosed }
    if ($Mode -eq 'CheckClosed') { exit 0 }
    if ($Mode -eq 'Uninstall') {
        & (Join-Path $PSScriptRoot 'uninstall.ps1') -Installation $Destination -RemoveSettings:$RemoveSettings
        exit 0
    }
    if (Test-Path -LiteralPath $statePath) { throw 'A previous installation transaction has not finished.' }
    if (-not (Test-Path -LiteralPath (Join-Path $SimulatorDirectory 'FlightSimulator2024.exe') -PathType Leaf)) { throw 'Select the MSFS 2024 Content directory containing FlightSimulator2024.exe.' }
    if ($StartupMode -eq 'Automatic' -and $ExeXml -and [IO.Path]::GetFileName($ExeXml) -ine 'exe.xml') { throw 'Select the simulator launch configuration named exe.xml.' }
    $destFull = [IO.Path]::GetFullPath($Destination).TrimEnd('\')
    $payloadFull = (Resolve-Path -LiteralPath $PayloadDirectory).Path
    # XML is optional. Its native transaction owns a verified sibling backup, avoiding
    # cross-volume/EFS copies into Setup's temporary directory before installation.
    $targets = @((Join-Path $destFull 'installation.json'), (Join-Path $destFull '380-taxi-cam.exe'))
    # Setup can pass an 8.3 temporary path. Directory enumeration expands it,
    # so slicing absolute paths by the original prefix length corrupts targets.
    foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll','taxi-camera-mounts.cfg','LICENSE.txt','THIRD_PARTY_NOTICES.txt','setup-diagnostics.log')) {
        $targets += Join-Path $destFull $name
    }
    $settingsRoots = @{}
    if ($ResetSettings) {
        foreach ($target in @(Get-TaxiSettingsTargets -Installation $destFull -IncludeMount)) {
            $targets += $target.path
            $settingsRoots[$target.path] = $target.root
        }
    }
    $legacy = Join-Path $SimulatorDirectory 'taxi-camera-native.addon64'
    $targets += $legacy
    $snapshot = @(); $index = 0
    foreach ($target in ($targets | Select-Object -Unique)) {
        $backup = Join-Path $StateDirectory ("backup-$index"); $index++
        $existed = Test-Path -LiteralPath $target -PathType Leaf
        $operation = "Back up '$target' to '$backup'"
        if ($existed) { Copy-Item -LiteralPath $target -Destination $backup }
        $settingsRoot = if ($settingsRoots.ContainsKey($target)) { $settingsRoots[$target] } else { '' }
        $snapshot += [pscustomobject]@{path=$target; root=$settingsRoot; backup=$backup; existed=$existed;owned=$false;installedHash=''}
    }
    $state = [ordered]@{files=$snapshot;createdLegacyBackup='';createdLegacyHash=''}
    $state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statePath -Encoding utf8
    $nativeSucceeded = $false
    try {
        $operation = "Install runtime into '$destFull' and configure startup '$ExeXml'"
        $nativeResult = $null
        & (Join-Path $payloadFull 'install.ps1') -SimulatorDirectory $SimulatorDirectory -ExeXml $ExeXml -Destination $destFull -PayloadDirectory $payloadFull -NoShortcut -ResetSettings:$ResetSettings -StartupMode $StartupMode -InstallResult ([ref]$nativeResult)
        $nativeSucceeded = $true
        $installedRecord = $nativeResult.Record
        $state.createdLegacyBackup = $nativeResult.CreatedLegacyBackup
        $state.createdLegacyHash = $nativeResult.CreatedLegacyHash
        foreach ($entry in $snapshot) {
            if ($nativeResult.Writes.ContainsKey($entry.path)) {
                $entry.installedHash = $nativeResult.Writes[$entry.path]
                $entry.owned = $true
            }
        }
        if ($installedRecord.startupUpdated) {
            # Use the hash of the content WE wrote, never adopt a subsequent external edit.
            $snapshot += [pscustomobject]@{path=$installedRecord.exeXml; root=''; backup=$installedRecord.exeXmlBackup;
                existed=[bool]$installedRecord.exeXmlBackup; owned=$true; installedHash=$installedRecord.exeXmlInstalledHash}
            $state.files = $snapshot
        }
        $state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statePath -Encoding utf8
        $warningPath = Join-Path $StateDirectory 'warning.txt'
        if ($installedRecord.startupWarning) {
            [IO.File]::WriteAllText($warningPath, $installedRecord.startupWarning, [Text.UTF8Encoding]::new($false))
        } elseif (Test-Path -LiteralPath $warningPath) { Remove-Item -LiteralPath $warningPath }
        $diagnostics = @("Taxi Cam Setup $($installedRecord.version) build $($installedRecord.buildNumber)",
            "UTC: $([DateTime]::UtcNow.ToString('o'))", "Destination: $destFull", "Startup requested: $StartupMode",
            "Startup status: $($installedRecord.startupStatus)", "Startup file: $($installedRecord.exeXml)",
            $installedRecord.startupWarning, $installedRecord.startupError) -join [Environment]::NewLine
        [IO.File]::WriteAllText((Join-Path $StateDirectory 'setup-diagnostics.log'), $diagnostics, [Text.UTF8Encoding]::new($false))
        foreach ($name in @('setup-diagnostics.log')) {
            $source = Join-Path $StateDirectory $name
            $target = Join-Path $destFull $name
            $operation = "Copy '$source' to '$target'"
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            $entry = @($snapshot | Where-Object { $_.path -eq $target })[0]
            $entry.installedHash = (Get-FileHash -LiteralPath $source).Hash
            $entry.owned = $true
            $state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statePath -Encoding utf8
            $noticeTemp = Join-Path $destFull ('.notice-' + [Guid]::NewGuid().ToString('N'))
            try {
                Copy-Item -LiteralPath $source -Destination $noticeTemp
                if (Test-Path -LiteralPath $target) { [IO.File]::Replace($noticeTemp, $target, [NullString]::Value) }
                else { [IO.File]::Move($noticeTemp, $target) }
            } finally { if (Test-Path -LiteralPath $noticeTemp) { Remove-Item -LiteralPath $noticeTemp } }
        }
    } catch {
        # install.ps1 owns its own failure rollback. Never undo its concurrent XML edit guard.
        if ($nativeSucceeded) { Restore-Transaction $state } else { Remove-Item -LiteralPath $statePath }
        throw
    }
} catch {
    $lines = @("Operation: $operation", $_.Exception.ToString(), $_.InvocationInfo.PositionMessage, $_.ScriptStackTrace)
    $exception = $_.Exception
    while ($exception) {
        $lines += ('{0}: HRESULT 0x{1:X8}' -f $exception.GetType().FullName, $exception.HResult)
        if ($exception -is [ComponentModel.Win32Exception]) { $lines += "Windows error: $($exception.NativeErrorCode)" }
        foreach ($key in @('TaxiStartupOperation','TaxiStartupSource','TaxiStartupDestination')) {
            if ($exception.Data.Contains($key)) { $lines += "${key}: $($exception.Data[$key])" }
        }
        $exception = $exception.InnerException
    }
    $detail = $lines -join [Environment]::NewLine
    [IO.File]::WriteAllText((Join-Path $StateDirectory 'error.txt'), $detail, [Text.UTF8Encoding]::new($false))
    exit 1
}
