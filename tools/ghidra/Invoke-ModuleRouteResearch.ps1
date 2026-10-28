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
Export-Research 'loader-symbols' 'ExportSymbolEvidence.java' @('LoadLibrary') 5
Export-Research 'procedure-symbols' 'ExportSymbolEvidence.java' @('GetProcAddress') 2
Export-Research 'loaders' 'ExportFunctionEvidence.java' @('0xc7290','0x14eeb0') 2
Export-Research 'loader-callers' 'ExportFunctionEvidence.java' @('0x1491f0','0xf72a0','0x34bf0','0x1254a0','0x108a60','0xfec70','0xfac50','0x1266e0','0x153910','0x130ed0','0x14d690','0x14e730') 12
Export-Research 'resolvers' 'ExportFunctionEvidence.java' @('0x147ee0','0xc2e70','0x11e380','0xc6620','0x11d7d0','0x110f90','0x14440','0x14470','0x340f0','0xc69c0') 10
Export-Research 'singleton-types' 'ExportAddressEvidence.java' @('0x1fd238','0x1fd260') 2
Export-Research 'extension-symbols' 'ExportSymbolEvidence.java' @('Extension') 77
Export-Research 'extension-table' 'ExportAddressEvidence.java' @('0x1bf5c0') 1
Export-Research 'module-lifetime' 'ExportFunctionEvidence.java' @('0xc7f40','0x11e050','0x11e080') 3
Export-Research 'holder-cleanup' 'ExportFunctionEvidence.java' @('0x384d0') 1
Export-Research 'endpoint-strings' 'ExportAddressEvidence.java' @('0x1baae0','0x1bab18','0x18b720','0x18b730','0x18b748','0x1fd240','0x1fd268') 7
Export-Research 'directory-selection' 'ExportFunctionEvidence.java' @('0xcea40','0xce640') 2
Assert-Binary
@{path=$binary; sha256=$expected; version=(Get-Item -LiteralPath $binary).VersionInfo.FileVersion} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'binary.json') -Encoding utf8
Get-ChildItem -LiteralPath $output -File | ForEach-Object {
    @{name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'evidence-sha256.json') -Encoding utf8
