[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$dependency = (Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') | ConvertFrom-Json).'llvm-mingw'
$root = Join-Path $PSScriptRoot 'build/deps'
New-Item -ItemType Directory -Force -Path $root | Out-Null
$archive = Join-Path $root 'llvm-mingw.zip'
if (-not (Test-Path -LiteralPath $archive)) {
    $partial = $archive + '.partial'
    Invoke-WebRequest -Uri $dependency.url -OutFile $partial
    if ((Get-FileHash -LiteralPath $partial).Hash -ne $dependency.sha256) { throw 'Compiler archive hash mismatch.' }
    Move-Item -LiteralPath $partial -Destination $archive
}
if ((Get-FileHash -LiteralPath $archive).Hash -ne $dependency.sha256) { throw 'Cached compiler hash mismatch.' }
$destination = Join-Path $root $dependency.directory
if (-not (Test-Path -LiteralPath $destination)) { Expand-Archive -LiteralPath $archive -DestinationPath (Split-Path -Parent $destination) }
Write-Output 'Pinned compiler ready. The native build does not download ReShade or ImGui.'
