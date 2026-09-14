Set-StrictMode -Version Latest

function Get-TaxiVersion([string]$Repository, [int]$BuildNumber = 0) {
    if ($BuildNumber -lt 0) { throw 'Build number must not be negative.' }
    $config = Get-Content -Raw -LiteralPath (Join-Path $Repository 'version.json') | ConvertFrom-Json
    if ($config.version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') { throw 'version.json must contain a major.minor.patch version.' }
    $parts = @($Matches[1], $Matches[2], $Matches[3])
    foreach ($part in $parts) {
        $number = 0
        if (-not [int]::TryParse($part, [ref]$number) -or $number -gt 65535) { throw 'Windows version components must fit in 16 bits.' }
    }
    $shallow = & git -C $Repository rev-parse --is-shallow-repository
    if ($LASTEXITCODE -ne 0 -or $shallow -ne 'false') { throw 'Version calculation requires a full Git checkout.' }
    $history = @(& git -C $Repository log --first-parent '--format=%H' -- version.json)
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the version configuration history.' }
    $baseCommit = ''; $height = 0
    foreach ($commit in $history) {
        $prior = & git -C $Repository show "${commit}:version.json"
        if ($LASTEXITCODE -ne 0) { throw 'Could not read a historical version configuration.' }
        if (($prior -join "`n" | ConvertFrom-Json).version -cne $config.version) { break }
        $baseCommit = $commit
    }
    if ($baseCommit) {
        # A changed version value establishes the baseline. Formatting-only
        # edits do not reset it; subsequent first-parent commits advance patch.
        $count = & git -C $Repository rev-list --first-parent --count "$baseCommit..HEAD"
        if ($LASTEXITCODE -ne 0 -or -not [int]::TryParse($count, [ref]$height)) { throw 'Could not count commits since the version baseline.' }
    }
    $patch = [long]$parts[2] + $height
    if ($patch -gt 65535) { throw 'Patch version exceeds the Windows version limit; set a new minor version in version.json.' }
    [pscustomobject]@{
        Version = "$($parts[0]).$($parts[1]).$patch"
        Major = [int]$parts[0]; Minor = [int]$parts[1]; Patch = [int]$patch
        BuildNumber = $BuildNumber; BaseCommit = $baseCommit; CommitsSinceBase = $height
    }
}
