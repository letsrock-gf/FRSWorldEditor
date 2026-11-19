# Isolated Blender round trip on the NFSW map (own build, posted messages, scratch folder; never writes to the game):
# selects objects, exports them (Objects > Export, as Edit in Blender does), runs Blender in the background with the
# editor's own frs_quickedit.py plus blender_edit.py (raise one mesh, cut another, Ctrl+S), imports the saved OBJ the
# way a Blender save does, saves, then starts again so the edit list is read back.
# The game folder is a scratch one: a junction to Data2015\TRACKS and \GLOBAL (read only) and an empty nfsw.exe
# marker, so anything the editor writes beside "the game" lands in the scratch folder; FRS_WRITE_TRACKS takes the
# section writes. The junctions are removed with rmdir (never a recursive delete through them).
# Usage: test_mesh.ps1 -Out <folder> [-Exe <bin folder>] [-NoSave] [-Points "x,y","x,y"]   (env FRS_EDIT=cut: see blender_edit.py)
param([Parameter(Mandatory = $true)][string]$Out, [string]$Exe = 'D:\NFS World\work\nfsw-world-editor\build-mesh\bin\Release',
      [switch]$NoSave, [string[]]$Points)
if (-not $Points) { $Points = @('0.30,0.62', '0.32,0.535') }
$tools = $PSScriptRoot
$repo = Split-Path (Split-Path $tools)
$log = "$Exe\FRSWorldEditor.log"
$blender = 'C:\Program Files\Blender Foundation\Blender 5.1\blender.exe'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Me {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
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
function Unlink($path) { if (Test-Path $path) { cmd /c rmdir "$path" | Out-Null } }
function Loaded { if (Test-Path $log) { @(Select-String -Path $log -Pattern 'region loaded').Count } else { 0 } }
function Wait-Loaded($count) {
    $deadline = (Get-Date).AddMinutes(8)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited -and (Loaded) -lt $count) { Start-Sleep -Seconds 2 }
    Start-Sleep -Seconds 3
}
function Start-Editor {
    Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Exe*" } | Stop-Process -Force
    Remove-Item $log -ErrorAction SilentlyContinue
    $script:p = Start-Process "$Exe\FRSWorldEditor.exe" -WorkingDirectory $Exe -PassThru
    Wait-Loaded 1
    $script:main = [Me]::MainOf([uint32]$p.Id)
    $script:view = [Me]::FindWindowExW($main, [IntPtr]::Zero, 'FRSWorldEditor.Viewport', $null)
    return ($main -ne [IntPtr]::Zero -and $view -ne [IntPtr]::Zero)
}
function Post($h, [uint32]$msg, [long]$w, [long]$l) { [Me]::PostMessageW($h, $msg, [IntPtr]$w, [IntPtr]$l) | Out-Null; Start-Sleep -Milliseconds 600 }
function Cam($name) { & "$tools\cam.ps1" -ProcessId $p.Id -X 2500 -Y 2000 -Z 220 -Yaw 0.8 -Pitch -0.25 -Out "$Out\$name" -Command 1006 | Out-Null }
function Show-Files { foreach ($dir in "$Out\tracks", "$Out\game\models", "$Out\game\nfsw-models") {
    if (Test-Path $dir) { Get-ChildItem $dir -Recurse -File | ForEach-Object { "{0,10} {1}" -f $_.Length, $_.FullName.Substring($Out.Length + 1) } } } }

# --- scratch game folder --------------------------------------------------------------------------
Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"
foreach ($dir in "$Out\tracks", "$Out\edit", "$Out\game\models") { if (Test-Path $dir) { Remove-Item $dir -Recurse -Force } }
Get-ChildItem $Out -File -ErrorAction SilentlyContinue | Remove-Item -Force
New-Item -ItemType Directory -Force "$Out\game", "$Out\tracks", "$Out\edit" | Out-Null
cmd /c mklink /J "$Out\game\TRACKS" "D:\NFS World\Data2015\TRACKS" | Out-Null
cmd /c mklink /J "$Out\game\GLOBAL" "D:\NFS World\Data2015\GLOBAL" | Out-Null
if (-not (Test-Path "$Out\game\nfsw.exe")) { New-Item -ItemType File "$Out\game\nfsw.exe" | Out-Null }

