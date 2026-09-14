Set-StrictMode -Version Latest
function Read-TaxiLaunchXml([string]$Path) {
    $document = [Xml.XmlDocument]::new()
    $document.PreserveWhitespace = $true
    $document.XmlResolver = $null
    if (Test-Path -LiteralPath $Path) {
        $settings = [Xml.XmlReaderSettings]::new()
        $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
        $settings.XmlResolver = $null
        $reader = [Xml.XmlReader]::Create($Path, $settings)
        try { $document.Load($reader) } finally { $reader.Dispose() }
        if ($document.DocumentElement.Name -ne 'SimBase.Document' -or $document.DocumentElement.GetAttribute('Type') -ne 'Launch') { throw 'Unrecognized exe.xml launch document.' }
    } else {
        $document.LoadXml('<?xml version="1.0" encoding="utf-8"?><SimBase.Document Type="Launch" version="1,0"><Descr>Launch</Descr><Filename>exe.xml</Filename><Disabled>False</Disabled><Launch.ManualLoad>False</Launch.ManualLoad></SimBase.Document>')
    }
    return ,$document
}
function Set-TaxiStartupEntry([Xml.XmlDocument]$Document, [string]$Executable, [string]$Simulator, [switch]$Remove) {
    if (-not $Remove -and (-not [IO.Path]::IsPathRooted($Executable) -or -not [IO.Path]::IsPathRooted($Simulator))) { throw 'Startup executable paths must be absolute.' }
    $root = $Document.DocumentElement
    $matches = @($root.SelectNodes('Launch.Addon') | Where-Object { $node=$_.SelectSingleNode('Name'); $null -ne $node -and $node.InnerText -eq '380 Taxi Cam' })
    if ($matches.Count -gt 1) { throw 'Multiple 380 Taxi Cam startup entries found; refusing an ambiguous update.' }
    if ($Remove) { foreach ($entry in $matches) { [void]$root.RemoveChild($entry) }; return }
    $entry = if ($matches.Count) { $matches[0] } else { $Document.CreateElement('Launch.Addon') }
    foreach ($pair in @(
        @('Name', '380 Taxi Cam'), @('Disabled', 'False'), @('ManualLoad', 'False'),
        @('Path', $Executable), @('CommandLine', ('--background --simulator "' + $Simulator + '"')), @('NewConsole', 'False')
    )) {
        $node = $entry.SelectSingleNode($pair[0])
        if (-not $node) { $node = $Document.CreateElement($pair[0]); [void]$entry.AppendChild($node) }
        $node.InnerText = $pair[1]
    }
    if (-not $entry.ParentNode) { [void]$root.AppendChild($entry) }
}
function Save-TaxiLaunchXml([Xml.XmlDocument]$Document,[string]$Path,[string]$ExpectedHash) {
    $absolute = [IO.Path]::GetFullPath($Path)
    if ([IO.Path]::GetFileName($absolute) -ine 'exe.xml') { throw 'Startup target must be named exe.xml.' }
    $parent = Split-Path -Parent $absolute
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    if (Test-Path -LiteralPath $absolute) {
        if (-not $ExpectedHash -or (Get-FileHash -LiteralPath $absolute).Hash -ne $ExpectedHash) { throw 'exe.xml changed during installation; no startup entry was written.' }
    } elseif ($ExpectedHash) { throw 'exe.xml disappeared during installation.' }
    $temporary = Join-Path $parent ('exe.xml.taxi-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $settings = [Xml.XmlWriterSettings]::new()
    $settings.Encoding = [Text.UTF8Encoding]::new($false)
    $settings.Indent = $true
    $settings.NewLineHandling = [Xml.NewLineHandling]::None
    $writer = [Xml.XmlWriter]::Create($temporary, $settings)
    try { $Document.Save($writer) } finally { $writer.Dispose() }
    [void](Read-TaxiLaunchXml $temporary)
    $backup = $null
    if (Test-Path -LiteralPath $absolute) {
        $backup = $absolute + '.taxi-backup-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
        Copy-Item -LiteralPath $absolute -Destination $backup
        if ((Get-FileHash -LiteralPath $backup).Hash -ne $ExpectedHash) { throw 'Startup backup verification failed.' }
        if ((Get-FileHash -LiteralPath $absolute).Hash -ne $ExpectedHash) { throw 'exe.xml changed before replacement.' }
        [IO.File]::Replace($temporary, $absolute, [NullString]::Value)
    } else { [IO.File]::Move($temporary, $absolute) }
    return $backup
}
