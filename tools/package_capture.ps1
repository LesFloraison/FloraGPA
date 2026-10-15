param(
    [string]$BuildDirectory = "$PSScriptRoot\..\build\capture",
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = [System.IO.Path]::GetFullPath($BuildDirectory)
$destination = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $destination) { throw 'Capture package output must be a new directory.' }
$cache = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')) {
    if ($line -match '^([^#/:][^:]*):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
}
if ($cache['FLORA_BUILD_CAPTURE'] -ne 'ON') { throw 'Build does not enable independent capture.' }
$vs = ($cache['CMAKE_GENERATOR_INSTANCE'] -split ',version=', 2)[0]
if (-not $vs) { throw 'Configured Visual Studio instance is missing.' }
$redist = Join-Path $vs 'VC\Redist\MSVC'
$crt = Get-ChildItem -LiteralPath $redist -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending | ForEach-Object { Join-Path $_.FullName 'x64\Microsoft.VC143.CRT' } |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $crt) { throw 'Configured x64 VC143 redistributable runtime is missing.' }
$sources = @{}
$binarySubdirectory = if ($cache['FLORA_BUILD_REPLAY'] -eq 'ON') { 'Release' } else { 'capture\Release' }
foreach ($name in @('FloraCapture.exe','FloraCaptureRuntime.dll','FloraCaptureShim.dll')) { $sources[$name] = Join-Path $build "$binarySubdirectory\$name" }
foreach ($name in @('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) { $sources[$name] = Join-Path $crt $name }
foreach ($source in $sources.Values) { if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing package input: $source" } }
$identityText = & $sources['FloraCapture.exe'] --version
if ($LASTEXITCODE -ne 0) { throw 'Cannot query the capture controller build identity.' }
$identity = $identityText | ConvertFrom-Json
if ($identity.schema -ne 1 -or $identity.application -ne 'FloraCapture' -or $identity.architecture -ne 'x64' -or
    ($identity.protocol_version -isnot [int] -and $identity.protocol_version -isnot [long]) -or
    $identity.protocol_version -lt 1 -or $identity.protocol_version -gt [uint32]::MaxValue) {
    throw 'Invalid capture controller build identity.'
}
New-Item -ItemType Directory -Path $destination | Out-Null
foreach ($name in $sources.Keys) { Copy-Item -LiteralPath $sources[$name] -Destination (Join-Path $destination $name) }
$licenses = Join-Path $destination 'licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'third_party\detours\LICENSE.md') -Destination (Join-Path $licenses 'detours-MIT.txt')
Copy-Item -LiteralPath (Join-Path $repo 'third_party\nlohmann\LICENSE.MIT') -Destination (Join-Path $licenses 'nlohmann-json-MIT.txt')
Copy-Item -LiteralPath (Join-Path $repo 'src\capture\dependencies.json') -Destination $destination
$hashes = @{}
foreach ($file in Get-ChildItem -LiteralPath $destination -File -Recurse) {
    $stream = [System.IO.File]::OpenRead($file.FullName)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try { $hash = ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
    $hashes[$file.FullName.Substring($destination.Length+1).Replace('\','/')] = $hash
}
@{schema=1;milestone='native_present_capture';capture_profile='limited; query runtime status';protocol_version=$identity.protocol_version;files=$hashes} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destination 'manifest.json') -Encoding UTF8
Write-Output "Native capture package: $destination"
