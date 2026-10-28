param(
    [Parameter(Mandatory=$true)][string]$Probe,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$executable = [IO.Path]::GetFullPath($Probe)
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a fresh interface research directory' }
$before = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
New-Item -ItemType Directory -Path $output | Out-Null
$records = @()
foreach ($vendor in @(0, 4318, 32902)) {
    foreach ($fixture in @('array', 'static-array', 'single', 'single-u3', 'single-11_0', 'single-dynamic')) {
        $name = "$vendor-$fixture"
        $directory = Join-Path $output $name
        $stdout = Join-Path $output "$name.stdout.txt"
        $stderr = Join-Path $output "$name.stderr.txt"
        $process = Start-Process -FilePath $executable -ArgumentList @(('"{0}"' -f $directory), "$vendor", $fixture) `
            -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        if (!$process.WaitForExit(30000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Interface probe timed out: $name"
        }
        $process.Refresh()
        $reportPath = Join-Path $directory 'result.json'
        if (!(Test-Path -LiteralPath $reportPath)) { throw "Missing interface result: $name" }
        $report = Get-Content -Raw -LiteralPath $reportPath | ConvertFrom-Json
        $exitCode = $process.ExitCode
        if ($exitCode -ne $(if ($report.passed) { 0 } else { 1 })) {
            throw "Inconsistent interface probe exit: $name"
        }
        $records += @{name=$name;vendor=$vendor;fixture=$fixture;exit_code=$exitCode;passed=$report.passed}
    }
}
if ((Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $before) {
    throw 'Interface probe binary changed during research'
}
$manifest = Get-ChildItem -LiteralPath $output -Recurse -File | ForEach-Object {
    @{path=[IO.Path]::GetRelativePath($output, $_.FullName).Replace('\','/');
      sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$failed = @($records | Where-Object { !$_.passed })
@{schema=1;probe_sha256=$before;complete=$true;passed=($failed.Count -eq 0);records=$records;files=$manifest} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'matrix.json') -Encoding utf8
Write-Output "Completed $($records.Count) independent probes; $($failed.Count) probes differ from authored arithmetic."
if ($failed.Count) { exit 1 }
