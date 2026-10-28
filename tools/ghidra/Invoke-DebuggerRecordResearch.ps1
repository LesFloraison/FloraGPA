param(
    [Parameter(Mandatory=$true)][string]$Headless,
    [Parameter(Mandatory=$true)][string]$ProjectDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$GpaRoot = 'C:\Program Files\IntelSWTools\GPA'
)
$ErrorActionPreference = 'Stop'
$output = [IO.Path]::GetFullPath($OutputDirectory)
$project = [IO.Path]::GetFullPath($ProjectDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a new evidence directory' }
$entries = @(
    @{ Name='dx11_player.dll'; Hash='39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b';
       Rvas=@('0x10dd30','0x10d0b0','0x10cec0','0x109890','0x1097d0','0x109630','0x10dbd0') },
    @{ Name='dx11_playback.dll'; Hash='0a531905386af6c5f2c0357d4179cec208bb171a287b262f3c4a5539d483bf5e';
       Rvas=@('0x262690','0x261470','0x2611b0','0x2617d0','0x25c810','0x25c660','0x262620') }
)
function Assert-Binaries {
    foreach ($entry in $entries) {
        if ((Get-FileHash -LiteralPath (Join-Path $GpaRoot $entry.Name)).Hash.ToLowerInvariant() -ne $entry.Hash) {
            throw "Pinned binary changed: $($entry.Name)"
        }
    }
}
Assert-Binaries
New-Item -ItemType Directory -Path $output | Out-Null
foreach ($entry in $entries) {
    $target = Join-Path $output ($entry.Name + '.txt')
    $log = Join-Path $output ($entry.Name + '.log')
    & $Headless $project Dx11NativeResearch -process $entry.Name -readOnly -noanalysis -scriptPath $PSScriptRoot `
        -postScript ExportFunctionEvidence.java $target @($entry.Rvas) *> $log
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $target) -or
        (Select-String -LiteralPath $log -SimpleMatch 'ERROR REPORT SCRIPT ERROR') -or
        (Select-String -LiteralPath $target -Pattern '^NO FUNCTION ') -or
        (Get-Content -LiteralPath $target -Tail 1) -ne 'COMPLETE 7' -or
        (Get-Content -LiteralPath $target -TotalCount 1) -ne "PROGRAM $($entry.Name) SHA256 $($entry.Hash) BASE 180000000") {
        throw "Incomplete or mismatched export: $($entry.Name)"
    }
}
Assert-Binaries
$entries | ForEach-Object {
    $path = Join-Path $GpaRoot $_.Name
    @{path=$path; sha256=$_.Hash; version=(Get-Item -LiteralPath $path).VersionInfo.FileVersion}
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'binaries.json') -Encoding utf8
Get-ChildItem -LiteralPath $output -File | ForEach-Object {
    @{name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'evidence-sha256.json') -Encoding utf8
