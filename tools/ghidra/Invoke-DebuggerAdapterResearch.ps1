param(
    [Parameter(Mandatory=$true)][string]$Headless,
    [Parameter(Mandatory=$true)][string]$ProjectDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$GpaRoot = 'C:\Program Files\IntelSWTools\GPA'
)
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath($ProjectDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a new evidence output directory' }
New-Item -ItemType Directory -Path $output | Out-Null
$binaries = @(
    @{ Path='dx11_player.dll'; Sha256='39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b' },
    @{ Path='dx11_playback.dll'; Sha256='0a531905386af6c5f2c0357d4179cec208bb171a287b262f3c4a5539d483bf5e' },
    @{ Path='com/intel/framedb/framedb_qt.dll'; Sha256='35a03995d50f390ed74722385583e7bb011f84e563a579c88086d8039ad1df80' }
)
function Assert-Binaries {
    foreach ($binary in $binaries) {
        $path = Join-Path $GpaRoot $binary.Path
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $binary.Sha256) {
            throw "Pinned research binary changed: $path"
        }
    }
}
function Export-Research([string]$program, [string]$name, [string]$script, [string[]]$arguments, [string]$completion) {
    $textPath = Join-Path $output ($name + '.txt')
    $logPath = Join-Path $output ($name + '.log')
    & $Headless $project Dx11NativeResearch -process $program -readOnly -noanalysis -scriptPath $PSScriptRoot `
        -postScript $script $textPath @arguments *> $logPath
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $textPath) -or
        (Select-String -LiteralPath $logPath -SimpleMatch 'ERROR REPORT SCRIPT ERROR') -or
        (Select-String -LiteralPath $textPath -Pattern '^NO FUNCTION ') -or
        (Get-Content -LiteralPath $textPath -Tail 1) -notmatch $completion) {
        throw "Incomplete Ghidra evidence: $name"
    }
}
Assert-Binaries
Export-Research 'framedb_qt.dll' 'framedb-adapter-stores' 'ExportFieldStores.java' @('0x1a8') '^COMPLETE 133$'
Export-Research 'framedb_qt.dll' 'framedb-adapter-addresses' 'ExportFieldStores.java' @('0x1a8','addresses') '^COMPLETE 108$'
Export-Research 'framedb_qt.dll' 'framedb-queue-assignment' 'ExportFunctionEvidence.java' @('0x35e050','0x3606c0','0x3619b0','0x361ca0','0x366740') '^COMPLETE 5$'
Export-Research 'framedb_qt.dll' 'framedb-legacy-creation' 'ExportFunctionEvidence.java' @('0x3624b0','0x365a90') '^COMPLETE 2$'
Export-Research 'framedb_qt.dll' 'framedb-factory-lookup' 'ExportFunctionEvidence.java' @('0x5c90f0','0x5c9140') '^COMPLETE 2$'
Export-Research 'framedb_qt.dll' 'framedb-factory-loader' 'ExportFunctionEvidence.java' @('0x5c76c0') '^COMPLETE 1$'
Export-Research 'dx11_playback.dll' 'playback-factory-symbols' 'ExportSymbolEvidence.java' @('Factory') '^COMPLETE [1-9][0-9]*$'
Export-Research 'dx11_playback.dll' 'playback-device-symbols' 'ExportSymbolEvidence.java' @('legacy::Device') '^COMPLETE [1-9][0-9]*$'
Export-Research 'dx11_playback.dll' 'playback-context-symbols' 'ExportSymbolEvidence.java' @('legacy::Playback') '^COMPLETE [1-9][0-9]*$'
Export-Research 'dx11_playback.dll' 'playback-factory-create' 'ExportFunctionEvidence.java' @('0x7400','0x49a20') '^COMPLETE 2$'
Export-Research 'dx11_playback.dll' 'playback-context-factory' 'ExportFunctionEvidence.java' @('0x4c700','0x4c950') '^COMPLETE 2$'
Export-Research 'dx11_playback.dll' 'playback-debugger-method' 'ExportFunctionEvidence.java' @('0x5f2e0','0x56530') '^COMPLETE 2$'
Export-Research 'dx11_playback.dll' 'playback-debugger-wire' 'ExportFunctionEvidence.java' @('0x6be00','0x264ff0','0x263d30','0x262690') '^COMPLETE 4$'
Export-Research 'dx11_playback.dll' 'playback-result-fields' 'ExportFunctionEvidence.java' @('0x261470','0x2611b0','0x25c810','0x2617d0','0x25cbe0') '^COMPLETE 5$'
Export-Research 'dx11_player.dll' 'player-wire-dispatch' 'ExportFunctionEvidence.java' @('0xa2710','0xab500','0xb0bc0','0xade30') '^COMPLETE 4$'
Export-Research 'dx11_player.dll' 'player-wire-codec' 'ExportFunctionEvidence.java' @('0x10ba30','0x10dd30') '^COMPLETE 2$'
Export-Research 'dx11_player.dll' 'player-wire-primitives' 'ExportFunctionEvidence.java' @('0x108f20','0x1097d0','0x10d0b0') '^COMPLETE 3$'
Assert-Binaries
$binaries | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'binaries.json') -Encoding utf8
Get-ChildItem -LiteralPath $output -File |
    ForEach-Object { @{ name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'evidence-sha256.json') -Encoding utf8
