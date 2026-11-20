# Captures the editor's look (build-test copy, posted messages, no focus): main window, the texture browser at its
# first size and after two resizes, and saves one texture as DDS through a scripted path.
param([string]$Out)
$tools = $PSScriptRoot
$d = "$(Split-Path (Split-Path $PSScriptRoot))\build-test\bin\Release"
$log = "$d\FRSWorldEditor.log"
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Ui {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
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
[Ui]::SetProcessDPIAware() | Out-Null
function Shot($h, $name) {
    Start-Sleep -Milliseconds 900
    $r = New-Object Ui+RECT; [Ui]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    if ($w -le 0 -or $ht -le 0) { "window $name has no size"; return }
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [Ui]::PrintWindow($h, $dc, 2) | Out-Null
    $g.ReleaseHdc($dc); $g.Dispose()
    $bmp.Save("$Out\$name.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    "$name ${w}x${ht}"
}
function Post($h, [uint32]$msg, [long]$w, [long]$l) { [Ui]::PostMessageW($h, $msg, [IntPtr]$w, [IntPtr]$l) | Out-Null; Start-Sleep -Milliseconds 600 }

New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -Recurse -File | Remove-Item -Force
Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item Env:FRS_TEST_TEXTURE, Env:FRS_TEST_EXPORT -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = $Out
$env:FRS_TEST_SAVE = $Out
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(6)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if ($p.HasExited) { "editor exited: $($p.ExitCode)"; return }
Start-Sleep -Seconds 3
$main = [Ui]::Find([uint32]$p.Id, 'FRSWorldEditor.MainWindow', '')
$view = [Ui]::FindWindowExW($main, [IntPtr]::Zero, 'FRSWorldEditor.Viewport', $null)
if ($main -eq [IntPtr]::Zero -or $view -eq [IntPtr]::Zero) { "editor windows not found"; Stop-Process -Id $p.Id -Force; return }
& "$tools\cam.ps1" -ProcessId $p.Id -X 2500 -Y 2000 -Z 220 -Yaw 0.8 -Pitch -0.25 -Out "$Out\ignore.png" -Command 1006 | Out-Null
$r = New-Object Ui+RECT; [Ui]::GetClientRect($view, [ref]$r) | Out-Null
$l = [long]((([int]($r.B * 0.535)) -shl 16) -bor ([int]($r.R * 0.32)))
Post $view 0x0201 1 $l; Post $view 0x0202 0 $l          # select the sign gantry
Shot $main 'main'
Post $main 0x0111 40209 0                                # texture browser
$tex = [Ui]::Find([uint32]$p.Id, '#32770', 'Texture browser')
if ($tex -eq [IntPtr]::Zero) { "texture browser not found" } else {
    Shot $tex 'textures-1'
    Post $tex 0x0111 2104 0                               # Save as PNG (scripted folder)
    [Ui]::SetWindowPos($tex, [IntPtr]::Zero, 0, 0, 1100, 760, 0x0002 -bor 0x0004 -bor 0x0010) | Out-Null   # no move, no z-order, no activate
    Shot $tex 'textures-2-larger'
    [Ui]::SetWindowPos($tex, [IntPtr]::Zero, 0, 0, 700, 520, 0x0002 -bor 0x0004 -bor 0x0010) | Out-Null
    Shot $tex 'textures-3-smaller'
}
Stop-Process -Id $p.Id -Force
