$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'release-notes.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $repo ('build/release-note-tests/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $fixture | Out-Null
$checks = 0
function Invoke-TestGit([string[]]$Arguments) {
    $output = @(& git -C $fixture -c user.name='Taxi Cam release note tests' -c user.email='release-notes-tests@example.invalid' -c commit.gpgSign=false -c core.hooksPath=disabled-test-hooks @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "Release-note fixture Git command failed: $($Arguments[0])" }
    return ,$output
}
function Get-FixtureCommit([string]$Rev) {
    $sha = [string](& git -C $fixture rev-parse $Rev)
    if ($LASTEXITCODE -ne 0 -or $sha -notmatch '^[0-9a-fA-F]{40}$') { throw "Fixture revision $Rev was not a commit." }
    return $sha
}
function Commit-Fixture([string]$Subject, [string]$Body = '') {
    $null = Invoke-TestGit @('add','--all')
    $arguments = @('commit','--quiet','-m',$Subject)
    if ($Body) { $arguments += @('-m',$Body) }
    $null = Invoke-TestGit $arguments
}
function Merge-Fixture([string]$Branch, [string]$Subject, [string]$Body) {
    $arguments = @('merge','--quiet','--no-ff',$Branch,'-m',$Subject)
    if ($Body) { $arguments += @('-m',$Body) }
    $null = Invoke-TestGit $arguments
}
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function Assert-Entry([object[]]$Entries, [string]$Title, [int]$Number = 0) {
    $match = @($Entries | Where-Object {
        $_.Title -ceq $Title -and [int]$_.Number -eq $Number
    })
    if ($match.Count -ne 1) { throw "Expected one entry '$Title' (#$Number); got $($Entries.Count) entries." }
    $script:checks++
    return $match[0]
}
function Assert-Absent([object[]]$Entries, [string]$Pattern) {
    $hit = @($Entries | Where-Object { $_.Title -match $Pattern -or $_.Summary -match $Pattern })
    if ($hit.Count -ne 0) { throw "Unexpected release-note entry matching $Pattern." }
    $script:checks++
}

Assert-True (Test-TaxiReleaseChangeIsNoise 'ci: skip documentation releases' @('src/app/companion.cpp')) `
    'Conventional ci: subjects are unpublished CI noise.'
Assert-True (Test-TaxiReleaseChangeIsNoise 'Gate publication' @('.github/workflows/release.yml','ci/publish-release.ps1','docs/releases.md')) `
    'Workflow, publish-script and process-doc paths are CI noise.'
