# Isolated test (build-test copy, posted messages, scratch folder): drags the gizmo's centre square seen from above,
# and checks that the texture browser lists only the selection's textures.
param([string]$Out)
$tools = $PSScriptRoot
$d = 'D:\NFS World\work\nfsw-world-editor\build-test\bin\Release'
$log = "$d\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class G {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder b, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder b, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    public static IntPtr MainOf(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => { uint owner; GetWindowThreadProcessId(h, out owner); if (owner != pid) return true;
            var b = new System.Text.StringBuilder(128); GetClassNameW(h, b, 128);
            if (b.ToString() == "FRSWorldEditor.MainWindow") { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
    public static List<string> Titles(uint pid) {
        var list = new List<string>();
        EnumWindows((h, l) => { uint owner; GetWindowThreadProcessId(h, out owner); if (owner != pid) return true;
            var b = new System.Text.StringBuilder(256); GetWindowTextW(h, b, 256); if (b.Length > 0) list.Add(b.ToString()); return true; }, IntPtr.Zero);
        return list;
    }
}
'@
New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -Recurse -File | Remove-Item -Force
Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item Env:FRS_TEST_TEXTURE -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = $Out
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(6)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if ($p.HasExited) { "editor exited: $($p.ExitCode)"; return }
Start-Sleep -Seconds 3
$main = [G]::MainOf([uint32]$p.Id)
$view = [G]::FindWindowExW($main, [IntPtr]::Zero, 'FRSWorldEditor.Viewport', $null)
if ($main -eq [IntPtr]::Zero -or $view -eq [IntPtr]::Zero) { "editor windows not found"; Stop-Process -Id $p.Id -Force; return }
function Post($h, [uint32]$msg, [long]$w, [long]$l) { [G]::PostMessageW($h, $msg, [IntPtr]$w, [IntPtr]$l) | Out-Null; Start-Sleep -Milliseconds 500 }
function At($fx, $fy, $dx = 0, $dy = 0) { $r = New-Object G+RECT; [G]::GetClientRect($view, [ref]$r) | Out-Null; [long]((([int]($r.B * $fy) + $dy) -shl 16) -bor ([int]($r.R * $fx) + $dx)) }
& "$tools\cam.ps1" -ProcessId $p.Id -X 2500 -Y 2000 -Z 220 -Yaw 0.8 -Pitch -0.25 -Out "$Out\0-start.png" -Command 1006 | Out-Null

Post $main 0x0111 40209 0                                  # open the texture browser
"browser, nothing selected: " + (([G]::Titles([uint32]$p.Id) | Where-Object { $_ -like 'Texture browser*' }) -join ' | ')
$l = At 0.30 0.62; Post $view 0x0201 1 $l; Post $view 0x0202 0 $l    # select the tree
Start-Sleep -Milliseconds 600
"browser, tree selected:    " + (([G]::Titles([uint32]$p.Id) | Where-Object { $_ -like 'Texture browser*' }) -join ' | ')

Post $main 0x0111 40434 0                                  # top view, about the selection
Post $view 0x0100 0x46 1; Post $view 0x0101 0x46 1         # F: frame it -> its middle is the middle of the view
& "$tools\shot.ps1" -ProcessId $p.Id -Out "$Out\1-top.png" | Out-Null
$before = (Select-String -Path $log -Pattern 'pick:').Count
function Px($x, $y) { [long](($y -shl 16) -bor $x) }
$c = Px 407 305; Post $view 0x0201 1 $c                    # press on the centre square
"press on the yellow square started a gizmo drag (no new pick): " + ((Select-String -Path $log -Pattern 'pick:').Count -eq $before)
Post $view 0x0200 1 (Px 467 305)                      # drag right
Post $view 0x0200 1 (Px 527 350)                    # and down
Post $view 0x0202 0 (Px 527 350)                    # release keeps it
& "$tools\shot.ps1" -ProcessId $p.Id -Out "$Out\2-dragged.png" | Out-Null
Post $main 0x0111 40404 0                                  # save (redirected)
Start-Sleep -Seconds 2
Get-Content "$Out\frs-original\L5RA-edits.txt" -ErrorAction SilentlyContinue
Select-String -Path $log -Pattern 'pick:' | Select-Object -Last 2 | ForEach-Object { $_.Line }
Stop-Process -Id $p.Id -Force
