param(
    [string]$QtRoot = 'D:\Qt\6.11.2\msvc2022_64',
    [ValidateSet('Release')][string]$Configuration = 'Release',
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo "build\vs2022\$Configuration"
$destination = if ($OutputDirectory) { [System.IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $repo 'out\FloraGPA' }
New-Item -ItemType Directory -Path $destination -Force | Out-Null
foreach ($name in @('FloraGPA.exe','FloraGPA.Worker.exe','FloraGPA.Cli.exe','FloraGPA.Rdc.exe')) {
    Copy-Item -LiteralPath (Join-Path $build $name) -Destination $destination -Force
}
$env:VSINSTALLDIR = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$env:VCINSTALLDIR = Join-Path $env:VSINSTALLDIR 'VC'
& (Join-Path $QtRoot 'bin\windeployqt.exe') --release --compiler-runtime --include-plugins qoffscreen --no-translations --no-opengl-sw (Join-Path $destination 'FloraGPA.exe')
if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
$redistRoot = Join-Path $env:VCINSTALLDIR 'Redist\MSVC'
$crt = Get-ChildItem -LiteralPath $redistRoot -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending | ForEach-Object { Join-Path $_.FullName 'x64\Microsoft.VC143.CRT' } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $crt) { throw 'Cannot find the x64 VC143 redistributable runtime.' }
Copy-Item -Path (Join-Path $crt '*.dll') -Destination $destination -Force
Copy-Item -LiteralPath (Join-Path $repo 'THIRD_PARTY.md') -Destination $destination -Force
$licenses = Join-Path $destination 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'third_party\nlohmann\LICENSE.MIT') -Destination (Join-Path $licenses 'nlohmann-json-MIT.txt') -Force
Copy-Item -LiteralPath (Join-Path $repo 'third_party\renderdoc\LICENSE.MIT') -Destination (Join-Path $licenses 'renderdoc-api-MIT.txt') -Force
Copy-Item -Path (Join-Path $repo 'third_party\qt\*.txt') -Destination $licenses -Force
$qtLicenses = Join-Path $QtRoot '..\..\Licenses'
if (Test-Path -LiteralPath $qtLicenses) {
    Copy-Item -Path (Join-Path $qtLicenses '*') -Destination $licenses -Force
}
Write-Output "Portable application: $destination\FloraGPA.exe"
