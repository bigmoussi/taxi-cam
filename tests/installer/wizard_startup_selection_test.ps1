[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$testRoot = Join-Path $repoRoot ('build/tests/installer/wizard-startup-' + [Guid]::NewGuid().ToString('N'))
$payload = Join-Path $testRoot 'payload'
$store = Join-Path $testRoot 'Store/Content'
$steam = Join-Path $testRoot 'Steam/MSFS2024'
$oldXml = Join-Path $testRoot 'Store-config/exe.xml'
$newXml = Join-Path $testRoot 'Steam-config/exe.xml'
New-Item -ItemType Directory -Force -Path $payload,$store,$steam,(Split-Path -Parent $oldXml),(Split-Path -Parent $newXml) | Out-Null
[IO.File]::WriteAllText((Join-Path $payload 'fixture.txt'), 'This fixture never installs a native runtime.')
foreach ($sim in @($store,$steam)) { [IO.File]::WriteAllText((Join-Path $sim 'FlightSimulator2024.exe'), 'File-existence fixture only.') }
foreach ($xml in @($oldXml,$newXml)) { [IO.File]::WriteAllText($xml, '<SimBase.Document Type="Launch"/>') }
$xmlHashes = @{}
foreach ($xml in @($oldXml,$newXml)) { $xmlHashes[$xml] = (Get-FileHash -LiteralPath $xml).Hash }

# Execute the compiled wizard, but replace its external helper with an argument
# recorder which always refuses PrepareToInstall. No native installer, registry
# registration, shortcuts, settings changes or live simulator access can occur.
$runtime = Join-Path $testRoot 'capture-runtime.ps1'
@'
param([string]$Mode,[string]$Destination,[string]$StateDirectory,[string]$SimulatorDirectory,
      [string]$ExeXml,[string]$PayloadDirectory,[string]$StartupMode,[string]$UpdateFromPid,[switch]$ResetSettings)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $StateDirectory | Out-Null
if ($Mode -eq 'Discover') {
    $record = Get-Content -LiteralPath (Join-Path $Destination 'installation.json') -Raw | ConvertFrom-Json
    @('[Paths]', ('Simulator=' + (Split-Path -Parent $record.simulator)), ('ExeXml=' + $record.exeXml), 'Startup=automatic') |
        Set-Content -LiteralPath (Join-Path $StateDirectory 'choices.ini') -Encoding Unicode
    exit 0
}
if ($Mode -eq 'Install') {
    [ordered]@{simulator=$SimulatorDirectory; exeXml=$ExeXml; exeXmlWasPassed=$PSBoundParameters.ContainsKey('ExeXml'); startup=$StartupMode} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Destination 'captured-arguments.json') -Encoding utf8
    [IO.File]::WriteAllText((Join-Path $StateDirectory 'error.txt'), 'Wizard argument fixture stopped before installation.')
    exit 1
}
throw "Unexpected fixture mode: $Mode"
'@ | Set-Content -LiteralPath $runtime -Encoding utf8
$settings = Join-Path $repoRoot 'installer/settings.ps1'
& (Join-Path $repoRoot 'installer/embed-uninstaller.ps1') -Output (Join-Path $testRoot 'uninstall-scripts.iss') `
    -RuntimeScript $runtime -UninstallScript (Join-Path $repoRoot 'installer/uninstall.ps1') `
    -ExeXmlScript (Join-Path $repoRoot 'installer/exe_xml.ps1') -SettingsScript $settings
$compiler = & (Join-Path $repoRoot 'installer/bootstrap.ps1')
$script:checks = 0
function Assert-Wizard([bool]$Condition,[string]$Message) {
    $script:checks++
    if (-not $Condition) { throw $Message }
}
function Build-WizardFixture([string]$Source,[string]$Name) {
    $log = Join-Path $testRoot ($Name + '-compiler.log')
    & $compiler "/DPayloadDir=$payload" "/DInternalDir=$testRoot" "/DAppIcon=$(Join-Path $repoRoot 'src/app/taxi-cam.ico')" `
        "/DRuntimeScript=$runtime" "/DSettingsScript=$settings" '/DAppVersion=0.0.0' '/DBuildNumber=0' '/DInstallerTest=1' `
        "/DOutputBase=$Name" "/O$testRoot" $Source *> $log
    if ($LASTEXITCODE -ne 0) { throw "Wizard fixture compilation failed. See $log" }
    return Join-Path $testRoot ($Name + '.exe')
}
function Invoke-WizardCase([string]$Fixture,[string]$Name,[string[]]$Options,[string]$ExpectedSimulator,
                           [bool]$ExpectXml,[string]$ExpectedXml = '',[string]$ExpectedStartup = 'Automatic') {
    $app = Join-Path $testRoot $Name
    New-Item -ItemType Directory -Path $app | Out-Null
    $record = Join-Path $app 'installation.json'
    @{simulator=(Join-Path $store 'FlightSimulator2024.exe'); exeXml=$oldXml} |
        ConvertTo-Json | Set-Content -LiteralPath $record -Encoding utf8
    $recordHash = (Get-FileHash -LiteralPath $record).Hash
    $log = Join-Path $testRoot ($Name + '.log')
    $arguments = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/DIR="' + $app + '"'),('/LOG="' + $log + '"')) + $Options
    $process = Start-Process -FilePath $Fixture -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(30000)) {
        # Only stop the process tree launched for this fixture; never discover or
        # stop any simulator, companion or unrelated setup process.
        if (-not $process.HasExited) { & (Join-Path $env:WINDIR 'System32/taskkill.exe') /PID $process.Id /T /F *> $null }
        throw "Wizard fixture timed out. See $log"
    }
    $process.Refresh()
    Assert-Wizard ($process.ExitCode -ne 0) 'Argument fixture unexpectedly completed installation.'
    $capturePath = Join-Path $app 'captured-arguments.json'
    Assert-Wizard (Test-Path -LiteralPath $capturePath -PathType Leaf) "Wizard did not reach the argument recorder: $log"
    $capture = Get-Content -LiteralPath $capturePath -Raw | ConvertFrom-Json
    Assert-Wizard ($capture.simulator -eq $ExpectedSimulator) "Wrong simulator passed for $Name."
    Assert-Wizard ($capture.exeXmlWasPassed -eq $ExpectXml) "Wrong inherited/explicit XML argument decision for $Name."
    Assert-Wizard ($capture.exeXml -eq $ExpectedXml) "Wrong XML path passed for $Name."
    Assert-Wizard ($capture.startup -eq $ExpectedStartup) "Startup mode changed for $Name."
    Assert-Wizard ((Get-FileHash -LiteralPath $record).Hash -eq $recordHash) 'Fixture changed the installation record.'
    Assert-Wizard (-not (Test-Path -LiteralPath (Join-Path $app 'taxi-cam.exe'))) 'Fixture wrote a native application.'
}

