[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SimulatorDirectory, [string]$ExeXml)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'install-native.ps1') -SimulatorDirectory $SimulatorDirectory -ExeXml $ExeXml
