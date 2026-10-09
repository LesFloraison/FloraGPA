param(
    [string]$QtRoot = '',
    [ValidateSet('Release')][string]$Configuration = 'Release',
    [string]$OutputDirectory = '',
    [string]$VisualStudioRoot = '',
    [string]$RenderDocRoot = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo "build\vs2022\$Configuration"
$destination = if ($OutputDirectory) { [System.IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $repo 'out\FloraGPA' }
if ((Test-Path -LiteralPath $destination) -and
    ((-not (Test-Path -LiteralPath $destination -PathType Container)) -or
     @(Get-ChildItem -LiteralPath $destination -Force).Count)) {
    throw 'Package output must be an empty directory. Choose a new -OutputDirectory.'
}
$cache = @{}
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
    if ((Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash -ne $file.Value) {
        throw "RenderDoc runtime differs from the pinned official 1.45 x64 build: $candidate"
    }
}
$rdcLicenses = Join-Path $repo 'third_party\renderdoc\runtime-licenses'
foreach ($file in $rdcManifest.licenses) {
    $candidate = Join-Path $rdcLicenses $file.path
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf) -or
        (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "RenderDoc runtime notice is missing or modified: $($file.path)"
    }
}
$cachePath = Join-Path $repo 'build\vs2022\CMakeCache.txt'
if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
    foreach ($line in Get-Content -LiteralPath $cachePath) {
        if ($line -match '^([^#/:][^:]*):[^=]+=(.*)$') {
            $cache[$Matches[1]] = $Matches[2]
        }
    }
}
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
$rdcDestination = Join-Path $destination 'RenderDoc'
New-Item -ItemType Directory -Path $rdcDestination | Out-Null
foreach ($file in $rdcManifest.files.PSObject.Properties) {
    Copy-Item -LiteralPath (Join-Path $RenderDocRoot $file.Name) -Destination $rdcDestination
}
Copy-Item -LiteralPath $rdcManifestPath -Destination (Join-Path $rdcDestination 'runtime-manifest.json')
Copy-Item -LiteralPath $rdcLicenses -Destination (Join-Path $rdcDestination 'licenses') -Recurse
Copy-Item -LiteralPath (Join-Path $repo 'THIRD_PARTY.md') -Destination $destination -Force
$licenses = Join-Path $destination 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'third_party\nlohmann\LICENSE.MIT') -Destination (Join-Path $licenses 'nlohmann-json-MIT.txt') -Force
Copy-Item -LiteralPath (Join-Path $repo 'third_party\renderdoc\LICENSE.MIT') -Destination (Join-Path $licenses 'renderdoc-api-MIT.txt') -Force
Copy-Item -LiteralPath (Join-Path $repo 'third_party\metrics-discovery\LICENSE.md') -Destination (Join-Path $licenses 'intel-metrics-discovery-MIT.txt') -Force
Copy-Item -Path (Join-Path $repo 'third_party\qt\*.txt') -Destination $licenses -Force
$qtLicenses = Join-Path $QtRoot '..\..\Licenses'
if (Test-Path -LiteralPath $qtLicenses) {
    Copy-Item -Path (Join-Path $qtLicenses '*') -Destination $licenses -Force
}
Write-Output "Portable application: $destination\FloraGPA.exe"
