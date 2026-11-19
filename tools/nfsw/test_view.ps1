# Isolated look at the view (no input, nothing saved): starts the editor given by -Dir, goes to a camera, saves a
# capture and prints the process's memory and frame figures. Run it on two builds to compare what the view draws.
# The saved camera in the registry (shared with the editor Gui uses) is put back afterwards.
param([string]$Dir = 'D:\NFS World\work\nfsw-world-editor\build-test\bin\Release', [string]$Out,
      [single]$X = 2500, [single]$Y = 2000, [single]$Z = 220, [single]$Yaw = 0.8, [single]$Pitch = -0.25)
$tools = $PSScriptRoot
$log = "$Dir\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class V { [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd); }
'@
$key = 'HKCU:\Software\FRSWorldEditorNFSW'
$saved = (Get-ItemProperty -Path $key -Name Camera -ErrorAction SilentlyContinue).Camera

Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Dir*" } | Stop-Process -Force
Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item Env:FRS_TEST_TEXTURE, Env:FRS_TEST_EXPORT, Env:FRS_TEST_SAVE -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = "$env:TEMP\frs-test-view"
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
[V]::ShowWindow($p.MainWindowHandle, 4) | Out-Null       # shown, not activated: a minimised window draws nothing
Start-Sleep -Seconds 1
& "$tools\cam.ps1" -ProcessId $p.Id -X $X -Y $Y -Z $Z -Yaw $Yaw -Pitch $Pitch -Out $Out -Command 1006
Start-Sleep -Seconds 4
& "$tools\shot.ps1" -ProcessId $p.Id -Out $Out
$p.Refresh()
"private {0:N0} MB, working set {1:N0} MB" -f ($p.PrivateMemorySize64 / 1MB), ($p.WorkingSet64 / 1MB)
Stop-Process -Id $p.Id -Force
if ($saved) { Set-ItemProperty -Path $key -Name Camera -Value $saved -Type Binary }
