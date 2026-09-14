$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskCompiler = Join-Path $taskRoot 'build/deps/llvm-mingw/llvm-mingw-20260908-ucrt-x86_64/bin/clang++.exe'
$taskSources = @('image_inventory','reference_inventory','pointer_inventory','guard_inventory','static_literals','service_inventory','aircraft_inventory','slot_prefix','callee_prefix','import_slot','rtti_metadata','command_list_inventory','llvm_decoder','static_numeric','source_pose_compare') | ForEach-Object { Join-Path $PSScriptRoot ($_ + '.cpp') }
$taskSources += @('code_contract','activation_mask','verified_profile','source_view') | ForEach-Object { Join-Path $taskRoot ('native-camera/' + $_ + '.cpp') }
& $taskCompiler '-std=c++20' '-O2' '-Wall' '-Wextra' '-Werror' '-DNOMINMAX' '-D_WIN32_WINNT=0x0A00' '-static' @taskSources '-ladvapi32' '-o' (Join-Path $PSScriptRoot 'build/source-pose-compare.exe')
if ($LASTEXITCODE -ne 0) { throw 'Source pose diagnostic compilation failed.' }
