[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'standalone/validation_receipt.ps1')
$payload = Join-Path $PSScriptRoot 'build/native'
[void](Assert-TaxiNativeReceipt $payload)
$tag = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$package = Join-Path $PSScriptRoot ('build/packages/380-taxi-cam-0.8.0-' + $tag)
New-Item -ItemType Directory -Path $package -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $package 'standalone') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $package 'licenses') -Force | Out-Null
foreach ($file in @('380-taxi-cam.exe','taxi-camera-bridge.dll','validation.json')) {
    Copy-Item -LiteralPath (Join-Path $payload $file) -Destination (Join-Path $package $file)
}
foreach ($file in @('install-native.ps1','uninstall-native.ps1','README.md','taxi-camera-mounts.cfg','THIRD_PARTY_NOTICES.md',
    'standalone/exe_xml.ps1','standalone/validation_receipt.ps1','licenses/LLVM.txt','licenses/ReShade.txt','licenses/Dear-ImGui.txt')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $package $file)
}
Get-ChildItem -LiteralPath $package -File -Recurse | ForEach-Object {
    [ordered]@{file=$_.FullName.Substring($package.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $package 'manifest.json') -Encoding utf8
Compress-Archive -LiteralPath $package -DestinationPath ($package + '.zip')
Write-Output ($package + '.zip')
