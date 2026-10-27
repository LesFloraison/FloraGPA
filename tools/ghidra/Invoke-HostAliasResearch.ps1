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
# These offsets are candidate aliases of host +0x3c0, not established owners.
Export-Research 'host-plus8' 'ExportFieldStores.java' @('0x3b8') 16
Export-Research 'host-plus48' 'ExportFieldStores.java' @('0x378') 6
Export-Research 'host-plusf8' 'ExportFieldStores.java' @('0x2c8') 11
Export-Research 'host-plus8-addresses' 'ExportFieldStores.java' @('0x3b8','addresses') 10
Export-Research 'host-plus48-addresses' 'ExportFieldStores.java' @('0x378','addresses') 12
Export-Research 'host-plusf8-addresses' 'ExportFieldStores.java' @('0x2c8','addresses') 3
Export-Research 'nonstack-stores' 'ExportFunctionEvidence.java' @('0xb2870','0xb3170','0x6cf70','0x114bd0') 4
Export-Research 'api-aliases' 'ExportFunctionEvidence.java' @('0x372e0','0x35d60','0x3ebd0','0x4c530','0x63d80','0xb6be0') 6
Export-Research 'state-tables' 'ExportAddressEvidence.java' @('0x179318','0x17fbd0') 2
Export-Research 'state-symbols' 'ExportSymbolEvidence.java' @('StateBlock') 54
Export-Research 'state-owner' 'ExportFunctionEvidence.java' @('0x61640','0x3a790') 2
Export-Research 'host-owner' 'ExportFunctionEvidence.java' @('0x89c70','0x89f90','0x8a260','0x340f0') 4
Assert-Binary
@{path=$binary; sha256=$expected; version=(Get-Item -LiteralPath $binary).VersionInfo.FileVersion} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'binary.json') -Encoding utf8
Get-ChildItem -LiteralPath $output -File | ForEach-Object {
    @{name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'evidence-sha256.json') -Encoding utf8
