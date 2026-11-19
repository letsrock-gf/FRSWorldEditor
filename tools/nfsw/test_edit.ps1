# Drives the ported editor by posted messages only (no focus, no real cursor): select, delete, undo, redo, orbit.
param([string]$Tag = 'edit')
$s = Split-Path $PSScriptRoot -Parent
$d = 'D:\NFS World\work\nfsw-world-editor\build\bin\Release'
$log = "$d\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class Win {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder b, int n);
    public static IntPtr Largest(IntPtr parent) {
        IntPtr best = IntPtr.Zero; long area = 0;
        EnumChildWindows(parent, (h, l) => { RECT r; GetClientRect(h, out r); long a = (long)r.R * r.B; if (a > area) { area = a; best = h; } return true; }, IntPtr.Zero);
        return best;
    }
}
'@
Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item Env:FRS_GAME_FOLDER -ErrorAction SilentlyContinue
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(6)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'uploaded \d+ sections' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if ($p.HasExited) { "editor exited: $($p.ExitCode)"; return }
Start-Sleep -Seconds 3
$main = (Get-Process -Id $p.Id).MainWindowHandle
function Shot($name) { Start-Sleep -Milliseconds 1200; & "$PSScriptRoot\shot.ps1" -ProcessId $p.Id -Out "$s\$Tag-$name.png" | Out-Null }
function Status { $b = New-Object System.Text.StringBuilder 400; $null = [Win]::EnumChildWindows($main, { param($h, $l) $true }, [IntPtr]::Zero); }
& "$PSScriptRoot\cam.ps1" -ProcessId $p.Id -X 2500 -Y 2000 -Z 220 -Yaw 0.8 -Pitch -0.25 -Out "$s\$Tag-0-start.png" -Command 1006 | Out-Null
$view = [Win]::Largest($main)
$r = New-Object Win+RECT; [Win]::GetClientRect($view, [ref]$r) | Out-Null
"viewport ${($r.R)}x$($r.B)"
$x = [int]($r.R * 0.47); $y = [int]($r.B * 0.50)
$lp = [IntPtr](($y -shl 16) -bor $x)
[Win]::PostMessageW($view, 0x0201, [IntPtr]1, $lp) | Out-Null   # WM_LBUTTONDOWN
[Win]::PostMessageW($view, 0x0202, [IntPtr]0, $lp) | Out-Null   # WM_LBUTTONUP
Shot '1-selected'
[Win]::PostMessageW($main, 0x0111, [IntPtr]40403, [IntPtr]::Zero) | Out-Null   # delete
Shot '2-deleted'
[Win]::PostMessageW($main, 0x0111, [IntPtr]40420, [IntPtr]::Zero) | Out-Null   # undo
Shot '3-undone'
[Win]::PostMessageW($main, 0x0111, [IntPtr]40421, [IntPtr]::Zero) | Out-Null   # redo
Shot '4-redone'
[Win]::PostMessageW($main, 0x0111, [IntPtr]40420, [IntPtr]::Zero) | Out-Null   # undo again, object back
[Win]::PostMessageW($view, 0x0201, [IntPtr]1, $lp) | Out-Null; [Win]::PostMessageW($view, 0x0202, [IntPtr]0, $lp) | Out-Null
foreach ($i in 1..3) { [Win]::PostMessageW($view, 0x0100, [IntPtr]0x66, [IntPtr]1) | Out-Null; [Win]::PostMessageW($view, 0x0101, [IntPtr]0x66, [IntPtr]1) | Out-Null }   # numpad 6 x3
Shot '5-orbit45'
[Win]::PostMessageW($view, 0x0100, [IntPtr]0x46, [IntPtr]1) | Out-Null; [Win]::PostMessageW($view, 0x0101, [IntPtr]0x46, [IntPtr]1) | Out-Null   # F
Shot '6-framed'
Select-String -Path $log -Pattern 'pick:|Deleted|removed|Undone|Redone' | Select-Object -Last 8 | ForEach-Object { $_.Line }
"mem=$([int]((Get-Process -Id $p.Id).WorkingSet64 / 1MB)) MB"
Stop-Process -Id $p.Id -Force
