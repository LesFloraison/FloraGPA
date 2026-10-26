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
$text = Join-Path $output 'gpu-selection.txt'
$log = Join-Path $output 'gpu-selection.log'
& $Headless $project Dx11NativeResearch -process dx11_player.dll -readOnly -noanalysis -scriptPath $PSScriptRoot `
    -postScript ExportFunctionEvidence.java $text 0x87c10 0x8a260 0x11bd30 0x11beb0 0x11bf10 0x11c100 0x123100 0x1238e0 0x126510 0x1266e0 `
    -postScript ExportAddressEvidence.java (Join-Path $output 'selector-table.txt') 0x1bc548 `
    -postScript ExportSymbolEvidence.java (Join-Path $output 'enumerator-symbols.txt') EnumerateWin *> $log
if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $log -SimpleMatch 'ERROR REPORT SCRIPT ERROR')) {
    throw 'Ghidra export failed'
}
foreach ($entry in @(@{Name='gpu-selection.txt'; Tail='COMPLETE 10'},
                     @{Name='selector-table.txt'; Tail='COMPLETE 1'},
                     @{Name='enumerator-symbols.txt'; Tail='COMPLETE 7'})) {
    $path = Join-Path $output $entry.Name
    if (!(Test-Path -LiteralPath $path) -or (Get-Content -LiteralPath $path -Tail 1) -ne $entry.Tail -or
        (Select-String -LiteralPath $path -Pattern '^NO FUNCTION ')) { throw "Incomplete export: $path" }
}
Assert-Binary
@{path=$binary; sha256=$expected; version=(Get-Item -LiteralPath $binary).VersionInfo.FileVersion} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'binary.json') -Encoding utf8
Get-ChildItem -LiteralPath $output -File | ForEach-Object {
    @{name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'evidence-sha256.json') -Encoding utf8
