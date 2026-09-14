$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'ci/toolchain.ps1')
Set-StrictMode -Version Latest
$nativeRoot = $repoRoot
$compiler = Join-Path (Get-TaxiToolchain $repoRoot) 'clang++.exe'
$output = Join-Path $repoRoot 'build/tools/diagnostics'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$flags = @('-std=c++20','-O2','-Wall','-Wextra','-Werror','-fno-exceptions','-mno-avx','-mno-avx2','-D_WIN32_WINNT=0x0A00','-static')
& $compiler @flags (Join-Path $repoRoot 'tools/diagnostics/view_material_inventory.cpp') (Join-Path $repoRoot 'tests/diagnostics/view_material_inventory_test.cpp') '-o' (Join-Path $output 'view-material-test.exe')
if ($LASTEXITCODE -ne 0) { throw 'View material regression compile failed.' }
$sources = @('view_material_live.cpp','view_material_inventory.cpp','image_inventory.cpp') | ForEach-Object { Join-Path $PSScriptRoot $_ }
$sources += @('code_contract.cpp','verified_profile.cpp','aircraft_inventory.cpp') | ForEach-Object { Join-Path (Join-Path $nativeRoot 'src/camera') $_ }
& $compiler @flags @sources '-lpsapi' '-ladvapi32' '-o' (Join-Path $output 'view-material-live.exe')
if ($LASTEXITCODE -ne 0) { throw 'Read-only view material host compile failed.' }
$readobj = Join-Path (Split-Path $compiler -Parent) 'llvm-readobj.exe'
foreach ($name in @('view-material-test.exe','view-material-live.exe')) {
  $imports = & $readobj '--coff-imports' (Join-Path $output $name)
  if ($LASTEXITCODE -ne 0) { throw 'PE import inspection failed.' }
  foreach ($line in $imports) {
    if ($line -match '^\s+Name:\s+(.*)$' -and $Matches[1] -notmatch '^(KERNEL32|ADVAPI32|api-ms-win-crt-[a-z0-9-]+)\.dll$') {
      throw "Unexpected helper dependency: $($Matches[1]). Refusing to launch $name."
    }
  }
}
$testOut = Join-Path $output 'view-material-test-load.json'
$testError = Join-Path $output 'view-material-test-load.stderr'
$testProcess = Start-Process -FilePath (Join-Path $output 'view-material-test.exe') -WindowStyle Hidden -PassThru -Wait -RedirectStandardOutput $testOut -RedirectStandardError $testError
if ($testProcess.ExitCode -ne 0) { throw "View material regression failed; inspect $testError" }
Get-Content -LiteralPath $testOut
$negativeOut = Join-Path $output 'view-material-wrong-process.json'
$negativeError = Join-Path $output 'view-material-wrong-process.stderr'
$negativeProcess = Start-Process -FilePath (Join-Path $output 'view-material-live.exe') -ArgumentList @('--pid', "$PID") -WindowStyle Hidden -PassThru -Wait -RedirectStandardOutput $negativeOut -RedirectStandardError $negativeError
if ($negativeProcess.ExitCode -ne 1 -or (Get-Content -LiteralPath $negativeOut -Raw | ConvertFrom-Json).error -ne 'wrong_process') { throw 'Wrong-process refusal failed.' }
Write-Output 'PASS: bounded metadata tests, strict host compile and wrong-process refusal. No simulator read.'
