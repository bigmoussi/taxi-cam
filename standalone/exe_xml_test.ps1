$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'exe_xml.ps1')
$testRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'build/native/xml-validation'
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
$path = Join-Path $testRoot 'exe.xml'
@'
<?xml version="1.0" encoding="utf-8"?>
<SimBase.Document Type="Launch" version="1,0">
<!-- An unrelated add-on owns this comment -->
<Disabled>False</Disabled><Launch.ManualLoad>False</Launch.ManualLoad>
<Launch.Addon><Name>Existing &amp; Addon</Name><Path>C:\Other App\other.exe</Path><CommandLine>--keep="exact"</CommandLine><Disabled>True</Disabled><Custom>preserved</Custom></Launch.Addon>
</SimBase.Document>
'@ | Set-Content -LiteralPath $path -Encoding utf8
$document = Read-TaxiLaunchXml $path
$untouched = $document.SelectSingleNode('//Launch.Addon').OuterXml
$original = (Get-FileHash -LiteralPath $path).Hash
Set-TaxiStartupEntry $document 'C:\Native Camera & Tools\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
$backup = Save-TaxiLaunchXml $document $path $original
if (-not $backup -or (Get-FileHash -LiteralPath $backup).Hash -ne $original) { throw 'Original startup file was not preserved.' }
$read = Read-TaxiLaunchXml $path
if ($read.SelectSingleNode('//Launch.Addon[Name="Existing & Addon"]').OuterXml -ne $untouched) { throw 'Unrelated entry changed.' }
if ($read.SelectSingleNode('//Launch.Addon[Name="Taxi Cam"]/CommandLine').InnerText -ne '--background --simulator "C:\MSFS\FlightSimulator2024.exe"') { throw 'Argument quoting changed.' }
Set-TaxiStartupEntry $read 'C:\Native Camera & Tools\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
if ($read.SelectNodes('//Launch.Addon[Name="Taxi Cam"]').Count -ne 1) { throw 'Duplicate startup entry.' }
$read.SelectSingleNode('//Launch.Addon[Name="Taxi Cam"]/Name').InnerText = '380 Taxi Cam'
Set-TaxiStartupEntry $read 'C:\Native Camera & Tools\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
if ($read.SelectNodes('//Launch.Addon').Count -ne 2 -or $read.SelectNodes('//Launch.Addon[Name="Taxi Cam"]').Count -ne 1) { throw 'Rename did not migrate the existing entry in place.' }
$duplicate = $read.SelectSingleNode('//Launch.Addon[Name="Taxi Cam"]').CloneNode($true)
$duplicate.SelectSingleNode('Name').InnerText = '380 Taxi Cam'
[void]$read.DocumentElement.AppendChild($duplicate)
$refused = $false
try { Set-TaxiStartupEntry $read 'C:\Native Camera & Tools\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe' } catch { $refused = $true }
if (-not $refused) { throw 'Mixed old/new startup entries must refuse an ambiguous upgrade.' }
[void]$read.DocumentElement.RemoveChild($duplicate)
$refused = $false
try { Save-TaxiLaunchXml $read $path ('0' * 64) } catch { $refused = $true }
if (-not $refused) { throw 'Concurrent edit guard did not refuse.' }
Set-TaxiStartupEntry $read '' '' -Remove
if ($read.SelectNodes('//Launch.Addon').Count -ne 1 -or $read.SelectSingleNode('//Launch.Addon').OuterXml -ne $untouched) { throw 'Uninstall changed unrelated startup.' }
$invalid = Join-Path $testRoot 'dtd.xml'
'<!DOCTYPE SimBase.Document [<!ENTITY external SYSTEM "file:///C:/Windows/win.ini">]><SimBase.Document Type="Launch">&external;</SimBase.Document>' | Set-Content -LiteralPath $invalid
$refused = $false
try { Read-TaxiLaunchXml $invalid } catch { $refused = $true }
if (-not $refused) { throw 'External XML entity was not refused.' }
Write-Output 'PASS exe.xml: preserve unrelated entries/comments, escaped paths, exact arguments, backup, idempotence, concurrent-edit refusal, targeted removal, DTD refusal.'
