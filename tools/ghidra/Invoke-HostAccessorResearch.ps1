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
function Export-Research([string]$name, [string]$script, [string[]]$arguments, [int]$count) {
    $text = Join-Path $output ($name + '.txt')
    $log = Join-Path $output ($name + '.log')
    & $Headless $project Dx11NativeResearch -process dx11_player.dll -readOnly -noanalysis -scriptPath $PSScriptRoot `
        -postScript $script $text @arguments *> $log
    if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $log -SimpleMatch 'ERROR REPORT SCRIPT ERROR') -or
        !(Test-Path -LiteralPath $text) -or (Get-Content -LiteralPath $text -Tail 1) -ne "COMPLETE $count" -or
        (Select-String -LiteralPath $text -Pattern '^NO FUNCTION ')) { throw "Incomplete export: $name" }
    if ((Get-Content -LiteralPath $text -TotalCount 1) -ne "PROGRAM dx11_player.dll SHA256 $expected BASE 180000000") {
        throw 'Research database identity differs from installed binary'
    }
}
# Follow concrete singleton callers and immediate host-pointer consumers.
Export-Research 'callers' 'ExportFunctionEvidence.java' @('0x87ce3','0x898e4','0x89f40','0x89d90') 4
Export-Research 'alternate' 'ExportFunctionEvidence.java' @('0x8ab90','0xa7cea','0xa8138') 3
Export-Research 'global' 'ExportAddressEvidence.java' @('0x208540') 1
Export-Research 'accessors' 'ExportFunctionEvidence.java' @('0x89420','0x89430','0x87bf0','0x87bb0','0x87b90','0x87bd0') 6
Export-Research 'consumers' 'ExportFunctionEvidence.java' @('0x157cc','0x5c476','0xaaa55') 3
Export-Research 'request' 'ExportFunctionEvidence.java' @('0x8aab0','0xabac0','0x9b8e0') 3
Export-Research 'types' 'ExportAddressEvidence.java' @('0x201e90','0x202818','0x1fc670','0x200750','0x174140','0x174ab8') 6
Assert-Binary
@{path=$binary; sha256=$expected; version=(Get-Item -LiteralPath $binary).VersionInfo.FileVersion} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'binary.json') -Encoding utf8
Get-ChildItem -LiteralPath $output -File | ForEach-Object {
    @{name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'evidence-sha256.json') -Encoding utf8
