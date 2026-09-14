$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'update-check.ps1')
$script:Count = 0
function Assert([bool]$Condition, [string]$Name) {
    if (-not $Condition) { throw "FAIL: $Name" }
    $script:Count++
}
function Reject([scriptblock]$Action, [string]$Name) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Assert $rejected $Name
}
function Release {
    return ('{"draft":false,"prerelease":false,"tag_name":"v0.8.0-build.12","assets":[{"name":"taxi-cam-0.8.0-build.12-windows-x64-setup.exe","state":"uploaded","size":1234,"browser_download_url":"https://github.com/rthomson83/taxi-cam/releases/download/v0.8.0-build.12/taxi-cam-0.8.0-build.12-windows-x64-setup.exe","digest":"sha256:' + ('a' * 64) + '"}]}') | ConvertFrom-Json
}
$current = ConvertTo-UpdateVersion 'v0.8.0-build.9'
Assert (Test-NewerUpdate (ConvertTo-UpdateVersion 'v0.8.0-build.10') $current) 'Numeric build ordering'
Assert (Test-NewerUpdate (ConvertTo-UpdateVersion 'v0.8.1-build.1') $current) 'Patch version before build'
Assert (Test-NewerUpdate (ConvertTo-UpdateVersion 'v0.9.0-build.1') $current) 'Version before build'
Assert (-not (Test-NewerUpdate $current $current)) 'No reinstall'
foreach ($tag in @('v0.8.0-build.01','v0.8.0-build.-1','v0.8.0-build.4294967296','v0.8.0-build.10;calc','v0.8.0-build.10-preview')) {
    Reject { ConvertTo-UpdateVersion $tag } 'Malformed tag'
}
$release = Release
$selected = Select-UpdateAsset $release $current
Assert ($selected.Digest -ceq ('a' * 64)) 'GitHub asset digest selected'
Assert ($null -eq (Select-UpdateAsset $release (ConvertTo-UpdateVersion 'v0.9.0-build.1'))) 'Older release ignored'
foreach ($field in @('draft', 'prerelease')) {
    $release = Release
    $release.$field = $true
    Reject { Select-UpdateAsset $release $current } 'Draft/prerelease rejected'
}
$release = Release
$release.assets += $release.assets[0]
Reject { Select-UpdateAsset $release $current } 'Duplicate installers rejected'
foreach ($url in @('http://github.com/a','https://github.com.evil.example/a','https://github.com@evil.example/a','https://github.com:444/a','file:///c:/a','https://github.com/a#b')) {
    Reject { Assert-DownloadUri $url } 'Unsafe transport rejected'
}
$release = Release
$release.assets[0].browser_download_url += '?other=true'
Reject { Select-UpdateAsset $release $current } 'Non-exact installer URL rejected'
$release = Release
$release.assets[0].size = 257MB
Reject { Select-UpdateAsset $release $current } 'Oversized installer rejected'
$release = Release
$release.assets[0].digest = 'md5:abcd'
Reject { Select-UpdateAsset $release $current } 'Malformed digest cannot fallback'
$release = Release
$release.assets[0].digest = $null
Reject { Select-UpdateAsset $release $current } 'No digest or checksum rejected'
$release.assets += [pscustomobject]@{ name='SHA256SUMS.txt'; state='uploaded'; size=100; browser_download_url='https://github.com/rthomson83/taxi-cam/releases/download/v0.8.0-build.12/SHA256SUMS.txt' }
$selected = Select-UpdateAsset $release $current
Assert ($selected.Sums.name -ceq 'SHA256SUMS.txt') 'Checksum fallback selected'
$line = ('b' * 64) + '  ' + $selected.Name
Assert ((Read-InstallerChecksum $line $selected.Name) -ceq ('b' * 64)) 'Exact checksum filename'
Reject { Read-InstallerChecksum "$line`n$line" $selected.Name } 'Duplicate checksum rejected'
Reject { Read-InstallerChecksum ($line + '.other') $selected.Name } 'Similar filename rejected'
Write-Output "Updater release selection checks passed: $script:Count"
