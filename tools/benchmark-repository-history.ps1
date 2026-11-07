# Time repository-history operations on a disposable copy of a native stage.
param(
    [Parameter(Mandatory)] [string] $Executable,
    [Parameter(Mandatory)] [string] $Stage,
    [Parameter(Mandatory)] [string] $Report
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$stages = Join-Path $root 'build\development\native-custom-export'
$source = (Resolve-Path $Stage).Path
$Executable = (Resolve-Path $Executable).Path

if (-not $source.StartsWith($stages, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Use a stage under $stages"
}

$Report = [IO.Path]::GetFullPath((Join-Path $root $Report))

if (-not $Report.StartsWith((Join-Path $root 'agents\reports'), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Write reports under agents\reports'
}

$work = Join-Path $stages ('bench-' + (Split-Path $source -Leaf))
$plugins = Join-Path $Report 'plugins'

foreach ($path in @($work, $plugins)) {
    if (Test-Path $path) { Remove-Item -Recurse -Force $path -Confirm:$false }
}

New-Item -ItemType Directory -Force $plugins | Out-Null
robocopy $source $work /E /XD build .git /NFL /NDL /NJH /NJS /NP | Out-Null
$steps = @('inspect', 'song-b-stock', 'song-b-stock-update', 'replace-layered auto-piano',
           'song-b-stock-update', 'revert-sample', 'add', 'song-b', 'revert-song-a', 'revert-all')
$results = @()

foreach ($step in $steps) {
    $arguments = @($plugins, $work) + ($step -split ' ')
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $output = & $Executable @arguments 2>&1 | Out-String
    $timer.Stop()
    $status = $LASTEXITCODE
    $results += [pscustomobject]@{ step = $step; seconds = [math]::Round($timer.Elapsed.TotalSeconds, 3); status = $status }
    '{0,-28} {1,8:N3}s  status {2}' -f $step, $timer.Elapsed.TotalSeconds, $status

    if ($status -ne 0) {
        $output
        break
    }
}

$results | ConvertTo-Json | Out-File -Encoding utf8 (Join-Path $Report 'timings.json')
Remove-Item -Recurse -Force $work -Confirm:$false
