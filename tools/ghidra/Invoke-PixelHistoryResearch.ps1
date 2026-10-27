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
$log = Join-Path $output 'export.log'
& $Headless $project Dx11NativeResearch -process dx11_player.dll -readOnly -noanalysis -scriptPath $PSScriptRoot `
    -postScript ExportSymbolEvidence.java (Join-Path $output 'symbols.txt') PlaybackRequestPixelHistory `
    -postScript ExportAddressEvidence.java (Join-Path $output 'tables.txt') 0x183150 0x17ae40 0x176038 0x1760f0 `
    -postScript ExportFunctionEvidence.java (Join-Path $output 'request.txt') 0x88d80 0x94480 0x91520 0x3ff50 0x46d20 0x20700 `
    -postScript ExportFunctionEvidence.java (Join-Path $output 'passes.txt') 0x435f0 0x4de80 0x24e30 0x47ee0 0x48e70 0x47ba0 0x43b70 0x3fa80 `
    -postScript ExportFunctionEvidence.java (Join-Path $output 'state-wrappers.txt') 0x6b990 0x6be10 0x6b920 0x6bcc0 0x6b9e0 0x6bdb0 `
    *> $log
if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $log -SimpleMatch 'ERROR REPORT SCRIPT ERROR')) {
    throw 'Ghidra export failed'
}
foreach ($entry in @(@{Name='symbols.txt'; Tail='^COMPLETE [1-9][0-9]*$'},
                     @{Name='tables.txt'; Tail='^COMPLETE 4$'},
                     @{Name='request.txt'; Tail='^COMPLETE 6$'},
                     @{Name='passes.txt'; Tail='^COMPLETE 8$'},
                     @{Name='state-wrappers.txt'; Tail='^COMPLETE 6$'})) {
    $path = Join-Path $output $entry.Name
    if (!(Test-Path -LiteralPath $path) -or (Get-Content -LiteralPath $path -Tail 1) -notmatch $entry.Tail -or
        (Select-String -LiteralPath $path -Pattern '^NO FUNCTION ')) { throw "Incomplete export: $path" }
}
Assert-Binary
@{path=$binary; sha256=$expected; version=(Get-Item -LiteralPath $binary).VersionInfo.FileVersion} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'binary.json') -Encoding utf8
Get-ChildItem -LiteralPath $output -File | ForEach-Object {
    @{name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'evidence-sha256.json') -Encoding utf8