$obj = "$Out\edit\selection.obj"
Remove-Item Env:FRS_TEST_TEXTURE -ErrorAction SilentlyContinue
$env:FRS_GAME_FOLDER = "$Out\game"
$env:FRS_WRITE_TRACKS = "$Out\tracks"
$env:FRS_TEST_EXPORT = $obj
$env:FRS_TEST_IMPORT = $obj
try {
    if (-not (Start-Editor)) { "editor windows not found"; return }
    Cam '0-start.png'
    $r = New-Object Me+RECT; [Me]::GetClientRect($view, [ref]$r) | Out-Null
    $first = $true
    foreach ($point in $Points) {
        $f = $point.Split(',') | ForEach-Object { [double]$_ }
        $l = [long]((([int]($r.B * $f[1])) -shl 16) -bor ([int]($r.R * $f[0])))
        Post $view 0x0201 ($(if ($first) { 1 } else { 9 })) $l; Post $view 0x0202 0 $l
        $first = $false
    }
    Post $main 0x0111 40120 0                                  # export the selection
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline -and -not (Test-Path "$Out\edit\selection.json")) { Start-Sleep -Seconds 1 }
    Start-Sleep -Seconds 2
    "--- export"
    Select-String -Path $log -Pattern 'pick:|exported' | Select-Object -Last 4 | ForEach-Object { $_.Line }
    Copy-Item $obj "$Out\edit\selection-exported.obj"

    "--- Blender (background, frs_quickedit.py + blender_edit.py, then save)"
    & $blender --background --factory-startup --python "$repo\blender\frs_quickedit.py" --python "$tools\blender_edit.py" -- $obj 2>&1 |
        Where-Object { $_ -match 'FRS|Error|error' } | ForEach-Object { "  $_" }
    if (-not (Test-Path "$obj.sent")) { "Blender did not send the OBJ"; return }

    "--- import (as a save in Blender)"
    $before = Loaded
    Post $main 0x0111 40121 0
    Start-Sleep -Seconds 8
    Wait-Loaded $before                                        # an import that reopens the region logs it again
    Select-String -Path $log -Pattern 'import|quick edit|mesh edit|NFS World|overlay|refus' | Select-Object -Last 12 | ForEach-Object { $_.Line }
    Cam '1-imported.png'
    if (-not $NoSave) {
        "--- save"
        Post $main 0x0111 40404 0
        Start-Sleep -Seconds 6
        Select-String -Path $log -Pattern 'NFS World|mesh|written|Could not|failed|error' | Select-Object -Last 10 | ForEach-Object { $_.Line }
    }
    Show-Files
    Get-Content "$Out\tracks\frs-original\L5RA-edits.txt" -ErrorAction SilentlyContinue | ForEach-Object { $_.Substring(0, [Math]::Min(160, $_.Length)) }
    Stop-Process -Id $p.Id -Force

    if (-not $NoSave) {
        "--- second run: the list read back"
        Remove-Item Env:FRS_TEST_EXPORT, Env:FRS_TEST_IMPORT -ErrorAction SilentlyContinue
        if (-not (Start-Editor)) { "editor windows not found"; return }
        Cam '2-reloaded.png'
        Select-String -Path $log -Pattern 'NFS World edits|mesh edit' | ForEach-Object { $_.Line }
        Stop-Process -Id $p.Id -Force
    }
} finally {
    Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Exe*" } | Stop-Process -Force
    Remove-Item Env:FRS_GAME_FOLDER, Env:FRS_WRITE_TRACKS, Env:FRS_TEST_EXPORT, Env:FRS_TEST_IMPORT -ErrorAction SilentlyContinue
    Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"
}
