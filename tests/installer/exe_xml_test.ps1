$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'installer/exe_xml.ps1')
$testRoot = Join-Path $repoRoot 'build/tests/installer/xml-validation'
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
$writtenHash = ''
$backup = Save-TaxiLaunchXml $document $path $original ([ref]$writtenHash)
if (-not $backup -or (Get-FileHash -LiteralPath $backup).Hash -ne $original) { throw 'Original startup file was not preserved.' }
if ($writtenHash -ne (Get-FileHash -LiteralPath $path).Hash) { throw 'Written hash does not describe the committed XML bytes.' }
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
try { Save-TaxiLaunchXml $read $path ('0' * 64) } catch { $refused = $_.Exception -is [InvalidOperationException] -and $_.Exception.Data['TaxiStartupConflict'] }
if (-not $refused) { throw 'Concurrent edit guard did not refuse.' }
Set-TaxiStartupEntry $read '' '' -Remove
if ($read.SelectNodes('//Launch.Addon').Count -ne 1 -or $read.SelectSingleNode('//Launch.Addon').OuterXml -ne $untouched) { throw 'Uninstall changed unrelated startup.' }
$invalid = Join-Path $testRoot 'dtd.xml'
'<!DOCTYPE SimBase.Document [<!ENTITY external SYSTEM "file:///C:/Windows/win.ini">]><SimBase.Document Type="Launch">&external;</SimBase.Document>' | Set-Content -LiteralPath $invalid
$refused = $false
try { Read-TaxiLaunchXml $invalid } catch { $refused = $true }
if (-not $refused) { throw 'External XML entity was not refused.' }
Write-Output 'PASS exe.xml: preserve unrelated entries/comments, escaped paths, exact arguments, backup, idempotence, concurrent-edit refusal, targeted removal, DTD refusal.'

