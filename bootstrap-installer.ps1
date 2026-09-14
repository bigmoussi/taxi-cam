[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$dependency = (Get-Content -Raw (Join-Path $PSScriptRoot 'dependencies.json') | ConvertFrom-Json).'inno-setup'
$root = Join-Path $PSScriptRoot 'build/deps'
$destination = Join-Path $root $dependency.directory
$archive = Join-Path $root "innosetup-$($dependency.version).exe"
New-Item -ItemType Directory -Force -Path $root | Out-Null
if (-not (Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest -Uri $dependency.url -OutFile ($archive + '.partial')
    if ((Get-FileHash -LiteralPath ($archive + '.partial') -Algorithm SHA256).Hash -ne $dependency.sha256) { throw 'Inno Setup download SHA256 mismatch.' }
    Move-Item -LiteralPath ($archive + '.partial') -Destination $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $dependency.sha256) { throw 'Cached Inno Setup SHA256 mismatch.' }
$compiler = Join-Path $destination 'ISCC.exe'
if (-not (Test-Path -LiteralPath $compiler)) {
    # Official portable mode disables registry integration, shortcuts and uninstall registration.
    $process = Start-Process -FilePath $archive -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER','/PORTABLE=1',('/DIR="' + $destination + '"')) -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $compiler)) { throw "Inno Setup portable extraction failed: $($process.ExitCode)" }
}
Write-Output $compiler
