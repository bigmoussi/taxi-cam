[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer/validation_receipt.ps1')
$directory = Join-Path $PSScriptRoot 'build/native'
[void](Assert-TaxiNativeReceipt $directory)
& (Join-Path $directory 'native-smoke-validation.exe') (Join-Path $directory 'taxi-camera-bridge.dll')
if ($LASTEXITCODE -ne 0) { throw 'Native exact-DLL smoke failed.' }