# Failures before replacement must leave user XML intact and identify whether
# callers may safely continue with manual startup. All fixtures remain in build/.
$failureRoot = Join-Path $testRoot ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $failureRoot | Out-Null
$failurePath = Join-Path $failureRoot 'exe.xml'
$failureContents = '<SimBase.Document Type="Launch"><Launch.Addon><Name>Keep Me</Name><Path>C:\Other.exe</Path></Launch.Addon></SimBase.Document>'
[IO.File]::WriteAllText($failurePath, $failureContents)
$failureHash = (Get-FileHash -LiteralPath $failurePath).Hash
$failureDocument = Read-TaxiLaunchXml $failurePath
Set-TaxiStartupEntry $failureDocument 'C:\Native Camera\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
$script:copyFailure = ''
function Copy-Item {
    param([string]$LiteralPath,[string]$Destination)
    if ($Destination -like '*.taxi-backup-*') {
        switch ($script:copyFailure) {
            'encryption' { throw [ComponentModel.Win32Exception]::new(6000) }
            'permission' { throw [UnauthorizedAccessException]::new('Fixture denies the backup operation.') }
            'partial-backup' {
                [IO.File]::WriteAllText($Destination, '<SimBase.Document')
                throw [ComponentModel.Win32Exception]::new(6000)
            }
            'cleanup-failure' {
                [IO.File]::WriteAllText($Destination, '<SimBase.Document')
                [IO.File]::SetAttributes($Destination, [IO.FileAttributes]::ReadOnly)
                throw [ComponentModel.Win32Exception]::new(6000)
            }
            'changed-source' {
                [IO.File]::AppendAllText($LiteralPath, '<!-- concurrent edit during failed backup -->')
                throw [ComponentModel.Win32Exception]::new(6000)
            }
            'corrupt-backup' { [IO.File]::WriteAllText($Destination, 'not the original XML'); return }
        }
    }
    Microsoft.PowerShell.Management\Copy-Item -LiteralPath $LiteralPath -Destination $Destination
}
try {
    foreach ($kind in @('encryption','permission','partial-backup')) {
        $script:copyFailure = $kind
        $failure = $null; $writtenHash = 'not committed'
        try { Save-TaxiLaunchXml $failureDocument $failurePath $failureHash ([ref]$writtenHash) } catch { $failure = $_ }
        if (-not $failure -or $failure.Exception.Data['TaxiStartupUnsafe'] -or $failure.Exception.Data['TaxiStartupConflict']) { throw "Safe $kind failure was not classified as recoverable." }
        if ($kind -eq 'encryption' -and $failure.Exception.NativeErrorCode -ne 6000) { throw 'Synthetic encryption error lost its Windows error code.' }
        if ($failure.Exception.Data['TaxiStartupOperation'] -ne 'Back up startup file' -or
            $failure.Exception.Data['TaxiStartupSource'] -ne $failurePath -or
            $failure.Exception.Data['TaxiStartupDestination'] -notlike ($failurePath + '.taxi-backup-*')) {
            throw "Failed $kind backup omitted its exact operation, source or destination."
        }
        if ($writtenHash -or (Get-FileHash -LiteralPath $failurePath).Hash -ne $failureHash) { throw "Failed $kind backup changed the original XML or reported a commit." }
        if (@(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-*.tmp').Count) { throw "Failed $kind backup left temporary XML." }
        if (@(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-backup-*').Count) { throw "Failed $kind backup left an unverified copy." }
    }
    $script:copyFailure = 'corrupt-backup'; $failure = $null
    try { Save-TaxiLaunchXml $failureDocument $failurePath $failureHash } catch { $failure = $_ }
    if (-not $failure -or -not $failure.Exception.Data['TaxiStartupConflict']) { throw 'Backup verification failure did not retain the conflict guard.' }
    if ((Get-FileHash -LiteralPath $failurePath).Hash -ne $failureHash) { throw 'Bad backup changed the original XML.' }
    if (@(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-backup-*').Count) { throw 'Hash-mismatched backup was retained.' }

    $script:copyFailure = 'cleanup-failure'; $failure = $null
    try {
        try { Save-TaxiLaunchXml $failureDocument $failurePath $failureHash } catch { $failure = $_ }
        if (-not $failure -or -not $failure.Exception.Data['TaxiStartupUnsafe']) { throw 'Unverified backup cleanup failure was allowed to fall back to manual startup.' }
        $remaining = @(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-backup-*')
        if ($remaining.Count -ne 1 -or -not $failure.Exception.Message.Contains($remaining[0].FullName)) { throw 'Cleanup failure did not identify the retained unverified backup.' }
        if ((Get-FileHash -LiteralPath $failurePath).Hash -ne $failureHash) { throw 'Cleanup failure changed the original XML.' }
    } finally {
        foreach ($item in @(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-backup-*')) {
            [IO.File]::SetAttributes($item.FullName, [IO.FileAttributes]::Normal)
            [IO.File]::Delete($item.FullName)
        }
    }

    $script:copyFailure = 'changed-source'; $failure = $null
    try { Save-TaxiLaunchXml $failureDocument $failurePath $failureHash } catch { $failure = $_ }
    if (-not $failure -or -not $failure.Exception.Data['TaxiStartupUnsafe']) { throw 'Failed backup with uncertain original state was allowed to continue.' }
    if (-not [IO.File]::ReadAllText($failurePath).Contains('<!-- concurrent edit during failed backup -->')) { throw 'Failure handler erased another writer''s XML edit.' }
    if (@(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-*.tmp').Count) { throw 'Failure paths left temporary XML.' }
} finally { Remove-Item Function:\Copy-Item }

# Simulate the EFS attribute/encryption calls in a private copy of the helper.
# The backup copy succeeds with matching bytes but without its expected EFS flag.
# No personal EFS key or encrypted fixture is created by this default test.
[IO.File]::WriteAllText($failurePath, $failureContents)
& {
    $helper = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'installer/exe_xml.ps1')
    foreach ($argument in @('$absolute','$temporary','$backup')) {
        $call = '[IO.File]::GetAttributes(' + $argument + ')'
        if (-not $helper.Contains($call)) { throw 'The simulated encryption attribute hook no longer matches.' }
        $helper = $helper.Replace($call, ('(Get-FixtureAttributes ' + $argument + ')'))
    }
    $encryptCall = '[IO.File]::Encrypt($temporary)'
    if (-not $helper.Contains($encryptCall)) { throw 'The simulated encryption operation hook no longer matches.' }
    $helper = $helper.Replace($encryptCall, 'Set-FixtureEncryption $temporary')
    . ([scriptblock]::Create($helper))
    $encryptedPaths = @{$failurePath = $true}
    function Get-FixtureAttributes([string]$Path) {
        $attributes = [IO.File]::GetAttributes($Path)
        if ($encryptedPaths.ContainsKey($Path)) { return $attributes -bor [IO.FileAttributes]::Encrypted }
        return $attributes
    }
    function Set-FixtureEncryption([string]$Path) { $encryptedPaths[$Path] = $true }
    $failure = $null; $writtenHash = 'not committed'
    try { Save-TaxiLaunchXml $failureDocument $failurePath $failureHash ([ref]$writtenHash) } catch { $failure = $_ }
    if (-not $failure -or $failure.Exception.Message -ne 'Could not preserve exe.xml encryption on the backup file.') { throw 'The encryption-mismatched backup fixture did not reach verification.' }
    if ($failure.Exception.Data['TaxiStartupUnsafe'] -or $failure.Exception.Data['TaxiStartupConflict']) { throw 'Successful unverified backup cleanup blocked safe manual startup.' }
    if ($writtenHash -or (Get-FileHash -LiteralPath $failurePath).Hash -ne $failureHash) { throw 'Encryption-mismatched backup changed the original XML or reported a commit.' }
    if (@(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-backup-*').Count) { throw 'Encryption-mismatched backup left an unencrypted copy.' }
    if (@(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-*.tmp').Count) { throw 'Encryption-mismatched backup left temporary XML.' }
}

# A real access-denied replacement, after a verified backup exists, is still safe
# to downgrade only when the original bytes and encryption remain unchanged.
[IO.File]::WriteAllText($failurePath, $failureContents)
[IO.File]::SetAttributes($failurePath, [IO.FileAttributes]::ReadOnly)
try {
    $failure = $null; $writtenHash = ''
    try { Save-TaxiLaunchXml $failureDocument $failurePath $failureHash ([ref]$writtenHash) } catch { $failure = $_ }
    if (-not $failure -or $failure.Exception.Data['TaxiStartupUnsafe'] -or $writtenHash) { throw 'Read-only original did not produce a safe uncommitted failure.' }
    if ((Get-FileHash -LiteralPath $failurePath).Hash -ne $failureHash) { throw 'Read-only original changed.' }
    if (@(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-*.tmp').Count) { throw 'Failed replacement left temporary XML.' }
    $validBackups = @(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-backup-*' | Where-Object { (Get-FileHash -LiteralPath $_.FullName).Hash -eq $failureHash })
    if (-not $validBackups.Count) { throw 'Failed replacement discarded its verified recovery backup.' }
} finally { [IO.File]::SetAttributes($failurePath, [IO.FileAttributes]::Normal) }

# New XML has no backup, but still reports the bytes owned by the transaction.
$newRoot = Join-Path $failureRoot 'new'
$newPath = Join-Path $newRoot 'exe.xml'
$newDocument = Read-TaxiLaunchXml $newPath
Set-TaxiStartupEntry $newDocument 'C:\Native Camera\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
$writtenHash = ''
$newBackup = Save-TaxiLaunchXml $newDocument $newPath '' ([ref]$writtenHash)
if ($newBackup -or $writtenHash -ne (Get-FileHash -LiteralPath $newPath).Hash) { throw 'New-file transaction ownership is incorrect.' }
Write-Output 'PASS exe.xml failure resilience: encryption error 6000 and access denial preserve original, partial/hash/encryption-mismatched backups removed, cleanup failure blocks fallback, typed conflicts, exact committed hashes and retained verified backups.'

# Real EFS coverage is opt-in through tests/installer/test-encryption.ps1 -RunEfsFixture.
# That fixture requires an existing EFS key; this default test must not cause
# Windows to create a personal encryption key on supported developer or CI hosts.
