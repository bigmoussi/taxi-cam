$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'tests/support/installer_fixture.ps1')
. (Join-Path $repoRoot 'installer/settings.ps1')
$fixture = Join-Path $repoRoot ('build/installer-settings-tests/' + [Guid]::NewGuid().ToString('N'))
$local = Join-Path $fixture 'Local'
$sim = Join-Path $fixture 'sim'
$app = Join-Path $fixture 'app'
$payload = Join-Path $fixture 'payload'
$xml = Join-Path $fixture 'exe.xml'
$checks = 0
function Assert-SettingsTest([bool]$Condition, [string]$Message) {
    $script:checks++
    if (-not $Condition) { throw $Message }
}
$otherCompanion = $false
function Get-Process {
    param([string[]]$Name, $ErrorAction)
    if ($otherCompanion -and 'taxi-cam' -in $Name) { [pscustomobject]@{Path='C:\Other Installation\taxi-cam.exe'} }
}
function Seed-Settings {
    $map = @{}
    foreach ($target in @(Get-TaxiSettingsTargets -Installation $app -IncludeMount)) {
        Assert-SettingsTest ($target.path.StartsWith($fixture + '\', [StringComparison]::OrdinalIgnoreCase)) 'Test attempted to leave its fixture.'
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target.path) | Out-Null
        [IO.File]::WriteAllText($target.path, ('saved fixture: ' + $target.path))
        $map[$target.path] = (Get-FileHash -LiteralPath $target.path).Hash
    }
    return $map
}
function Assert-Saved($Expected) {
    foreach ($path in $Expected.Keys) {
        Assert-SettingsTest ((Test-Path -LiteralPath $path -PathType Leaf) -and (Get-FileHash -LiteralPath $path).Hash -eq $Expected[$path]) "Saved file changed: $path"
    }
}
function Invoke-Install([switch]$Reset) {
    & (Join-Path $repoRoot 'installer/install.ps1') -SimulatorDirectory $sim -ExeXml $xml -Destination $app -PayloadDirectory $payload -NoShortcut -ResetSettings:$Reset
}
function Invoke-Runtime([string]$Mode, [string]$State, [switch]$Reset) {
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$hostScript,'-Runtime',(Join-Path $repoRoot 'installer/runtime.ps1'),'-Mode',$Mode,'-Destination',$app,'-SimulatorDirectory',$sim,'-ExeXml',$xml,'-PayloadDirectory',$payload,'-StateDirectory',$State)
    if ($Reset) { $arguments += '-ResetSettings' }
    & (Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe') @arguments
    return $LASTEXITCODE
}
$priorLocal = $env:LOCALAPPDATA
$priorTemp = $env:TEMP
$priorTmp = $env:TMP
try {
    foreach ($directory in @($local,$sim,$app,$payload,(Join-Path $fixture 'temp'))) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
    $env:LOCALAPPDATA = $local
    $env:TEMP = $env:TMP = Join-Path $fixture 'temp'
    New-TaxiFixtureImage (Join-Path $sim 'FlightSimulator2024.exe') $false
    New-TaxiFixtureImage (Join-Path $sim 'SimConnect_internal.dll')
    [IO.File]::WriteAllText((Join-Path $sim 'taxi-camera-mounts.cfg'),'stale simulator calibration must not be imported on reset')
    [IO.File]::WriteAllText($xml,'<SimBase.Document Type="Launch"><Launch.Addon><Name>Keep Me</Name><Path>C:\Other.exe</Path></Launch.Addon></SimBase.Document>')
    foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll','validation.json')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "build/native/$name") -Destination $payload
    }
    foreach ($name in @('install.ps1','exe_xml.ps1','validation_receipt.ps1','prerequisites.ps1','settings.ps1')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "installer/$name") -Destination $payload
    }
    Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $payload 'LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $repoRoot 'licenses/native-runtime-notices.txt') -Destination (Join-Path $payload 'THIRD_PARTY_NOTICES.txt')
    Copy-Item -LiteralPath (Join-Path $repoRoot 'taxi-camera-mounts.cfg') -Destination $payload
    $defaultHash = (Get-FileHash -LiteralPath (Join-Path $payload 'taxi-camera-mounts.cfg')).Hash
    $saved = Seed-Settings
    $unrelated = @{}
    foreach ($path in @((Join-Path $local 'Taxi Cam/logs/keep.log'),(Join-Path $local 'Taxi Cam/history/old.ini'),(Join-Path $local 'Taxi Cam/profiles/custom-aircraft.ini'),(Join-Path $local '380 Taxi Cam/profiles/unknown.ini'))) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
        [IO.File]::WriteAllText($path,'unrelated fixture')
        $unrelated[$path] = (Get-FileHash -LiteralPath $path).Hash
    }
    Invoke-Install
    Assert-Saved $saved
    Assert-Saved $unrelated

    $otherCompanion = $true
    $refused = $false
    try { Invoke-Install -Reset } catch { $refused = $_.Exception.Message -like 'Exit every Taxi Cam*' }
    Assert-SettingsTest $refused 'Reset failed to refuse a companion from another installation.'
    Assert-Saved $saved
    $otherCompanion = $false

    $bundledMount = Join-Path $payload 'taxi-camera-mounts.cfg'
    $heldMount = $bundledMount + '.held'
    Move-Item -LiteralPath $bundledMount -Destination $heldMount
    $refused = $false
    try { Invoke-Install -Reset } catch { $refused = $_.Exception.Message -like 'Bundled default camera mounts*' }
    finally { Move-Item -LiteralPath $heldMount -Destination $bundledMount }
    Assert-SettingsTest $refused 'Reset silently imported simulator calibration when bundled defaults were missing.'
    Assert-Saved $saved

    $xmlLock = [IO.File]::Open($xml, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $refused = $false
    try { Invoke-Install -Reset } catch { $refused = $true } finally { $xmlLock.Dispose() }
    Assert-SettingsTest $refused 'Locked startup XML did not fail reset installation.'
    Assert-Saved $saved
    Assert-Saved $unrelated

    Invoke-Install -Reset
    foreach ($target in @(Get-TaxiSettingsTargets -Installation $app)) {
        Assert-SettingsTest (-not (Test-Path -LiteralPath $target.path)) 'Explicit reset retained a known current or legacy setting.'
    }
    Assert-SettingsTest ((Get-FileHash -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg')).Hash -eq $defaultHash) 'Reset did not install bundled camera defaults.'
    Invoke-Install
    Assert-SettingsTest ((Get-FileHash -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg')).Hash -eq $defaultHash) 'A subsequent keep install reimported stale simulator calibration.'
    Assert-Saved $unrelated

    $saved = Seed-Settings
    $otherCompanion = $true
    $refused = $false
    try { & (Join-Path $repoRoot 'installer/uninstall.ps1') -Installation $app -RemoveSettings }
    catch { $refused = $_.Exception.Message -like 'Exit every Taxi Cam*' }
    Assert-SettingsTest $refused 'Uninstall removal accepted another running installation.'
    $otherCompanion = $false
    $xmlBefore = (Get-FileHash -LiteralPath $xml).Hash
    $settingsLock = [IO.File]::Open((Join-Path $local 'Taxi Cam/hotkeys.ini'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $refused = $false
    try { & (Join-Path $repoRoot 'installer/uninstall.ps1') -Installation $app -RemoveSettings }
    catch { $refused = $true } finally { $settingsLock.Dispose() }
    Assert-SettingsTest $refused 'Locked settings did not fail uninstall removal.'
    Assert-Saved $saved
    Assert-SettingsTest ((Get-FileHash -LiteralPath $xml).Hash -eq $xmlBefore) 'Failed settings removal did not restore simulator startup.'
    & (Join-Path $repoRoot 'installer/uninstall.ps1') -Installation $app
    Assert-Saved $saved
    & (Join-Path $repoRoot 'installer/uninstall.ps1') -Installation $app -RemoveSettings
    foreach ($target in @(Get-TaxiSettingsTargets -Installation $app -IncludeMount)) {
        Assert-SettingsTest (-not (Test-Path -LiteralPath $target.path)) 'Explicit uninstall removal retained a known saved setting.'
    }
    Invoke-Install
    Assert-SettingsTest ((Get-FileHash -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg')).Hash -eq $defaultHash) 'Default reinstall into a recorded installation resurrected old simulator calibration.'
    foreach ($target in @(Get-TaxiSettingsTargets -Installation $app)) {
        Assert-SettingsTest (-not (Test-Path -LiteralPath $target.path)) 'Default reinstall resurrected known legacy profile settings.'
    }
    Assert-Saved $unrelated

    # Run the real outer transaction in a child Windows PowerShell process.
    # Only the fixture host masks process enumeration; LOCALAPPDATA is inherited
    # from this private directory and no executable is launched or stopped.
    $hostScript = Join-Path $fixture 'runtime-host.ps1'
    @'
param([string]$Runtime,[string]$Mode,[string]$Destination,[string]$SimulatorDirectory,[string]$ExeXml,[string]$PayloadDirectory,[string]$StateDirectory,[switch]$ResetSettings)
function Get-Process { param($Name,$Id,$ErrorAction) }
& $Runtime -Mode $Mode -Destination $Destination -SimulatorDirectory $SimulatorDirectory -ExeXml $ExeXml -PayloadDirectory $PayloadDirectory -StateDirectory $StateDirectory -ResetSettings:$ResetSettings
exit $LASTEXITCODE
'@ | Set-Content -LiteralPath $hostScript -Encoding utf8
    $saved = Seed-Settings
    $state = Join-Path $fixture 'outer-reset'
    $result = @(Invoke-Runtime Install $state -Reset)
    Assert-SettingsTest ($result[-1] -eq 0) 'Outer reset installation failed.'
    $result = @(Invoke-Runtime Rollback $state)
    Assert-SettingsTest ($result[-1] -eq 0) 'Outer Setup rollback failed.'
    Assert-Saved $saved
    Assert-Saved $unrelated

    $state = Join-Path $fixture 'outer-conflict'
    $result = @(Invoke-Runtime Install $state -Reset)
    Assert-SettingsTest ($result[-1] -eq 0) 'Outer conflict fixture install failed.'
    $concurrent = Join-Path $local 'Taxi Cam/profiles/ini-a380.ini'
    [IO.File]::WriteAllText($concurrent,'newer concurrent settings')
    $result = @(Invoke-Runtime Rollback $state)
    Assert-SettingsTest ($result[-1] -ne 0 -and [IO.File]::ReadAllText($concurrent) -eq 'newer concurrent settings') 'Outer rollback overwrote a newer settings writer.'
    Assert-Saved $unrelated

    $refused = $false
    try { Assert-TaxiSettingsPath (Join-Path $fixture 'outside.ini') (Join-Path $local 'Taxi Cam') } catch { $refused = $true }
    Assert-SettingsTest $refused 'Settings path bounds accepted an outside file.'
    $unsafeLocal = Join-Path $fixture 'unsafe-local'
    $unknownTarget = Join-Path $fixture 'junction-target'
    New-Item -ItemType Directory -Force -Path (Join-Path $unsafeLocal 'Taxi Cam'),$unknownTarget | Out-Null
    [IO.File]::WriteAllText((Join-Path $unknownTarget 'ini-a380.ini'),'outside junction target')
    New-Item -ItemType Junction -Path (Join-Path $unsafeLocal 'Taxi Cam/profiles') -Target $unknownTarget | Out-Null
    $env:LOCALAPPDATA = $unsafeLocal
    $refused = $false
    try { [void]@(Get-TaxiSettingsTargets -Installation $app) } catch { $refused = $_.Exception.Message -like '*reparse point*' }
    Assert-SettingsTest $refused 'A junction in the settings path was followed.'
    Assert-SettingsTest ([IO.File]::ReadAllText((Join-Path $unknownTarget 'ini-a380.ini')) -eq 'outside junction target') 'Reparse refusal changed its target.'
    $env:LOCALAPPDATA = Join-Path $fixture 'directory-local'
    New-Item -ItemType Directory -Force -Path (Join-Path $env:LOCALAPPDATA 'Taxi Cam/settings.ini') | Out-Null
    $refused = $false
    try { [void]@(Get-TaxiSettingsTargets -Installation $app) } catch { $refused = $_.Exception.Message -like '*file is a directory*' }
    Assert-SettingsTest $refused 'A directory was accepted as a known settings file.'
    Write-Output "PASS settings lifecycle: $checks checks; keep defaults, reset defaults, inner/outer rollback, concurrent writer, legacy imports, explicit uninstall removal, unrelated files and path guards. Fixture: $fixture"
} finally {
    $env:LOCALAPPDATA = $priorLocal
    $env:TEMP = $priorTemp
    $env:TMP = $priorTmp
}
