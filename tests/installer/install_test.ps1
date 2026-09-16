$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$root = $repoRoot
. (Join-Path $repoRoot 'tests/support/installer_fixture.ps1')
$fixture = Join-Path $root ('build/native/install-validation-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
$sim = Join-Path $fixture 'sim'
$app = Join-Path $fixture 'app'
New-Item -ItemType Directory -Path $sim -Force | Out-Null
New-TaxiFixtureImage (Join-Path $sim 'FlightSimulator2024.exe') $false
New-TaxiFixtureImage (Join-Path $sim 'SimConnect_internal.dll')
[IO.File]::WriteAllText((Join-Path $sim 'taxi-camera-native.addon64'),'legacy fixture')
[IO.File]::WriteAllText((Join-Path $sim 'dxgi.dll'),'unrelated graphics fixture')
Copy-Item -LiteralPath (Join-Path $root 'taxi-camera-mounts.cfg') -Destination (Join-Path $sim 'taxi-camera-mounts.cfg')
$mountHash = (Get-FileHash -LiteralPath (Join-Path $sim 'taxi-camera-mounts.cfg')).Hash
$xml = Join-Path $fixture 'exe.xml'
[IO.File]::WriteAllText($xml,'<SimBase.Document Type="Launch"><Launch.Addon><Name>Keep Me</Name><Path>C:\Other.exe</Path></Launch.Addon></SimBase.Document>')
# Override process enumeration only inside this test script. All writes target the
# fresh fixture above; the user's simulator and companion remain untouched.
$fixtureSimulatorRunning = $true
$fixtureOldCompanionRunning = $false
function Get-Process {
    param([string[]]$Name, $ErrorAction)
    if ($fixtureSimulatorRunning -and 'FlightSimulator2024' -in $Name) {
        [pscustomobject]@{ProcessName='FlightSimulator2024'; Path=(Join-Path $sim 'FlightSimulator2024.exe')}
    }
    if ($fixtureOldCompanionRunning -and '380-taxi-cam' -in $Name) {
        [pscustomobject]@{ProcessName='380-taxi-cam'; Path=(Join-Path $app '380-taxi-cam.exe')}
    }
}
$refused = $false
try {
    & (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $sim -ExeXml $xml -Destination $app -NoShortcut
} catch { $refused = $_.Exception.Message -like 'Close MSFS*' }
if (-not $refused -or (Test-Path -LiteralPath (Join-Path $app 'taxi-cam.exe'))) { throw 'Running simulator installation guard failed.' }
$fixtureSimulatorRunning = $false
$fixtureOldCompanionRunning = $true
$refused = $false
try {
    & (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $sim -ExeXml $xml -Destination $app -NoShortcut
} catch { $refused = $_.Exception.Message -like 'Exit the taxi camera app*' }
if (-not $refused) { throw 'Running former-name companion installation guard failed.' }
$fixtureOldCompanionRunning = $false
# A packaged payload must include both legal files before any destination writes.
$payload = Join-Path $fixture 'payload'
New-Item -ItemType Directory -Path $payload | Out-Null
foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll','validation.json')) {
    Copy-Item -LiteralPath (Join-Path $root "build/native/$name") -Destination $payload
}
Copy-Item -LiteralPath (Join-Path $root 'licenses/native-runtime-notices.txt') -Destination (Join-Path $payload 'THIRD_PARTY_NOTICES.txt')
$refused = $false
try {
    & (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $sim -ExeXml $xml -Destination $app -PayloadDirectory $payload -NoShortcut
} catch { $refused = $_.Exception.Message -eq 'Required runtime legal file is missing: LICENSE.txt' }
if (-not $refused -or (Test-Path -LiteralPath $app)) { throw 'A packaged install without LICENSE.txt was not refused before writes.' }
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $payload 'LICENSE.txt')
& (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $sim -ExeXml $xml -Destination $app -NoShortcut
$record = Get-Content -Raw -LiteralPath (Join-Path $app 'installation.json') | ConvertFrom-Json
[xml]$doc = Get-Content -Raw -LiteralPath $xml
if ($doc.SelectNodes('//Launch.Addon').Count -ne 2 -or -not $doc.SelectSingleNode('//Launch.Addon[Name="Keep Me"]')) { throw 'Installer changed unrelated startup.' }
if ((Get-FileHash -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg')).Hash -ne $mountHash) { throw 'Calibration import changed.' }
if (-not (Test-Path -LiteralPath $record.legacyBackup) -or (Test-Path -LiteralPath (Join-Path $sim 'taxi-camera-native.addon64'))) { throw 'Legacy add-on not retained and disabled.' }
if ([IO.File]::ReadAllText((Join-Path $sim 'dxgi.dll')) -ne 'unrelated graphics fixture') { throw 'Unrelated graphics files changed.' }
foreach ($name in @('LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
    if ((Get-FileHash -LiteralPath (Join-Path $app $name)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $payload $name)).Hash) {
        throw "Repository installation omitted or changed $name."
    }
}
$expectedFiles = @('taxi-cam.exe','taxi-camera-bridge.dll','taxi-camera-mounts.cfg','LICENSE.txt','THIRD_PARTY_NOTICES.txt','installation.json')
if (@(Compare-Object (@(Get-ChildItem -LiteralPath $app -File | ForEach-Object Name) | Sort-Object) ($expectedFiles | Sort-Object)).Count) {
    throw 'Installed runtime inventory differs from the required files and installation record.'
}
$firstLegacyBackup = $record.legacyBackup
# Updates replace bundled legal text, and a failed transaction must restore it.
foreach ($name in @('LICENSE.txt','THIRD_PARTY_NOTICES.txt')) { [IO.File]::WriteAllText((Join-Path $app $name), "prior $name fixture") }
# A concurrent edit is still fatal, unlike a safe optional-startup failure.
$xmlReadState = @{Count = 0}
function Get-FileHash {
    param([string]$LiteralPath)
    if ($LiteralPath -eq $xml) {
        $xmlReadState.Count++
        if ($xmlReadState.Count -eq 2) { Add-Content -LiteralPath $xml -Value '<!-- concurrent update -->' }
    }
    Microsoft.PowerShell.Utility\Get-FileHash -LiteralPath $LiteralPath
}
$refused = $false
try {
    & (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $sim -ExeXml $xml -Destination $app -PayloadDirectory $payload -NoShortcut
} catch { $refused = $true }
finally { Remove-Item Function:\Get-FileHash }
if (-not $refused) { throw 'Concurrent startup edit did not fail the update transaction.' }
if (-not ([IO.File]::ReadAllText($xml)).Contains('<!-- concurrent update -->')) { throw 'Failed update lost the concurrent startup edit.' }
foreach ($name in @('LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
    if ([IO.File]::ReadAllText((Join-Path $app $name)) -ne "prior $name fixture") { throw "Failed update did not restore $name." }
}
& (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $sim -ExeXml $xml -Destination $app -PayloadDirectory $payload -NoShortcut
foreach ($name in @('LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
    if ((Get-FileHash -LiteralPath (Join-Path $app $name)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $payload $name)).Hash) {
        throw "Packaged update omitted or changed $name."
    }
}
$updated = Get-Content -Raw -LiteralPath (Join-Path $app 'installation.json') | ConvertFrom-Json
if ($updated.legacyBackup -ne $firstLegacyBackup) { throw 'Update lost the original legacy rollback file.' }
[xml]$doc = Get-Content -Raw -LiteralPath $xml
if ($doc.SelectNodes('//Launch.Addon[Name="Taxi Cam"]').Count -ne 1) { throw 'Update duplicated startup.' }
& (Join-Path $root 'installer/uninstall.ps1') -Installation $app -RestoreLegacy
[xml]$doc = Get-Content -Raw -LiteralPath $xml
if ($doc.SelectNodes('//Launch.Addon').Count -ne 1 -or $doc.SelectSingleNode('//Launch.Addon/Name').InnerText -ne 'Keep Me') { throw 'Uninstall removed unrelated startup.' }
if ([IO.File]::ReadAllText((Join-Path $sim 'taxi-camera-native.addon64')) -ne 'legacy fixture') { throw 'Rollback did not restore the exact old file.' }
if (-not (Test-Path -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg'))) { throw 'Uninstall removed user calibration.' }
Write-Output 'PASS native install/rollback: isolated fixtures, missing-license refusal, exact runtime inventory and legal text, legal-file update rollback, process guards, binary receipts, startup and calibration preservation, legacy retention.'

. (Join-Path $repoRoot 'installer/validation_receipt.ps1')
$requested = 'C:\Users\Pilot\AppData\Local\Taxi Cam\app\taxi-cam.exe'
$redirected = '\\?\C:\Users\Pilot\AppData\Local\Packages\Example.Desktop_123\LocalCache\Local\Taxi Cam\app\taxi-cam.exe'
if (-not (Test-TaxiRedirectedInstallPath $requested $redirected)) { throw 'Packaged LocalAppData redirection was accepted.' }
if (-not (Test-TaxiRedirectedInstallPath $requested ($redirected -replace 'LocalCache\\Local','LocalCache\Roaming'))) { throw 'Packaged roaming redirection was accepted.' }
if (Test-TaxiRedirectedInstallPath $requested ('\\?\' + $requested)) { throw 'Ordinary per-user installation was rejected.' }
if (Test-TaxiRedirectedInstallPath 'C:\Users\Pilot\Apps\Taxi Cam\taxi-cam.exe' '\\?\D:\Apps\Taxi Cam\taxi-cam.exe') { throw 'A normal filesystem alias was rejected.' }
if (Test-TaxiRedirectedInstallPath $redirected $redirected) { throw 'An explicitly selected physical path was rejected.' }
$fixturePhysical = Get-TaxiPhysicalFilePath (Join-Path $sim 'dxgi.dll')
if (-not $fixturePhysical.EndsWith('\sim\dxgi.dll', [StringComparison]::OrdinalIgnoreCase)) { throw 'Physical file path lookup failed.' }
Write-Output 'PASS installation visibility: ordinary paths, physical aliases, explicit cache paths, Local/Roaming redirection and real handle resolution.'

# Steam launch files can contain real Launch.Addon entries but carry the
# SimConnect document header. Repair belongs to the explicit installer path;
# preserve the exact original file and every unrelated launch entry/comment.
$steamRepair = Join-Path $fixture 'steam-header-repair'
$steamRepairXml = Join-Path $steamRepair 'exe.xml'
$steamRepairApp = Join-Path $steamRepair 'app'
New-Item -ItemType Directory -Path $steamRepair -Force | Out-Null
$mislabelledSteamLaunch = @'
<?xml version="1.0" encoding="utf-8"?>
<!-- Steam launch configuration: keep this comment -->
<SimBase.Document Type="SimConnect" version="1,0">
  <Descr>SimConnect</Descr>
  <Filename>SimConnect.xml</Filename>
  <Disabled>False</Disabled>
  <!-- Keep both existing launchers and their parameters -->
  <Launch.Addon><Name>Steam Utility A</Name><Disabled>False</Disabled><Path>C:\Other Apps\utility-a.exe</Path><CommandLine>--keep &amp; preserve</CommandLine></Launch.Addon>
  <Launch.Addon><Name>Steam Utility B</Name><ManualLoad>False</ManualLoad><Path>D:\Other Apps\utility-b.exe</Path><NewConsole>False</NewConsole></Launch.Addon>
</SimBase.Document>
'@
[IO.File]::WriteAllText($steamRepairXml, $mislabelledSteamLaunch, [Text.UTF8Encoding]::new($true))
$steamOriginalHash = (Get-FileHash -LiteralPath $steamRepairXml).Hash
$steamOriginalDocument = [Xml.XmlDocument]::new()
$steamOriginalDocument.PreserveWhitespace = $true
$steamOriginalDocument.Load($steamRepairXml)
$steamOriginalAddons = @($steamOriginalDocument.SelectNodes('/SimBase.Document/Launch.Addon') | ForEach-Object OuterXml)
$steamOriginalComments = @($steamOriginalDocument.SelectNodes('//comment()') | ForEach-Object Value)
& (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $sim -ExeXml $steamRepairXml -Destination $steamRepairApp -NoShortcut
$steamRepairRecord = Get-Content -Raw -LiteralPath (Join-Path $steamRepairApp 'installation.json') | ConvertFrom-Json
if ($steamRepairRecord.startupStatus -ne 'configured' -or -not $steamRepairRecord.startupUpdated -or $steamRepairRecord.startupError) {
    throw 'Recognized Steam launch-header repair did not configure automatic startup.'
}
if (-not $steamRepairRecord.exeXmlBackup -or (Get-FileHash -LiteralPath $steamRepairRecord.exeXmlBackup).Hash -ne $steamOriginalHash) {
    throw 'Steam header repair did not retain the byte-exact original launch file.'
}
$steamRepairedDocument = [Xml.XmlDocument]::new()
$steamRepairedDocument.PreserveWhitespace = $true
$steamRepairedDocument.Load($steamRepairXml)
if ($steamRepairedDocument.DocumentElement.GetAttribute('Type') -ne 'Launch' -or
    $steamRepairedDocument.SelectSingleNode('/SimBase.Document/Filename').InnerText -ne 'exe.xml' -or
    $steamRepairedDocument.SelectNodes('/SimBase.Document/Launch.Addon').Count -ne 3 -or
    $steamRepairedDocument.SelectNodes('/SimBase.Document/Launch.Addon[Name="Taxi Cam"]').Count -ne 1) {
    throw 'Steam repair did not normalize the header and add one Taxi Cam entry.'
}
$steamRetainedAddons = @($steamRepairedDocument.SelectNodes('/SimBase.Document/Launch.Addon[Name!="Taxi Cam"]') | ForEach-Object OuterXml)
$steamRetainedComments = @($steamRepairedDocument.SelectNodes('//comment()') | ForEach-Object Value)
if (@(Compare-Object $steamOriginalAddons $steamRetainedAddons).Count -or
    @(Compare-Object $steamOriginalComments $steamRetainedComments).Count) {
    throw 'Steam header repair changed unrelated launch nodes or comments.'
}
$steamRepairedHash = (Get-FileHash -LiteralPath $steamRepairXml).Hash
& (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $sim -ExeXml $steamRepairXml -Destination $steamRepairApp -NoShortcut
$steamReinstalledRecord = Get-Content -Raw -LiteralPath (Join-Path $steamRepairApp 'installation.json') | ConvertFrom-Json
if ($steamReinstalledRecord.startupStatus -ne 'configured' -or
    (Get-FileHash -LiteralPath $steamRepairXml).Hash -ne $steamRepairedHash -or
    (Get-FileHash -LiteralPath $steamRepairRecord.exeXmlBackup).Hash -ne $steamOriginalHash) {
    throw 'Repeated Steam install changed repaired startup contents or the original backup.'
}
Write-Output 'PASS Steam launch repair: configured startup, exact original backup, unrelated add-ons/comments preserved and idempotent repeat install.'

# Candidate discovery is isolated to temporary profile roots, restored even on
# failure. A previous Store exe.xml is reusable only for the identical simulator.
$savedInstallerAppData = $env:APPDATA
$savedInstallerLocalAppData = $env:LOCALAPPDATA
try {
    $env:APPDATA = Join-Path $fixture 'cross-store/profile-roaming'
    $env:LOCALAPPDATA = Join-Path $fixture 'cross-store/profile-local'
    $crossStoreSim = Join-Path $fixture 'cross-store/store-sim'
    $crossSteamSim = Join-Path $fixture 'cross-store/steam-sim'
    foreach ($simulatorFixture in @($crossStoreSim, $crossSteamSim)) {
        New-TaxiFixtureImage (Join-Path $simulatorFixture 'FlightSimulator2024.exe') $false
        New-TaxiFixtureImage (Join-Path $simulatorFixture 'SimConnect_internal.dll')
    }
    $crossApp = Join-Path $fixture 'cross-store/app'
    $customStoreXml = Join-Path $fixture 'cross-store/custom-store/exe.xml'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $customStoreXml) | Out-Null
    [IO.File]::WriteAllText($customStoreXml, '<SimBase.Document Type="Launch"><Launch.Addon><Name>Store Utility</Name><Path>C:\StoreUtility.exe</Path></Launch.Addon></SimBase.Document>')
    & (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $crossStoreSim -ExeXml $customStoreXml -Destination $crossApp -NoShortcut
    $customStoreHash = (Get-FileHash -LiteralPath $customStoreXml).Hash
    $discoveredSteamXml = Join-Path $env:APPDATA 'Microsoft Flight Simulator 2024/exe.xml'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $discoveredSteamXml) | Out-Null
    [IO.File]::WriteAllText($discoveredSteamXml, $mislabelledSteamLaunch)
    $discoveredSteamOriginalHash = (Get-FileHash -LiteralPath $discoveredSteamXml).Hash
    & (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $crossSteamSim -Destination $crossApp -NoShortcut
    $crossRecord = Get-Content -Raw -LiteralPath (Join-Path $crossApp 'installation.json') | ConvertFrom-Json
    if ($crossRecord.simulator -ne (Join-Path $crossSteamSim 'FlightSimulator2024.exe') -or
        $crossRecord.exeXml -ne $discoveredSteamXml -or $crossRecord.startupStatus -ne 'configured' -or
        -not $crossRecord.startupUpdated -or $crossRecord.startupError -or
        (Get-FileHash -LiteralPath $customStoreXml).Hash -ne $customStoreHash -or
        (Get-FileHash -LiteralPath $crossRecord.exeXmlBackup).Hash -ne $discoveredSteamOriginalHash) {
        throw 'Store-to-Steam update reused the previous Store launch file or failed to discover and back up Steam startup.'
    }
    [xml]$crossSteamDocument = Get-Content -Raw -LiteralPath $discoveredSteamXml
    if ($crossSteamDocument.SelectSingleNode('/SimBase.Document/Launch.Addon[Name="Taxi Cam"]/CommandLine').InnerText -ne
        ('--background --simulator "' + (Join-Path $crossSteamSim 'FlightSimulator2024.exe') + '"')) {
        throw 'Discovered Steam startup retained the previous simulator executable.'
    }

    # With both standard paths present, same-simulator updates may retain their
    # prior explicit path; changing simulators must not guess between candidates.
    $discoveredStoreXml = Join-Path $env:LOCALAPPDATA 'Packages/Microsoft.Limitless_8wekyb3d8bbwe/LocalCache/exe.xml'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $discoveredStoreXml) | Out-Null
    [IO.File]::WriteAllText($discoveredStoreXml, '<SimBase.Document Type="Launch"><Launch.Addon><Name>Other Store Utility</Name><Path>C:\OtherStore.exe</Path></Launch.Addon></SimBase.Document>')
    $ambiguousApp = Join-Path $fixture 'cross-store/ambiguous-app'
    & (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $crossStoreSim -ExeXml $discoveredStoreXml -Destination $ambiguousApp -NoShortcut
    & (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $crossStoreSim -Destination $ambiguousApp -NoShortcut
    $sameStoreRecord = Get-Content -Raw -LiteralPath (Join-Path $ambiguousApp 'installation.json') | ConvertFrom-Json
    if ($sameStoreRecord.startupStatus -ne 'configured' -or $sameStoreRecord.exeXml -ne $discoveredStoreXml) {
        throw 'Same-simulator update failed to retain its prior explicit launch file.'
    }
    $storeBeforeAmbiguous = (Get-FileHash -LiteralPath $discoveredStoreXml).Hash
    $steamBeforeAmbiguous = (Get-FileHash -LiteralPath $discoveredSteamXml).Hash
    & (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $crossSteamSim -Destination $ambiguousApp -NoShortcut
    $ambiguousRecord = Get-Content -Raw -LiteralPath (Join-Path $ambiguousApp 'installation.json') | ConvertFrom-Json
    if ($ambiguousRecord.startupRequested -ne 'automatic' -or $ambiguousRecord.startupUpdated -or
        $ambiguousRecord.startupStatus -notin @('manual','unchanged') -or
        $ambiguousRecord.startupError -notlike '*Select the simulator exe.xml*' -or
        $ambiguousRecord.exeXml -eq $discoveredStoreXml -or
        (Get-FileHash -LiteralPath $discoveredStoreXml).Hash -ne $storeBeforeAmbiguous -or
        (Get-FileHash -LiteralPath $discoveredSteamXml).Hash -ne $steamBeforeAmbiguous) {
        throw 'Ambiguous Store-to-Steam discovery guessed a launch file instead of preserving both and reporting manual-startup fallback.'
    }
    Write-Output 'PASS simulator-specific startup inheritance: Store-to-Steam discovery, old-file preservation, matching-simulator reuse and ambiguous-candidate fallback.'
} finally {
    $env:APPDATA = $savedInstallerAppData
    $env:LOCALAPPDATA = $savedInstallerLocalAppData
}
