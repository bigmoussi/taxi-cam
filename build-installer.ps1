[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Package)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$zip = (Resolve-Path -LiteralPath $Package).Path
$base = [IO.Path]::GetFileNameWithoutExtension($zip)
if ($base -notmatch '^380-taxi-cam-(\d+\.\d+\.\d+)-build\.(\d+)-windows-x64$' -or [IO.Path]::GetExtension($zip) -ne '.zip') { throw 'Expected a versioned build.N Windows x64 release ZIP.' }
$version = $Matches[1]; $buildNumber = [int]$Matches[2]
$work = Join-Path $PSScriptRoot ('build/installer/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null
# Expand-Archive rejects traversal entries; use a fresh directory for each immutable input.
Expand-Archive -LiteralPath $zip -DestinationPath $work
$payload = Join-Path $work $base
if (-not (Test-Path -LiteralPath $payload -PathType Container)) { throw 'ZIP payload directory does not match its asset name.' }
. (Join-Path $PSScriptRoot 'standalone/validation_receipt.ps1')
$receipt = Assert-TaxiNativeReceipt (Join-Path $PSScriptRoot 'build/native')
$info = Get-Content -Raw -LiteralPath ([IO.Path]::ChangeExtension($zip, '.build-info.json')) | ConvertFrom-Json
if ($receipt.version -ne $version -or $receipt.buildNumber -ne $buildNumber -or $info.buildNumber -ne $buildNumber -or $info.sourceCommit -notmatch '^[0-9a-fA-F]{40}$' -or $info.version -ne $version -or $info.build -ne "build.$buildNumber") { throw 'Package version, build number or source provenance does not match.' }
$manifest = @(Get-Content -Raw -LiteralPath ([IO.Path]::ChangeExtension($zip, '.manifest.json')) | ConvertFrom-Json)
$allowed = @('380-taxi-cam.exe','taxi-camera-bridge.dll','taxi-camera-mounts.cfg','THIRD_PARTY_NOTICES.txt')
if ($manifest.Count -ne $allowed.Count) { throw 'The runtime package must contain exactly four required files.' }
$seen = @{}
foreach ($entry in $manifest) {
    $relative = [string]$entry.file
    if ($relative -notin $allowed) { throw "Unexpected runtime package file: $relative" }
    if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[/\\])\.\.([/\\]|$)' -or $relative.Contains(':') -or $seen.ContainsKey($relative)) { throw 'Unsafe or duplicate package manifest path.' }
    $seen[$relative] = $true
    if ((Get-FileHash -LiteralPath (Join-Path $payload $relative) -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Package manifest mismatch: $relative" }
}
foreach ($file in Get-ChildItem -LiteralPath $payload -File -Recurse) {
    $relative = $file.FullName.Substring($payload.Length + 1).Replace('\','/')
    if (-not $seen.ContainsKey($relative)) { throw "Unmanifested package file: $relative" }
}
foreach ($name in @('380-taxi-cam.exe','taxi-camera-bridge.dll')) {
    if ((Get-FileHash -LiteralPath (Join-Path $payload $name)).Hash -ne $receipt.files.PSObject.Properties[$name].Value) { throw "Package binary differs from validated build: $name" }
}
New-Item -ItemType Directory -Path (Join-Path $payload 'standalone') | Out-Null
foreach ($name in @('install-native.ps1','uninstall-native.ps1','standalone/exe_xml.ps1','standalone/validation_receipt.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $payload $name)
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build/native/validation.json') -Destination $payload
# The uninstaller retains these scripts in compiled Pascal strings, never as installed loose files.
& (Join-Path $PSScriptRoot 'installer/embed-uninstaller.ps1') -Output (Join-Path $work 'uninstall-scripts.iss') -RuntimeScript (Join-Path $PSScriptRoot 'installer/runtime.ps1') -UninstallScript (Join-Path $PSScriptRoot 'uninstall-native.ps1') -ExeXmlScript (Join-Path $PSScriptRoot 'standalone/exe_xml.ps1')
$compiler = & (Join-Path $PSScriptRoot 'bootstrap-installer.ps1')
$output = Join-Path $PSScriptRoot 'build/packages'
$asset = Join-Path $output ($base + '-setup.exe')
if (Test-Path -LiteralPath $asset) { throw 'Installer already exists; retain release assets and use a new build.' }
$log = Join-Path $work 'compiler.log'
& $compiler "/DPayloadDir=$payload" "/DInternalDir=$work" "/DAppVersion=$version" "/DBuildNumber=$buildNumber" "/DOutputBase=$base-setup" "/O$output" (Join-Path $PSScriptRoot 'installer/380-taxi-cam.iss') *> $log
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $asset)) { throw "Installer compilation failed. See $log" }
[ordered]@{version=$version;buildNumber=$buildNumber;sourceCommit=$info.sourceCommit;sourceDirty=$info.sourceDirty;installerSha256=(Get-FileHash -LiteralPath $asset).Hash;packageSha256=(Get-FileHash -LiteralPath $zip).Hash;files=$receipt.files;compilerPayload=$payload;compilerInternal=$work} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath ($asset + '.json') -Encoding utf8
Write-Output $asset
