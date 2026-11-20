# Isolated test of a scenery tree that has to grow (io/WorldPlace.cpp AddLeaf): an edit list with -Count placements
# into scenery container -Section (default A479's own, 5 records in one node: 5 free slots), saved; the editor opened
# again on that list and saved again (must come out the same). Never writes to the game, never starts while nfsw.exe
# runs. Check with verify_tree.py, verify_place.py and verify_table.py on $Out\T1.
# Usage: test_treegrow.ps1 -Out <folder> [-Exe <bin folder>] [-Section 479] [-Model 8D902783] [-Count 30]
param([Parameter(Mandatory = $true)][string]$Out, [string]$Exe = "$(Split-Path (Split-Path $PSScriptRoot))\build-new\bin\Release",
      [int]$Section = 479, [string]$Model = '8D902783', [int]$Count = 30,
      [double]$X = 4604.56, [double]$Y = 2811.24, [double]$Z = 146.27)
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: not starting the editor"; return }
$log = "$Exe\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Tg {
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
    $script:main = [Tg]::MainOf([uint32]$p.Id)
    return $main -ne [IntPtr]::Zero
}
function Save {
    $before = @(Select-String -Path $log -Pattern 'NFS World save:|save failed').Count
    [Tg]::PostMessageW($main, 0x0111, [IntPtr]40404, [IntPtr]::Zero) | Out-Null
    $deadline = (Get-Date).AddMinutes(3)
    while ((Get-Date) -lt $deadline -and @(Select-String -Path $log -Pattern 'NFS World save:|save failed').Count -le $before) { Start-Sleep -Seconds 2 }
    Start-Sleep -Seconds 2
    Select-String -Path $log -Pattern 'NFS World save|save failed|cannot take|edits:' | Select-Object -Last 3 | ForEach-Object { "  " + $_.Line }
}
function Keep($step) {
    $dir = "$Out\$step"; if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    Copy-Item "$Out\tracks" $dir -Recurse
}

Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"
foreach ($dir in 'tracks', 'T1', 'T2') { if (Test-Path "$Out\$dir") { Remove-Item "$Out\$dir" -Recurse -Force } }
New-Item -ItemType Directory -Force "$Out\game", "$Out\tracks\frs-original" | Out-Null
cmd /c mklink /J "$Out\game\TRACKS" "$(& $PSScriptRoot\game.ps1)\TRACKS" | Out-Null   # read only: the editor reads its frs-original copies
cmd /c mklink /J "$Out\game\GLOBAL" "$(& $PSScriptRoot\game.ps1)\GLOBAL" | Out-Null
if (-not (Test-Path "$Out\game\nfsw.exe")) { New-Item -ItemType File "$Out\game\nfsw.exe" | Out-Null }

$lines = for ($i = 0; $i -lt $Count; $i++) {
    [string]::Format([cultureinfo]::InvariantCulture, "P $Section $Section $Model 1 0 0 0 0 1 0 0 0 0 1 0 {0} {1} {2} 1",
                     ($X + ($i % 6) * 3), ($Y + [int]($i / 6) * 3), $Z)
}
Set-Content -Path "$Out\tracks\frs-original\L5RA-edits.txt" -Encoding ascii -Value $lines

$env:FRS_GAME_FOLDER = "$Out\game"
$env:FRS_WRITE_TRACKS = "$Out\tracks"
try {
    "=== T1: $Count placements into scenery $Section, save"
    if (-not (Start-Editor)) { "editor window not found"; return }
    Save
    Keep 'T1'
    Stop-Process -Id $p.Id -Force
    "=== T2: opened again on that list, save"
    if (-not (Start-Editor)) { "editor window not found"; return }
    Save
    Keep 'T2'
    Copy-Item $log "$Out\T2.log"
    Stop-Process -Id $p.Id -Force
} finally {
    Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Exe*" } | Stop-Process -Force
    Remove-Item Env:FRS_GAME_FOLDER, Env:FRS_WRITE_TRACKS -ErrorAction SilentlyContinue
    Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"
}
