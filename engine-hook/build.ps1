param()
$ErrorActionPreference = 'Stop'
$hookRoot = $PSScriptRoot
$nativeRoot = Split-Path -Parent $hookRoot
$toolchain = Join-Path $nativeRoot 'build/deps/llvm-mingw/llvm-mingw-20260908-ucrt-x86_64/bin'
$compiler = Join-Path $toolchain 'clang++.exe'
$archiver = Join-Path $toolchain 'llvm-ar.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'The parent native probe pinned LLVM-MinGW compiler is required.' }
$outputDirectory = Join-Path $hookRoot 'build'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$common = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-D_WIN32_WINNT=0x0A00', '-mno-avx', '-mno-avx2', '-mno-avx512f')
$hookObject = Join-Path $outputDirectory 'observer_hook.o'
$thunkObject = Join-Path $outputDirectory 'observer_thunk.o'
& $compiler @common '-fno-exceptions' '-c' (Join-Path $hookRoot 'observer_hook.cpp') '-o' $hookObject
if ($LASTEXITCODE -ne 0) { throw 'Observer hook failed to compile.' }
& $compiler '-c' (Join-Path $hookRoot 'observer_thunk.S') '-o' $thunkObject
if ($LASTEXITCODE -ne 0) { throw 'Observer thunk failed to assemble.' }
$library = Join-Path $outputDirectory 'engine-hook.a'
& $archiver 'rcs' $library $hookObject $thunkObject
if ($LASTEXITCODE -ne 0) { throw 'Observer library failed to archive.' }
$validationExecutable = Join-Path $outputDirectory 'engine-hook-validation.exe'
& $compiler @common '-static' (Join-Path $hookRoot 'validation.cpp') (Join-Path $hookRoot 'validation_thunks.S') $library '-o' $validationExecutable
if ($LASTEXITCODE -ne 0) { throw 'Observer validation failed to compile.' }
& $validationExecutable
if ($LASTEXITCODE -ne 0) { throw 'Observer validation failed.' }
$faultHookObject = Join-Path $outputDirectory 'observer_hook_fault_validation.o'
& $compiler @common '-fno-exceptions' '-DTAXI_ENGINE_HOOK_VALIDATION' '-c' (Join-Path $hookRoot 'observer_hook.cpp') '-o' $faultHookObject
if ($LASTEXITCODE -ne 0) { throw 'Protection-failure validation hook failed to compile.' }
$faultValidationExecutable = Join-Path $outputDirectory 'engine-hook-protection-validation.exe'
# Only this isolated image-data fixture disables ASLR: its linked read-only
# function-pointer page must remain pristine until the tested copy-on-write.
& $compiler @common '-static' '-Wl,--disable-dynamicbase' (Join-Path $hookRoot 'protection_validation.cpp') $faultHookObject $thunkObject '-o' $faultValidationExecutable
if ($LASTEXITCODE -ne 0) { throw 'Protection-failure validation failed to compile.' }
foreach ($scenario in @('install-restore', 'remove-restore', 'mismatch-restore', 'writable-change',
                       'image-data', 'image-data-restore', 'image-private-refusal')) {
    & $faultValidationExecutable $scenario
    if ($LASTEXITCODE -ne 0) { throw "Protection-failure validation failed: $scenario" }
}
& (Join-Path $toolchain 'llvm-readobj.exe') '--unwind' $thunkObject | Set-Content -LiteralPath (Join-Path $outputDirectory 'observer-unwind.txt')
if ($LASTEXITCODE -ne 0) { throw 'Unwind metadata inspection failed.' }
Write-Output "Built: $library"
Write-Output "Built: $validationExecutable"
Write-Output "Built: $faultValidationExecutable"
