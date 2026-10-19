param(
    [string]$QtRoot = '',
    [ValidateSet('Release')][string]$Configuration = 'Release',
    [string]$OutputDirectory = '',
    [string]$VisualStudioRoot = '',
    [string]$RenderDocRoot = '',
    [string]$PatchedRenderDocRoot = '',
    [string]$DxcRoot = '',
    [string]$BuildDirectory = "$PSScriptRoot\..\build\vs2022"
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildRoot = [System.IO.Path]::GetFullPath($BuildDirectory)
$build = Join-Path $buildRoot $Configuration
$destination = if ($OutputDirectory) { [System.IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $repo 'out\FloraGPA' }
if ((Test-Path -LiteralPath $destination) -and
    ((-not (Test-Path -LiteralPath $destination -PathType Container)) -or
     @(Get-ChildItem -LiteralPath $destination -Force).Count)) {
    throw 'Package output must be an empty directory. Choose a new -OutputDirectory.'
}
$cache = @{}
if (-not $DxcRoot) { $DxcRoot = Join-Path $repo 'artifacts\dxc-1.8.2505.1' }
& (Join-Path $PSScriptRoot 'deploy_dxc.ps1') -DxcRoot $DxcRoot -ValidateOnly
function Get-PackageSha256([string]$Path) {
    # Avoid module auto-loading differences between Windows PowerShell and pwsh.
    $stream = [System.IO.File]::OpenRead($Path)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}
# Bundle the exact native replay ABI accepted by FloraGPA, never whichever DLL
# happens to be first on PATH. Validate before creating a partial package.
if (-not $RenderDocRoot) { $RenderDocRoot = Join-Path $env:ProgramFiles 'RenderDoc' }
$RenderDocRoot = [System.IO.Path]::GetFullPath($RenderDocRoot)
$rdcManifestPath = Join-Path $repo 'third_party\renderdoc\runtime-manifest.json'
$rdcManifest = Get-Content -LiteralPath $rdcManifestPath -Raw | ConvertFrom-Json
foreach ($file in $rdcManifest.files.PSObject.Properties) {
    $candidate = Join-Path $RenderDocRoot $file.Name
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "RenderDoc runtime is missing: $candidate. Pass -RenderDocRoot pointing to the official 1.45 x64 ZIP contents."
    }
    if ((Get-PackageSha256 $candidate) -ne $file.Value) {
        throw "RenderDoc runtime differs from the pinned official 1.45 x64 build: $candidate"
    }
}
$patchedLibrary = $null
$patchPath = Join-Path $repo 'third_party/renderdoc/patches/dx12-d16-history.patch'
$workgroupPatchPath = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxbc-workgroup.patch'
$usagePatchPath = Join-Path $repo 'third_party/renderdoc/patches/dx12-usage-visibility.patch'
$depthSnapshotPatchPath = Join-Path $repo 'third_party/renderdoc/patches/dx12-readonly-depth-snapshot.patch'
$gpuDotPatchPath = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxbc-gpu-dot.patch'
$gpuMadPatchPath = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxbc-gpu-mad.patch'
$dxilContractPatchPath = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxil-fast-contraction.patch'
$retainedComputePatchPath = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxil-retained-compute.patch'
$uavTexturePatchPath = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxil-uav-texture.patch'
$textureStoresPatchPath = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxil-texture-stores.patch'
$packedStorePatchPath = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxil-packed-store.patch'
$preparedComputePatchPath = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxil-prepared-compute.patch'
$placedHeapPatchPath = Join-Path $repo 'third_party/renderdoc/patches/dx12-placed-heap-restore.patch'
if ($PatchedRenderDocRoot) {
    $PatchedRenderDocRoot = [IO.Path]::GetFullPath($PatchedRenderDocRoot)
    $patchManifestPath = Join-Path $PatchedRenderDocRoot 'flora-d16-patch.json'
    $patchManifest = Get-Content -LiteralPath $patchManifestPath -Raw | ConvertFrom-Json
    $patchedLibrary = Join-Path $PatchedRenderDocRoot 'renderdoc.dll'
    if ($patchManifest.format -ne 'flora-renderdoc-d16-build-1' -or
        $patchManifest.upstream_commit -ne $rdcManifest.commit -or
        $patchManifest.version -ne '1.45' -or $patchManifest.patch_version -ne 1 -or
        $patchManifest.configuration -ne 'Release|x64' -or
        $patchManifest.patch_sha256 -ne (Get-PackageSha256 $patchPath) -or
        $patchManifest.dxbc_workgroup_patch_version -ne 1 -or
        $patchManifest.dxbc_workgroup_patch_sha256 -ne (Get-PackageSha256 $workgroupPatchPath) -or
        $patchManifest.dx12_usage_visibility_patch_version -ne 1 -or
        $patchManifest.dx12_usage_visibility_patch_sha256 -ne (Get-PackageSha256 $usagePatchPath) -or
        $patchManifest.readonly_depth_snapshot_patch_version -ne 1 -or
        $patchManifest.readonly_depth_snapshot_patch_sha256 -ne (Get-PackageSha256 $depthSnapshotPatchPath) -or
        $patchManifest.dxbc_gpu_dot_patch_version -ne 1 -or
        $patchManifest.dxbc_gpu_dot_patch_sha256 -ne (Get-PackageSha256 $gpuDotPatchPath) -or
        $patchManifest.dxbc_gpu_mad_patch_version -ne 1 -or
        $patchManifest.dxbc_gpu_mad_patch_sha256 -ne (Get-PackageSha256 $gpuMadPatchPath) -or
        $patchManifest.dxil_fast_contraction_patch_version -ne 1 -or
        $patchManifest.dxil_retained_compute_patch_version -ne 2 -or
        $patchManifest.dxil_uav_texture_patch_version -ne 1 -or
        $patchManifest.dxil_texture_stores_patch_version -ne 1 -or
        $patchManifest.dxil_packed_store_patch_version -ne 1 -or
        $patchManifest.dxil_prepared_compute_patch_version -ne 1 -or
        $patchManifest.dx12_placed_heap_restore_patch_version -ne 1 -or
        $patchManifest.dx12_placed_heap_restore_patch_sha256 -ne (Get-PackageSha256 $placedHeapPatchPath) -or
        $patchManifest.dxil_prepared_compute_patch_sha256 -ne (Get-PackageSha256 $preparedComputePatchPath) -or
        $patchManifest.dxil_packed_store_patch_sha256 -ne (Get-PackageSha256 $packedStorePatchPath) -or
        $patchManifest.dxil_texture_stores_patch_sha256 -ne (Get-PackageSha256 $textureStoresPatchPath) -or
        $patchManifest.dxil_uav_texture_patch_sha256 -ne (Get-PackageSha256 $uavTexturePatchPath) -or
        $patchManifest.dxil_retained_compute_patch_sha256 -ne (Get-PackageSha256 $retainedComputePatchPath) -or
        $patchManifest.dxil_fast_contraction_patch_sha256 -ne (Get-PackageSha256 $dxilContractPatchPath) -or
        $patchManifest.renderdoc_sha256 -ne (Get-PackageSha256 $patchedLibrary)) {
        throw 'Patched RenderDoc does not match the pinned source patch and build manifest. Use tools/build_renderdoc_d16.ps1.'
    }
}
$rdcLicenses = Join-Path $repo 'third_party\renderdoc\runtime-licenses'
foreach ($file in $rdcManifest.licenses) {
    $candidate = Join-Path $rdcLicenses $file.path
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf) -or
        (Get-PackageSha256 $candidate) -ne $file.sha256) {
        throw "RenderDoc runtime notice is missing or modified: $($file.path)"
    }
}
$cachePath = Join-Path $buildRoot 'CMakeCache.txt'
if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
    foreach ($line in Get-Content -LiteralPath $cachePath) {
        if ($line -match '^([^#/:][^:]*):[^=]+=(.*)$') {
            $cache[$Matches[1]] = $Matches[2]
        }
    }
}
if ($cache['FLORA_BUILD_REPLAY'] -ne 'ON') { throw 'Application packaging requires a replay-enabled build.' }
if (-not $QtRoot -and $cache['Qt6_DIR']) {
    $QtRoot = [System.IO.Path]::GetFullPath((Join-Path $cache['Qt6_DIR'] '..\..\..'))
}
if (-not $VisualStudioRoot -and $cache['CMAKE_GENERATOR_INSTANCE']) {
    # CMake may append ",version=..." to the selected instance path.
    $VisualStudioRoot = ($cache['CMAKE_GENERATOR_INSTANCE'] -split ',version=', 2)[0]
}
if (-not $QtRoot) { throw 'Cannot find the configured Qt installation. Configure first or pass -QtRoot.' }
if (-not $VisualStudioRoot) { throw 'Cannot find the configured Visual Studio installation. Configure first or pass -VisualStudioRoot.' }
$QtRoot = [System.IO.Path]::GetFullPath($QtRoot)
$VisualStudioRoot = [System.IO.Path]::GetFullPath($VisualStudioRoot)
$deploy = Join-Path $QtRoot 'bin\windeployqt.exe'
if (-not (Test-Path -LiteralPath $deploy -PathType Leaf)) { throw "Qt deployment tool is missing: $deploy" }
$vcRoot = Join-Path $VisualStudioRoot 'VC'
$redistRoot = Join-Path $vcRoot 'Redist\MSVC'
if (-not (Test-Path -LiteralPath $redistRoot -PathType Container)) {
    throw "Visual Studio x64 redistributables are missing: $redistRoot"
}
$crt = Get-ChildItem -LiteralPath $redistRoot -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending | ForEach-Object { Join-Path $_.FullName 'x64\Microsoft.VC143.CRT' } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $crt) { throw 'Cannot find the x64 VC143 redistributable runtime.' }
foreach ($name in @('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $crt $name) -PathType Leaf)) { throw "Incomplete x64 VC143 runtime: $name" }
}
$binaries = @('FloraGPA.exe','FloraGPA.Worker.exe','FloraGPA.Cli.exe','FloraGPA.Rdc.exe','FloraGPA.Metrics.dll')
if ($cache['FLORA_BUILD_CAPTURE'] -eq 'ON') {
    $binaries += @('FloraCapture.exe','FloraCaptureRuntime.dll','FloraCaptureShim.dll')
}
foreach ($name in $binaries) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $name) -PathType Leaf)) {
        throw "Release binary is missing: $name. Build the GUI configuration before packaging."
    }
}
New-Item -ItemType Directory -Path $destination -Force | Out-Null
foreach ($name in $binaries) {
    Copy-Item -LiteralPath (Join-Path $build $name) -Destination $destination -Force
}
$previousVs = $env:VSINSTALLDIR
$previousVc = $env:VCINSTALLDIR
try {
    $env:VSINSTALLDIR = $VisualStudioRoot
    $env:VCINSTALLDIR = $vcRoot
    # DX11 uses D3DCompiler_47. Do not collect optional DX12 compiler DLLs from
    # whichever SDK/tool directories happen to be present on the caller's PATH.
    & $deploy --release --compiler-runtime --include-plugins qoffscreen --no-system-dxc-compiler --no-translations --no-opengl-sw (Join-Path $destination 'FloraGPA.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
} finally {
    $env:VSINSTALLDIR = $previousVs
    $env:VCINSTALLDIR = $previousVc
}
Copy-Item -Path (Join-Path $crt '*.dll') -Destination $destination -Force
& (Join-Path $PSScriptRoot 'deploy_dxc.ps1') -DxcRoot $DxcRoot -OutputDirectory (Join-Path $destination 'DXCompiler')
$rdcDestination = Join-Path $destination 'RenderDoc'
New-Item -ItemType Directory -Path $rdcDestination | Out-Null
foreach ($file in $rdcManifest.files.PSObject.Properties) {
    Copy-Item -LiteralPath (Join-Path $RenderDocRoot $file.Name) -Destination $rdcDestination
}
# The official native crash server is a separate EXE in this subdirectory.
# Give it app-local CRTs as well; its working directory is not a dependency path.
foreach ($name in @('msvcp140.dll','vcruntime140.dll')) {
    Copy-Item -LiteralPath (Join-Path $crt $name) -Destination $rdcDestination
}
Copy-Item -LiteralPath $rdcManifestPath -Destination (Join-Path $rdcDestination 'runtime-manifest.json')
if ($patchedLibrary) {
    Copy-Item -LiteralPath $patchedLibrary -Destination (Join-Path $rdcDestination 'renderdoc.dll') -Force
    # Retain official provenance separately; the active manifest must describe the shipped DLL.
    Copy-Item -LiteralPath $rdcManifestPath -Destination (Join-Path $rdcDestination 'upstream-runtime-manifest.json')
    $rdcManifest.files.'renderdoc.dll' = $patchManifest.renderdoc_sha256
    $rdcManifest | Add-Member -NotePropertyName flora_patch -NotePropertyValue $patchManifest
    $rdcManifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $rdcDestination 'runtime-manifest.json') -Encoding utf8
    Copy-Item -LiteralPath $patchManifestPath -Destination $rdcDestination
    Copy-Item -LiteralPath $patchPath -Destination $rdcDestination
    Copy-Item -LiteralPath $workgroupPatchPath -Destination $rdcDestination
    Copy-Item -LiteralPath $usagePatchPath -Destination $rdcDestination
    Copy-Item -LiteralPath $depthSnapshotPatchPath -Destination $rdcDestination
    Copy-Item -LiteralPath $gpuDotPatchPath -Destination $rdcDestination
    Copy-Item -LiteralPath $gpuMadPatchPath -Destination $rdcDestination
    Copy-Item -LiteralPath $dxilContractPatchPath -Destination $rdcDestination
    Copy-Item -LiteralPath $retainedComputePatchPath -Destination $rdcDestination
    Copy-Item -LiteralPath $uavTexturePatchPath -Destination $rdcDestination
    Copy-Item -LiteralPath $textureStoresPatchPath -Destination $rdcDestination
    Copy-Item -LiteralPath $packedStorePatchPath -Destination $rdcDestination
    Copy-Item -LiteralPath $preparedComputePatchPath -Destination $rdcDestination
    Copy-Item -LiteralPath $placedHeapPatchPath -Destination $rdcDestination
}
Copy-Item -LiteralPath $rdcLicenses -Destination (Join-Path $rdcDestination 'licenses') -Recurse
Copy-Item -LiteralPath (Join-Path $repo 'THIRD_PARTY.md') -Destination $destination -Force
$licenses = Join-Path $destination 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'third_party\nlohmann\LICENSE.MIT') -Destination (Join-Path $licenses 'nlohmann-json-MIT.txt') -Force
if ($cache['FLORA_BUILD_CAPTURE'] -eq 'ON') {
    Copy-Item -LiteralPath (Join-Path $repo 'third_party\detours\LICENSE.md') -Destination (Join-Path $licenses 'detours-MIT.txt')
    Copy-Item -LiteralPath (Join-Path $repo 'src\capture\dependencies.json') -Destination (Join-Path $destination 'capture-dependencies.json')
}
Copy-Item -LiteralPath (Join-Path $repo 'third_party\renderdoc\LICENSE.MIT') -Destination (Join-Path $licenses 'renderdoc-api-MIT.txt') -Force
Copy-Item -LiteralPath (Join-Path $repo 'third_party\metrics-discovery\LICENSE.md') -Destination (Join-Path $licenses 'intel-metrics-discovery-MIT.txt') -Force
Copy-Item -Path (Join-Path $repo 'third_party\qt\*.txt') -Destination $licenses -Force
$qtLicenses = Join-Path $QtRoot '..\..\Licenses'
if (Test-Path -LiteralPath $qtLicenses) {
    Copy-Item -Path (Join-Path $qtLicenses '*') -Destination $licenses -Force
}
Write-Output "Portable application: $destination\FloraGPA.exe"
