# Isolated test of Roads > Select All Roads Joined / Export the Selected Roads / Import Roads (27/11).
# Own build, posted messages, scratch folders; never writes to either game, never starts while nfsw.exe runs.
#  U  the fork on Underground 2 (scratch folder: junctions to the UG2 install, its own models folder holding
#     Gui's roads-L4RA.txt from the original editor plus a 2-road viaduct put first): select joined (starts at
#     the first road), export -> viaduct.txt (must hold the viaduct only)
#  W  the fork on NFS World (scratch game: junctions to Data2015\TRACKS and GLOBAL; nfsw-models = a copy of
#     Gui's): import viaduct.txt at spot 1, import the original editor's roads-L4RA.txt as is at spot 2, bake;
#     keep the written tracks in W1
# Then verify_transfer.py and the bake verifiers read them.
# Usage: test_roadtransfer.ps1 -Out <folder> [-Exe <bin folder>] [-Ug2 <UG2 folder>]
param([Parameter(Mandatory = $true)][string]$Out,
      [string]$Exe = "$(Split-Path (Split-Path $PSScriptRoot))\build-transfer\bin\Release",
      [string]$Ug2 = 'D:\Need for Speed - Underground 2 (1)\Need for Speed - Underground 2',
      [double]$X1 = 7440, [double]$Y1 = -2700, [double]$X2 = 7700, [double]$Y2 = -2660, [switch]$NoVerify)
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: not starting the editor"; return }
$tools = $PSScriptRoot
$log = "$Exe\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Rt {
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
function Unlink($path) { if (Test-Path $path) { cmd /c rmdir "$path" | Out-Null } }
function Start-Editor {
    Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Exe*" } | Stop-Process -Force
    Remove-Item $log -ErrorAction SilentlyContinue
    $script:p = Start-Process "$Exe\FRSWorldEditor.exe" -WorkingDirectory $Exe -PassThru
    $deadline = (Get-Date).AddMinutes(8)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }
        Start-Sleep -Seconds 2
    }
    Start-Sleep -Seconds 4
    $script:main = [Rt]::MainOf([uint32]$p.Id)
    return $main -ne [IntPtr]::Zero
}
function Run([uint32]$id, [string]$done, [int]$minutes = 5) {
    $before = @(Select-String -Path $log -Pattern $done).Count
    [Rt]::PostMessageW($main, 0x0111, [IntPtr]$id, [IntPtr]::Zero) | Out-Null
    $deadline = (Get-Date).AddMinutes($minutes)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited -and @(Select-String -Path $log -Pattern $done).Count -le $before) {
        $box = [Rt]::Dialog([uint32]$p.Id)
        if ($box) { "  MESSAGE: $($box -replace "`r?`n", ' | ')" }
        Start-Sleep -Seconds 2
    }
    Start-Sleep -Seconds 3
    $box = [Rt]::Dialog([uint32]$p.Id)
    if ($box) { "  MESSAGE: $($box -replace "`r?`n", ' | ')" }
}
# The camera straight down over (x, y), so the view centre is the ground there; a picture of it.
function Look($x, $y, $name) {
    try { & "$tools\cam.ps1" -ProcessId $p.Id -X $x -Y ($y - 60) -Z 260 -Yaw 1.5708 -Pitch -1.2 -Out "$Out\$name" -Command 1006 2>&1 | Out-Null }
    catch { "  (no picture: $_)" }
}
function Lines($pattern, $n = 12) { Select-String -Path $log -Pattern $pattern | Select-Object -Last $n | ForEach-Object { "  " + $_.Line } }
$selectJoined = 47194; $export = 47195; $import = 47196; $bake = 47106
$baked = 'roads: baked|nothing was baked|could not|Could not'

# Gui's saved NFS World camera, put back at the end (cam.ps1 writes it).
$camKey = 'HKCU:\Software\FRSWorldEditorNFSW'
$savedCam = (Get-ItemProperty -Path $camKey -Name Camera -ErrorAction SilentlyContinue).Camera

$original = "$Ug2\models\roads-L4RA.txt"
if (-not (Test-Path $original)) { "no $original"; return }
New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item $original "$Out\original-L4RA.txt" -Force

# --- U: scratch Underground 2 ------------------------------------------------------------------
$u = "$Out\ug2"
if (Test-Path $u) { Get-ChildItem $u -Directory | ForEach-Object { Unlink $_.FullName } ; Remove-Item $u -Recurse -Force }
New-Item -ItemType Directory -Force "$u\models", "$Out\ug2-tracks" | Out-Null
Get-ChildItem $Ug2 -Directory | Where-Object { $_.Name -ne 'models' } | ForEach-Object {
    cmd /c mklink /J "$u\$($_.Name)" "$($_.FullName)" | Out-Null }
New-Item -ItemType File "$u\SPEED2.EXE" | Out-Null
# The viaduct: three nodes, the middle one 8 above the ends, both roads bridges; put first so Select All
# Roads Joined (no click in a script: the first road) takes it and none of Gui's roads.
$lines = Get-Content $original
$template = ($lines | Where-Object { $_ -like 'segment *' } | Select-Object -First 1) -split ' '
$seg = { param($id, $a, $b) $t = $template.Clone(); $t[1] = $id; $t[2] = $a; $t[3] = $b; $t[30] = '1'; $t -join ' ' }
$head = @($lines | Where-Object { $_ -notlike 'node *' -and $_ -notlike 'segment *' })
$via = @('node 900 1300.0000 700.0000 0.0000 1 0.000 0 0 0.0000', 'node 901 1360.0000 700.0000 8.0000 1 0.000 0 0 0.0000',
         'node 902 1420.0000 700.0000 0.0000 1 0.000 0 0 0.0000', (& $seg 903 900 901), (& $seg 904 901 902))
