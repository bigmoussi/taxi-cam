$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'installer/prerequisites.ps1')
. (Join-Path $repoRoot 'tests/support/installer_fixture.ps1')
$fixture = Join-Path $repoRoot ('build/prerequisite-tests/' + [Guid]::NewGuid().ToString('N'))
$sim = Join-Path $fixture 'sim with spaces'
$system = Join-Path $fixture 'system'
$checks = 0
function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function Issues { return @(Get-TaxiPrerequisiteIssues -SimulatorDirectory $sim -SystemDirectory $system) }
$systemDlls = @('d3d12.dll','dxgi.dll','d3dcompiler_47.dll','ucrtbase.dll','msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')
foreach ($name in $systemDlls) { New-TaxiFixtureImage (Join-Path $system $name) }
New-TaxiFixtureImage (Join-Path $system 'WindowsPowerShell/v1.0/powershell.exe') $false
New-TaxiFixtureImage (Join-Path $sim 'FlightSimulator2024.exe') $false
New-TaxiFixtureImage (Join-Path $sim 'SimConnect_internal.dll')
Assert (@(Issues).Count -eq 0) 'Complete x64 prerequisites were refused.'
foreach ($name in $systemDlls) {
    $path = Join-Path $system $name
    Remove-Item -LiteralPath $path
    Assert ((@(Issues) -join ' ') -like "*$name*") "Missing $name was not diagnosed."
    New-TaxiFixtureImage $path $true 0x14C
    Assert ((@(Issues) -join ' ') -like "*$name*") "32-bit $name was accepted."
    New-TaxiFixtureImage $path
}
$simExecutable = Join-Path $sim 'FlightSimulator2024.exe'
Remove-Item -LiteralPath $simExecutable
Assert ((@(Issues) -join ' ') -like '*FlightSimulator2024.exe*') 'Missing simulator executable was not diagnosed.'
New-TaxiFixtureImage $simExecutable $false
foreach ($name in @('SimConnect_internal.dll')) {
    $path = Join-Path $sim $name
    $dll = $name.EndsWith('.dll')
    Remove-Item -LiteralPath $path
    Assert ((@(Issues) -join ' ') -like "*$name*") "Missing simulator file was not diagnosed: $name"
    New-TaxiFixtureImage $path $dll 0x14C
    Assert ((@(Issues) -join ' ') -like "*$name*") "32-bit simulator file was accepted: $name"
    New-TaxiFixtureImage $path (-not $dll)
    Assert ((@(Issues) -join ' ') -like "*$name*") "Wrong EXE/DLL image kind was accepted: $name"
    New-TaxiFixtureImage $path $dll
}
$client = Join-Path $sim 'SimConnect_internal.dll'
foreach ($offset in @(0, 0x7FFFFFFF, [uint32]::MaxValue)) {
    New-TaxiFixtureImage $client
    $bytes = [IO.File]::ReadAllBytes($client)
    [BitConverter]::GetBytes([uint32]$offset).CopyTo($bytes, 60)
    [IO.File]::WriteAllBytes($client, $bytes)
    Assert (-not (Test-TaxiAmd64Image $client)) 'Malformed PE header offset was accepted.'
}
foreach ($size in @(0, 1, 63, 64, 153, 300)) {
    New-TaxiFixtureImage $client
    $bytes = [IO.File]::ReadAllBytes($client)
    [Array]::Resize([ref]$bytes, $size)
    [IO.File]::WriteAllBytes($client, $bytes)
    Assert (-not (Test-TaxiAmd64Image $client)) 'Truncated PE image was accepted.'
}
New-TaxiFixtureImage $client
$localRuntime = Join-Path $sim 'msvcp140.dll'
New-TaxiFixtureImage $localRuntime $true 0x14C
Assert ((@(Issues) -join ' ') -like '*msvcp140.dll*') 'Invalid app-local runtime was hidden by a valid system copy.'
New-TaxiFixtureImage $localRuntime
Remove-Item -LiteralPath (Join-Path $system 'msvcp140.dll')
Assert (@(Issues).Count -eq 0) 'Valid app-local runtime was refused.'
$ps = Join-Path $system 'WindowsPowerShell/v1.0/powershell.exe'
Remove-Item -LiteralPath $ps
Assert ((@(Issues) -join ' ') -like '*Windows PowerShell*') 'Missing Windows PowerShell was not diagnosed.'
Write-Output "PASS prerequisites: $checks missing-component, image-header, architecture, truncation and local-runtime precedence checks; no simulator code executed."
