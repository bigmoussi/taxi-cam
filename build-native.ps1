[CmdletBinding()]
param([switch]$Validate, [switch]$Bootstrap, [switch]$WarpOnly)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($WarpOnly -and -not $Validate) { throw '-WarpOnly requires -Validate.' }
$taskRoot = $PSScriptRoot
$deps = Get-Content -Raw -LiteralPath (Join-Path $taskRoot 'dependencies.json') | ConvertFrom-Json
$compiler = Join-Path $taskRoot ('build/deps/' + $deps.'llvm-mingw'.directory + '/bin/clang++.exe')
if ($Bootstrap -and -not (Test-Path -LiteralPath $compiler)) { & (Join-Path $taskRoot 'bootstrap-native.ps1') }
if (-not (Test-Path -LiteralPath $compiler)) { throw 'Pinned LLVM-MinGW compiler missing. Run build-native.ps1 -Bootstrap.' }
$out = Join-Path $taskRoot 'build/native'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$receipt = Join-Path $out 'validation.json'
if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt }
$common = @('-std=c++20','-O2','-Wall','-Wextra','-Werror','-fms-extensions','-static',
    '-DNOMINMAX','-DWIN32_LEAN_AND_MEAN','-DTAXI_NATIVE_RUNTIME','-D_WIN32_WINNT=0x0A00',
    '-mno-avx','-mno-avx2','-mno-avx512f')
