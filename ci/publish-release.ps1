[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Package,
    [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')][string]$Repository,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$Commit,
    [Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$BuildNumber
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:GITHUB_REF -ne 'refs/heads/main') { throw 'Release publication runs only in the main-branch workflow.' }
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'standalone/validation_receipt.ps1')
$receiptPath = Join-Path $root 'build/native/validation.json'
$receipt = Assert-TaxiNativeReceipt (Split-Path -Parent $receiptPath)
$packagePath = (Resolve-Path -LiteralPath $Package).Path
$tag = "v$($receipt.version)-build.$BuildNumber"
$title = "380 Taxi Cam $($receipt.version) - Build $BuildNumber"
function Invoke-Gh([string[]]$Arguments) {
    $result = @(& gh @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "GitHub operation failed: $($Arguments[0])" }
    return ($result -join "`n")
}
$head = & git rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $head -ne $Commit) { throw 'Release target must be the exact checked-out commit.' }
$info = Get-Content -Raw -LiteralPath (Join-Path (Split-Path -Parent $packagePath) ([IO.Path]::GetFileNameWithoutExtension($packagePath) + '/build-info.json')) | ConvertFrom-Json
if ($info.sourceCommit -ne $Commit -or $info.build -ne "build.$BuildNumber") { throw 'Package provenance does not match this workflow.' }

$releases = @(Invoke-Gh -Arguments @('api',"repos/$Repository/releases?per_page=100",'--paginate','--slurp','--jq','add') | ConvertFrom-Json)
$existing = $releases | Where-Object { $_.tag_name -eq $tag } | Select-Object -First 1
if ($existing) {
    if ($existing.target_commitish -ne $Commit) { throw 'Existing release targets a different commit.' }
    if (-not $existing.draft) {
        Write-Output "Release already published; preserving its assets: $($existing.html_url)"
        "Release: $($existing.html_url)" >> $env:GITHUB_STEP_SUMMARY
        return
    }
}
# Only use published ancestor releases as the notes baseline. Direct pushes are
# listed explicitly because GitHub's generated notes primarily describe PRs.
$previous = $null
foreach ($candidate in @($releases | Where-Object { -not $_.draft -and $_.tag_name -match '^v[0-9].*-build\.[0-9]+$' -and $_.tag_name -ne $tag } | Sort-Object published_at -Descending)) {
    # Checkout has no persisted credentials. Fetch only this tag through gh's
    # credential helper, scoped to this command; no token is written to config.
    & git -c 'credential.helper=!gh auth git-credential' fetch origin "refs/tags/$($candidate.tag_name):refs/tags/$($candidate.tag_name)"
    if ($LASTEXITCODE -ne 0) { throw 'Could not fetch release baseline tag.' }
    & git merge-base --is-ancestor $candidate.tag_name $Commit
    if ($LASTEXITCODE -eq 0) { $previous = $candidate.tag_name; break }
    if ($LASTEXITCODE -ne 1) { throw 'Could not check release ancestry.' }
}
$range = if ($previous) { "$previous..$Commit" } else { $Commit }
$commits = @(& git log '--format=%H%x09%s' --reverse $range)
if ($LASTEXITCODE -ne 0) { throw 'Could not collect release commits.' }
$runUrl = "$env:GITHUB_SERVER_URL/$Repository/actions/runs/$env:GITHUB_RUN_ID"
$notes = @(
    "Windows x64 native package from commit [$($Commit.Substring(0,7))](https://github.com/$Repository/commit/$Commit).",
    '',
    'Download the ZIP, extract it, and run install-native.ps1 with MSFS closed. SHA256SUMS.txt covers the ZIP.',
    '',
    'Validation: strict native build, software D3D12 (WARP), smoke, IPC, camera lifecycle, graphics-state, installer and package checks. Hardware GPU and live MSFS checks are not performed on the hosted runner.',
    '',
    "[Build and validation logs]($runUrl)",
    '',
    '## Commits',
    ''
)
foreach ($line in $commits) {
    $parts = $line -split "`t",2
    $subject = [System.Net.WebUtility]::HtmlEncode($parts[1])
    $notes += "- $subject ([$($parts[0].Substring(0,7))](https://github.com/$Repository/commit/$($parts[0])))"
}
$notesPath = Join-Path $root 'build/release-notes.md'
$notes | Set-Content -LiteralPath $notesPath -Encoding utf8
$checksum = Join-Path $root 'build/SHA256SUMS.txt'
"$((Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($packagePath))" |
    Set-Content -LiteralPath $checksum -Encoding ascii
if (-not $existing) {
    $arguments = @('release','create',$tag,'--repo',$Repository,'--target',$Commit,'--title',$title,'--draft',
        '--notes-file',$notesPath,'--generate-notes')
    if ($previous) { $arguments += @('--notes-start-tag',$previous) }
    [void](Invoke-Gh -Arguments $arguments)
}
# Keep the release a draft until all assets are uploaded. Reruns repair drafts
# but never replace a published release's assets.
[void](Invoke-Gh -Arguments @('release','upload',$tag,$packagePath,$checksum,$receiptPath,'--repo',$Repository,'--clobber'))
$main = Invoke-Gh -Arguments @('api',"repos/$Repository/commits/main",'--jq','.sha')
$latest = if ($main -eq $Commit) { '--latest=true' } else { '--latest=false' }
[void](Invoke-Gh -Arguments @('release','edit',$tag,'--repo',$Repository,'--draft=false',$latest))
$url = Invoke-Gh -Arguments @('release','view',$tag,'--repo',$Repository,'--json','url','--jq','.url')
Write-Output "Published $url"
"Release: $url" >> $env:GITHUB_STEP_SUMMARY
