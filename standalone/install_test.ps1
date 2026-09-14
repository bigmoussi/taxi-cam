$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $root ('build/native/install-validation-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
$sim = Join-Path $fixture 'sim'
$app = Join-Path $fixture 'app'
New-Item -ItemType Directory -Path $sim -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $sim 'FlightSimulator2024.exe'),'fixture; never executed')
[IO.File]::WriteAllText((Join-Path $sim 'taxi-camera-native.addon64'),'legacy fixture')
[IO.File]::WriteAllText((Join-Path $sim 'dxgi.dll'),'unrelated ReShade fixture')
Copy-Item -LiteralPath (Join-Path $root 'taxi-camera-mounts.cfg') -Destination (Join-Path $sim 'taxi-camera-mounts.cfg')
$mountHash = (Get-FileHash -LiteralPath (Join-Path $sim 'taxi-camera-mounts.cfg')).Hash
$xml = Join-Path $fixture 'exe.xml'
[IO.File]::WriteAllText($xml,'<SimBase.Document Type="Launch"><Launch.Addon><Name>Keep Me</Name><Path>C:\Other.exe</Path></Launch.Addon></SimBase.Document>')
& (Join-Path $root 'install-native.ps1') -SimulatorDirectory $sim -ExeXml $xml -Destination $app -NoShortcut
$record = Get-Content -Raw -LiteralPath (Join-Path $app 'installation.json') | ConvertFrom-Json
[xml]$doc = Get-Content -Raw -LiteralPath $xml
if ($doc.SelectNodes('//Launch.Addon').Count -ne 2 -or -not $doc.SelectSingleNode('//Launch.Addon[Name="Keep Me"]')) { throw 'Installer changed unrelated startup.' }
if ((Get-FileHash -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg')).Hash -ne $mountHash) { throw 'Calibration import changed.' }
if (-not (Test-Path -LiteralPath $record.legacyBackup) -or (Test-Path -LiteralPath (Join-Path $sim 'taxi-camera-native.addon64'))) { throw 'Legacy add-on not retained and disabled.' }
if ([IO.File]::ReadAllText((Join-Path $sim 'dxgi.dll')) -ne 'unrelated ReShade fixture') { throw 'Other ReShade files changed.' }
$firstLegacyBackup = $record.legacyBackup
& (Join-Path $root 'install-native.ps1') -SimulatorDirectory $sim -ExeXml $xml -Destination $app -NoShortcut
$updated = Get-Content -Raw -LiteralPath (Join-Path $app 'installation.json') | ConvertFrom-Json
if ($updated.legacyBackup -ne $firstLegacyBackup) { throw 'Update lost the original legacy rollback file.' }
[xml]$doc = Get-Content -Raw -LiteralPath $xml
if ($doc.SelectNodes('//Launch.Addon[Name="380 Taxi Cam"]').Count -ne 1) { throw 'Update duplicated startup.' }
& (Join-Path $root 'uninstall-native.ps1') -Installation $app -RestoreLegacy
[xml]$doc = Get-Content -Raw -LiteralPath $xml
if ($doc.SelectNodes('//Launch.Addon').Count -ne 1 -or $doc.SelectSingleNode('//Launch.Addon/Name').InnerText -ne 'Keep Me') { throw 'Uninstall removed unrelated startup.' }
if ([IO.File]::ReadAllText((Join-Path $sim 'taxi-camera-native.addon64')) -ne 'legacy fixture') { throw 'Rollback did not restore the exact old file.' }
if (-not (Test-Path -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg'))) { throw 'Uninstall removed user calibration.' }
Write-Output 'PASS native install/rollback: real installer with isolated paths, exact binary receipts, startup preservation, calibration import, legacy retention and unrelated ReShade preserved.'
