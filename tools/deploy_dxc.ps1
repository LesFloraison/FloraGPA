param(
    [Parameter(Mandatory)][string]$DxcRoot,
    [string]$OutputDirectory = '',
    [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'
$notices = Join-Path $PSScriptRoot '..\third_party\dxc'
$manifestPath = Join-Path $notices 'runtime-manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$DxcRoot = [IO.Path]::GetFullPath($DxcRoot)
function Test-DxcFile([string]$Path, [string]$Expected) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Pinned DXC file is missing: $Path" }
    $stream = [IO.File]::OpenRead($Path)
    $hash = [Security.Cryptography.SHA256]::Create()
    try { $actual = ([BitConverter]::ToString($hash.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $hash.Dispose() }
    if ($actual -ne $Expected) { throw "Pinned DXC checksum mismatch: $Path" }
}
foreach ($file in $manifest.files.PSObject.Properties) {
    Test-DxcFile (Join-Path $DxcRoot ('bin\x64\' + $file.Name)) $file.Value
}
foreach ($file in $manifest.licenses.PSObject.Properties) {
    Test-DxcFile (Join-Path $notices $file.Name) $file.Value
}
if ($ValidateOnly) { return }
if (-not $OutputDirectory) { throw 'Specify the DXCompiler output directory.' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $OutputDirectory) -and
    ((-not (Test-Path -LiteralPath $OutputDirectory -PathType Container)) -or
     @(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count)) {
    throw 'DXCompiler output must be a new or empty directory.'
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
foreach ($file in $manifest.files.PSObject.Properties) {
    Copy-Item -LiteralPath (Join-Path $DxcRoot ('bin\x64\' + $file.Name)) -Destination $OutputDirectory
}
$licenseOutput = Join-Path $OutputDirectory 'licenses'
New-Item -ItemType Directory -Path $licenseOutput | Out-Null
foreach ($file in $manifest.licenses.PSObject.Properties) {
    Copy-Item -LiteralPath (Join-Path $notices $file.Name) -Destination $licenseOutput
}
Copy-Item -LiteralPath $manifestPath -Destination $OutputDirectory
Write-Output "Pinned DXC runtime: $OutputDirectory"
