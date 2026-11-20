# Isolated test (build-test copy, posted messages, scratch folder) of textures whose alpha is not opacity:
# replaces one with an opaque image at load (FRS_TEST_TEXTURE) and saves into the scratch folder, then selects what
# is at a point of the view and saves its first texture as PNG. verify_alpha.py reads the results.
param([string]$Out, [string]$Hash, [string]$Image, [double]$ClickX = 0.60, [double]$ClickY = 0.80)
$tools = $PSScriptRoot
$d = "$(Split-Path (Split-Path $PSScriptRoot))\build-test\bin\Release"
$log = "$d\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class A {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder b, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder b, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    public static IntPtr Find(uint pid, string cls, string titleStart) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => { uint owner; GetWindowThreadProcessId(h, out owner); if (owner != pid) return true;
            var c = new System.Text.StringBuilder(128); GetClassNameW(h, c, 128);
            var t = new System.Text.StringBuilder(256); GetWindowTextW(h, t, 256);
            if (c.ToString() == cls && t.ToString().StartsWith(titleStart)) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
}
'@
function Post($h, [uint32]$msg, [long]$w, [long]$l) { [A]::PostMessageW($h, $msg, [IntPtr]$w, [IntPtr]$l) | Out-Null; Start-Sleep -Milliseconds 600 }

New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -Recurse -File | Remove-Item -Force
Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item Env:FRS_TEST_EXPORT -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = $Out
$env:FRS_TEST_SAVE = $Out
$env:FRS_TEST_TEXTURE = "$Hash;$Image"
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(6)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'NFS World save|Could not' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if ($p.HasExited) { "editor exited: $($p.ExitCode)"; Get-Content $log -Tail 5; return }
Start-Sleep -Seconds 3
Select-String -Path $log -Pattern 'texture [0-9A-F]{8}:|NFS World save' | Select-Object -Last 3 | ForEach-Object { $_.Line }
$main = [A]::Find([uint32]$p.Id, 'FRSWorldEditor.MainWindow', '')
$view = [A]::FindWindowExW($main, [IntPtr]::Zero, 'FRSWorldEditor.Viewport', $null)
if ($main -eq [IntPtr]::Zero -or $view -eq [IntPtr]::Zero) { "editor windows not found"; Stop-Process -Id $p.Id -Force; return }
& "$tools\cam.ps1" -ProcessId $p.Id -X 2500 -Y 2000 -Z 220 -Yaw 0.8 -Pitch -0.25 -Out "$Out\view.png" -Command 1006 | Out-Null
$r = New-Object A+RECT; [A]::GetClientRect($view, [ref]$r) | Out-Null
$l = [long]((([int]($r.B * $ClickY)) -shl 16) -bor ([int]($r.R * $ClickX)))
Post $view 0x0201 1 $l; Post $view 0x0202 0 $l          # select what is there
Select-String -Path $log -Pattern "pick: '" | Select-Object -Last 1 | ForEach-Object { $_.Line }
Post $main 0x0111 40209 0                                # texture browser, filtered to the selection
$tex = [A]::Find([uint32]$p.Id, '#32770', 'Texture browser')
if ($tex -eq [IntPtr]::Zero) { "texture browser not found" } else {
    Post $tex 0x0111 2104 0; Start-Sleep -Seconds 1
    # Restore goes the way a replacement does after its file dialog: the list must stay the selection's.
    $t = New-Object System.Text.StringBuilder 256
    [A]::GetWindowTextW($tex, $t, 256) | Out-Null; "browser before restore: $t"
    Post $tex 0x0111 2107 0; Start-Sleep -Seconds 2
    [A]::GetWindowTextW($tex, $t, 256) | Out-Null; "browser after restore:  $t"
}
Get-ChildItem $Out -File -Filter *.png | Where-Object { $_.Name -ne 'view.png' } | ForEach-Object { "saved $($_.Name) $($_.Length) bytes" }
Stop-Process -Id $p.Id -Force