$graphics = @(
 'standalone/d3d12_bridge.cpp',
 'src/scene_handoff.cpp','src/scene_capture_d3d12.cpp','src/scene_capture_manager.cpp','src/scene_source_state.cpp',
 'src/scene_frame_output.cpp','src/scene_runtime.cpp','src/pfd_stamp_state.cpp','src/pfd_stamp_d3d12.cpp',
 'engine-hook/queue_submit_observer.cpp','engine-hook/pfd_state_observer.cpp','engine-hook/render_boundary_observer.cpp'
)
$engine = @(
 'native-camera/probe.cpp','native-camera/local_memory.cpp','native-camera/code_contract.cpp','native-camera/activation_mask.cpp',
 'native-camera/view_resize.cpp','native-camera/source_view.cpp','native-camera/body_pose_provider.cpp','native-camera/mount_config.cpp',
 'native-camera/verified_profile.cpp','discovery/aircraft_inventory.cpp','engine-camera/entry_pair.cpp',
 'engine-camera/owned_entry_inventory.cpp','engine-camera/owned_view.cpp','engine-camera/view_pool.cpp',
 'engine-hook/observer_hook.cpp','engine-hook/observer_thunk.S'
)
$libs = @('-ld3d12','-ldxgi','-ldxguid','-ld3dcompiler')
$objects = @()
foreach ($source in @($graphics + $engine)) {
    $object = Join-Path $out (($source -replace '[/\\]','_') + '.o')
    & $compiler @common '-c' (Join-Path $taskRoot $source) '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "Native compilation failed: $source" }
    $objects += $object
}
if (Test-Path -LiteralPath (Join-Path $taskRoot 'standalone/bridge_main.cpp')) {
    & $compiler @common '-shared' (Join-Path $taskRoot 'standalone/bridge_main.cpp') @objects @libs '-Wl,--no-insert-timestamp' '-o' (Join-Path $out 'taxi-camera-bridge.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Native bridge link failed.' }
}
if (Test-Path -LiteralPath (Join-Path $taskRoot 'standalone/companion.cpp')) {
    $windres = Join-Path (Split-Path -Parent $compiler) 'llvm-windres.exe'
    $resource = Join-Path $out 'app.res.o'
    & $windres '-I' (Join-Path $taskRoot 'standalone') '-i' (Join-Path $taskRoot 'standalone/app.rc') '-O' 'coff' '-o' $resource
    if ($LASTEXITCODE -ne 0) { throw 'Windows application manifest compilation failed.' }
    & $compiler @common '-municode' '-mwindows' (Join-Path $taskRoot 'standalone/companion.cpp') $resource '-lshell32' '-lcomctl32' '-ladvapi32' '-ldwmapi' '-luxtheme' '-Wl,--no-insert-timestamp' '-o' (Join-Path $out '380-taxi-cam.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Windows companion build failed.' }
}
if ($Validate) {
    $gpu = Join-Path $out 'native-graphics-validation.exe'
    & $compiler @common '-municode' (Join-Path $taskRoot 'standalone/graphics_validation.cpp') @($objects | Select-Object -First $graphics.Count) @libs '-o' $gpu
    if ($LASTEXITCODE -ne 0) { throw 'Native validation compilation failed.' }
    if (-not $WarpOnly) {
        & $gpu
        if ($LASTEXITCODE -ne 0) { throw 'Hardware native graphics validation failed.' }
    } else { Write-Output 'Hardware GPU validation not run: explicit WARP-only CI mode.' }
    & $gpu --warp
    if ($LASTEXITCODE -ne 0) { throw 'WARP native graphics validation failed.' }

    $smoke = Join-Path $out 'native-smoke-validation.exe'
    & $compiler @common '-municode' (Join-Path $taskRoot 'standalone/smoke_validation.cpp') '-ladvapi32' '-o' $smoke
    if ($LASTEXITCODE -ne 0) { throw 'Native smoke compilation failed.' }
    & $smoke (Join-Path $out 'taxi-camera-bridge.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Exact native DLL smoke failed.' }
    foreach ($entry in @(
        @{Name='native-launcher-path'; Sources=@('standalone/launcher_path_test.cpp')},
        @{Name='native-slots'; Sources=@('standalone/native_slots_test.cpp')},
        @{Name='native-root-state'; Sources=@('standalone/root_state_test.cpp','src/pfd_stamp_state.cpp')},
        @{Name='taxi-routes'; Sources=@('tests/taxi_button_routes_test.cpp')},
        @{Name='pfd-detector'; Sources=@('tests/pfd_target_detector_test.cpp')},
        @{Name='display-exposure'; Sources=@('tests/display_exposure_test.cpp')},
        @{Name='calibration'; Sources=@('tests/calibration_test.cpp')},
        @{Name='write-budget'; Sources=@('tests/write_budget_test.cpp')},
        @{Name='queue-submit'; Sources=@('engine-hook/queue_submit_observer_test.cpp','engine-hook/queue_submit_observer.cpp')}
    )) {
        $exe = Join-Path $out ($entry.Name + '-test.exe')
        $sources = @($entry.Sources | ForEach-Object { Join-Path $taskRoot $_ })
        & $compiler @common @sources @libs '-o' $exe
        if ($LASTEXITCODE -ne 0) { throw "Native test compilation failed: $($entry.Name)" }
        & $exe
        if ($LASTEXITCODE -ne 0) { throw "Native test failed: $($entry.Name)" }
    }
    & (Join-Path $taskRoot 'standalone/exe_xml_test.ps1')
    & (Join-Path $taskRoot 'validation/render_boundary_test.ps1') -Compiler $compiler
    & (Join-Path $taskRoot 'engine-hook/build.ps1')
    & (Join-Path $taskRoot 'native-camera/build.ps1')
    $readobj = Join-Path (Split-Path -Parent $compiler) 'llvm-readobj.exe'
    $hashes = [ordered]@{}
    foreach ($name in @('380-taxi-cam.exe','taxi-camera-bridge.dll')) {
        $path = Join-Path $out $name
        $imports = & $readobj '--coff-imports' $path
        if ($LASTEXITCODE -ne 0) { throw "Import audit failed: $name" }
        if (($imports -join "`n") -match '(?i)(reshade|imgui|libc\+\+|libunwind|libwinpthread).*\.dll') {
            throw "Non-native runtime dependency found in $name."
        }
        $imports | Set-Content -LiteralPath (Join-Path $out ($name+'.imports.txt'))
        $hashes[$name] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    }
    $closure = @()
    foreach ($source in @($graphics + $engine + @('standalone/bridge_main.cpp','standalone/companion.cpp'))) {
        if ($source.EndsWith('.S')) { continue }
        $dependencyList = & $compiler @common '-MM' (Join-Path $taskRoot $source)
        if ($LASTEXITCODE -ne 0) { throw "Native dependency audit failed: $source" }
        if (($dependencyList -join ' ') -match '(?i)(reshade[^\s]*\.hpp|imgui[^\s]*\.h)') { throw "ReShade/ImGui header dependency remains: $source" }
        $closure += $dependencyList
    }
    $closure | Set-Content -LiteralPath (Join-Path $out 'native-dependencies.txt')
    $gpuTests = @('WARP GPU')
    if (-not $WarpOnly) { $gpuTests = @('hardware GPU') + $gpuTests }
    [ordered]@{
        passed=$true; version='0.8.0'; createdUtc=[DateTime]::UtcNow.ToString('o'); files=$hashes;
        tests=@($gpuTests + @('pre-existing graphics objects','graphics state replay','exact DLL smoke',
            'settings persistence and IPC','launcher file identity','native COM slots','TAXI routing','PFD detector','exposure','calibration','write budget',
            'queue submit','render boundary','engine hook','camera telemetry and lifecycle','exe.xml preservation',
            'native imports and header dependency closure'));
        gpuValidation=[ordered]@{hardware=$(if ($WarpOnly) { 'not-run' } else { 'passed' });warp='passed'};
        simulatorVerified=$false
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'validation.json') -Encoding utf8

}
Write-Output "Native build complete: $out (no ReShade or ImGui build inputs)"