Assert-True (-not (Test-TaxiReleaseChangeIsNoise 'fix: restore owned views' @('src/camera/probe.cpp','docs/releases.md'))) `
    'User-facing source changes are kept even when process docs also change.'

$null = Invoke-TestGit @('init','--quiet','-b','main')
$null = Invoke-TestGit @('commit','--quiet','--allow-empty','-m','Initial fixture')
$published = Get-FixtureCommit 'HEAD'
$null = Invoke-TestGit @('tag','v0.8.0-build.1')

$null = Invoke-TestGit @('switch','--quiet','-c','feature-overlay')
New-Item -ItemType Directory -Force -Path (Join-Path $fixture 'src') | Out-Null
'overlay' | Set-Content -LiteralPath (Join-Path $fixture 'src/app.txt') -Encoding ascii
Commit-Fixture 'feat: add overlay detail'
$null = Invoke-TestGit @('switch','--quiet','main')
Merge-Fixture 'feature-overlay' 'Merge pull request #10 from example/feature-overlay' 'feat: add taxi overlay'

$null = Invoke-TestGit @('switch','--quiet','-c','ci-workflow')
New-Item -ItemType Directory -Force -Path (Join-Path $fixture '.github/workflows') | Out-Null
'name: ci' | Set-Content -LiteralPath (Join-Path $fixture '.github/workflows/ci.yml') -Encoding ascii
Commit-Fixture 'ci: tweak hosted workflow'
$null = Invoke-TestGit @('switch','--quiet','main')
Merge-Fixture 'ci-workflow' 'Merge pull request #11 from example/ci-workflow' 'ci: tweak hosted workflow'

$null = Invoke-TestGit @('switch','--quiet','-c','gate')
New-Item -ItemType Directory -Force -Path (Join-Path $fixture 'ci'), (Join-Path $fixture 'docs') | Out-Null
'publish' | Set-Content -LiteralPath (Join-Path $fixture 'ci/publish.ps1') -Encoding ascii
'process' | Set-Content -LiteralPath (Join-Path $fixture 'docs/releases.md') -Encoding ascii
Commit-Fixture 'ci: gate publication'
$null = Invoke-TestGit @('switch','--quiet','main')
Merge-Fixture 'gate' 'Merge pull request #12 from example/gate' 'Gate publication with approval'

'offset' | Set-Content -LiteralPath (Join-Path $fixture 'src/app.txt') -Encoding ascii
Commit-Fixture 'fix: correct mount offset'

'night' | Set-Content -LiteralPath (Join-Path $fixture 'src/app.txt') -Encoding ascii
Commit-Fixture 'feat: add night exposure (#13)'

$null = Invoke-TestGit @('switch','--quiet','-c','tracking')
New-Item -ItemType Directory -Force -Path (Join-Path $fixture '.github') | Out-Null
'noise' | Set-Content -LiteralPath (Join-Path $fixture '.github/noise.yml') -Encoding ascii
Commit-Fixture 'chore: tracking branch noise'
$null = Invoke-TestGit @('switch','--quiet','main')
Merge-Fixture 'tracking' "Merge remote-tracking branch 'origin/tracking'" ''

'dup' | Set-Content -LiteralPath (Join-Path $fixture 'src/app.txt') -Encoding ascii
Commit-Fixture 'feat: add night exposure again (#13)'

$head = Get-FixtureCommit 'HEAD'
$entries = @(Get-TaxiReleaseChangeEntries -Repository $fixture -FromRef 'v0.8.0-build.1' -ToCommit $head)
Assert-True ($entries.Count -eq 3) "Expected 3 rolled-up entries, got $($entries.Count)."
$null = Assert-Entry $entries 'feat: add taxi overlay' 10
$null = Assert-Entry $entries 'fix: correct mount offset'
$null = Assert-Entry $entries 'feat: add night exposure' 13
Assert-Absent $entries 'hosted workflow'
Assert-Absent $entries 'Gate publication'
Assert-Absent $entries 'remote-tracking'
Assert-True ((@($entries | Where-Object { $_.Number -eq 13 })).Count -eq 1) 'Duplicate pull-request numbers must collapse.'

$info = @{
    10 = [pscustomobject]@{
        number = 10
        title = 'Add taxi overlay'
        body = "Shows the taxi overlay on the PFD.`n`nValidation:`n- skip"
    }
}
$enriched = @(Get-TaxiReleaseChangeEntries -Repository $fixture -FromRef 'v0.8.0-build.1' -ToCommit $head -PullRequestInfo $info)
$overlay = Assert-Entry $enriched 'Add taxi overlay' 10
Assert-True ($overlay.Summary -ceq 'Shows the taxi overlay on the PFD.') 'Pull-request body should supply a short summary.'

$fromStart = @(Get-TaxiReleaseChangeEntries -Repository $fixture -ToCommit $head)
Assert-True ($fromStart.Count -ge 3) 'A first published release still rolls up first-parent history.'

$notes = New-TaxiReleaseNotes -Repository 'rthoms334/taxi-cam' -Commit ('a' * 40) -PreviousTag 'v0.8.0-build.1' `
    -BuildRunUrl 'https://github.com/rthoms334/taxi-cam/actions/runs/1' -Entries $enriched
$text = $notes -join "`n"
Assert-True ($text -match '## Changes') 'Release notes must include the changes heading.'
Assert-True ($text -match 'since `v0\.8\.0-build\.1`') 'Release notes must name the last published tag.'
Assert-True ($text -match 'Add taxi overlay') 'Release notes must include pull-request titles.'
Assert-True ($text -notmatch 'Gate publication') 'Release notes must omit CI-only merges.'
Assert-True ($text -match 'Full changelog') 'Release notes must link the compare range from the last published tag.'

$ciOnly = @(Get-TaxiReleaseChangeEntries -Repository $fixture -FromRef $published -ToCommit (Get-FixtureCommit 'gate'))
$emptyNotes = Format-TaxiReleaseChanges -Repository 'rthoms334/taxi-cam' -PreviousTag 'v0.8.0-build.1' -Commit $head -Entries @()
Assert-True ($ciOnly.Count -eq 1 -and $ciOnly[0].Number -eq 10) 'Range ending before later user commits keeps earlier merges.'
Assert-True (($emptyNotes -join "`n") -match 'No user-facing pull requests') 'An empty rollup must not invent features.'

Write-Output "PASS release note rollup: $checks checks for last-published range, pull-request titles, deduplication and CI-only exclusion."
