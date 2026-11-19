# Isolated test (build-test copy, posted messages): selects objects at the given viewport fractions and exports them
# to an OBJ in a scratch folder, the way Objects > Import / Export > Export Selection does.
# Usage: test_export.ps1 <out folder> "x,y" ["x,y" ...]      (camera: the highway view)
param([string]$Out, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Points)
$tools = $PSScriptRoot
$d = 'D:\NFS World\work\nfsw-world-editor\build-test\bin\Release'
$log = "$d\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class E {
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
Remove-Item Env:FRS_TEST_TEXTURE -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = $Out
$env:FRS_TEST_EXPORT = "$Out\selection.obj"
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(6)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if ($p.HasExited) { "editor exited: $($p.ExitCode)"; return }
Start-Sleep -Seconds 3
$main = [E]::MainOf([uint32]$p.Id)
$view = [E]::FindWindowExW($main, [IntPtr]::Zero, 'FRSWorldEditor.Viewport', $null)
if ($main -eq [IntPtr]::Zero -or $view -eq [IntPtr]::Zero) { "editor windows not found"; Stop-Process -Id $p.Id -Force; return }
function Post($h, [uint32]$msg, [long]$w, [long]$l) { [E]::PostMessageW($h, $msg, [IntPtr]$w, [IntPtr]$l) | Out-Null; Start-Sleep -Milliseconds 500 }
& "$tools\cam.ps1" -ProcessId $p.Id -X 2500 -Y 2000 -Z 220 -Yaw 0.8 -Pitch -0.25 -Out "$Out\editor.png" -Command 1006 | Out-Null
$r = New-Object E+RECT; [E]::GetClientRect($view, [ref]$r) | Out-Null
$first = $true
foreach ($point in $Points) {
    $f = $point.Split(',') | ForEach-Object { [double]$_ }
    $l = [long]((([int]($r.B * $f[1])) -shl 16) -bor ([int]($r.R * $f[0])))
    $keys = if ($first) { 1 } else { 9 }       # Ctrl adds to the selection
    Post $view 0x0201 $keys $l; Post $view 0x0202 0 $l
    $first = $false
}
& "$tools\shot.ps1" -ProcessId $p.Id -Out "$Out\selected.png" | Out-Null
Post $main 0x0111 40120 0                      # export the selection
Start-Sleep -Seconds 4
Select-String -Path $log -Pattern 'pick:|export|wrote|Wrote' | Select-Object -Last 8 | ForEach-Object { $_.Line }
Get-ChildItem $Out -Recurse -File -Exclude *.png | ForEach-Object { "{0,9} {1}" -f $_.Length, $_.FullName.Substring($Out.Length + 1) } | Select-Object -First 30
Stop-Process -Id $p.Id -Force
