param(
    [Parameter(Mandatory=$true)][string]$Headless,
    [Parameter(Mandatory=$true)][string]$ProjectDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$GpaRoot = 'C:\Program Files\IntelSWTools\GPA'
)
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath($ProjectDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory)
$binary = Join-Path $GpaRoot 'dx11_player.dll'
$expected = '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b'
function Assert-Binary {
    if ((Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
        throw 'Pinned player changed'
    }
}
Assert-Binary
if (Test-Path -LiteralPath $output) { throw 'Use a new evidence output directory' }
New-Item -ItemType Directory -Path $output | Out-Null
function Export-Research([string]$name, [string]$script, [string[]]$arguments, [string]$tail) {
    $text = Join-Path $output ($name + '.txt')
    $log = Join-Path $output ($name + '.log')
    & $Headless $project Dx11NativeResearch -process dx11_player.dll -readOnly -noanalysis -scriptPath $PSScriptRoot `
        -postScript $script $text @arguments *> $log
    if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $log -SimpleMatch 'ERROR REPORT SCRIPT ERROR') -or
        !(Test-Path -LiteralPath $text) -or (Get-Content -LiteralPath $text -Tail 1) -notmatch $tail -or
        (Select-String -LiteralPath $text -Pattern '^NO FUNCTION ')) { throw "Incomplete export: $name" }
    if ((Get-Content -LiteralPath $text -TotalCount 1) -ne "PROGRAM dx11_player.dll SHA256 $expected BASE 180000000") {
        throw 'Research database identity differs from installed binary'
    }
}
Export-Research 'host' 'ExportFunctionEvidence.java' @('0x89c70','0x89f90','0x8a260','0x8a070','0xa0130','0x9bab0','0x8e510','0x902b0') '^COMPLETE 8$'
Export-Research 'managers' 'ExportFunctionEvidence.java' @('0x340f0','0x452e0','0x3d5f0','0x50750','0x7cbb0','0x806f0','0x6f470') '^COMPLETE 7$'
Export-Research 'tables' 'ExportAddressEvidence.java' @('0x17ae40','0x17d668','0x180538') '^COMPLETE 3$'
Export-Research 'debug-symbols' 'ExportSymbolEvidence.java' @('Debug') '^COMPLETE [0-9]+$'
Export-Research 'query-symbols' 'ExportSymbolEvidence.java' @('QueryManager') '^COMPLETE [0-9]+$'
Assert-Binary
@{path=$binary; sha256=$expected; version=(Get-Item -LiteralPath $binary).VersionInfo.FileVersion} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'binary.json') -Encoding utf8
Get-ChildItem -LiteralPath $output -File | ForEach-Object {
    @{name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'evidence-sha256.json') -Encoding utf8
