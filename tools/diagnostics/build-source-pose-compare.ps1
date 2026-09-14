$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'ci/toolchain.ps1')
$taskRoot = $repoRoot
$taskCompiler = Join-Path (Get-TaxiToolchain $repoRoot) 'clang++.exe'
$taskSources = @('image_inventory','reference_inventory','pointer_inventory','guard_inventory','static_literals','service_inventory','slot_prefix','callee_prefix','import_slot','rtti_metadata','command_list_inventory','llvm_decoder','static_numeric','source_pose_compare') | ForEach-Object { Join-Path $PSScriptRoot ($_ + '.cpp') }
$taskSources += @('code_contract','activation_mask','verified_profile','source_view','aircraft_inventory') | ForEach-Object { Join-Path $taskRoot ('src/camera/' + $_ + '.cpp') }
$outputDirectory = Join-Path $repoRoot 'build/tools/diagnostics'
Write-TaxiLlvmConfig -Repository $repoRoot -OutputDirectory $outputDirectory
& $taskCompiler '-std=c++20' '-O2' '-Wall' '-Wextra' '-Werror' '-DNOMINMAX' '-D_WIN32_WINNT=0x0A00' '-static' '-I' $outputDirectory @taskSources '-ladvapi32' '-o' (Join-Path $outputDirectory 'source-pose-compare.exe')
if ($LASTEXITCODE -ne 0) { throw 'Source pose diagnostic compilation failed.' }
