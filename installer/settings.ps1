Set-StrictMode -Version Latest

function Assert-TaxiSettingsClosed {
    # These files are shared by every installation for this Windows user.
    if (Get-Process -Name taxi-cam,380-taxi-cam -ErrorAction SilentlyContinue) {
        throw 'Exit every Taxi Cam companion before resetting or removing shared settings.'
    }
}

function Assert-TaxiSettingsPath([string]$Path, [string]$Root) {
    if (-not [IO.Path]::IsPathRooted($Path) -or -not [IO.Path]::IsPathRooted($Root)) { throw 'Settings paths must be absolute.' }
    $absolute = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetFullPath($Root).TrimEnd('\','/')
    if ($parent -eq [IO.Path]::GetPathRoot($parent).TrimEnd('\','/') -or
        -not $absolute.StartsWith($parent + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Settings path is outside its dedicated directory: $absolute"
    }
    # Check the file and every existing ancestor, including LOCALAPPDATA itself.
    # Never follow junctions/symlinks or treat a directory as a settings file.
    $cursor = $absolute
    while ($cursor) {
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if ($item) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Settings path contains a reparse point: $cursor" }
            if ($cursor -eq $absolute -and $item.PSIsContainer) { throw "Settings file is a directory: $absolute" }
            if ($cursor -ne $absolute -and -not $item.PSIsContainer) { throw "Settings parent is not a directory: $cursor" }
        } elseif (Test-Path -LiteralPath $cursor -ErrorAction Stop) { throw "Settings path could not be inspected: $cursor" }
        $next = [IO.Path]::GetDirectoryName($cursor)
        if ($next -eq $cursor) { break }
        $cursor = $next
    }
}

function Get-TaxiSettingsTargets([string]$Installation, [switch]$IncludeMount) {
    if (-not $env:LOCALAPPDATA -or -not [IO.Path]::IsPathRooted($env:LOCALAPPDATA)) { throw 'LOCALAPPDATA must identify an absolute settings directory.' }
    $local = [IO.Path]::GetFullPath($env:LOCALAPPDATA)
    $current = Join-Path $local 'Taxi Cam'
    $legacy = Join-Path $local '380 Taxi Cam'
    $targets = @()
    foreach ($name in @('settings.ini','hotkeys.ini','startup-state')) {
        $targets += [pscustomobject]@{path=(Join-Path $current $name);root=$current}
    }
    foreach ($key in @('fbw-a380x','ini-a350-900','ini-a350-1000','ini-a380')) {
        foreach ($folder in @($current,$legacy)) {
            $targets += [pscustomobject]@{path=(Join-Path $folder "profiles/$key.ini");root=$folder}
        }
    }
    if ($IncludeMount) {
        $installationRoot = [IO.Path]::GetFullPath($Installation)
        $targets += [pscustomobject]@{path=(Join-Path $installationRoot 'taxi-camera-mounts.cfg');root=$installationRoot}
    }
    foreach ($target in $targets) { Assert-TaxiSettingsPath $target.path $target.root }
    return $targets
}

function Get-TaxiSettingsHash($Entry) {
    Assert-TaxiSettingsPath $Entry.path $Entry.root
    if (Test-Path -LiteralPath $Entry.path -PathType Leaf) { return (Get-FileHash -LiteralPath $Entry.path -Algorithm SHA256).Hash }
    return ''
}

function New-TaxiSettingsSnapshot([string]$Installation, [string]$BackupDirectory, [switch]$IncludeMount) {
    $targets = @(Get-TaxiSettingsTargets -Installation $Installation -IncludeMount:$IncludeMount)
    $snapshot = @(); $index = 0
    foreach ($target in $targets) {
        $priorHash = Get-TaxiSettingsHash $target
        $backup = Join-Path $BackupDirectory ("settings-$index.backup"); $index++
        if ($priorHash) {
            Copy-Item -LiteralPath $target.path -Destination $backup
            if ((Get-FileHash -LiteralPath $backup).Hash -ne $priorHash -or (Get-TaxiSettingsHash $target) -ne $priorHash) {
                throw "Settings changed while taking the recovery snapshot: $($target.path)"
            }
        }
        $snapshot += [pscustomobject]@{path=$target.path;root=$target.root;backup=$backup;existed=[bool]$priorHash;priorHash=$priorHash;owned=$false;installedHash=''}
    }
    return $snapshot
}

function Remove-TaxiSettingsSnapshot([object[]]$Snapshot) {
    Assert-TaxiSettingsClosed
    # Preflight the entire explicit list before the first deletion.
    foreach ($entry in $Snapshot) {
        if ((Get-TaxiSettingsHash $entry) -ne $entry.priorHash) { throw "Settings changed before removal: $($entry.path)" }
    }
    foreach ($entry in $Snapshot) {
        if (-not $entry.existed) { continue }
        if ((Get-TaxiSettingsHash $entry) -ne $entry.priorHash) { throw "Settings changed during removal: $($entry.path)" }
        Remove-Item -LiteralPath $entry.path -ErrorAction Stop
        $entry.owned = $true
        $entry.installedHash = ''
    }
}

function Set-TaxiSettingsFile($Entry, [string]$Source) {
    $expected = if ($Entry.owned) { $Entry.installedHash } else { $Entry.priorHash }
    if ((Get-TaxiSettingsHash $Entry) -ne $expected) { throw "Settings changed before replacement: $($Entry.path)" }
    $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    $temporary = Join-Path (Split-Path -Parent $Entry.path) ('taxi-settings-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        Copy-Item -LiteralPath $Source -Destination $temporary
        if ((Get-FileHash -LiteralPath $temporary).Hash -ne $sourceHash -or (Get-TaxiSettingsHash $Entry) -ne $expected) {
            throw "Settings changed while preparing replacement: $($Entry.path)"
        }
        if ($expected) { [IO.File]::Replace($temporary, $Entry.path, [NullString]::Value) }
        else { [IO.File]::Move($temporary, $Entry.path) }
        $Entry.owned = $true
        $Entry.installedHash = $sourceHash
    } finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) { Remove-Item -LiteralPath $temporary }
    }
}