Set-Content -Path "$u\models\roads-L4RA.txt" -Encoding ascii -Value ($head + $via[0..2] + @($lines | Where-Object { $_ -like 'node *' }) +
    $via[3..4] + @($lines | Where-Object { $_ -like 'segment *' }))
Copy-Item "$u\models\roads-L4RA.txt" "$Out\ug2-source-L4RA.txt" -Force

# --- W: scratch NFS World -------------------------------------------------------------------------
$game = & "$tools\game.ps1"
Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"
foreach ($dir in 'tracks', 'W1' | ForEach-Object { "$Out\$_" }) { if (Test-Path $dir) { Remove-Item $dir -Recurse -Force } }
New-Item -ItemType Directory -Force "$Out\game", "$Out\tracks" | Out-Null
cmd /c mklink /J "$Out\game\TRACKS" "$game\TRACKS" | Out-Null
cmd /c mklink /J "$Out\game\GLOBAL" "$game\GLOBAL" | Out-Null
if (-not (Test-Path "$Out\game\nfsw.exe")) { New-Item -ItemType File "$Out\game\nfsw.exe" | Out-Null }
$models = "$Exe\nfsw-models"
if (Test-Path $models) { Remove-Item $models -Recurse -Force }
Copy-Item "$(Split-Path (Split-Path $PSScriptRoot))\build\bin\Release\nfsw-models" $models -Recurse
Copy-Item "$models\roads-L5RA.txt" "$Out\gui-L5RA.txt" -Force

$env:FRS_ROADS_FILE = "$Out\transfer.txt"
try {
    "=== U: the fork on Underground 2: select joined, export"
    $env:FRS_GAME_FOLDER = $u; $env:FRS_WRITE_TRACKS = "$Out\ug2-tracks"
    if (-not (Start-Editor)) { "editor window not found (UG2)"; return }
    Lines 'region loaded|game folder' 3
    Remove-Item "$Out\transfer.txt" -ErrorAction SilentlyContinue
    [Rt]::PostMessageW($main, 0x0111, [IntPtr]$selectJoined, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Seconds 3
    Run $export 'roads: .* exported|Could not'
    Lines 'roads: .* exported' 2
    if (-not (Test-Path "$Out\transfer.txt")) { "export wrote nothing: stopped"; return }
    Move-Item "$Out\transfer.txt" "$Out\viaduct.txt" -Force
    Copy-Item $log "$Out\U.log"
    Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2

    "=== W: the fork on NFS World: import viaduct (spot 1), import the original editor's file (spot 2), bake"
    $env:FRS_GAME_FOLDER = "$Out\game"; $env:FRS_WRITE_TRACKS = "$Out\tracks"
    if (-not (Start-Editor)) { "editor window not found (NFSW)"; return }
    Look $X1 $Y1 'W-spot1.png'
    Copy-Item "$Out\viaduct.txt" "$Out\transfer.txt" -Force
    Run $import 'roads: .* imported|No road'
    Look $X2 $Y2 'W-spot2.png'
    Copy-Item "$Out\original-L4RA.txt" "$Out\transfer.txt" -Force
    Run $import 'roads: .* imported|No road'
    Lines 'roads: .* imported' 2
    Copy-Item "$models\roads-L5RA.txt" "$Out\imported-L5RA.txt" -Force
    Run $bake $baked 10
    Lines 'roads:|NFS World save|refus|error' 16
    Copy-Item "$Out\tracks" "$Out\W1" -Recurse
    Look $X1 $Y1 'W1-viaduct.png'
    Look $X2 $Y2 'W1-original.png'
    Copy-Item $log "$Out\W.log"
} finally {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Remove-Item Env:FRS_WRITE_TRACKS, Env:FRS_GAME_FOLDER, Env:FRS_ROADS_FILE -ErrorAction SilentlyContinue
    Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"
    if (Test-Path $u) { Get-ChildItem $u -Directory | Where-Object { $_.Name -ne 'models' } | ForEach-Object { Unlink $_.FullName } }
    if ($savedCam) { Set-ItemProperty -Path $camKey -Name Camera -Value $savedCam -Type Binary }
}
if ($NoVerify) { return }
"=== checks"
python "$tools\verify_transfer.py" "$Out" | Select-Object -Last 12
"--- W1: verify_roadbake";  python "$tools\verify_roadbake.py" "$Out\W1" "$game\TRACKS" | Select-Object -Last 12
"--- W1: verify_newmodel";  python "$tools\verify_newmodel.py" "$Out\W1" "$game\TRACKS" | Select-Object -Last 6
"--- W1: verify_roadcol";   python "$tools\verify_roadcol.py" "$Out\W1" "$game\TRACKS" | Select-Object -Last 8
"--- W1: verify_grid";      python "$tools\verify_grid.py" "$Out\W1" | Select-Object -Last 4
"--- W1: verify_zones";     python "$tools\verify_zones.py" "$Out\W1" "$game\TRACKS" | Select-Object -Last 12
"--- W1: verify_table";     python "$tools\verify_table.py" "$Out\W1" "$game\TRACKS\frs-original\L5RA.BUN" "$game\TRACKS\frs-original" "$game\TRACKS" | Select-Object -Last 4
