[CmdletBinding()]
param(
  [string] $Compiler,
  [string] $ReShadeInclude,
  [string] $ImGuiInclude,
  [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
$taskNativeRoot = Split-Path -Parent $PSScriptRoot
$taskDependencies = Get-Content -Raw -LiteralPath (Join-Path $taskNativeRoot 'dependencies.json') | ConvertFrom-Json
$taskDepsRoot = Join-Path $taskNativeRoot 'build/deps'
if (-not $Compiler) { $Compiler = Join-Path $taskDepsRoot ($taskDependencies.'llvm-mingw'.directory + '/bin/clang++.exe') }
if (-not $ReShadeInclude) { $ReShadeInclude = Join-Path $taskDepsRoot ($taskDependencies.reshade.directory + '/include') }
if (-not $ImGuiInclude) { $ImGuiInclude = Join-Path $taskDepsRoot $taskDependencies.imgui.directory }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $taskNativeRoot 'build/abi' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

# Read the actual translation-unit list from the add-on link expression. Every
# src/ dependency in the compiler's transitive closure is inventoried, including
# the now-integrated native compositor. Standalone *_test.cpp files are not link
# inputs and must never silently become production dependencies.
$taskBuildText = Get-Content -Raw -LiteralPath (Join-Path $taskNativeRoot 'build.ps1')
$taskLinkBlock = [regex]::Match($taskBuildText, '(?s)\$addonArguments\s*=.*?(?=&\s*\$compiler\s+@addonArguments)')
if (-not $taskLinkBlock.Success) { throw 'Cannot identify the production add-on link expression.' }
$taskProductionSources = @([regex]::Matches($taskLinkBlock.Value, "Join-Path\s+\`$PSScriptRoot\s+'([^']+\.cpp)'" ) |
    ForEach-Object { [IO.Path]::GetFullPath((Join-Path $taskNativeRoot $_.Groups[1].Value)) })
if (-not $taskProductionSources.Count) { throw 'No production translation units were found.' }
$taskSrcRoot = [IO.Path]::GetFullPath((Join-Path $taskNativeRoot 'src')) + [IO.Path]::DirectorySeparatorChar
$taskReviewedFiles = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$taskDependencyFiles = @()
foreach ($taskTranslationUnit in $taskProductionSources) {
  if ($taskTranslationUnit -match '[_-]test\.cpp$') { throw 'A standalone test is linked into the add-on.' }
  $taskDependencyFile = Join-Path $OutputDirectory (([IO.Path]::GetFileNameWithoutExtension($taskTranslationUnit)) + '-dependencies.d')
  & $Compiler '-std=c++20' '-fms-extensions' '-DNOMINMAX' '-DWIN32_LEAN_AND_MEAN' `
    '-isystem' $ReShadeInclude '-isystem' $ImGuiInclude '-M' '-MT' 'taxi_camera_addon' '-MF' $taskDependencyFile $taskTranslationUnit
  if ($LASTEXITCODE -ne 0) { throw "Production dependency check failed for $taskTranslationUnit." }
  $taskDependencyFiles += $taskDependencyFile
  $taskDependencyText = (Get-Content -Raw -LiteralPath $taskDependencyFile) -replace '\\\r?\n', ' '
  foreach ($taskWord in [regex]::Matches($taskDependencyText, '(?:\\.|[^\s])+')) {
    $taskPath = $taskWord.Value -replace '\\ ', ' '
    if (-not [IO.Path]::IsPathRooted($taskPath)) { continue }
    $taskPath = [IO.Path]::GetFullPath($taskPath)
    if (-not $taskPath.StartsWith($taskSrcRoot, [StringComparison]::OrdinalIgnoreCase)) { continue }
    if ($taskPath -match '[_-]test\.(cpp|hpp|h)$') { throw "Production source includes a test: $taskPath" }
    [void]$taskReviewedFiles.Add($taskPath)
  }
}
if (-not $taskReviewedFiles.Count) { throw 'Production dependency closure is empty.' }
# The newly linked native boundary observer uses raw Win32/COM interfaces only.
# Include its exact implementation/header in this review without broadening the
# external ReShade subset to other independent native discovery components.
foreach ($taskBoundaryFile in @('engine-hook/render_boundary_observer.cpp', 'engine-hook/render_boundary_observer.hpp')) {
  $taskBoundaryPath = [IO.Path]::GetFullPath((Join-Path $taskNativeRoot $taskBoundaryFile))
  if (Test-Path -LiteralPath $taskBoundaryPath) { [void]$taskReviewedFiles.Add($taskBoundaryPath) }
}

# This is a fail-closed inventory, not a general C++ ABI compatibility claim.
# A new external call needs a matching probe before it is allowed in the add-on.
$taskMemberCalls = @(
  'get_native', 'get_device', 'get_api', 'get_private_data', 'set_private_data', 'create_private_data', 'destroy_private_data',
  'get_immediate_command_list',
  'open_overlay',
  'draw', 'draw_indexed', 'create_resource_view', 'destroy_resource_view', 'copy_resource', 'copy_texture_region', 'wait_idle',
  # Native COM calls use the pinned Windows headers, including explicit-out
  # aggregate-return declarations. Hardware/WARP validation exercises this path.
  'GetType', 'GetDeviceRemovedReason', 'ClearRenderTargetView', 'GetDesc', 'GetHeapProperties', 'CreateCommittedResource',
  'Map', 'Unmap', 'CopyTextureRegion', 'Release', 'CreateFence', 'Signal', 'SetEventOnCompletion', 'GetCompletedValue',
  'AddRef', 'QueryInterface', 'GetDevice', 'GetPrivateData', 'GetResourceAllocationInfo', 'CopyResource', 'Wait', 'ExecuteCommandLists',
  'CreateCommandQueue', 'CreateCommandAllocator', 'CreateCommandList', 'Close', 'Reset', 'GetGPUVirtualAddress',
  'ResourceBarrier', 'Barrier', 'BeginRenderPass', 'EndRenderPass', 'CreateDescriptorHeap', 'CreateShaderResourceView', 'CreateRenderTargetView',
  'GetCPUDescriptorHandleForHeapStart', 'GetGPUDescriptorHandleForHeapStart', 'GetDescriptorHandleIncrementSize',
  'SetGraphicsRootSignature', 'SetPipelineState', 'SetDescriptorHeaps', 'SetGraphicsRootDescriptorTable',
  'SetGraphicsRoot32BitConstants', 'SetGraphicsRootConstantBufferView', 'SetGraphicsRootShaderResourceView',
  'SetGraphicsRootUnorderedAccessView', 'IASetPrimitiveTopology', 'RSSetViewports', 'RSSetScissorRects',
  'OMSetRenderTargets', 'DrawInstanced', 'DrawIndexedInstanced', 'CheckFeatureSupport', 'CreateRootSignature', 'CreateGraphicsPipelineState',
  'GetBufferPointer', 'GetBufferSize'
)
# These are project-owned methods compiled with the same compiler, not an
# external ABI claim. Keep their inventory explicit rather than accepting every
# lower-case method and accidentally allowing a new ReShade virtual return.
$taskInternalCalls = @('initialize', 'set_inputs', 'set_display_exposure', 'display_exposure', 'set_ground_speed', 'last_error', 'record', 'output', 'successful_reset',
  'before_submission', 'after_submission', 'submission_refused', 'record_render_target_before_transition',
  'record_render_target_before_enhanced_transition', 'record_texture_copy_after_forward')
$taskUiCalls = @(
  'InputInt', 'Checkbox', 'BeginChild', 'Selectable', 'Button', 'BeginDisabled', 'EndDisabled', 'EndChild',
  'TextUnformatted', 'Text', 'TextWrapped', 'CollapsingHeader', 'SliderFloat', 'DragFloat3', 'PushID', 'PopID', 'SameLine'
)
foreach ($taskSourcePath in $taskReviewedFiles) {
  $taskSource = Get-Item -LiteralPath $taskSourcePath
  if ($taskSource.Extension -notin @('.cpp', '.hpp', '.h')) { continue }
  $taskCode = Get-Content -Raw -LiteralPath $taskSource.FullName
  foreach ($taskMatch in [regex]::Matches($taskCode, '->\s*(\w+)\s*(?:<[^;{}()]*>)?\s*\(')) {
    if ($taskMatch.Groups[1].Value -notin $taskMemberCalls -and $taskMatch.Groups[1].Value -notin $taskInternalCalls) {
      throw "Unreviewed member call '$($taskMatch.Groups[1].Value)' in $($taskSource.FullName). Add an ABI probe first."
    }
  }
  foreach ($taskMatch in [regex]::Matches($taskCode, '\bImGui::(\w+)\s*\(')) {
    if ($taskMatch.Groups[1].Value -notin $taskUiCalls) {
      throw "Unreviewed ImGui call '$($taskMatch.Groups[1].Value)' in $($taskSource.FullName). Add an ABI probe first."
    }
  }
}

# Native COM follows the pinned Windows header's explicit Microsoft aggregate
# return wrappers. This is independent of the ReShade C++ subset comparison.
# Actual allocation/copy/composition/queue/state paths are hardware/WARP tested;
# those tests do not prove every driver or arbitrary COM method is compatible.
$taskNativeHeaderProbe = Join-Path $OutputDirectory 'native-com-header.cpp'
@'
#include <d3d12.h>
#include <cstddef>
#include <type_traits>
#include "render_boundary_observer.hpp"
#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#error The pinned Microsoft COM aggregate-return wrappers must be enabled.
#endif
static_assert(sizeof(D3D12_RESOURCE_DESC) == 56);
static_assert(sizeof(D3D12_RESOURCE_ALLOCATION_INFO) == 16);
static_assert(sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) == 8);
static_assert(sizeof(D3D12_GPU_DESCRIPTOR_HANDLE) == 8);
static_assert(sizeof(D3D12_RESOURCE_TRANSITION_BARRIER) == 24);
static_assert(sizeof(D3D12_RESOURCE_BARRIER) == 32);
static_assert(sizeof(D3D12_BARRIER_SUBRESOURCE_RANGE) == 24);
static_assert(sizeof(D3D12_BARRIER_SYNC) == 4);
static_assert(sizeof(D3D12_BARRIER_ACCESS) == 4);
static_assert(sizeof(D3D12_TEXTURE_BARRIER) == 64);
static_assert(offsetof(D3D12_TEXTURE_BARRIER, pResource) == 24);
static_assert(offsetof(D3D12_TEXTURE_BARRIER, Subresources) == 32);
static_assert(offsetof(D3D12_TEXTURE_BARRIER, Flags) == 56);
static_assert(sizeof(D3D12_BARRIER_GROUP) == 16);
static_assert(offsetof(D3D12_BARRIER_GROUP, pTextureBarriers) == 8);
static_assert(sizeof(D3D12_TEXTURE_COPY_LOCATION) == 48);
static_assert(offsetof(D3D12_TEXTURE_COPY_LOCATION, pResource) == 0);
static_assert(offsetof(D3D12_TEXTURE_COPY_LOCATION, Type) == 8);
static_assert(offsetof(D3D12_TEXTURE_COPY_LOCATION, PlacedFootprint) == 16);
static_assert(offsetof(D3D12_TEXTURE_COPY_LOCATION, SubresourceIndex) == 16);
static_assert(sizeof(D3D12_BOX) == 24);
using BoundaryCallbacks = taxi_camera::engine_hook::render_boundary::Callbacks;
using AfterDrawCallback = void(*)(void*,ID3D12GraphicsCommandList*,std::uint64_t,bool) noexcept;
using InvalidatedCallback = void(*)(void*,ID3D12GraphicsCommandList*,std::uint64_t,std::uint32_t) noexcept;
static_assert(std::is_same_v<decltype(BoundaryCallbacks{}.after_draw),AfterDrawCallback>);
static_assert(std::is_same_v<decltype(BoundaryCallbacks{}.recording_invalidated),InvalidatedCallback>);
static_assert(offsetof(BoundaryCallbacks,after_draw)==56);
static_assert(offsetof(BoundaryCallbacks,recording_invalidated)==64);
static_assert(sizeof(BoundaryCallbacks)==72);
using PrivateDataMethod = HRESULT (STDMETHODCALLTYPE ID3D12Object::*)(REFGUID, UINT*, void*);
static_assert(std::is_same_v<decltype(&ID3D12Object::GetPrivateData), PrivateDataMethod>);
extern "C" HRESULT native_get_private_data(ID3D12Object* object, REFGUID key, UINT* bytes, void* output) {
  return object->GetPrivateData(key, bytes, output);
}
extern "C" void native_desc(ID3D12Resource* p, D3D12_RESOURCE_DESC* out) { p->GetDesc(out); }
extern "C" void native_alloc(ID3D12Device* p, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_ALLOCATION_INFO* out) {
  p->GetResourceAllocationInfo(out, 0, 1, desc);
}
extern "C" void native_legacy_barrier(ID3D12GraphicsCommandList* p, UINT count, const D3D12_RESOURCE_BARRIER* values) {
  p->ResourceBarrier(count, values);
}
extern "C" void native_enhanced_barrier(ID3D12GraphicsCommandList7* p, UINT count, const D3D12_BARRIER_GROUP* values) {
  p->Barrier(count, values);
}
extern "C" void native_begin_render_pass(ID3D12GraphicsCommandList4* p, UINT count,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth, D3D12_RENDER_PASS_FLAGS flags) {
  p->BeginRenderPass(count, targets, depth, flags);
}
extern "C" void native_end_render_pass(ID3D12GraphicsCommandList4* p) { p->EndRenderPass(); }
extern "C" void native_draw_instanced(ID3D12GraphicsCommandList* p, UINT count, UINT instances, UINT first, UINT first_instance) {
  p->DrawInstanced(count, instances, first, first_instance);
}
extern "C" void native_draw_indexed_instanced(ID3D12GraphicsCommandList* p, UINT count, UINT instances, UINT first, INT base, UINT first_instance) {
  p->DrawIndexedInstanced(count, instances, first, base, first_instance);
}
extern "C" void native_copy_resource(ID3D12GraphicsCommandList* p, ID3D12Resource* dst, ID3D12Resource* src) { p->CopyResource(dst, src); }
extern "C" void native_graphics_root(ID3D12GraphicsCommandList* p, ID3D12RootSignature* root) { p->SetGraphicsRootSignature(root); }
extern "C" void native_graphics_table(ID3D12GraphicsCommandList* p, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) { p->SetGraphicsRootDescriptorTable(index, table); }
extern "C" void native_copy_texture(ID3D12GraphicsCommandList* p, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
    const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) { p->CopyTextureRegion(dst,x,y,z,src,box); }
'@ | Set-Content -LiteralPath $taskNativeHeaderProbe -Encoding utf8
& $Compiler '-std=c++20' '-O2' '-Wall' '-Wextra' '-Werror' '-I' (Join-Path $taskNativeRoot 'engine-hook') '-c' $taskNativeHeaderProbe '-o' (Join-Path $OutputDirectory 'native-com-header.o')
if ($LASTEXITCODE -ne 0) { throw 'Pinned native COM header contract failed.' }
$taskNativeHeaderAssembly = Join-Path $OutputDirectory 'native-com-header.s'
& $Compiler '-std=c++20' '-O2' '-Wall' '-Wextra' '-Werror' '-I' (Join-Path $taskNativeRoot 'engine-hook') '-S' $taskNativeHeaderProbe '-o' $taskNativeHeaderAssembly
if ($LASTEXITCODE -ne 0) { throw 'Pinned native COM vtable probe compilation failed.' }
$taskNativeAssemblyText = Get-Content -Raw -LiteralPath $taskNativeHeaderAssembly
foreach ($taskNativeSlot in @(@('native_draw_instanced', 96), @('native_draw_indexed_instanced', 104), @('native_legacy_barrier', 208), @('native_begin_render_pass', 544),
    @('native_end_render_pass', 552), @('native_enhanced_barrier', 640), @('native_copy_texture',128), @('native_copy_resource',136),
    @('native_graphics_root',240), @('native_graphics_table',256), @('native_get_private_data',24))) {
  $taskNativeBody = [regex]::Match($taskNativeAssemblyText, '(?ms)^' + $taskNativeSlot[0] + ':.*?(?=^\s*# -- End function|^\s*\.def\s|\z)').Value
  if ($taskNativeBody -notmatch ('\bjmpq?\s+\*' + $taskNativeSlot[1] + '\(')) {
    throw "Native COM slot/prototype changed for $($taskNativeSlot[0])."
  }
}
$taskReShadeComPath = Join-Path (Split-Path -Parent $ReShadeInclude) 'source/com_utils.hpp'
$taskIdentityPath = Join-Path $taskNativeRoot 'src/native_device_identity.hpp'
$taskPinnedGuid = [regex]::Match((Get-Content -Raw -LiteralPath $taskReShadeComPath), '(?s)IID_UnwrappedObject\s*=\s*\{(.*?)\};')
$taskLocalGuid = [regex]::Match((Get-Content -Raw -LiteralPath $taskIdentityPath), '(?s)ReShadeUnwrappedObject\s*=\s*\{(.*?)\};')
if (-not $taskPinnedGuid.Success -or -not $taskLocalGuid.Success) { throw 'Cannot verify the ReShade device-unwrapping IID.' }
$taskPinnedWords = @([regex]::Matches($taskPinnedGuid.Groups[1].Value, '0x[0-9a-fA-F]+') | ForEach-Object { [Convert]::ToUInt64($_.Value.Substring(2), 16) })
$taskLocalWords = @([regex]::Matches($taskLocalGuid.Groups[1].Value, '0x[0-9a-fA-F]+') | ForEach-Object { [Convert]::ToUInt64($_.Value.Substring(2), 16) })
if ($taskPinnedWords.Count -ne 11 -or $taskLocalWords.Count -ne 11 -or
    [string]::Join(',', $taskPinnedWords) -cne [string]::Join(',', $taskLocalWords)) {
  throw 'Device identity helper differs from the pinned ReShade IID; do not load the add-on.'
}

function Compile-AbiProbe([string] $Source, [string] $Target, [string] $Output) {
  & $Compiler -target $Target -S -O1 -std=c++20 -fms-extensions -Wno-ignored-attributes -nostdinc++ `
    -I (Join-Path $PSScriptRoot 'shims') -I $ReShadeInclude -I $ImGuiInclude $Source -o $Output
  if ($LASTEXITCODE -ne 0) { throw "ABI probe compilation failed for $Target ($Source)." }
}

function Get-AbiFunction([string] $Assembly, [string] $Name) {
  $taskPattern = '(?ms)^' + [regex]::Escape($Name) + ':.*?(?=^\s*# -- End function|^\s*\.def\s|\z)'
  $taskMatch = [regex]::Match($Assembly, $taskPattern)
  if (-not $taskMatch.Success) { throw "Missing ABI probe function: $Name" }
  # Comments do not affect code. Keep all instructions, directives, offsets,
  # stack slots and labels so register/return/vtable differences remain visible.
  return (($taskMatch.Value -split '\r?\n' | ForEach-Object { ($_ -replace '#.*$', '').Trim() } |
      Where-Object { $_ }) -join "`n")
}

$taskCreationHeaderRoot = Join-Path (Split-Path -Parent (Split-Path -Parent $Compiler)) 'include'
$taskPrivateDataProbe = Join-Path $OutputDirectory 'private-data-contract.cpp'
@'
#define CINTERFACE
#include <d3d12.h>
#include <stddef.h>
using PrivateDataFunction = HRESULT (STDMETHODCALLTYPE*)(ID3D12Object*, REFGUID, UINT*, void*);
static_assert(__is_same(decltype(ID3D12ObjectVtbl::GetPrivateData), PrivateDataFunction));
static_assert(offsetof(ID3D12ObjectVtbl, GetPrivateData) == 3 * sizeof(void*));
extern "C" HRESULT private_data_forward(ID3D12Object* object, REFGUID key, UINT* bytes, void* output) {
  return object->lpVtbl->GetPrivateData(object, key, bytes, output);
}
'@ | Set-Content -LiteralPath $taskPrivateDataProbe -Encoding utf8
foreach ($taskPair in @(@('gnu', 'x86_64-w64-windows-gnu'), @('msvc', 'x86_64-pc-windows-msvc'))) {
  & $Compiler -target $taskPair[1] -S -O1 -std=c++20 -fno-ms-compatibility -fms-extensions `
    -isystem $taskCreationHeaderRoot $taskPrivateDataProbe -o (Join-Path $OutputDirectory ('private-data-' + $taskPair[0] + '.s'))
  if ($LASTEXITCODE -ne 0) { throw "Private-data COM contract failed for $($taskPair[1])." }
}
$taskPrivateDataGnu = Get-Content -Raw -LiteralPath (Join-Path $OutputDirectory 'private-data-gnu.s')
$taskPrivateDataMsvc = Get-Content -Raw -LiteralPath (Join-Path $OutputDirectory 'private-data-msvc.s')
if ((Get-AbiFunction $taskPrivateDataGnu 'private_data_forward') -cne (Get-AbiFunction $taskPrivateDataMsvc 'private_data_forward')) {
  throw 'GetPrivateData COM forwarding differs across GNU/MSVC targets; do not load the add-on.'
}
foreach ($taskPair in @(@('gnu', 'x86_64-w64-windows-gnu'), @('msvc', 'x86_64-pc-windows-msvc'))) {
  & $Compiler -target $taskPair[1] -S -O1 -std=c++20 -fno-ms-compatibility -fms-extensions `
    -isystem $taskCreationHeaderRoot (Join-Path $PSScriptRoot 'resource_creation_slots.cpp') `
    -o (Join-Path $OutputDirectory ('resource-creation-' + $taskPair[0] + '.s'))
  if ($LASTEXITCODE -ne 0) { throw "Resource creation COM contract failed for $($taskPair[1])." }
}
$taskCreationGnu = Get-Content -Raw -LiteralPath (Join-Path $OutputDirectory 'resource-creation-gnu.s')
$taskCreationMsvc = Get-Content -Raw -LiteralPath (Join-Path $OutputDirectory 'resource-creation-msvc.s')
$taskCreationNames = @([regex]::Matches($taskCreationGnu, '(?m)^(creation_\w+):') | ForEach-Object { $_.Groups[1].Value })
if ($taskCreationNames.Count -ne 10) { throw 'Resource creation COM probes are incomplete.' }
foreach ($taskName in $taskCreationNames) {
  if ((Get-AbiFunction $taskCreationGnu $taskName) -cne (Get-AbiFunction $taskCreationMsvc $taskName)) {
    throw "Resource creation COM forwarding differs in $taskName; do not load the add-on."
  }
}

$taskPositiveSource = Join-Path $PSScriptRoot 'subset.cpp'
$taskNegativeSource = Join-Path $PSScriptRoot 'incompatible.cpp'
foreach ($taskPair in @(@('gnu', 'x86_64-w64-windows-gnu'), @('msvc', 'x86_64-pc-windows-msvc'))) {
  Compile-AbiProbe $taskPositiveSource $taskPair[1] (Join-Path $OutputDirectory ('subset-' + $taskPair[0] + '.s'))
  Compile-AbiProbe $taskNegativeSource $taskPair[1] (Join-Path $OutputDirectory ('incompatible-' + $taskPair[0] + '.s'))
}
$taskGnu = Get-Content -Raw -LiteralPath (Join-Path $OutputDirectory 'subset-gnu.s')
$taskMsvc = Get-Content -Raw -LiteralPath (Join-Path $OutputDirectory 'subset-msvc.s')
$taskNames = @([regex]::Matches($taskGnu, '(?m)^(probe_\w+):') | ForEach-Object { $_.Groups[1].Value })
if ($taskNames.Count -lt 20) { throw 'ABI subset probe is incomplete.' }
foreach ($taskName in $taskNames) {
  if ((Get-AbiFunction $taskGnu $taskName) -cne (Get-AbiFunction $taskMsvc $taskName)) {
    throw "ABI mismatch in allowed function '$taskName'; do not load the add-on. Compare assembly in $OutputDirectory."
  }
}
$taskNegativeGnu = Get-Content -Raw -LiteralPath (Join-Path $OutputDirectory 'incompatible-gnu.s')
$taskNegativeMsvc = Get-Content -Raw -LiteralPath (Join-Path $OutputDirectory 'incompatible-msvc.s')
foreach ($taskName in @('probe_large_return', 'probe_handle_return', 'probe_vec_return')) {
  if ((Get-AbiFunction $taskNegativeGnu $taskName) -ceq (Get-AbiFunction $taskNegativeMsvc $taskName)) {
    throw "Expected incompatible control '$taskName' did not differ; investigate compiler/probe changes."
  }
}
& $Compiler -target x86_64-pc-windows-msvc -c -O2 -std=c++20 -ffreestanding -fms-extensions -nostdinc++ `
  -I (Join-Path $PSScriptRoot 'shims') -I $ReShadeInclude (Join-Path $PSScriptRoot 'native_bridge.cpp') `
  -o (Join-Path $OutputDirectory 'native_bridge.obj')
if ($LASTEXITCODE -ne 0) { throw 'MSVC ABI bridge compilation failed.' }
$taskResult = [ordered]@{
  passed = $true
  testedFunctions = $taskNames
  incompatibleControls = @('get_resource_desc', 'get_resource_from_view', 'ImGui::GetWindowPos')
  reshadeCommit = $taskDependencies.reshade.commit
  imguiCommit = $taskDependencies.imgui.commit
  compiler = (& $Compiler --version | Select-Object -First 1)
  bridgeObject = (Join-Path $OutputDirectory 'native_bridge.obj')
  nativeComHeaderContract = $true
  nativePrivateDataSlot = 3
  nativePrivateDataForwardingMatches = $true
  nativeRenderBoundarySlots = @(12, 13, 16, 17, 26, 68, 69, 80)
  nativeResourceCreationSlots = @(27, 29, 30, 53, 55, 69, 70, 76, 77, 78)
  nativeResourceCreationForwarding = $taskCreationNames
  unwrappedDeviceIidMatchesPinnedReShade = $true
  nativeComMethods = @($taskMemberCalls | Where-Object { $_ -cmatch '^[A-Z]' })
  productionTranslationUnits = $taskProductionSources
  inventoriedSourceClosure = @($taskReviewedFiles | Sort-Object)
  addonDependencyFiles = $taskDependencyFiles
  limitation = 'Only inventoried ReShade/ImGui boundary calls and layouts are compared. Native COM uses pinned explicit-return headers plus separate GPU regression; no general MSVC/MinGW compatibility is implied.'
}
$taskResult | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
Write-Output "ABI gate passed: $($taskNames.Count) allowed boundary/layout probes match; all 3 incompatible-return controls differ."
