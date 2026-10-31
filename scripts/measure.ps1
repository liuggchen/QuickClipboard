param([Parameter(Mandatory=$true)][int]$ProcessId, [int]$IntervalSeconds = 5)
$ErrorActionPreference = 'Stop'
# 只读取进程资源计数，不操作应用界面。
if (-not ('QuickClipboardResources' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class QuickClipboardResources {
    [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr process, uint flags);
}
'@
}
$process = Get-Process -Id $ProcessId
$cpuStart = $process.TotalProcessorTime.TotalMilliseconds
$clock = [Diagnostics.Stopwatch]::StartNew()
Start-Sleep -Seconds $IntervalSeconds
$process.Refresh()
[pscustomobject]@{
    ProcessId = $ProcessId
    PrivateBytes = $process.PrivateMemorySize64
    WorkingSet = $process.WorkingSet64
    GDIObjects = [QuickClipboardResources]::GetGuiResources($process.Handle, 0)
    USERObjects = [QuickClipboardResources]::GetGuiResources($process.Handle, 1)
    Handles = $process.HandleCount
    CPUTimeDeltaMs = [Math]::Round($process.TotalProcessorTime.TotalMilliseconds - $cpuStart, 3)
    SampleSeconds = [Math]::Round($clock.Elapsed.TotalSeconds, 3)
} | ConvertTo-Json
