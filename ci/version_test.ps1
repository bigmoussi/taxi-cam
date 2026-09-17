$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'version.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $repo ('build/version-tests/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $fixture | Out-Null
$checks = 0
function Invoke-TestGit([string[]]$Arguments) {
    $output = @(& git -C $fixture -c user.name='Taxi Cam version tests' -c user.email='version-tests@example.invalid' -c commit.gpgSign=false -c core.hooksPath=disabled-test-hooks @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "Version fixture Git command failed: $($Arguments[0])" }
    return $output
}
function Commit-Fixture([string]$Message) {
    $null = Invoke-TestGit @('add','--all')
    $null = Invoke-TestGit @('commit','--quiet','-m',$Message)
}
function Write-Version([string]$Version) {
    @{version=$Version} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $fixture 'version.json') -Encoding ascii
}
function Assert-Version([string]$Expected, [int]$BuildNumber = 10) {
    $actual = Get-TaxiVersion $fixture $BuildNumber
    if ($actual.Version -ne $Expected -or $actual.BuildNumber -ne $BuildNumber) { throw "Expected $Expected, got $($actual.Version)." }
    $script:checks++
}
function Reject-Version {
    $rejected = $false
    try { Get-TaxiVersion $fixture | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw 'Invalid version was accepted.' }
    $script:checks++
}
$null = Invoke-TestGit @('init','--quiet','-b','main')
$null = Invoke-TestGit @('commit','--quiet','--allow-empty','-m','Initial fixture')
Write-Version '0.8.1'
Assert-Version '0.8.1' 0
Commit-Fixture 'Set version baseline'
Assert-Version '0.8.1'
'First change' | Set-Content -LiteralPath (Join-Path $fixture 'change.txt')
Commit-Fixture 'Change app'
Assert-Version '0.8.2'
Assert-Version '0.8.2' 99
'{"version":"0.8.1"}' | Set-Content -LiteralPath (Join-Path $fixture 'version.json') -Encoding ascii
Commit-Fixture 'Reformat version configuration'
Assert-Version '0.8.3'
$null = Invoke-TestGit @('switch','--quiet','-c','feature')
foreach ($number in 1..2) {
    "Feature $number" | Set-Content -LiteralPath (Join-Path $fixture 'change.txt')
    Commit-Fixture "Feature change $number"
}
$null = Invoke-TestGit @('switch','--quiet','main')
$null = Invoke-TestGit @('merge','--quiet','--no-ff','feature','-m','Merge feature')
Assert-Version '0.8.4'
# A version change arriving through a merge establishes its baseline on main.
$null = Invoke-TestGit @('switch','--quiet','-c','minor-release')
Write-Version '0.9.0'
Commit-Fixture 'Start minor release'
'Minor release change' | Set-Content -LiteralPath (Join-Path $fixture 'change.txt')
Commit-Fixture 'Finish minor release'
$null = Invoke-TestGit @('switch','--quiet','main')
$null = Invoke-TestGit @('merge','--quiet','--no-ff','minor-release','-m','Release new minor version')
Assert-Version '0.9.0'
'Documentation change' | Set-Content -LiteralPath (Join-Path $fixture 'docs.txt')
Commit-Fixture 'Update docs'
Assert-Version '0.9.1'
# Semantic version is stamped from version.json first-parent height at build
# time, not from the last publish tag. A later tag on main or a higher tag
# off main must not change the compiled version.
$null = Invoke-TestGit @('tag','v0.9.1-build.38')
Assert-Version '0.9.1'
$null = Invoke-TestGit @('switch','--quiet','-c','off-main-tag')
$null = Invoke-TestGit @('tag','v9.9.9-build.99')
$null = Invoke-TestGit @('switch','--quiet','main')
Assert-Version '0.9.1'
Write-Version '1.0.0'
Assert-Version '1.0.0'
Commit-Fixture 'Start major release'
Assert-Version '1.0.0'
foreach ($invalid in @('01.2.3','1.2','1.2.3-preview','1.2.3+build.4','1.2.-1','1.2.65536','999999999999.0.0')) {
    Write-Version $invalid
    Reject-Version
}
Write-Version '0.0.65535'
Assert-Version '0.0.65535'
Commit-Fixture 'Windows version limit'
'Overflow' | Set-Content -LiteralPath (Join-Path $fixture 'change.txt')
Commit-Fixture 'One more patch'
Reject-Version
Write-Output "PASS semantic versions: $checks checks for patch progression, stable reruns, merge history, minor/major baselines and Windows limits."
