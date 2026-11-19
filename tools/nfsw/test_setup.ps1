# Captures the setup wizard's first page (build-test copy, posted messages, no focus) to check the row of each
# game. The wizard is never finished: the editor is closed while the page is up, so nothing is stored.
param([string]$Out)
$d = 'D:\NFS World\work\nfsw-world-editor\build-test\bin\Release'
$log = "$d\FRSWorldEditor.log"
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class Setup {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder b, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, System.Text.StringBuilder l);
    public static List<IntPtr> Tops(uint pid) {
        var found = new List<IntPtr>();
        EnumWindows((h, l) => { uint owner; GetWindowThreadProcessId(h, out owner);
            if (owner == pid && IsWindowVisible(h)) found.Add(h); return true; }, IntPtr.Zero);
        return found;
    }
    public static string Title(IntPtr h) { var t = new System.Text.StringBuilder(256); GetWindowTextW(h, t, 256); return t.ToString(); }
    // Every child's text, read with WM_GETTEXT (works across processes).
    public static List<string> Texts(IntPtr top) {
        var texts = new List<string>();
        EnumChildWindows(top, (h, l) => { var t = new System.Text.StringBuilder(512);
            SendMessageW(h, 0x000D, (IntPtr)512, t); if (t.Length > 0) texts.Add(t.ToString()); return true; }, IntPtr.Zero);
        return texts;
    }
}
'@
[Setup]::SetProcessDPIAware() | Out-Null
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: not started"; return }
New-Item -ItemType Directory -Force $Out | Out-Null
Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item Env:FRS_TEST_TEXTURE, Env:FRS_TEST_EXPORT, Env:FRS_TEST_SAVE, Env:FRS_TEST_REVERT -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = $Out
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(6)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if ($p.HasExited) { "the editor exited with $($p.ExitCode)"; return }
$main = @([Setup]::Tops([uint32]$p.Id))
"main window: $([Setup]::Title($main[0]))"
[Setup]::PostMessageW($main[0], 0x8001, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # kRunSetupMessage
Start-Sleep -Seconds 4
$wizard = @([Setup]::Tops([uint32]$p.Id) | Where-Object { $main -notcontains $_ })
if (-not $wizard) { "no wizard window" } else {
    $h = $wizard[0]
    "wizard: $([Setup]::Title($h))"
    [Setup]::Texts($h) | ForEach-Object { "  | $_" }
    $r = New-Object Setup+RECT; [Setup]::GetWindowRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [Setup]::PrintWindow($h, $dc, 2) | Out-Null
    $g.ReleaseHdc($dc); $g.Dispose()
    $bmp.Save("$Out\setup-games.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    "saved $Out\setup-games.png"
}
Stop-Process -Id $p.Id -Force
