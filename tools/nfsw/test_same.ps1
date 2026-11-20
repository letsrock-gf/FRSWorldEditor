# Isolated test (build-test copy, posted messages, nothing saved) of Objects > Selection > Select All with the
# Same Name: selects what is at a point of the view, posts the command, and prints what the log says of both.
param([double]$ClickX = 0.60, [double]$ClickY = 0.80)
$tools = $PSScriptRoot
$d = "$(Split-Path (Split-Path $PSScriptRoot))\build-test\bin\Release"
$log = "$d\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class S {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder b, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    public static IntPtr Find(uint pid, string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => { uint owner; GetWindowThreadProcessId(h, out owner); if (owner != pid) return true;
            var c = new System.Text.StringBuilder(128); GetClassNameW(h, c, 128);
            if (c.ToString() == cls) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
}
'@
function Post($h, [uint32]$msg, [long]$w, [long]$l) { [S]::PostMessageW($h, $msg, [IntPtr]$w, [IntPtr]$l) | Out-Null; Start-Sleep -Milliseconds 600 }

Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item Env:FRS_TEST_TEXTURE, Env:FRS_TEST_EXPORT -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = "$env:TEMP\frs-test-same"
New-Item -ItemType Directory -Force $env:FRS_WRITE_TRACKS | Out-Null
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(6)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if ($p.HasExited) { "editor exited: $($p.ExitCode)"; return }
Start-Sleep -Seconds 3
$main = [S]::Find([uint32]$p.Id, 'FRSWorldEditor.MainWindow')
$view = [S]::FindWindowExW($main, [IntPtr]::Zero, 'FRSWorldEditor.Viewport', $null)
if ($main -eq [IntPtr]::Zero -or $view -eq [IntPtr]::Zero) { "editor windows not found"; Stop-Process -Id $p.Id -Force; return }
Post $main 0x0111 40419 0                                # nothing selected yet
& "$tools\cam.ps1" -ProcessId $p.Id -X 2500 -Y 2000 -Z 220 -Yaw 0.8 -Pitch -0.25 -Out "$env:FRS_WRITE_TRACKS\view.png" -Command 1006 2>$null | Out-Null
$r = New-Object S+RECT; [S]::GetClientRect($view, [ref]$r) | Out-Null
$l = [long]((([int]($r.B * $ClickY)) -shl 16) -bor ([int]($r.R * $ClickX)))
Post $view 0x0201 1 $l; Post $view 0x0202 0 $l           # select what is there
Post $main 0x0111 40419 0                                # and all of its name
Start-Sleep -Seconds 1
Select-String -Path $log -Pattern "pick: '|select same" | Select-Object -Last 3 | ForEach-Object { $_.Line }
Stop-Process -Id $p.Id -Force
