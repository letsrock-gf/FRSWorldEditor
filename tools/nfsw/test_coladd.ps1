# Isolated test (build-test copy, scratch folder, no input): collision added by the scripted hook
# FRS_TEST_COLLISION_ADD -- ground over a shipped triangle, a wall, a barrier, and a copy of the ground 3000 m
# away that no collision box holds -- is saved, and verify_coladd.py / verify_grid.py read the result. The far
# copy must be written (its nearest box grown, the grid told) when it lies inside the game's collision grid.
# -Section: the collision pack to add to (default: the first one col_cutspec.py names).
param([Parameter(Mandatory = $true)][string]$Out, [string]$Section)
$tools = $PSScriptRoot
$d = "$(Split-Path (Split-Path $PSScriptRoot))\build-test\bin\Release"
$log = "$d\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ColAdd { [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l); }
'@
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: not started"; return }
if (-not $Section) { $Section = ((python "$tools\col_cutspec.py" | Select-Object -First 1) -split ':')[0] }
"collision section: $Section"
Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force $Out | Out-Null
Remove-Item $log -ErrorAction SilentlyContinue
foreach ($v in 'FRS_TEST_EXPORT', 'FRS_TEST_TEXTURE', 'FRS_TEST_SAVE', 'FRS_TEST_REVERT', 'FRS_TEST_COLLISION_CUT', 'FRS_GAME_FOLDER') {
    Remove-Item "Env:$v" -ErrorAction SilentlyContinue
}
$env:FRS_WRITE_TRACKS = $Out
$env:FRS_TEST_COLLISION_ADD = $Section
try {
    $p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
    $deadline = (Get-Date).AddMinutes(8)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited -and
           -not ((Test-Path $log) -and (Select-String -Path $log -Pattern 'collision test:' -Quiet))) { Start-Sleep 2 }
    if ($p.HasExited) { "the editor exited with $($p.ExitCode)"; return }
    Start-Sleep 5
    # Save (menu command 40404), posted: no focus taken, written to $Out by FRS_WRITE_TRACKS.
    [ColAdd]::PostMessageW((Get-Process -Id $p.Id).MainWindowHandle, 0x0111, [IntPtr]40404, [IntPtr]::Zero) | Out-Null
    while ((Get-Date) -lt $deadline -and -not $p.HasExited -and
           -not (Select-String -Path $log -Pattern 'NFS World save|save failed|Could not' -Quiet)) { Start-Sleep 2 }
    Start-Sleep 2
} finally {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Remove-Item Env:FRS_TEST_COLLISION_ADD, Env:FRS_WRITE_TRACKS -ErrorAction SilentlyContinue
}
Select-String -Path $log -Pattern 'collision test|NFS World save|left out|\[error\]|Could not' | ForEach-Object { $_.Line }
"--- verify_coladd"; python "$tools\verify_coladd.py" $Out 2>&1 | Select-Object -Last 14
"--- verify_grid";   python "$tools\verify_grid.py" $Out 2>&1 | Select-Object -Last 8
