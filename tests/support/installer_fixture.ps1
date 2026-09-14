function New-TaxiFixtureImage([string]$Path, [bool]$Dll = $true, [int]$Machine = 0x8664) {
    # Header fixture only. It has no executable code and must never be loaded or launched.
    $bytes = New-Object byte[] 512
    [BitConverter]::GetBytes([uint16]0x5A4D).CopyTo($bytes, 0)
    [BitConverter]::GetBytes([uint32]128).CopyTo($bytes, 60)
    [BitConverter]::GetBytes([uint32]0x4550).CopyTo($bytes, 128)
    [BitConverter]::GetBytes([uint16]$Machine).CopyTo($bytes, 132)
    [BitConverter]::GetBytes([uint16]1).CopyTo($bytes, 134)
    [BitConverter]::GetBytes([uint16]240).CopyTo($bytes, 148)
    $flags = if ($Dll) { 0x2002 } else { 2 }
    [BitConverter]::GetBytes([uint16]$flags).CopyTo($bytes, 150)
    [BitConverter]::GetBytes([uint16]0x20B).CopyTo($bytes, 152)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [IO.File]::WriteAllBytes($Path, $bytes)
}
