param(
    [Parameter(Mandatory)][string]$SourceDirectory,
    [string]$MSBuild = '',
    [ValidateRange(1, 8)][int]$Jobs = 3
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$patch = Join-Path $repo 'third_party/renderdoc/patches/dx12-d16-history.patch'
$workgroupPatch = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxbc-workgroup.patch'
$usagePatch = Join-Path $repo 'third_party/renderdoc/patches/dx12-usage-visibility.patch'
$depthSnapshotPatch = Join-Path $repo 'third_party/renderdoc/patches/dx12-readonly-depth-snapshot.patch'
$gpuDotPatch = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxbc-gpu-dot.patch'
$gpuMadPatch = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxbc-gpu-mad.patch'
$dxilContractPatch = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxil-fast-contraction.patch'
$retainedComputePatch = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxil-retained-compute.patch'
$uavTexturePatch = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxil-uav-texture.patch'
$textureStoresPatch = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxil-texture-stores.patch'
$packedStorePatch = Join-Path $repo 'third_party/renderdoc/patches/dx12-dxil-packed-store.patch'
$patches = @($patch, $workgroupPatch, $usagePatch, $depthSnapshotPatch, $gpuDotPatch, $gpuMadPatch, $dxilContractPatch, $retainedComputePatch, $uavTexturePatch, $textureStoresPatch, $packedStorePatch)
$upstream = '2fc0bc04cb95499635f63986a55bc6f67849dd9f'
$head = & git -C $source rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $head -ne $upstream) { throw 'Use a dedicated RenderDoc v1.45 checkout at the pinned upstream commit.' }
$actual = (& git -C $source diff --binary --no-ext-diff --no-color HEAD) -join "`n"
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect RenderDoc source changes.' }
# Build the exact expected tree without touching the checkout's real index.
# Separate patches can modify files whose combined diff has a different order.
$temporaryIndex = Join-Path ([IO.Path]::GetTempPath()) ('flora-rdc-index-' + [Guid]::NewGuid().ToString('N'))
$previousIndex = $env:GIT_INDEX_FILE
try {
    $env:GIT_INDEX_FILE = $temporaryIndex
    & git -C $source read-tree $upstream
    if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize expected RenderDoc source tree.' }
    foreach ($fix in $patches) {
        & git -C $source apply --cached $fix
        if ($LASTEXITCODE -ne 0) { throw "RenderDoc patch does not apply to the pinned source: $fix" }
    }
    $expectedTree = & git -C $source write-tree
    if ($LASTEXITCODE -ne 0) { throw 'Cannot record expected RenderDoc source tree.' }
} finally {
    $env:GIT_INDEX_FILE = $previousIndex
    if (Test-Path -LiteralPath $temporaryIndex) { Remove-Item -LiteralPath $temporaryIndex }
}
if ($actual.TrimEnd() -eq '') {
    foreach ($fix in $patches) {
        & git -C $source apply $fix
        if ($LASTEXITCODE -ne 0) { throw "Cannot apply RenderDoc patch: $fix" }
    }
}
& git -C $source diff --quiet $expectedTree --
if ($LASTEXITCODE -ne 0) { throw 'RenderDoc source differs from the pinned Flora patches; use a dedicated checkout.' }
if (-not $MSBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $MSBuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild/**/Bin/MSBuild.exe' | Select-Object -First 1
}
if (-not $MSBuild -or -not (Test-Path -LiteralPath $MSBuild -PathType Leaf)) { throw 'MSBuild with the Visual Studio 2022 C++ toolset is required.' }
$log = Join-Path $source 'flora-d16-build.log'
$solution = $source.Replace('\', '/') + '/'
$projects = @(
    'renderdoc/3rdparty/breakpad/client/windows/common.vcxproj',
    'renderdoc/3rdparty/breakpad/client/windows/crash_generation/crash_generation_client.vcxproj',
    'renderdoc/3rdparty/breakpad/client/windows/handler/exception_handler.vcxproj',
    'renderdoc/renderdoc.vcxproj'
)
$oldCl = $env:CL
try {
    $env:CL = '/MP2'
    foreach ($project in $projects) {
        & $MSBuild (Join-Path $source $project) /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 "/p:SolutionDir=$solution" /p:TreatWarningAsError=false "/m:$Jobs" /verbosity:minimal *>> $log
        if ($LASTEXITCODE -ne 0) { throw "RenderDoc build failed; see $log" }
    }
} finally { $env:CL = $oldCl }
$runtime = Join-Path $source 'x64/Release'
$library = Join-Path $runtime 'renderdoc.dll'
if (-not (Test-Path -LiteralPath $library -PathType Leaf)) { throw 'RenderDoc build produced no DLL.' }
$manifest = [ordered]@{
    format = 'flora-renderdoc-d16-build-1'
    upstream_commit = $upstream
    version = '1.45'
    patch_version = 1
    patch_sha256 = (Get-FileHash -LiteralPath $patch -Algorithm SHA256).Hash.ToLowerInvariant()
    dxbc_workgroup_patch_version = 1
    dxbc_workgroup_patch_sha256 = (Get-FileHash -LiteralPath $workgroupPatch -Algorithm SHA256).Hash.ToLowerInvariant()
    dx12_usage_visibility_patch_version = 1
    dx12_usage_visibility_patch_sha256 = (Get-FileHash -LiteralPath $usagePatch -Algorithm SHA256).Hash.ToLowerInvariant()
    readonly_depth_snapshot_patch_version = 1
    readonly_depth_snapshot_patch_sha256 = (Get-FileHash -LiteralPath $depthSnapshotPatch -Algorithm SHA256).Hash.ToLowerInvariant()
    dxbc_gpu_dot_patch_version = 1
    dxbc_gpu_dot_patch_sha256 = (Get-FileHash -LiteralPath $gpuDotPatch -Algorithm SHA256).Hash.ToLowerInvariant()
    dxbc_gpu_mad_patch_version = 1
    dxbc_gpu_mad_patch_sha256 = (Get-FileHash -LiteralPath $gpuMadPatch -Algorithm SHA256).Hash.ToLowerInvariant()
    dxil_fast_contraction_patch_version = 1
    dxil_retained_compute_patch_version = 2
    dxil_uav_texture_patch_version = 1
    dxil_texture_stores_patch_version = 1
    dxil_packed_store_patch_version = 1
    dxil_packed_store_patch_sha256 = (Get-FileHash -LiteralPath $packedStorePatch -Algorithm SHA256).Hash.ToLowerInvariant()
    dxil_texture_stores_patch_sha256 = (Get-FileHash -LiteralPath $textureStoresPatch -Algorithm SHA256).Hash.ToLowerInvariant()
    dxil_uav_texture_patch_sha256 = (Get-FileHash -LiteralPath $uavTexturePatch -Algorithm SHA256).Hash.ToLowerInvariant()
    dxil_retained_compute_patch_sha256 = (Get-FileHash -LiteralPath $retainedComputePatch -Algorithm SHA256).Hash.ToLowerInvariant()
    dxil_fast_contraction_patch_sha256 = (Get-FileHash -LiteralPath $dxilContractPatch -Algorithm SHA256).Hash.ToLowerInvariant()
    renderdoc_sha256 = (Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash.ToLowerInvariant()
    toolset = 'v143'
    configuration = 'Release|x64'
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runtime 'flora-d16-patch.json') -Encoding utf8
Write-Output "Patched RenderDoc runtime: $runtime"
