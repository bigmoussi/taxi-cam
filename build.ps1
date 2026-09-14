[CmdletBinding()]
param([switch]$Bootstrap, [switch]$Validate, [switch]$WarpOnly)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
& (Join-Path $PSScriptRoot 'build-native.ps1') -Bootstrap:$Bootstrap -Validate:$Validate -WarpOnly:$WarpOnly
