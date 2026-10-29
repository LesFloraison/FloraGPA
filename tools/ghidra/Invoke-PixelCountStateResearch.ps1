param(
    [Parameter(Mandatory=$true)][string]$Headless,
    [Parameter(Mandatory=$true)][string]$ProjectDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$GpaRoot = 'C:\Program Files\IntelSWTools\GPA'
)
$ErrorActionPreference = 'Stop'
$binary = Join-Path $GpaRoot 'dx11_player.dll'
$expected = '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b'
function Assert-Binary {
    if ((Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
        throw 'Pinned player changed'
    }
}
Assert-Binary
$project = [IO.Path]::GetFullPath($ProjectDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a new evidence output directory' }
New-Item -ItemType Directory -Path $output | Out-Null
$log = Join-Path $output 'export.log'
& $Headless $project Dx11NativeResearch -process dx11_player.dll -readOnly -noanalysis -scriptPath $PSScriptRoot `
    -postScript ExportAddressEvidence.java (Join-Path $output 'constants.txt') 0x17d220 0x17d230 `
    -postScript ExportFunctionEvidence.java (Join-Path $output 'counter.txt') 0x4de80 0x47ee0 0x43b70 0x6b920 `
    *> $log
if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $log -SimpleMatch 'ERROR REPORT SCRIPT ERROR')) {
    throw 'Ghidra counter export failed'
}
foreach ($entry in @(@{Name='constants.txt'; Tail='^COMPLETE 2$'},
                     @{Name='counter.txt'; Tail='^COMPLETE 4$'})) {
    $path = Join-Path $output $entry.Name
    if (!(Test-Path -LiteralPath $path) -or (Get-Content -LiteralPath $path -Tail 1) -notmatch $entry.Tail -or
        (Select-String -LiteralPath $path -Pattern '^NO FUNCTION ')) { throw "Incomplete export: $path" }
    if ((Get-Content -LiteralPath $path -First 1) -notmatch $expected) { throw 'Ghidra binary differs' }
}
Assert-Binary
@{path=$binary; sha256=$expected; version=(Get-Item -LiteralPath $binary).VersionInfo.FileVersion} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'binary.json') -Encoding utf8
Get-ChildItem -LiteralPath $output -File | ForEach-Object {
    @{name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'evidence-sha256.json') -Encoding utf8