function Restore-TaxiSettingsSnapshot([object[]]$Snapshot) {
    if (-not @($Snapshot | Where-Object owned).Count) { return }
    Assert-TaxiSettingsClosed
    $conflicts = @()
    foreach ($entry in $Snapshot) {
        if (-not $entry.owned) { continue }
        try {
            if ((Get-TaxiSettingsHash $entry) -ne $entry.installedHash) { throw 'changed by another writer' }
            if ($entry.existed) { Set-TaxiSettingsFile $entry $entry.backup }
            elseif (Test-Path -LiteralPath $entry.path -PathType Leaf) { Remove-Item -LiteralPath $entry.path }
            $entry.owned = $false
        } catch { $conflicts += $entry.path }
    }
    if ($conflicts.Count) { throw ('Settings rollback preserved files changed by another writer or unsafe path: ' + ($conflicts -join ', ')) }
}

function Get-TaxiCameraRateTargets {
    if (-not $env:LOCALAPPDATA -or -not [IO.Path]::IsPathRooted($env:LOCALAPPDATA)) { throw 'LOCALAPPDATA must identify an absolute settings directory.' }
    $local = [IO.Path]::GetFullPath($env:LOCALAPPDATA)
    $current = Join-Path $local 'Taxi Cam'
    $legacy = Join-Path $local '380 Taxi Cam'
    $targets = @([pscustomobject]@{path=(Join-Path $current 'settings.ini');root=$current})
    foreach ($key in @('fbw-a380x','ini-a350-900','ini-a350-1000','ini-a380')) {
        foreach ($folder in @($current,$legacy)) {
            $targets += [pscustomobject]@{path=(Join-Path $folder "profiles/$key.ini");root=$folder}
        }
    }
    foreach ($target in $targets) { Assert-TaxiSettingsPath $target.path $target.root }
    return $targets
}

function Read-TaxiIniFile([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
        return [pscustomobject]@{
            encoding = 'utf16'
            text = [Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
        }
    }
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        return [pscustomobject]@{
            encoding = 'utf8bom'
            text = [Text.Encoding]::UTF8.GetString($bytes, 3, $bytes.Length - 3)
        }
    }
    return [pscustomobject]@{
        encoding = 'ansi'
        text = [Text.Encoding]::Default.GetString($bytes)
    }
}

function Write-TaxiIniFile([string]$Path, [string]$EncodingName, [string]$Text) {
    $bytes = switch ($EncodingName) {
        'utf16' { [Text.Encoding]::Unicode.GetPreamble() + [Text.Encoding]::Unicode.GetBytes($Text) }
        'utf8bom' { [Text.Encoding]::UTF8.GetPreamble() + [Text.Encoding]::UTF8.GetBytes($Text) }
        default { [Text.Encoding]::Default.GetBytes($Text) }
    }
    [IO.File]::WriteAllBytes($Path, $bytes)
}

