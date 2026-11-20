# Times how long the build-test editor takes to show its window: once right after the exe is linked again
# (-Relink) and then again with the same file. Nothing is saved; the editor is closed as soon as it is seen.
param([switch]$Relink, [int]$Runs = 2, [int]$TimeoutSeconds = 180)
$root = "$(Split-Path (Split-Path $PSScriptRoot))"
$d = "$root\build-test\bin\Release"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class W {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public static bool Shown(uint pid) {
        bool any = false;
        EnumWindows((top, l) => { uint owner; GetWindowThreadProcessId(top, out owner);
            if (owner == pid && IsWindowVisible(top)) any = true; return true; }, IntPtr.Zero);
        return any;
    }
}
'@
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: not started"; return }
Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
if ($Relink) {
    Remove-Item "$d\FRSWorldEditor.exe" -Force
    & 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build "$root\build-test" --config Release 2>&1 | Out-Null
    "linked again: $((Get-Item "$d\FRSWorldEditor.exe").LastWriteTime.ToString('HH:mm:ss'))"
}
Remove-Item Env:FRS_TEST_TEXTURE, Env:FRS_TEST_EXPORT, Env:FRS_TEST_SAVE, Env:FRS_TEST_REVERT -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = "$env:TEMP\frs-startup-test"
for ($run = 1; $run -le $Runs; ++$run) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
    $started = $clock.Elapsed.TotalSeconds
    $shown = $null
    while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds -and -not $p.HasExited) {
        if ([W]::Shown([uint32]$p.Id)) { $shown = $clock.Elapsed.TotalSeconds; break }
        Start-Sleep -Milliseconds 100
    }
    if ($p.HasExited) { "run ${run}: exited with $($p.ExitCode) after $([math]::Round($clock.Elapsed.TotalSeconds, 1)) s, no window" }
    elseif ($shown) { "run ${run}: process started in $([math]::Round($started, 1)) s, window shown at $([math]::Round($shown, 1)) s" }
    else { "run ${run}: no window in $TimeoutSeconds s" }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Start-Sleep -Seconds 2
}
