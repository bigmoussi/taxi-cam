$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$workflow = Get-Content -Raw -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) '.github/workflows/pr-test-build.yml')
$checks = 0
function Assert-Contains([string]$Pattern, [string]$Message) {
    if ($workflow -notmatch $Pattern) { throw $Message }
    $script:checks++
}
function Assert-Absent([string]$Pattern, [string]$Message) {
    if ($workflow -match $Pattern) { throw $Message }
    $script:checks++
}
Assert-Contains '(?m)^on:\s*$' 'PR workflow trigger block is required.'
Assert-Contains '(?m)^\s+pull_request:\s*$' 'PR workflow must run on pull_request.'
Assert-Contains '(?m)^\s+branches:\s*\[main\]\s*$' 'PR workflow must target main.'
Assert-Contains 'build\.ps1 -Validate -WarpOnly' 'PR workflow must validate with the pinned Warp-only native build.'
Assert-Contains '(?m)^\s+\./smoke-test\.ps1\s*$' 'PR workflow must smoke-test the exact build outputs.'
Assert-Contains 'windows-pr-test-build-' 'PR workflow artifact name must identify a test/PR build.'
Assert-Contains 'build/native/taxi-cam.exe' 'PR workflow must upload taxi-cam.exe.'
Assert-Contains 'build/native/taxi-camera-bridge.dll' 'PR workflow must upload taxi-camera-bridge.dll.'
Assert-Contains 'build/native/validation.json' 'PR workflow must upload the validation receipt hashes.'
Assert-Contains 'build/native/SHA256SUMS.txt' 'PR workflow must upload SHA-256 checksums.'
Assert-Contains '(?m)^\s+contents:\s*read\s*$' 'PR workflow must stay read-only.'
Assert-Absent 'publish-release\.ps1' 'PR workflow must not publish releases.'
Assert-Absent 'installer/package\.ps1' 'PR workflow must not package a release ZIP.'
Assert-Absent 'installer/build\.ps1' 'PR workflow must not compile a release installer.'
Assert-Absent '(?m)^\s+environment:\s*$' 'PR workflow must not wait for the release environment.'
Assert-Absent 'contents:\s*write' 'PR workflow must not request write access.'
Assert-Absent 'gh release' 'PR workflow must not create GitHub releases.'
Write-Output "PASS PR test-build workflow: $checks checks for pull_request CI, native validate/smoke, test artifacts and no publication."
