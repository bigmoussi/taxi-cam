[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$')][string]$BuildLabel,
    [ValidatePattern('^[0-9a-fA-F]{40}$')][string]$SourceCommit
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'standalone/validation_receipt.ps1')
$payload = Join-Path $PSScriptRoot 'build/native'
$receipt = Assert-TaxiNativeReceipt $payload
$label = if ($BuildLabel) { $BuildLabel } else { [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff') }
$package = Join-Path $PSScriptRoot ("build/packages/380-taxi-cam-$($receipt.version)-$label-windows-x64")
$zip = $package + '.zip'
if ((Test-Path -LiteralPath $package) -or (Test-Path -LiteralPath $zip)) { throw 'Package already exists; use a new build label.' }
New-Item -ItemType Directory -Path $package -Force | Out-Null
foreach ($directory in @('standalone','licenses','docs')) {
    New-Item -ItemType Directory -Path (Join-Path $package $directory) | Out-Null
}
foreach ($file in @('380-taxi-cam.exe','taxi-camera-bridge.dll','validation.json')) {
    Copy-Item -LiteralPath (Join-Path $payload $file) -Destination (Join-Path $package $file)
}
foreach ($file in @('install-native.ps1','uninstall-native.ps1','README.md','taxi-camera-mounts.cfg','THIRD_PARTY_NOTICES.md',
    'standalone/exe_xml.ps1','standalone/validation_receipt.ps1','licenses/LLVM.txt','licenses/ReShade.txt','licenses/Dear-ImGui.txt',
    'docs/architecture.md','docs/runtime-reference.md','docs/aircraft-profiles.md','docs/releases.md')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $package $file)
}
[ordered]@{
    version=$receipt.version; build=$label; sourceCommit=$SourceCommit;
    createdUtc=[DateTime]::UtcNow.ToString('o'); simulatorVerified=$receipt.simulatorVerified
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $package 'build-info.json') -Encoding utf8
Get-ChildItem -LiteralPath $package -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{file=$_.FullName.Substring($package.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $package 'manifest.json') -Encoding utf8
Compress-Archive -LiteralPath $package -DestinationPath $zip
Write-Output $zip
