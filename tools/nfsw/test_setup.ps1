# Captures the setup wizard's first page (build-test copy, posted messages, no focus) to check the row of each
# game. The wizard is never finished: the editor is closed while the page is up, so nothing is stored.
# -Payload <folder>: put beside the editor as its "game" folder for the run (what the package carries).
# -Install: with -World, goes on to the plugins page and presses Install: that folder gets the files.
param([string]$Out, [string]$World, [string]$Payload, [switch]$Install)
$d = "$(Split-Path (Split-Path $PSScriptRoot))\build-test\bin\Release"
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
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr SetText(IntPtr h, uint msg, IntPtr w, string l);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    // The control with that id anywhere under `top` (the page is a child dialog of the wizard).
    public static IntPtr Child(IntPtr top, int id) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(top, (h, l) => { if (GetDlgCtrlID(h) == id) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
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
if (Test-Path "$d\game") { Remove-Item "$d\game" -Recurse -Force }
if ($Payload) { Copy-Item $Payload "$d\game" -Recurse }
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
    function Shot([string]$name) {
        $r = New-Object Setup+RECT; [Setup]::GetWindowRect($h, [ref]$r) | Out-Null
        $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
        $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
        [Setup]::PrintWindow($h, $dc, 2) | Out-Null
        $g.ReleaseHdc($dc); $g.Dispose()
        $bmp.Save("$Out\$name.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
        "saved $Out\$name.png"
    }
    Shot 'setup-games'
    # -World <folder>: that folder typed into Need for Speed World's row, and what the row then says.
    if ($World) {
        [Setup]::SetText([Setup]::Child($h, 2720), 0x000C, [IntPtr]::Zero, $World) | Out-Null   # IDC_SETUP_NFSW_PATH
        Start-Sleep -Seconds 2
        "with ${World}:"
        [Setup]::Texts($h) | Where-Object { $_ -match 'Found|Not this|Not set\.$' } | ForEach-Object { "  | $_" }
    }
    if ($World -and $Install) {
        [Setup]::PostMessageW($h, 0x0471, [IntPtr]1, [IntPtr]::Zero) | Out-Null   # PSM_PRESSBUTTON, PSBTN_NEXT
        Start-Sleep -Seconds 2
        Shot 'setup-plugins'
        [Setup]::PostMessageW($h, 0x0471, [IntPtr]1, [IntPtr]::Zero) | Out-Null   # the same button, now Install
        Start-Sleep -Seconds 3
        Shot 'setup-done'
        "after Install:"
        [Setup]::Texts($h) | Where-Object { $_ -match 'Kept|FAILED|installed|Installed|Launch Game asks|Save writes' } | ForEach-Object { "  | $_" }
        "in the game folder: " + ((Get-ChildItem $World -Recurse -File | ForEach-Object { $_.FullName.Substring($World.Length + 1) }) -join ', ')
    }
}
Stop-Process -Id $p.Id -Force
if (Test-Path "$d\game") { Remove-Item "$d\game" -Recurse -Force }
