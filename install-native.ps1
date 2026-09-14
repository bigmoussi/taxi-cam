[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SimulatorDirectory,
    [string]$ExeXml,
    [string]$Destination = (Join-Path $env:LOCALAPPDATA '380 Taxi Cam\app'),
    [string]$PayloadDirectory,
    [switch]$NoShortcut
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'standalone/exe_xml.ps1')
. (Join-Path $PSScriptRoot 'standalone/validation_receipt.ps1')
if (-not $PayloadDirectory) {
    $PayloadDirectory = if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'build/native/validation.json')) { Join-Path $PSScriptRoot 'build/native' } else { $PSScriptRoot }
}
$payload = (Resolve-Path -LiteralPath $PayloadDirectory).Path
$receipt = Assert-TaxiNativeReceipt $payload
$sim = (Resolve-Path -LiteralPath $SimulatorDirectory).Path
$simExe = Join-Path $sim 'FlightSimulator2024.exe'
if (-not (Test-Path -LiteralPath $simExe -PathType Leaf)) { throw 'Select the MSFS 2024 Content directory containing FlightSimulator2024.exe.' }
if (-not $ExeXml) {
    $choices = @(
        (Join-Path $env:LOCALAPPDATA 'Packages/Microsoft.Limitless_8wekyb3d8bbwe/LocalCache/exe.xml'),
        (Join-Path $env:APPDATA 'Microsoft Flight Simulator 2024/exe.xml')
    )
    $existing = @($choices | Where-Object { Test-Path -LiteralPath $_ })
    if ($existing.Count -ne 1) { throw 'Provide -ExeXml with the simulator launch configuration path.' }
    $ExeXml = $existing[0]
}
$ExeXml = [IO.Path]::GetFullPath($ExeXml)
$dest = [IO.Path]::GetFullPath($Destination)
if ($dest -eq [IO.Path]::GetPathRoot($dest) -or $dest -eq $sim) { throw 'Use a dedicated companion installation directory.' }
$exe = Join-Path $dest '380-taxi-cam.exe'
function Assert-Closed {
    if (Get-Process -Name FlightSimulator2024 -ErrorAction SilentlyContinue) { throw 'Close MSFS before replacing its native camera bridge.' }
    foreach ($p in @(Get-Process -Name 380-taxi-cam -ErrorAction SilentlyContinue)) {
        if ($p.Path -and [string]::Equals($p.Path,$exe,[StringComparison]::OrdinalIgnoreCase)) { throw 'Exit 380 Taxi Cam from its tray menu before updating it.' }
    }
}
Assert-Closed
$hash = if (Test-Path -LiteralPath $ExeXml) { (Get-FileHash -LiteralPath $ExeXml).Hash } else { '' }
$document = Read-TaxiLaunchXml $ExeXml
$globalDisabled = $document.DocumentElement.SelectSingleNode('Disabled')
$globalManual = $document.DocumentElement.SelectSingleNode('Launch.ManualLoad')
if (($globalDisabled -and $globalDisabled.InnerText -ieq 'True') -or
    ($globalManual -and $globalManual.InnerText -ieq 'True')) {
    throw 'The simulator launch document disables automatic startup globally; retain its settings and resolve that explicitly.'
}
Set-TaxiStartupEntry $document $exe $simExe
$tag = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$staging = Join-Path $dest ('staging-' + $tag)
New-Item -ItemType Directory -Path $staging | Out-Null
foreach ($name in @('380-taxi-cam.exe','taxi-camera-bridge.dll')) {
    Copy-Item -LiteralPath (Join-Path $payload $name) -Destination (Join-Path $staging $name)
    if ((Get-FileHash -LiteralPath (Join-Path $staging $name)).Hash -ne $receipt.files.PSObject.Properties[$name].Value) { throw "Staging verification failed: $name" }
}
$prior = @{}
$installed = @()
$legacy = Join-Path $sim 'taxi-camera-native.addon64'
$disabled = $null
$previousLegacy = $null
$previousRecordPath = Join-Path $dest 'installation.json'
if (Test-Path -LiteralPath $previousRecordPath) {
    $previousRecord = Get-Content -Raw -LiteralPath $previousRecordPath | ConvertFrom-Json
    if ($previousRecord.legacyBackup -and (Test-Path -LiteralPath $previousRecord.legacyBackup -PathType Leaf) -and
        (Split-Path -Parent $previousRecord.legacyBackup) -eq $sim -and
        (Split-Path -Leaf $previousRecord.legacyBackup) -like 'taxi-camera-native.addon64.disabled-native-*') {
        $previousLegacy = $previousRecord.legacyBackup
    }
}
$xmlBackup = $null
try {
    Assert-Closed
    foreach ($name in @('380-taxi-cam.exe','taxi-camera-bridge.dll')) {
        $target = Join-Path $dest $name
        if (Test-Path -LiteralPath $target) {
            $backup = $target + '.backup-' + $tag
            Copy-Item -LiteralPath $target -Destination $backup
            $prior[$name] = $backup
        }
        Copy-Item -LiteralPath (Join-Path $staging $name) -Destination $target -Force
        $installed += $name
        if ((Get-FileHash -LiteralPath $target).Hash -ne $receipt.files.PSObject.Properties[$name].Value) { throw "Installed binary verification failed: $name" }
    }
    $mount = Join-Path $dest 'taxi-camera-mounts.cfg'
    if (-not (Test-Path -LiteralPath $mount)) {
        $sourceMount = if (Test-Path -LiteralPath (Join-Path $sim 'taxi-camera-mounts.cfg')) { Join-Path $sim 'taxi-camera-mounts.cfg' }
            elseif (Test-Path -LiteralPath (Join-Path $payload 'taxi-camera-mounts.cfg')) { Join-Path $payload 'taxi-camera-mounts.cfg' }
            else { Join-Path $PSScriptRoot 'taxi-camera-mounts.cfg' }
        Copy-Item -LiteralPath $sourceMount -Destination $mount
    }
    Assert-Closed
    if (Test-Path -LiteralPath $legacy) {
        $disabled = $legacy + '.disabled-native-' + $tag
        Move-Item -LiteralPath $legacy -Destination $disabled
    }
    $xmlBackup = Save-TaxiLaunchXml $document $ExeXml $hash
} catch {
    foreach ($name in $installed) {
        $target = Join-Path $dest $name
        if ($prior.ContainsKey($name)) { Copy-Item -LiteralPath $prior[$name] -Destination $target -Force }
        elseif (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target }
    }
    if ($disabled -and (Test-Path -LiteralPath $disabled) -and -not (Test-Path -LiteralPath $legacy)) {
        Move-Item -LiteralPath $disabled -Destination $legacy
    }
    throw
}
$startMenu = Join-Path $env:APPDATA 'Microsoft/Windows/Start Menu/Programs/380 Taxi Cam.lnk'
try {
    if ($NoShortcut) { $startMenu = $null } else {
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($startMenu); $shortcut.TargetPath=$exe
    $shortcut.WorkingDirectory=$dest; $shortcut.Description='380 Taxi Cam settings'; $shortcut.Save()
    }
} catch { Write-Warning 'Installed successfully, but the Start menu shortcut could not be created.' }
[ordered]@{
    version='0.8.0'; installedUtc=[DateTime]::UtcNow.ToString('o'); destination=$dest;
    simulator=$simExe; exeXml=$ExeXml; exeXmlBackup=$xmlBackup; legacyBackup=$(if ($disabled) { $disabled } else { $previousLegacy });
    files=$receipt.files; shortcut=$startMenu; simulatorVerified=$false
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $dest 'installation.json') -Encoding utf8
Write-Output "Installed 380 Taxi Cam: $exe"
Write-Output "Automatic tray startup: $ExeXml"
Write-Output "Legacy taxi add-on retained: $disabled"
Write-Output 'Existing ReShade files and other startup entries were preserved.'
