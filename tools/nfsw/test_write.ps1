# Isolated test of the NFS World writer: the editor (build-test copy) removes one object and moves another by posted
# messages, saves into a scratch folder (FRS_WRITE_TRACKS), then undoes both and saves again. Never writes to the game.
param([string]$Out)
$tools = $PSScriptRoot
$d = 'D:\NFS World\work\nfsw-world-editor\build-test\bin\Release'
$log = "$d\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class W {
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
New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -Recurse -File | Remove-Item -Force
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
Start-Sleep -Seconds 3
$main = [W]::MainOf([uint32]$p.Id)
$view = [W]::FindWindowExW($main, [IntPtr]::Zero, 'FRSWorldEditor.Viewport', $null)
if ($main -eq [IntPtr]::Zero -or $view -eq [IntPtr]::Zero) { "editor windows not found"; Stop-Process -Id $p.Id -Force; return }
function Post($h, [uint32]$msg, [long]$w, [long]$l) { [W]::PostMessageW($h, $msg, [IntPtr]$w, [IntPtr]$l) | Out-Null; Start-Sleep -Milliseconds 500 }
function At($fx, $fy) { $r = New-Object W+RECT; [W]::GetClientRect($view, [ref]$r) | Out-Null; [long]((([int]($r.B * $fy)) -shl 16) -bor ([int]($r.R * $fx))) }
function Click($fx, $fy) { $l = At $fx $fy; Post $view 0x0201 1 $l; Post $view 0x0202 0 $l }
& "$tools\cam.ps1" -ProcessId $p.Id -X 2500 -Y 2000 -Z 220 -Yaw 0.8 -Pitch -0.25 -Out "$Out\0-start.png" -Command 1006 | Out-Null

Click 0.47 0.50                      # object A
Post $main 0x0111 40403 0             # remove it
Click 0.30 0.62                      # object B
Post $view 0x0100 0x47 1              # G: move
Post $view 0x0200 0 (At 0.36 0.66)    # mouse move
$l = At 0.36 0.66; Post $view 0x0201 1 $l; Post $view 0x0202 0 $l   # click keeps it
& "$tools\shot.ps1" -ProcessId $p.Id -Out "$Out\1-edited.png" | Out-Null
Post $main 0x0111 40404 0             # write placements to game (redirected to the scratch folder)
Start-Sleep -Seconds 2
"--- after first save"
Get-ChildItem $Out -Recurse -File -Exclude *.png | ForEach-Object { "{0,10} {1}" -f $_.Length, $_.FullName.Substring($Out.Length + 1) }
Copy-Item "$Out\frs-original\L5RA-edits.txt" "$Out\edits-after-save.txt" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$Out\patched" | Out-Null
Get-ChildItem $Out -Filter 'STREAML5RA_*.BUN' | Copy-Item -Destination "$Out\patched"

Post $main 0x0111 40420 0             # undo the move
Post $main 0x0111 40420 0             # undo the removal
Start-Sleep -Milliseconds 800
Post $main 0x0111 40404 0             # save again: the files go back as shipped
Start-Sleep -Seconds 2
"--- after undo and second save"
Get-ChildItem $Out -File -Filter '*.BUN' | ForEach-Object { "{0,10} {1}" -f $_.Length, $_.Name }
"kept copies left: " + (Get-ChildItem "$Out\frs-original" -File -ErrorAction SilentlyContinue | Measure-Object).Count
"--- log"
Select-String -Path $log -Pattern "pick:|move |NFS World|Deleted|removed|Undone" | Select-Object -Last 14 | ForEach-Object { $_.Line }
Stop-Process -Id $p.Id -Force
