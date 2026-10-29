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
# Follow the resource request's verified host-field lookups and output writers.
Export-Research 'request' 'ExportFunctionEvidence.java' @('0xabac0','0xaa9e0') 2
Export-Research 'helpers' 'ExportFunctionEvidence.java' @('0x865a0','0x85f40','0x9b350','0xaa7b0','0xaafc0','0xab140','0xab330','0xa3980') 8
Export-Research 'writers' 'ExportFunctionEvidence.java' @('0xa3b50','0xa3d10','0xa3ee0','0xa40b0','0xa6230','0xa5ff0','0xb0ef0','0xb0ea0','0xb2640') 9
Export-Research 'constructors' 'ExportFunctionEvidence.java' @('0xb26f0','0xb2720','0xb2750','0xd4470','0xd43b0') 5
Export-Research 'callbacks' 'ExportFunctionEvidence.java' @('0xb1510','0xab6a0','0x87bb0','0x86520') 4
Export-Research 'types' 'ExportAddressEvidence.java' @('0x200458','0x200538','0x200618','0x200750','0x185250','0x1852f0','0x185328','0x185360','0x184c08') 9
Assert-Binary
@{path=$binary; sha256=$expected; version=(Get-Item -LiteralPath $binary).VersionInfo.FileVersion} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'binary.json') -Encoding utf8
Get-ChildItem -LiteralPath $output -File | ForEach-Object {
    @{name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'evidence-sha256.json') -Encoding utf8
