# Isolated check of what the editor reads from the NFS World map for visibility zones, object animations and the sky
# (nothing written: FRS_WRITE_TRACKS points at a scratch folder). Starts the editor in -Dir, turns on View > Visibility
# Zones, View > Animations and View > Sky through posted WM_COMMAND (no focus taken), captures two views and prints
# the log lines that count what was read. The saved camera in the registry is put back afterwards.
# Refuses to run while nfsw.exe runs.
param([string]$Dir = 'D:\NFS World\work\nfsw-world-editor\build-read\bin\Release', [string]$Out = "$env:TEMP\frs-test-read",
      [int[]]$Commands = @(44801, 44602, 44601), [switch]$Catch)
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: not starting the editor"; return }
$tools = $PSScriptRoot
$log = "$Dir\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class R {
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
'@
$key = 'HKCU:\Software\FRSWorldEditorNFSW'
$saved = (Get-ItemProperty -Path $key -Name Camera -ErrorAction SilentlyContinue).Camera
New-Item -ItemType Directory -Force $Out | Out-Null
Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Dir*" } | Stop-Process -Force
Remove-Item $log -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = "$env:TEMP\frs-test-read-tracks"
New-Item -ItemType Directory -Force $env:FRS_WRITE_TRACKS | Out-Null
$p = Start-Process "$Dir\FRSWorldEditor.exe" -WorkingDirectory $Dir -PassThru
$deadline = (Get-Date).AddMinutes(6)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if ($p.HasExited) { "editor exited: $($p.ExitCode)"; return }
Start-Sleep -Seconds 3
$p.Refresh()
$h = $p.MainWindowHandle
[R]::ShowWindow($h, 4) | Out-Null
if ($Catch) {   # a debugger of our own on it, to say where it dies (catch_crash.ps1)
    $catcher = Start-Process powershell -ArgumentList '-NoProfile', '-File', "$tools\catch_crash.ps1", '-ProcessId', $p.Id,
        '-Seconds', 240 -RedirectStandardOutput "$Out\crash.txt" -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 5
}
foreach ($id in $Commands) { [R]::PostMessageW($h, 0x0111, [IntPtr]$id, [IntPtr]::Zero) | Out-Null; Start-Sleep -Milliseconds 500 }
# Zone 3103 (box 5856..6054, -102..199) from above, then the animated trees of section 440 (about 4970, 2550, 227).
& "$tools\cam.ps1" -ProcessId $p.Id -X 5955 -Y 48 -Z 700 -Yaw 0 -Pitch -1.5 -Out "$Out\zones.png" -Command 1006
& "$tools\cam.ps1" -ProcessId $p.Id -X 4880 -Y 2450 -Z 300 -Yaw 0.8 -Pitch -0.35 -Out "$Out\animations.png" -Command 1006
Start-Sleep -Seconds 2
if ($p.HasExited) { "editor EXITED during the run: $($p.ExitCode)" } else { Stop-Process -Id $p.Id -Force }
if ($saved) { Set-ItemProperty -Path $key -Name Camera -Value $saved -Type Binary }
Select-String -Path $log -Pattern 'preculler|NFS World animations|object animations|sky|region loaded' | ForEach-Object { $_.Line }
