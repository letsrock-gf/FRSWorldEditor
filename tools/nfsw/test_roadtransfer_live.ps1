# In-game check of road import (27/11), on Data2015 itself: a scratch bake cannot be installed (FRS_WRITE_TRACKS
# reads the edit list from the scratch folder, so its output is the shipped world plus the bake: boot crash 27/11).
#  1  full backup of Data2015\TRACKS (robocopy) unless -Backup already holds one
#  2  this build's nfsw-models = a fresh copy of Gui's (build\bin\Release\nfsw-models), so the bake replaces his last
#     bake as his editor would
#  3  editor (this build, Gui's game folder): import viaduct.txt at spot 1, the original editor's roads-L4RA.txt at
#     spot 2, bake (writes Data2015\TRACKS); closed
#  4  the game is NOT started here: install_and_boot.ps1 -Pose ... does that; afterwards -Restore puts TRACKS back
# Needs the game lock held by -Owner. Never while nfsw.exe or Gui's editor runs.
# Usage: test_roadtransfer_live.ps1 -Owner <lock owner> -Out <test folder with viaduct.txt, original-L4RA.txt> -Backup <folder>
param([Parameter(Mandatory = $true)][string]$Owner, [Parameter(Mandatory = $true)][string]$Out,
      [Parameter(Mandatory = $true)][string]$Backup,
      [string]$Exe = "$(Split-Path (Split-Path $PSScriptRoot))\build-transfer\bin\Release",
      [double]$X1 = 7440, [double]$Y1 = -2700, [double]$X2 = 7700, [double]$Y2 = -2660)
$lock = Get-Content 'D:\NFS World\work\game-session.lock' -Raw -ErrorAction SilentlyContinue
if ($lock -notmatch [regex]::Escape($Owner)) { "lock not held by $Owner"; exit 1 }
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: stop"; exit 1 }
if (Get-Process FRSWorldEditor -ErrorAction SilentlyContinue) { "an editor is open: stop"; exit 1 }
$tools = $PSScriptRoot
$game = 'D:\NFS World\Data2015'
$log = "$Exe\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Rl {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder b, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetDlgItemTextW(IntPtr h, int id, System.Text.StringBuilder b, int n);
    public static IntPtr MainOf(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => { uint owner; GetWindowThreadProcessId(h, out owner); if (owner != pid) return true;
            var b = new System.Text.StringBuilder(128); GetClassNameW(h, b, 128);
            if (b.ToString() == "FRSWorldEditor.MainWindow") { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
    public static string Dialog(uint pid) {
        string text = null;
        EnumWindows((h, l) => { uint owner; GetWindowThreadProcessId(h, out owner); if (owner != pid) return true;
            var b = new System.Text.StringBuilder(64); GetClassNameW(h, b, 64);
            if (b.ToString() != "#32770") return true;
            var t = new System.Text.StringBuilder(2048); GetDlgItemTextW(h, 0xFFFF, t, 2048);
            text = t.ToString(); PostMessageW(h, 0x0010, IntPtr.Zero, IntPtr.Zero); return false; }, IntPtr.Zero);
        return text;
    }
}
'@
function Run([uint32]$id, [string]$done, [int]$minutes = 5) {
    $before = @(Select-String -Path $log -Pattern $done).Count
    [Rl]::PostMessageW($main, 0x0111, [IntPtr]$id, [IntPtr]::Zero) | Out-Null
    $deadline = (Get-Date).AddMinutes($minutes)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited -and @(Select-String -Path $log -Pattern $done).Count -le $before) {
        $box = [Rl]::Dialog([uint32]$p.Id); if ($box) { "  MESSAGE: $($box -replace "`r?`n", ' | ')" }
        Start-Sleep -Seconds 2
    }
    Start-Sleep -Seconds 3
    $box = [Rl]::Dialog([uint32]$p.Id); if ($box) { "  MESSAGE: $($box -replace "`r?`n", ' | ')" }
}
function Look($x, $y, $name) {
    try { & "$tools\cam.ps1" -ProcessId $p.Id -X $x -Y ($y - 60) -Z 260 -Yaw 1.5708 -Pitch -1.2 -Out "$Out\$name" -Command 1006 2>&1 | Out-Null }
    catch { "  (no picture: $_)" }
}
$camKey = 'HKCU:\Software\FRSWorldEditorNFSW'
$savedCam = (Get-ItemProperty -Path $camKey -Name Camera -ErrorAction SilentlyContinue).Camera

# 1 backup
if (-not (Test-Path "$Backup\TRACKS\frs-original")) {
    robocopy "$game\TRACKS" "$Backup\TRACKS" /MIR /NFL /NDL /NJH /NP /R:1 /W:1 | Select-Object -Last 4
    if ($LASTEXITCODE -ge 8) { "backup failed ($LASTEXITCODE)"; exit 1 }
}
"backup: $Backup\TRACKS"
# 2 Gui's road network, fresh
$models = "$Exe\nfsw-models"
if (Test-Path $models) { Remove-Item $models -Recurse -Force }
Copy-Item "$(Split-Path (Split-Path $PSScriptRoot))\build\bin\Release\nfsw-models" $models -Recurse
Copy-Item "$models\roads-L5RA.txt" "$Out\live-gui-L5RA.txt" -Force
# 3 import + bake on Data2015
Remove-Item Env:FRS_GAME_FOLDER, Env:FRS_WRITE_TRACKS -ErrorAction SilentlyContinue
$env:FRS_ROADS_FILE = "$Out\transfer.txt"
try {
    Remove-Item $log -ErrorAction SilentlyContinue
    $script:p = Start-Process "$Exe\FRSWorldEditor.exe" -WorkingDirectory $Exe -PassThru
    $deadline = (Get-Date).AddMinutes(8)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }; Start-Sleep 2 }
    Start-Sleep 4
    $script:main = [Rl]::MainOf([uint32]$p.Id)
    if ($main -eq [IntPtr]::Zero) { "editor window not found"; return }
    Select-String -Path $log -Pattern 'game folder|NFS World edits' | Select-Object -Last 3 | ForEach-Object { "  " + $_.Line }
    Look $X1 $Y1 'L-spot1.png'
    Copy-Item "$Out\viaduct.txt" "$Out\transfer.txt" -Force
    Run 47196 'roads: .* imported|No road'
    Look $X2 $Y2 'L-spot2.png'
    Copy-Item "$Out\original-L4RA.txt" "$Out\transfer.txt" -Force
    Run 47196 'roads: .* imported|No road'
    Copy-Item "$models\roads-L5RA.txt" "$Out\live-imported-L5RA.txt" -Force
    Run 47106 'roads: baked|nothing was baked|could not|Could not' 12
    Select-String -Path $log -Pattern 'roads: .*imported|roads: baked|NFS World save|stored polygons|error' |
        Select-Object -Last 10 | ForEach-Object { "  " + $_.Line }
    Look $X1 $Y1 'L1-viaduct.png'
    Copy-Item $log "$Out\L.log"
} finally {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Remove-Item Env:FRS_ROADS_FILE -ErrorAction SilentlyContinue
    if ($savedCam) { Set-ItemProperty -Path $camKey -Name Camera -Value $savedCam -Type Binary }
}
