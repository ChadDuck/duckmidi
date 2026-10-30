param(
    [Parameter(Mandatory=$true)][int]$TargetPid,
    [Parameter(Mandatory=$true)][string]$ReportDirectory,
    [int]$Seconds=1200
)
$ErrorActionPreference='Stop'
$report=(Resolve-Path -LiteralPath $ReportDirectory).Path
$phaseFile=Join-Path $report 'phase.txt'
$stopFile=Join-Path $report 'stop-sampling'
$samples=Join-Path $report 'resources.csv'
$logical=[Environment]::ProcessorCount
$watch=[Diagnostics.Stopwatch]::StartNew()
$previous=Get-Process -Id $TargetPid
$lastCpu=$previous.TotalProcessorTime.TotalSeconds
$lastTime=$watch.Elapsed.TotalSeconds
$hardware=[pscustomobject]@{
    CPU=(Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name)
    LogicalProcessors=$logical
    GPU=@(Get-CimInstance Win32_VideoController | Select-Object -ExpandProperty Name)
    PhysicalRAMGiB=[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1GB,2)
    TargetPid=$TargetPid
    StartedUtc=[DateTime]::UtcNow.ToString('o')
}
$hardware | ConvertTo-Json | Set-Content (Join-Path $report 'hardware.json')
while ($watch.Elapsed.TotalSeconds -lt $Seconds -and -not (Test-Path -LiteralPath $stopFile)) {
    $phase=if(Test-Path -LiteralPath $phaseFile){(Get-Content -LiteralPath $phaseFile -Raw).Trim()}else{'settling'}
    try { $process=Get-Process -Id $TargetPid } catch { break }
    $gpu=$null; $gpuSum=$null; $gpuAvailable=$false
    try {
        $counter=Get-Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction Stop
        $gpuAvailable=$counter.CounterSamples.Count -gt 0
        $engines=@($counter.CounterSamples | Where-Object { $_.InstanceName -like "pid_${TargetPid}_*" -and $_.Status -eq 0 })
        if($gpuAvailable) {
            $gpu=0.0; $gpuSum=0.0
            foreach($engine in $engines) {
                $value=[Math]::Max([double]0,[double]$engine.CookedValue)
                $gpu=[Math]::Max($gpu,$value); $gpuSum+=$value
            }
        }
    } catch { }
    $process.Refresh()
    $now=$watch.Elapsed.TotalSeconds
    $cpu=$process.TotalProcessorTime.TotalSeconds
    $row=[pscustomobject]@{
        Utc=[DateTime]::UtcNow.ToString('o');ElapsedSeconds=[math]::Round($now,3);Phase=$phase
        IntervalSeconds=[math]::Round($now-$lastTime,3)
        CpuPercent=[math]::Round(100*($cpu-$lastCpu)/($now-$lastTime)/$logical,3)
        WorkingSetMiB=[math]::Round($process.WorkingSet64/1MB,3)
        PrivateMiB=[math]::Round($process.PrivateMemorySize64/1MB,3)
        PeakWorkingSetMiB=[math]::Round($process.PeakWorkingSet64/1MB,3)
        GpuPercent=if($null -eq $gpu){$null}else{[math]::Round($gpu,3)}
        GpuEngineSumPercent=if($null -eq $gpuSum){$null}else{[math]::Round($gpuSum,3)}
        GpuCountersAvailable=$gpuAvailable
    }
    $row | Export-Csv -NoTypeInformation -Append -LiteralPath $samples
    $lastTime=$now; $lastCpu=$cpu
    Start-Sleep -Milliseconds 500
}
