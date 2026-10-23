param(
    [Parameter(Mandatory=$true)][string]$Headless,
    [Parameter(Mandatory=$true)][string]$ProjectDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$GpaRoot = 'C:\Program Files\IntelSWTools\GPA',
    [switch]$Import
)
$ErrorActionPreference = 'Stop'
$scriptDirectory = $PSScriptRoot
$output = [IO.Path]::GetFullPath($OutputDirectory)
$project = [IO.Path]::GetFullPath($ProjectDirectory)
New-Item -ItemType Directory -Force -Path $output, $project | Out-Null
$binaries = @(
    @{ Name='dx11_player.dll'; Sha256='39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b' },
    @{ Name='dx11_playback.dll'; Sha256='0a531905386af6c5f2c0357d4179cec208bb171a287b262f3c4a5539d483bf5e' }
)
$manifest = @()
foreach ($binary in $binaries) {
    $path = Join-Path $GpaRoot $binary.Name
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $binary.Sha256) { throw "Research RVA map does not apply to $path ($hash)" }
    $manifest += @{ name=$binary.Name; sha256=$hash; version=(Get-Item -LiteralPath $path).VersionInfo.FileVersion }
    if ($Import) {
        & $Headless $project Dx11NativeResearch -import $path *> (Join-Path $output ($binary.Name + '-import.log'))
        if ($LASTEXITCODE -ne 0) { throw "Ghidra import failed for $path" }
    }
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'binaries.json') -Encoding utf8
function Export-Evidence([string]$program, [string]$name, [string[]]$rvas) {
    & $Headless $project Dx11NativeResearch -process $program -readOnly -noanalysis -scriptPath $scriptDirectory `
        -postScript ExportFunctionEvidence.java (Join-Path $output ($name + '.txt')) @rvas *> (Join-Path $output ($name + '.log'))
    if ($LASTEXITCODE -ne 0) { throw "Ghidra export failed for $name" }
}
Export-Evidence 'dx11_player.dll' 'debugger-host' @('0x89c70','0x89f90','0x902b0','0x93cb0','0x87c10','0x89880','0x88950','0x8a260','0x8ab90')
Export-Evidence 'dx11_player.dll' 'pixel-history' @('0x91520','0x435f0','0x4de80','0x24e30','0x47ee0','0x48e70','0x47ba0','0x43b70','0x3fa80')
Export-Evidence 'dx11_player.dll' 'geometry-query' @('0x90c60','0x40330','0x40070','0x50b40','0x12fa20')
Export-Evidence 'dx11_playback.dll' 'playback-analysis' @('0x639c0','0x61480','0x24ee0')
& $Headless $project Dx11NativeResearch -process dx11_player.dll -readOnly -noanalysis -scriptPath $scriptDirectory `
    -postScript FindFieldAccess.java (Join-Path $output 'player-field-3c0.txt') 0x3c0 *> (Join-Path $output 'player-field-3c0.log')
if ($LASTEXITCODE -ne 0) { throw 'Ghidra field access scan failed' }
foreach ($binary in $binaries) {
    if ((Get-FileHash -LiteralPath (Join-Path $GpaRoot $binary.Name) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $binary.Sha256) {
        throw 'Original GPA binary changed during research'
    }
}
Get-ChildItem -LiteralPath $output -File | Where-Object Name -ne 'evidence-sha256.json' |
    ForEach-Object { @{ name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'evidence-sha256.json') -Encoding utf8
