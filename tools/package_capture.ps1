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
foreach ($name in @('FloraCapture.exe','FloraCaptureRuntime.dll')) { $sources[$name] = Join-Path $build "$binarySubdirectory\$name" }
foreach ($name in @('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) { $sources[$name] = Join-Path $crt $name }
foreach ($source in $sources.Values) { if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing package input: $source" } }
New-Item -ItemType Directory -Path $destination | Out-Null
foreach ($name in $sources.Keys) { Copy-Item -LiteralPath $sources[$name] -Destination (Join-Path $destination $name) }
$licenses = Join-Path $destination 'licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'third_party\detours\LICENSE.md') -Destination (Join-Path $licenses 'detours-MIT.txt')
Copy-Item -LiteralPath (Join-Path $repo 'third_party\nlohmann\LICENSE.MIT') -Destination (Join-Path $licenses 'nlohmann-json-MIT.txt')
Copy-Item -LiteralPath (Join-Path $repo 'src\capture\dependencies.json') -Destination $destination
$hashes = @{}
foreach ($file in Get-ChildItem -LiteralPath $destination -File -Recurse) {
    $hashes[$file.FullName.Substring($destination.Length+1).Replace('\','/')] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
@{schema=1;milestone='native_bootstrap_only';capture_available=$false;files=$hashes} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destination 'manifest.json') -Encoding UTF8
Write-Output "Capture bootstrap package: $destination"