$productionSource = Join-Path $repoRoot 'installer/taxi-cam.iss'
$fixture = Build-WizardFixture $productionSource 'wizard-startup-capture'
Invoke-WizardCase $fixture 'unchanged-simulator' @() $store $true $oldXml
Invoke-WizardCase $fixture 'silent-store-to-steam' @('/SIMULATORDIR="' + $steam + '"') $steam $false
Invoke-WizardCase $fixture 'explicit-new-xml' @('/SIMULATORDIR="' + $steam + '"','/EXEXML="' + $newXml + '"') $steam $true $newXml
Invoke-WizardCase $fixture 'explicit-old-xml' @('/SIMULATORDIR="' + $steam + '"','/EXEXML="' + $oldXml + '"') $steam $true $oldXml
Invoke-WizardCase $fixture 'manual-simulator-change' @('/SIMULATORDIR="' + $steam + '"','/STARTUP=manual') $steam $false '' 'Manual'

# A private source copy assigns control values immediately before the final
# production guard. It exercises that second guard independently of page Next,
# and triggers the real edit event instead of reimplementing the selection rule.
$source = Get-Content -LiteralPath $productionSource -Raw
$marker = "  ClearStaleInheritedXml;`n  ExtractTemporaryFiles"
$normalized = $source.Replace("`r`n", "`n")
Assert-Wizard ($normalized.Split([string[]]@($marker),[StringSplitOptions]::None).Length -eq 2) 'Final selection-guard test marker is ambiguous.'
$driver = @'
  if ExpandConstant('{param:TESTEDITXML|}') <> '' then
    XmlPage.Values[0] := ExpandConstant('{param:TESTEDITXML|}');
  if ExpandConstant('{param:TESTRESELECTINHERITED|0}') = '1' then begin
    XmlPage.Values[0] := InheritedXml + '.selection-in-progress';
    XmlPage.Values[0] := InheritedXml;
  end;
  if ExpandConstant('{param:TESTLATESIM|}') <> '' then
    SimulatorPage.Values[0] := ExpandConstant('{param:TESTLATESIM|}');
  ClearStaleInheritedXml;
  ExtractTemporaryFiles
'@
$driverSource = Join-Path $testRoot 'wizard-control-driver.iss'
$normalized.Replace($marker,$driver.Replace("`r`n","`n")) | Set-Content -LiteralPath $driverSource -Encoding utf8
$driverFixture = Build-WizardFixture $driverSource 'wizard-control-driver'
Invoke-WizardCase $driverFixture 'prepare-guard-simulator-change' @('/TESTLATESIM="' + $steam + '"') $steam $false
Invoke-WizardCase $driverFixture 'preserve-user-edit' @('/TESTLATESIM="' + $steam + '"','/TESTEDITXML="' + $newXml + '"') $steam $true $newXml
Invoke-WizardCase $driverFixture 'preserve-user-reselection' @('/TESTLATESIM="' + $steam + '"','/TESTRESELECTINHERITED=1') $steam $true $oldXml
foreach ($xml in @($oldXml,$newXml)) {
    Assert-Wizard ((Get-FileHash -LiteralPath $xml).Hash -eq $xmlHashes[$xml]) 'Wizard fixture changed a startup XML file.'
}
[ordered]@{
    utc=[DateTime]::UtcNow.ToString('o'); checks=$checks; cases=8; nativeInstallationPerformed=$false;
    method='Compiled wizard argument capture; helper always refuses before installation. Private control driver exercises edit events and final guard.';
    sourceSha256=(Get-FileHash -LiteralPath $productionSource).Hash;
    compilerSha256=(Get-FileHash -LiteralPath $compiler).Hash;
    fixtureSha256=(Get-FileHash -LiteralPath $fixture).Hash;
    driverFixtureSha256=(Get-FileHash -LiteralPath $driverFixture).Hash
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'result.json') -Encoding utf8
Write-Output "PASS wizard startup selection: $checks checks across 8 compiled wizard cases; no installation performed. Evidence: $testRoot"
