[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Output, [Parameter(Mandatory=$true)][string]$RuntimeScript,
    [Parameter(Mandatory=$true)][string]$UninstallScript, [Parameter(Mandatory=$true)][string]$ExeXmlScript)
$ErrorActionPreference = 'Stop'
$embedded = @('procedure WriteUninstallScripts(Directory: String);', 'begin', '  ForceDirectories(Directory);')
foreach ($item in @(@('runtime.ps1', $RuntimeScript), @('uninstall.ps1', $UninstallScript), @('exe_xml.ps1', $ExeXmlScript))) {
    $lines = @(Get-Content -LiteralPath $item[1] | ForEach-Object { "'" + $_.Replace("'", "''") + "' + #13#10" })
    $embedded += "  SaveStringToFile(Directory + '\$($item[0])',"
    $embedded += ($lines -join " +`r`n") + ', False);'
}
$embedded += 'end;'
$embedded | Set-Content -LiteralPath $Output -Encoding utf8
