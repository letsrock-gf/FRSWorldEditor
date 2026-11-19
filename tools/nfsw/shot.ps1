# Captures a process's main window without giving it focus. Usage: shot.ps1 <pid> <out.png>
param([int]$ProcessId, [string]$Out)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Shot {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[Shot]::SetProcessDPIAware() | Out-Null
$h = (Get-Process -Id $ProcessId).MainWindowHandle
$r = New-Object Shot+RECT
[Shot]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $ht = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
$ok = [Shot]::PrintWindow($h, $dc, 2)
$g.ReleaseHdc($dc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
"printwindow=$ok ${w}x${ht} -> $Out"
