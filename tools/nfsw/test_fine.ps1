# Isolated test (build-test copy, posted messages, scratch folder): drags the gizmo's centre square 60 px to the
# right, twice plainly and once with Shift in the mouse messages, and prints how far each moved the object. The Shift
# drag must move a tenth as far.
param([string]$Out)
$tools = $PSScriptRoot
$d = 'D:\NFS World\work\nfsw-world-editor\build-test\bin\Release'
$log = "$d\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class F {
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
Remove-Item Env:FRS_TEST_TEXTURE, Env:FRS_TEST_EXPORT, Env:FRS_TEST_SAVE -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = $Out
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(6)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if ($p.HasExited) { "editor exited: $($p.ExitCode)"; return }
Start-Sleep -Seconds 3
$main = [F]::MainOf([uint32]$p.Id)
$view = [F]::FindWindowExW($main, [IntPtr]::Zero, 'FRSWorldEditor.Viewport', $null)
if ($main -eq [IntPtr]::Zero -or $view -eq [IntPtr]::Zero) { "editor windows not found"; Stop-Process -Id $p.Id -Force; return }
function Post($h, [uint32]$msg, [long]$w, [long]$l) { [F]::PostMessageW($h, $msg, [IntPtr]$w, [IntPtr]$l) | Out-Null; Start-Sleep -Milliseconds 500 }
function Px($x, $y) { [long](($y -shl 16) -bor $x) }
# Saves (redirected) and reads the moved object's position: the fourth row of the M line's matrix.
function Where-Is {
    Post $main 0x0111 40404 0; Start-Sleep -Seconds 5
    $line = Get-Content "$Out\frs-original\L5RA-edits.txt" -ErrorAction SilentlyContinue | Where-Object { $_ -like 'M *' } | Select-Object -Last 1
    if (-not $line) { return $null }
    $f = $line.Split(' ')
    return [double[]]@($f[16], $f[17], $f[18])
}
& "$tools\cam.ps1" -ProcessId $p.Id -X 2500 -Y 2000 -Z 220 -Yaw 0.8 -Pitch -0.25 -Out "$Out\0-start.png" -Command 1006 | Out-Null
$rc = New-Object F+RECT; [F]::GetClientRect($view, [ref]$rc) | Out-Null
# The 0.2.0 panels go by where the pointer was last seen: say where it is before every press.
$l = Px ([int]($rc.R * 0.30)) ([int]($rc.B * 0.62)); Post $view 0x0200 0 $l; Post $view 0x0201 1 $l; Post $view 0x0202 0 $l    # select the tree
Post $main 0x0111 40434 0                                  # top view, about the selection
Post $view 0x0100 0x46 1; Post $view 0x0101 0x46 1         # F: frame it -> its origin is the middle of the view
Start-Sleep -Seconds 3                                     # the camera flies there: wait for it to arrive
$cx = 613; $cy = 264                                       # the centre square in the 0.2.0 layout: the log gives the origin on screen (601, 298) at a grab, the square is 12 right and 34 up of it
"view $($rc.R)x$($rc.B), square at $cx,$cy"

function Drag($from, $keys) {                               # 60 px to the right in two steps; keys 1 = button, 5 = button + Shift
    Post $view 0x0200 0 (Px $from $cy); Post $view 0x0201 1 (Px $from $cy)
    Post $view 0x0200 $keys (Px ($from + 30) $cy); Post $view 0x0200 $keys (Px ($from + 60) $cy)
    Post $view 0x0202 0 (Px ($from + 60) $cy)
    return Where-Is
}
$p1 = Drag $cx 1                                           # the square follows the object: 60 px on after each plain drag
$p2 = Drag ($cx + 60) 1
$p3 = Drag ($cx + 120) 5                                   # with Shift: a tenth of the way
"after plain drag 1: $($p1 -join ' ')"
"after plain drag 2: $($p2 -join ' ')"
"after shift drag:   $($p3 -join ' ')"
if ($p1 -and $p2 -and $p3) {
    $plain = [math]::Sqrt([math]::Pow($p2[0] - $p1[0], 2) + [math]::Pow($p2[1] - $p1[1], 2))
    $fine  = [math]::Sqrt([math]::Pow($p3[0] - $p2[0], 2) + [math]::Pow($p3[1] - $p2[1], 2))
    "plain moved {0:F4}, shift moved {1:F4}, ratio {2:F4} (want 0.1000), height change {3:F4}" -f $plain, $fine, ($fine / $plain), ($p3[2] - $p1[2])
}
& "$tools\shot.ps1" -ProcessId $p.Id -Out "$Out\1-after.png" | Out-Null
Stop-Process -Id $p.Id -Force
