# Isolated check of the toolbar's Fast test on an NFS World folder: starts the editor given by -Dir with
# FRS_TEST_LAUNCH_POSE (Launch Game then only writes the pose there: nothing saved, no game started), sends
# the Fast test command (DriveMode kDriveCommand 44901) and checks the pose file appeared.
# Do not run while nfsw.exe is running.
param([string]$Dir = "$(Split-Path (Split-Path $PSScriptRoot))\build-look\bin\Release")
if (Get-Process nfsw -ErrorAction SilentlyContinue) { 'nfsw.exe is running: not started'; return }
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class W { [DllImport("user32.dll")] public static extern IntPtr PostMessageW(IntPtr h, int m, IntPtr w, IntPtr l); }
'@
$log = "$Dir\FRSWorldEditor.log"
$pose = "$env:TEMP\frs-fasttest-pose.txt"
Remove-Item $pose, $log -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = "$env:TEMP\frs-test-view"
$env:FRS_TEST_LAUNCH_POSE = $pose
New-Item -ItemType Directory -Force $env:FRS_WRITE_TRACKS | Out-Null
try {
    $p = Start-Process "$Dir\FRSWorldEditor.exe" -WorkingDirectory $Dir -PassThru
    $deadline = (Get-Date).AddMinutes(6)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited -and
           -not ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet))) { Start-Sleep 2 }
    if ($p.HasExited) { "editor exited: $($p.ExitCode)"; return }
    Start-Sleep 3
    $p.Refresh()
    [W]::PostMessageW($p.MainWindowHandle, 0x111, [IntPtr]44901, [IntPtr]::Zero) | Out-Null
    Start-Sleep 3
    Select-String -Path $log -Pattern 'launch pose test|test drive|Fast test' | ForEach-Object { $_.Line }
    if (Test-Path $pose) { "pose file: $(Get-Content $pose)"; 'PASSED' } else { 'FAILED: no pose file' }
    Stop-Process -Id $p.Id -Force
} finally { Remove-Item Env:FRS_TEST_LAUNCH_POSE, Env:FRS_WRITE_TRACKS -ErrorAction SilentlyContinue }
