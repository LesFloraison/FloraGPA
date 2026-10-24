param(
    [Parameter(Mandatory=$true)][string]$Headless,
    [Parameter(Mandatory=$true)][string]$ProjectDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$GpaRoot = 'C:\Program Files\IntelSWTools\GPA',
    [switch]$ImportFramedb
)
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath($ProjectDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null
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
Assert-Binaries
if ($ImportFramedb) {
    & $Headless $project Dx11NativeResearch -import (Join-Path $GpaRoot $binaries[2].Path) -analysisTimeoutPerFile 600 `
        *> (Join-Path $output 'framedb-import.log')
    if ($LASTEXITCODE -ne 0 -or !(Select-String -LiteralPath (Join-Path $output 'framedb-import.log') -SimpleMatch 'REPORT: Analysis succeeded')) {
        throw 'Incomplete framedb import or analysis'
    }
}
function Export-Research([string]$program, [string]$name, [string]$script, [string[]]$arguments, [string]$completion) {
    $textPath = Join-Path $output ($name + '.txt')
    $logPath = Join-Path $output ($name + '.log')
    [IO.File]::WriteAllText($textPath, '')
    & $Headless $project Dx11NativeResearch -process $program -readOnly -noanalysis -scriptPath $PSScriptRoot `
        -postScript $script $textPath @arguments *> $logPath
    if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $logPath -SimpleMatch 'ERROR REPORT SCRIPT ERROR') -or
        (Get-Content -LiteralPath $textPath -Tail 1) -notmatch $completion) {
        throw "Incomplete Ghidra evidence: $name"
    }
}
Export-Research 'framedb_qt.dll' 'framedb-addresses' 'ExportAddressEvidence.java' @('0x789298','0x7b1cf8','0x7b1c98','0x7c19a8','0x775abd','0x7859dd') '^COMPLETE 6$'
Export-Research 'framedb_qt.dll' 'framedb-functions' 'ExportFunctionEvidence.java' @('0x130530','0x2a3860','0x2a9190','0x37da20') '^COMPLETE 4$'
Export-Research 'framedb_qt.dll' 'framedb-task' 'ExportFunctionEvidence.java' @('0x37c110','0x38ae40','0x3887c0','0x37b690','0x36e6a0') '^COMPLETE 5$'
Export-Research 'framedb_qt.dll' 'framedb-task-execution' 'ExportFunctionEvidence.java' @('0x372710','0x386520','0x368e80','0x388c30','0x36dda0') '^COMPLETE 5$'
Export-Research 'framedb_qt.dll' 'framedb-symbols' 'ExportSymbolEvidence.java' @('ShaderDebug') '^COMPLETE [1-9][0-9]*$'
Export-Research 'framedb_qt.dll' 'framedb-setting-functions' 'ExportFunctionEvidence.java' @('0x134c90','0x139c60','0x12ea50') '^COMPLETE 3$'
Export-Research 'dx11_playback.dll' 'playback-addresses' 'ExportAddressEvidence.java' @('0x2bbd88') '^COMPLETE 1$'
Export-Research 'dx11_playback.dll' 'playback-functions' 'ExportFunctionEvidence.java' @('0x57550') '^COMPLETE 1$'
Export-Research 'dx11_player.dll' 'player-lifetime' 'ExportFunctionEvidence.java' @('0x89c70','0x89f90','0x86fc0','0x87c10','0x902b0') '^COMPLETE 5$'
Export-Research 'dx11_player.dll' 'player-debugger-symbols' 'ExportSymbolEvidence.java' @('Debugger') '^COMPLETE [1-9][0-9]*$'
Export-Research 'dx11_player.dll' 'player-debugger-detection' 'ExportFunctionEvidence.java' @('0x148c74','0x148f30') '^COMPLETE 2$'
Assert-Binaries
$binaries | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'binaries.json') -Encoding utf8
Get-ChildItem -LiteralPath $output -File | Where-Object Name -ne 'evidence-sha256.json' |
    ForEach-Object { @{ name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'evidence-sha256.json') -Encoding utf8
