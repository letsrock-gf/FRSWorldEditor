# Opens the build-test editor (posted messages, no focus), turns View > Collision on and captures the same
# views with and without it. Its own build tree, so it never stops an editor another test started.
# Usage: test_collision.ps1 -Out <folder> [-Build build-col]
param([string]$Out, [string]$Build = 'build-col')
$tools = $PSScriptRoot
$d = "D:\NFS World\work\nfsw-world-editor\$Build\bin\Release"
$log = "$d\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Col {
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
'@
New-Item -ItemType Directory -Force $Out | Out-Null
Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
Remove-Item $log -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = $Out
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(6)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if ($p.HasExited) { "editor exited: $($p.ExitCode)"; return }
Start-Sleep -Seconds 8
$views = @(
    @{ n = 'highway'; x = 2500; y = 2000; z = 220; yaw = 0.8; pitch = -0.25 },
    @{ n = 'above';   x = 2500; y = 2000; z = 420; yaw = 0.8; pitch = -1.2 },
    @{ n = 'd61';     x = 3100; y = -1620; z = 90; yaw = 0.7; pitch = -0.35 }
)
foreach ($v in $views) {
    & "$tools\cam.ps1" -ProcessId $p.Id -X $v.x -Y $v.y -Z $v.z -Yaw $v.yaw -Pitch $v.pitch -Out "$Out\$($v.n)-off.png" -Command 1006 | Out-Null; Start-Sleep 1; & "$tools\shot.ps1" -ProcessId $p.Id -Out "$Out\$($v.n)-off.png"
}
[Col]::PostMessageW($p.MainWindowHandle, 0x0111, [IntPtr]40214, [IntPtr]::Zero) | Out-Null   # ID_VIEW_COLLISION
Start-Sleep -Seconds 2
foreach ($v in $views) {
    & "$tools\cam.ps1" -ProcessId $p.Id -X $v.x -Y $v.y -Z $v.z -Yaw $v.yaw -Pitch $v.pitch -Out "$Out\$($v.n)-on.png" -Command 1006 | Out-Null; Start-Sleep 1; & "$tools\shot.ps1" -ProcessId $p.Id -Out "$Out\$($v.n)-on.png"
}
Select-String -Path $log -Pattern 'collision|WARN|ERROR' | Select-Object -Last 12 | ForEach-Object { $_.Line }
"working set MB: $([int]((Get-Process -Id $p.Id).WorkingSet64 / 1MB))"
Stop-Process -Id $p.Id -Force
