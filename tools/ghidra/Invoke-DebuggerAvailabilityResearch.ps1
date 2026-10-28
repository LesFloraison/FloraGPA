param(
    [Parameter(Mandatory=$true)][string]$Headless,
    [Parameter(Mandatory=$true)][string]$ProjectDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$GpaRoot = 'C:\Program Files\IntelSWTools\GPA'
)
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath($ProjectDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory)
$binary = Join-Path $GpaRoot 'com/intel/framedb/framedb_qt.dll'
$expected = '35a03995d50f390ed74722385583e7bb011f84e563a579c88086d8039ad1df80'
function Assert-Binary {
    if ((Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
        throw 'Pinned framedb image changed'
    }
}
Assert-Binary
if (Test-Path -LiteralPath $output) { throw 'Use a fresh evidence directory' }
New-Item -ItemType Directory -Path $output | Out-Null
foreach ($item in @(
    @{Name='presenter-dispatch'; Script='ExportFunctionEvidence.java'; Arguments=@('0xb66d0','0x118980','0xd22e0'); Count=3},
    @{Name='presenter-metadata'; Script='ExportAddressEvidence.java'; Arguments=@('0x774f00','0x7754c0','0x775d50'); Count=3}
)) {
    $text = Join-Path $output ($item.Name + '.txt')
    $log = Join-Path $output ($item.Name + '.log')
    $scriptArgs = $item.Arguments
    & $Headless $project Dx11NativeResearch -process framedb_qt.dll -readOnly -noanalysis -scriptPath $PSScriptRoot `
        -postScript $item.Script $text @scriptArgs *> $log
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $text) -or
        (Select-String -LiteralPath $log -SimpleMatch 'ERROR REPORT SCRIPT ERROR') -or
        (Get-Content -LiteralPath $text -Tail 1) -ne "COMPLETE $($item.Count)" -or
        (Select-String -LiteralPath $text -Pattern '^NO FUNCTION ') -or
        (Get-Content -LiteralPath $text -TotalCount 1) -ne "PROGRAM framedb_qt.dll SHA256 $expected BASE 180000000") {
        throw "Incomplete original availability export: $($item.Name)"
    }
}
Assert-Binary
@{path=$binary;sha256=$expected;version=(Get-Item -LiteralPath $binary).VersionInfo.FileVersion} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'binary.json') -Encoding utf8
Get-ChildItem -LiteralPath $output -File | ForEach-Object {
    @{name=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'evidence-sha256.json') -Encoding utf8
