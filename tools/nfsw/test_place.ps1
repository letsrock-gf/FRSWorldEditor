# Isolated test of writing newly placed objects (build-test copy, posted messages, scratch folder):
# copies and pastes two objects, saves, then starts again so the edit list is read back. Never writes to the game.
param([string]$Out)
$tools = $PSScriptRoot
$d = 'D:\NFS World\work\nfsw-world-editor\build-test\bin\Release'
$log = "$d\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Pl {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder b, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    public static IntPtr MainOf(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => { uint owner; GetWindowThreadProcessId(h, out owner); if (owner != pid) return true;
            var b = new System.Text.StringBuilder(128); GetClassNameW(h, b, 128);
            if (b.ToString() == "FRSWorldEditor.MainWindow") { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
}
'@
function Start-Editor {
    Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
    Remove-Item $log -ErrorAction SilentlyContinue
    $script:p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
    $deadline = (Get-Date).AddMinutes(6)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }
        Start-Sleep -Seconds 2
    }
    Start-Sleep -Seconds 3
    $script:main = [Pl]::MainOf([uint32]$p.Id)
    $script:view = [Pl]::FindWindowExW($main, [IntPtr]::Zero, 'FRSWorldEditor.Viewport', $null)
    return ($main -ne [IntPtr]::Zero -and $view -ne [IntPtr]::Zero)
}
function Post($h, [uint32]$msg, [long]$w, [long]$l) { [Pl]::PostMessageW($h, $msg, [IntPtr]$w, [IntPtr]$l) | Out-Null; Start-Sleep -Milliseconds 600 }
function Click($fx, $fy) { $r = New-Object Pl+RECT; [Pl]::GetClientRect($view, [ref]$r) | Out-Null; $l = [long]((([int]($r.B * $fy)) -shl 16) -bor ([int]($r.R * $fx))); Post $view 0x0201 1 $l; Post $view 0x0202 0 $l }

New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -Recurse -File | Remove-Item -Force
Remove-Item Env:FRS_TEST_TEXTURE, Env:FRS_TEST_EXPORT -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = $Out
if (-not (Start-Editor)) { "editor windows not found"; return }
& "$tools\cam.ps1" -ProcessId $p.Id -X 2500 -Y 2000 -Z 220 -Yaw 0.8 -Pitch -0.25 -Out "$Out\0-start.png" -Command 1006 | Out-Null
Click 0.30 0.62; Post $main 0x0111 40408 0; Post $main 0x0111 40409 0      # a tree: copy, paste at the view centre
Click 0.50 0.60; Post $main 0x0111 40408 0; Post $main 0x0111 40409 0      # a second tree (the 0.2.0 panels moved what lies under the old point): copy, paste
& "$tools\shot.ps1" -ProcessId $p.Id -Out "$Out\1-pasted.png" | Out-Null
Post $main 0x0111 40404 0
Start-Sleep -Seconds 3
"--- first run"
Select-String -Path $log -Pattern 'pick:|placed |NFS World|Copied|Could not|not written' | Select-Object -Last 8 | ForEach-Object { $_.Line }
Get-ChildItem $Out -Recurse -File -Exclude *.png | ForEach-Object { "{0,10} {1}" -f $_.Length, $_.FullName.Substring($Out.Length + 1) }
Get-Content "$Out\frs-original\L5RA-edits.txt" -ErrorAction SilentlyContinue
Stop-Process -Id $p.Id -Force

"--- second run: the list read back"
if (-not (Start-Editor)) { "editor windows not found"; return }
& "$tools\cam.ps1" -ProcessId $p.Id -X 2500 -Y 2000 -Z 220 -Yaw 0.8 -Pitch -0.25 -Out "$Out\2-reloaded.png" -Command 1006 | Out-Null
Select-String -Path $log -Pattern 'NFS World edits|uploaded' | ForEach-Object { $_.Line }
Stop-Process -Id $p.Id -Force
