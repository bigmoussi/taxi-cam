Set-StrictMode -Version Latest

function Test-TaxiAmd64Image([string]$Path, [bool]$Dll = $true) {
    # Inspect bounded PE headers only. Never load or execute a selected simulator file.
    $stream = $null; $reader = $null
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        $reader = New-Object IO.BinaryReader($stream)
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) { return $false }
        $stream.Position = 60
        $offset = $reader.ReadUInt32()
        if ($offset -lt 64 -or $offset -gt 1MB -or [long]$offset + 26 -gt $stream.Length) { return $false }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { return $false }
        $sections = $reader.ReadUInt16()
        $stream.Position = [long]$offset + 20
        $optionalSize = $reader.ReadUInt16()
        $flags = $reader.ReadUInt16()
        if ($sections -lt 1 -or $sections -gt 96 -or $optionalSize -lt 112 -or $optionalSize -gt 4096 -or
            [long]$offset + 24 + $optionalSize + 40 * $sections -gt $stream.Length -or
            ($flags -band 2) -eq 0 -or (($flags -band 0x2000) -ne 0) -ne $Dll) { return $false }
        return $reader.ReadUInt16() -eq 0x20B
    } catch { return $false }
    finally {
        if ($reader) { $reader.Dispose() }
        elseif ($stream) { $stream.Dispose() }
    }
}

function Get-TaxiPrerequisiteIssues([string]$SimulatorDirectory, [string]$SystemDirectory = [Environment]::SystemDirectory) {
    if (-not [Environment]::Is64BitOperatingSystem -or [Environment]::OSVersion.Version.Major -lt 10) {
        'Taxi Cam requires 64-bit Windows 10 or Windows 11.'
    }
    if (-not [Environment]::Is64BitProcess -or $PSVersionTable.PSVersion -lt [version]'5.1') {
        'Setup requires 64-bit Windows PowerShell 5.1 or later. Restore the Windows PowerShell component and run setup again.'
    }
    foreach ($name in @('d3d12.dll', 'dxgi.dll', 'd3dcompiler_47.dll', 'ucrtbase.dll')) {
        if (-not (Test-TaxiAmd64Image (Join-Path $SystemDirectory $name))) {
            "Required Windows component is missing, unreadable or not x64: $name. Repair Windows and install Windows updates."
        }
    }
    if (-not (Test-TaxiAmd64Image (Join-Path $SystemDirectory 'WindowsPowerShell/v1.0/powershell.exe') $false)) {
        'Windows PowerShell is missing, unreadable or not x64. Restore the Windows PowerShell 5.1 component; setup and updates require it.'
    }
    # Xbox/MS Store installations can deny file-content reads even for a valid EXE.
    # Check presence here; the launcher verifies the loaded AMD64 image at connection time.
    if (-not (Test-Path -LiteralPath (Join-Path $SimulatorDirectory 'FlightSimulator2024.exe') -PathType Leaf)) {
        'Select the MSFS 2024 installation folder containing FlightSimulator2024.exe.'
    }
    if (-not (Test-TaxiAmd64Image (Join-Path $SimulatorDirectory 'SimConnect_internal.dll'))) {
        'SimConnect_internal.dll is missing, unreadable or not x64 in the selected MSFS folder. Verify or repair MSFS 2024; a separate SimConnect SDK installation is not needed.'
    }
    # These are imports of MSFS 2024's SimConnect client, not of Taxi Cam's static runtime.
    # Respect app-local DLL precedence: a wrong-architecture local copy can shadow a valid system copy.
    foreach ($name in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        $path = Join-Path $SimulatorDirectory $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $path = Join-Path $SystemDirectory $name }
        if (-not (Test-TaxiAmd64Image $path)) {
            "The MSFS SimConnect client requires the Microsoft Visual C++ v14 x64 runtime: $name is missing, unreadable or not x64. Install or repair it from https://aka.ms/vc14/vc_redist.x64.exe. If a bad copy is in the simulator folder, verify or repair MSFS 2024."
        }
    }
}

function Assert-TaxiPrerequisites([string]$SimulatorDirectory) {
    $issues = @(Get-TaxiPrerequisiteIssues -SimulatorDirectory $SimulatorDirectory)
    if ($issues.Count) { throw ("Required components are missing or invalid:`r`n- " + ($issues -join "`r`n- ")) }
}
