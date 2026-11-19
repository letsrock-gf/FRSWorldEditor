# Posts one message to the running ported editor (by class and process, never another program) and shows new log lines.
# Usage: post.ps1 <pid> <main|view> <msg> <wParam> [<lParam>|centre]
param([int]$ProcessId, [string]$Target, [uint32]$Msg, [long]$W, [string]$L = '0')
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class P {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll", SetLastError = true)] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
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
[P]::SetProcessDPIAware() | Out-Null
$log = 'D:\NFS World\work\nfsw-world-editor\build\bin\Release\FRSWorldEditor.log'
$before = (Get-Content $log).Count
$main = [P]::MainOf([uint32]$ProcessId)
$view = [P]::FindWindowExW($main, [IntPtr]::Zero, 'FRSWorldEditor.Viewport', $null)
if ($main -eq [IntPtr]::Zero -or $view -eq [IntPtr]::Zero) { "editor windows not found"; return }
$r = New-Object P+RECT; [P]::GetWindowRect($view, [ref]$r) | Out-Null
if ($L -eq 'centre') { $lp = [long](((([int](($r.T + $r.B) / 2)) -band 0xFFFF) -shl 16) -bor (([int](($r.L + $r.R) / 2)) -band 0xFFFF)) } else { $lp = [long]$L }
$h = if ($Target -eq 'view') { $view } else { $main }
$ok = [P]::PostMessageW($h, $Msg, [IntPtr]$W, [IntPtr]$lp)
"post to $Target msg=0x$($Msg.ToString('X')) w=$W l=$lp ok=$ok err=$([Runtime.InteropServices.Marshal]::GetLastWin32Error()) view=$($r.L),$($r.T)..$($r.R),$($r.B)"
Start-Sleep -Milliseconds 700
Get-Content $log | Select-Object -Skip $before
