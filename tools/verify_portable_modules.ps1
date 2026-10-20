param(
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$CaptureDirectory,
    [Parameter(Mandatory)][string[]]$Reports,
    [Parameter(Mandatory)][string]$OutputFile
)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path.TrimEnd('\') + '\'
$sdk = (Resolve-Path -LiteralPath (Join-Path $CaptureDirectory 'D3D12\AgilitySDK')).Path.TrimEnd('\') + '\'
$windows = [IO.Path]::GetFullPath($env:SystemRoot).TrimEnd('\') + '\'
if (Test-Path -LiteralPath $OutputFile) { throw 'Use a new verification output file.' }
$checks = @()
foreach ($reportPath in $Reports) {
    $resolvedReport = (Resolve-Path -LiteralPath $reportPath).Path
    $report = Get-Content -LiteralPath $resolvedReport -Raw | ConvertFrom-Json
    if ($report.backend -ne 'dx12' -or ($report.completed -ne $true -and $report.ok -ne $true)) {
        throw "Expected a successful DX12 replay or analysis report: $resolvedReport"
    }
    if (-not $report.loaded_modules -or $report.module_inventory_error) {
        throw "Missing complete module inventory: $resolvedReport"
    }
    $modules = @()
    $executables = @()
    foreach ($module in $report.loaded_modules) {
        if ($module -isnot [string] -or -not [IO.Path]::IsPathRooted($module) -or
            -not (Test-Path -LiteralPath $module -PathType Leaf)) {
            throw "Invalid or missing module path in ${resolvedReport}: $module"
        }
        $path = [IO.Path]::GetFullPath($module)
        $name = [IO.Path]::GetFileName($path)
        if ($path -match '\\IntelSWTools\\GPA(?:\\|$)' -or
            $name -match '^(python.*\.dll|.*\.pyd|dx11_player\.dll|dx11_playback\.dll|shimd3d64\.dll|gpa.*\.dll)$') {
            throw "GPA/Python module loaded: $path"
        }
        $origin = if ($path.StartsWith($package, [StringComparison]::OrdinalIgnoreCase)) { 'package' }
            elseif ($path.StartsWith($sdk, [StringComparison]::OrdinalIgnoreCase) -and
                $name -in @('D3D12Core.dll', 'd3d12SDKLayers.dll')) { 'capture_agility_sdk' }
            elseif ($path.StartsWith($windows, [StringComparison]::OrdinalIgnoreCase)) { 'windows' }
            else { throw "External runtime dependency: $path" }
        if ([IO.Path]::GetExtension($path) -ieq '.exe') {
            if ($origin -ne 'package' -or $name -notin @('FloraGPA.Worker.exe','FloraGPA.Cli.exe','FloraGPA.Rdc.exe')) {
                throw "Unexpected replay/analysis executable: $path"
            }
            $executables += $name
        }
        $item = [ordered]@{ path = $path; origin = $origin }
        if ($origin -ne 'windows') { $item.sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
        $modules += $item
    }
    if ($executables.Count -ne 1) { throw "Expected one packaged executable in $resolvedReport" }
    $checks += [ordered]@{
        report = $resolvedReport
        report_sha256 = (Get-FileHash -LiteralPath $resolvedReport -Algorithm SHA256).Hash.ToLowerInvariant()
        executable = $executables[0]
        module_count = $modules.Count
        modules = $modules
    }
}
if (-not $checks.Count) { throw 'At least one replay or analysis report is required.' }
[ordered]@{
    format = 'flora-portable-module-verification-1'
    scope = 'Successful DX12 process snapshots only; not a lifetime DLL/process trace, clean-machine test or full feature acceptance.'
    package = $package
    capture_sdk = $sdk
    checks = $checks
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputFile -Encoding utf8
Write-Output "Verified $($checks.Count) packaged DX12 process snapshots: $OutputFile"
