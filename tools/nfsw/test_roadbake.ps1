# Isolated test of Roads > Bake on NFS World (own build, posted messages, scratch folders; never writes to the game,
# never starts while nfsw.exe runs). The road network is a file the script writes into the build's nfsw-models:
# one two-lane road with pavements, its textures and street lights named as Underground 2 names them.
#  A  bake, keep A1; Ctrl+Z, keep A2 (the game's files as shipped again)
#  B  bake again, keep B1 (must be A1 byte for byte)
#  C  start again (the list read back), Save, keep C1 (must be B1); Roads > Remove baked roads, keep C2 (shipped but
#     for the ground fitted under the road, which stays as on Underground 2)
# Then verify_roadbake.py, verify_newmodel.py, verify_roadcol.py, verify_grid.py and verify_table.py read them.
# (verify_coladd.py predates boxes growing in other packs and the cut of ground under raised ground: not used here.)
# Usage: test_roadbake.ps1 -Out <folder> [-Exe <bin folder>] [-X 6642 -Y -1250 -Z 128.6 -X2 6646 -Y2 -1282 -Z2 128.1]
#        [-Network <a roads-L5RA.txt to bake instead of the scripted road>]
param([Parameter(Mandatory = $true)][string]$Out, [string]$Exe = "$(Split-Path (Split-Path $PSScriptRoot))\build-roads\bin\Release",
      [double]$X = 6642, [double]$Y = -1250, [double]$Z = 128.6, [double]$X2 = 6646, [double]$Y2 = -1282, [double]$Z2 = 128.1,
      [string]$Network, [switch]$NoVerify)
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: not starting the editor"; return }
$tools = $PSScriptRoot
$log = "$Exe\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Rb {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder b, int n);
    public static IntPtr MainOf(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => { uint owner; GetWindowThreadProcessId(h, out owner); if (owner != pid) return true;
            var b = new System.Text.StringBuilder(128); GetClassNameW(h, b, 128);
            if (b.ToString() == "FRSWorldEditor.MainWindow") { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
    // A message box the editor opened (class #32770 of its process): its text, then closed (WM_CLOSE).
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder b, int n);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetDlgItemTextW(IntPtr h, int id, System.Text.StringBuilder b, int n);
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
    $script:main = [Rb]::MainOf([uint32]$p.Id)
    return $main -ne [IntPtr]::Zero
}
# Posts a command and waits (5 minutes at most) until the log has one more line matching $done; a message box the
# editor opens meanwhile is printed and answered OK.
function Run([uint32]$id, [string]$done) {
    $before = @(Select-String -Path $log -Pattern $done).Count
    [Rb]::PostMessageW($main, 0x0111, [IntPtr]$id, [IntPtr]::Zero) | Out-Null
    $deadline = (Get-Date).AddMinutes(5)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited -and @(Select-String -Path $log -Pattern $done).Count -le $before) {
        $box = [Rb]::Dialog([uint32]$p.Id)
        if ($box) { "  MESSAGE: $($box -replace "`r?`n", ' | ')" }
        Start-Sleep -Seconds 2
    }
    Start-Sleep -Seconds 3
    $box = [Rb]::Dialog([uint32]$p.Id)
    if ($box) { "  MESSAGE: $($box -replace "`r?`n", ' | ')" }
}
# A picture from above one end of the road, looking along it (cam.ps1: no focus taken).
function Shot($name) {
    $yaw = [Math]::Atan2($Y2 - $Y, $X2 - $X)
    try { & "$tools\cam.ps1" -ProcessId $p.Id -X ($X - 12 * [Math]::Cos($yaw)) -Y ($Y - 12 * [Math]::Sin($yaw)) -Z ($Z + 9) `
            -Yaw $yaw -Pitch -0.45 -Out "$Out\$name" -Command 1006 2>&1 | Out-Null } catch { "  (no picture: $_)" }
}
function Lines($pattern, $n = 12) { Select-String -Path $log -Pattern $pattern | Select-Object -Last $n | ForEach-Object { "  " + $_.Line } }
function Keep($step) {
    $dir = "$Out\$step"; if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    if (Test-Path "$Out\tracks") { Copy-Item "$Out\tracks" $dir -Recurse } else { New-Item -ItemType Directory $dir | Out-Null }
    $files = @(Get-ChildItem $dir -File); $list = "$dir\frs-original\L5RA-edits.txt"
    "  kept $step : $($files.Count) files, list $(if (Test-Path $list) { (Get-Content $list | Measure-Object -Line).Lines } else { 0 }) lines"
}
function Same($a, $b) {
    $fa = @(Get-ChildItem "$Out\$a" -Recurse -File | ForEach-Object { $_.FullName.Substring("$Out\$a".Length) } | Sort-Object)
    $fb = @(Get-ChildItem "$Out\$b" -Recurse -File | ForEach-Object { $_.FullName.Substring("$Out\$b".Length) } | Sort-Object)
    $diff = @(Compare-Object $fa $fb)
    $bad = @($fa | Where-Object { $fb -contains $_ } | Where-Object { (Get-FileHash "$Out\$a$_").Hash -ne (Get-FileHash "$Out\$b$_").Hash })
    if ($diff.Count -eq 0 -and $bad.Count -eq 0) { "   ok   $b = $a byte for byte ($($fa.Count) files)" }
    else { "   FAIL $b differs from $a : $($diff.Count) files only in one, $($bad.Count) differ: $(($bad + ($diff | ForEach-Object { $_.InputObject })) -join ', ')" }
}
$save = 40404; $undo = 40420; $bake = 47106; $unbake = 47107
$baked = 'roads: baked|nothing was baked|could not|Could not'

# --- scratch folders: the game is a junction to Data2015\TRACKS and \GLOBAL (read only) ------------
Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"
foreach ($dir in 'tracks', 'A1', 'A2', 'B1', 'C1', 'C2' | ForEach-Object { "$Out\$_" }) { if (Test-Path $dir) { Remove-Item $dir -Recurse -Force } }
New-Item -ItemType Directory -Force "$Out\game", "$Out\tracks" | Out-Null
cmd /c mklink /J "$Out\game\TRACKS" "$(& $PSScriptRoot\game.ps1)\TRACKS" | Out-Null
cmd /c mklink /J "$Out\game\GLOBAL" "$(& $PSScriptRoot\game.ps1)\GLOBAL" | Out-Null
if (-not (Test-Path "$Out\game\nfsw.exe")) { New-Item -ItemType File "$Out\game\nfsw.exe" | Out-Null }

# --- the road network: the build's own models folder, emptied of earlier roads first ---------------
$models = "$Exe\nfsw-models"
New-Item -ItemType Directory -Force $models | Out-Null
Get-ChildItem $models -File | Where-Object { $_.Name -match '^(roads|terrain|ground|lights)-L5RA' } | Remove-Item
$inv = [Globalization.CultureInfo]::InvariantCulture
if ($Network) { Copy-Item $Network "$models\roads-L5RA.txt" -Force } else {
Set-Content -Path "$models\roads-L5RA.txt" -Encoding ascii -Value @(
    "FRSROADS 2",
    [string]::Format($inv, "node 1 {0} {1} {2} 0 0", $X, $Y, $Z),
    [string]::Format($inv, "node 2 {0} {1} {2} 0 0", $X2, $Y2, $Z2),
    "segment 1 1 2 1 1 3.5 10 RDP_LANEA_YS_AA_KT RDP_LANEB_WD_AA_KT 2 0.15 ARC_SIDEWALK_E 0 0 1 ARC_BARRIERTYPEFWY_WH 4 0") }

Remove-Item Env:FRS_TEST_TEXTURE, Env:FRS_TEST_EXPORT, Env:FRS_TEST_IMPORT -ErrorAction SilentlyContinue
$env:FRS_GAME_FOLDER = "$Out\game"
$env:FRS_WRITE_TRACKS = "$Out\tracks"
try {
    "=== A: bake; Ctrl+Z"
    if (-not (Start-Editor)) { "editor window not found"; return }
    Run $bake $baked
    Lines 'roads:|new model .* staged|NFS World save|left alone|refus|error' 14
    Keep 'A1'
    if (-not (Test-Path "$Out\A1\frs-original\L5RA-edits.txt")) { "the bake wrote nothing: stopped"; return }
    Shot 'A1-baked.png'
    Run $undo 'NFS World save|could not'
    Lines 'undo|NFS World save' 4
    Keep 'A2'
    Copy-Item $log "$Out\A.log"

    "=== B: bake again"
    Run $bake $baked
    Lines 'roads: baked|NFS World save' 3
    Keep 'B1'
    Copy-Item $log "$Out\B.log"

    "=== C: start again (the list read back), Save; remove the baked roads"
    if (-not (Start-Editor)) { "editor window not found"; return }
    Lines 'NFS World edits' 2
    Run $save 'NFS World save|save failed'
    Keep 'C1'
    Run $unbake 'NFS World save|could not'
    Lines 'roads:|NFS World save' 4
    Keep 'C2'
    Copy-Item $log "$Out\C.log"
} finally {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Remove-Item Env:FRS_WRITE_TRACKS, Env:FRS_GAME_FOLDER -ErrorAction SilentlyContinue
    Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"
}
if ($NoVerify) { return }
$game = & "$tools\game.ps1"
"=== checks"
Same 'A1' 'B1'
Same 'B1' 'C1'
"--- A1: verify_roadbake";  python "$tools\verify_roadbake.py" "$Out\A1" "$game\TRACKS" | Select-Object -Last 12
"--- A1: verify_newmodel";  python "$tools\verify_newmodel.py" "$Out\A1" "$game\TRACKS" | Select-Object -Last 6
"--- A1: verify_roadcol";   python "$tools\verify_roadcol.py" "$Out\A1" "$game\TRACKS" | Select-Object -Last 8
"--- A1: verify_grid";      python "$tools\verify_grid.py" "$Out\A1" | Select-Object -Last 4
"--- A1: verify_zones";     python "$tools\verify_zones.py" "$Out\A1" "$game\TRACKS" | Select-Object -Last 12
"--- A1: verify_table";     python "$tools\verify_table.py" "$Out\A1" "$game\TRACKS\frs-original\L5RA.BUN" "$game\TRACKS\frs-original" "$game\TRACKS" | Select-Object -Last 4
"--- A2: verify_roadbake --undone"; python "$tools\verify_roadbake.py" "$Out\A2" "$game\TRACKS" --undone | Select-Object -Last 4
"--- C2: verify_roadbake --unbaked"; python "$tools\verify_roadbake.py" "$Out\C2" "$game\TRACKS" --unbaked | Select-Object -Last 4