function Get-TaxiIniKey([string]$Path, [string]$Section, [string]$Key) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $inSection = $false
    foreach ($line in [regex]::Split((Read-TaxiIniFile $Path).text, '\r\n|\n')) {
        $trim = $line.Trim()
        if ($trim.StartsWith('[') -and $trim.EndsWith(']') -and $trim.Length -ge 2) {
            $inSection = $trim.Substring(1, $trim.Length - 2).Trim() -ieq $Section
            continue
        }
        if (-not $inSection -or $trim.StartsWith(';') -or $trim.StartsWith('#') -or -not $trim.Contains('=')) { continue }
        $separator = $trim.IndexOf('=')
        if ($trim.Substring(0, $separator).Trim() -ieq $Key) { return $trim.Substring($separator + 1).Trim() }
    }
    return $null
}

function Set-TaxiIniKey([string]$Path, [string]$Section, [string]$Key, [string]$Value) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Settings file is missing: $Path" }
    $loaded = Read-TaxiIniFile $Path
    $newline = if ($loaded.text.Contains("`r`n")) { "`r`n" } elseif ($loaded.text.Contains("`n")) { "`n" } else { "`r`n" }
    $lines = New-Object System.Collections.Generic.List[string]
    if ($loaded.text.Length) {
        foreach ($line in [regex]::Split($loaded.text, '\r\n|\n')) { [void]$lines.Add($line) }
    }
    $sectionIndex = -1
    $keyIndex = -1
    $inSection = $false
    for ($index = 0; $index -lt $lines.Count; $index++) {
        $trim = $lines[$index].Trim()
        if ($trim.StartsWith('[') -and $trim.EndsWith(']') -and $trim.Length -ge 2) {
            $inSection = $trim.Substring(1, $trim.Length - 2).Trim() -ieq $Section
            if ($inSection) { $sectionIndex = $index }
            continue
        }
        if (-not $inSection -or $trim.StartsWith(';') -or $trim.StartsWith('#') -or -not $trim.Contains('=')) { continue }
        $separator = $trim.IndexOf('=')
        if ($trim.Substring(0, $separator).Trim() -ieq $Key) { $keyIndex = $index }
    }
    $assignment = $Key + '=' + $Value
    if ($keyIndex -ge 0) { $lines[$keyIndex] = $assignment }
    elseif ($sectionIndex -ge 0) { $lines.Insert($sectionIndex + 1, $assignment) }
    else {
        if ($lines.Count -and $lines[$lines.Count - 1].Length) { [void]$lines.Add('') }
        [void]$lines.Add(('[' + $Section + ']'))
        [void]$lines.Add($assignment)
    }
    Write-TaxiIniFile $Path $loaded.encoding ([string]::Join($newline, $lines))
}

function New-TaxiCameraRateSnapshot([string]$BackupDirectory) {
    $snapshot = @(); $index = 0
    foreach ($target in @(Get-TaxiCameraRateTargets)) {
        $priorHash = Get-TaxiSettingsHash $target
        $backup = Join-Path $BackupDirectory ("camera-rate-$index.backup"); $index++
        if ($priorHash) {
            Copy-Item -LiteralPath $target.path -Destination $backup
            if ((Get-FileHash -LiteralPath $backup).Hash -ne $priorHash -or (Get-TaxiSettingsHash $target) -ne $priorHash) {
                throw "Settings changed while taking the camera-rate snapshot: $($target.path)"
            }
        }
        $snapshot += [pscustomobject]@{
            path=$target.path; root=$target.root; backup=$backup; existed=[bool]$priorHash
            priorHash=$priorHash; owned=$false; installedHash=''
        }
    }
    return $snapshot
}

function Set-TaxiForcedCameraRate([object[]]$Snapshot, [int]$Rate = 5) {
    if ($Rate -lt 5 -or $Rate -gt 60) { throw "Camera rate $Rate is outside 5-60." }
    foreach ($entry in $Snapshot) {
        if (-not $entry.existed) { continue }
        $expected = if ($entry.owned) { $entry.installedHash } else { $entry.priorHash }
        if ((Get-TaxiSettingsHash $entry) -ne $expected) { throw "Settings changed before camera rate update: $($entry.path)" }
        $temporary = Join-Path (Split-Path -Parent $entry.path) ('taxi-camera-rate-' + [Guid]::NewGuid().ToString('N') + '.tmp')
        try {
            Copy-Item -LiteralPath $entry.path -Destination $temporary
            Set-TaxiIniKey $temporary 'display' 'camera_rate' ([string]$Rate)
            $sourceHash = (Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash
            if ((Get-TaxiSettingsHash $entry) -ne $expected) { throw "Settings changed while preparing camera rate update: $($entry.path)" }
            if ($expected) { [IO.File]::Replace($temporary, $entry.path, [NullString]::Value) }
            else { [IO.File]::Move($temporary, $entry.path) }
            $entry.owned = $true
            $entry.installedHash = $sourceHash
        } finally {
            if (Test-Path -LiteralPath $temporary -PathType Leaf) { Remove-Item -LiteralPath $temporary }
        }
    }
}
