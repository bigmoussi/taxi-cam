$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$label = 'package-test-' + [Guid]::NewGuid().ToString('N')
$commit = 'a' * 40
$zip = & (Join-Path $root 'package-native.ps1') -BuildLabel $label -SourceCommit $commit
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $entries = @{}
    foreach ($entry in $archive.Entries) {
        if (-not $entry.Name) { continue }
        $relative = ($entry.FullName.Replace('\','/') -split '/',2)[1]
        if ($entries.ContainsKey($relative)) { throw 'Duplicate ZIP entry.' }
        $entries[$relative] = $entry
    }
    foreach ($required in @('380-taxi-cam.exe','taxi-camera-bridge.dll','validation.json','install-native.ps1','uninstall-native.ps1',
        'standalone/exe_xml.ps1','standalone/validation_receipt.ps1','docs/architecture.md','docs/runtime-reference.md',
        'docs/aircraft-profiles.md','docs/releases.md','build-info.json','manifest.json','taxi-camera-mounts.cfg','licenses/LLVM.txt')) {
        if (-not $entries.ContainsKey($required)) { throw "Package entry missing: $required" }
    }
    function Read-Entry([string]$Name) {
        $reader = [IO.StreamReader]::new($entries[$Name].Open())
        try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
    }
    $manifest = @(Read-Entry 'manifest.json' | ConvertFrom-Json)
    if ($manifest.Count -ne $entries.Count - 1) { throw 'Manifest does not cover the complete package.' }
    foreach ($file in $manifest) {
        $stream = $entries[$file.file].Open()
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','') }
        finally { $sha.Dispose(); $stream.Dispose() }
        if ($hash -ne $file.sha256) { throw "Package manifest hash mismatch: $($file.file)" }
    }
    $info = Read-Entry 'build-info.json' | ConvertFrom-Json
    if ($info.sourceCommit -ne $commit -or $info.build -ne $label -or $info.simulatorVerified) { throw 'Incorrect package provenance.' }
} finally { $archive.Dispose() }
$refused = $false
try { & (Join-Path $root 'package-native.ps1') -BuildLabel $label -SourceCommit $commit | Out-Null } catch { $refused = $true }
if (-not $refused) { throw 'Existing package was overwritten.' }
$refused = $false
try { & (Join-Path $root 'package-native.ps1') -BuildLabel '../escape' | Out-Null } catch { $refused = $true }
if (-not $refused) { throw 'Invalid package label accepted.' }
Write-Output "PASS package: $($entries.Count) entries, complete manifest hashes, provenance, required install/docs files and overwrite/path refusal."
