[CmdletBinding()]
param([switch]$LegacyReShade)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $LegacyReShade) { & (Join-Path $PSScriptRoot 'bootstrap-native.ps1'); return }
$dependencyRoot = Join-Path $PSScriptRoot 'build/deps'
$dependencies = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') | ConvertFrom-Json
New-Item -ItemType Directory -Path $dependencyRoot -Force | Out-Null

foreach ($entry in $dependencies.PSObject.Properties) {
    $dependency = $entry.Value
    $archive = Join-Path $dependencyRoot ($entry.Name + '.zip')
    $destination = Join-Path $dependencyRoot $dependency.directory
    if (-not (Test-Path -LiteralPath $archive)) {
        Write-Output "Downloading $($entry.Name) $($dependency.version)"
        $partial = $archive + '.partial'
        Invoke-WebRequest -Uri $dependency.url -OutFile $partial
        $digest = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash
        if ($digest -ne $dependency.sha256) {
            throw "SHA256 mismatch for $($entry.Name); the download was not extracted."
        }
        Move-Item -LiteralPath $partial -Destination $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $dependency.sha256) {
        throw "SHA256 mismatch for cached $archive; the archive was not extracted."
    }
    if (-not (Test-Path -LiteralPath $destination)) {
        Write-Output "Extracting $($entry.Name)"
        Expand-Archive -LiteralPath $archive -DestinationPath (Split-Path -Parent $destination)
    }
}

Write-Output 'Pinned dependencies are available under build/deps. No system tools or simulator files were changed.'
