# Isolated test of NEW models written into an NFS World section (own build, posted messages, scratch folders; never
# writes to the game, never starts while nfsw.exe runs):
#  A  import an OBJ the way a save of Objects > New Object in Blender comes back (manifest with an origin and no
#     objects; FRS_TEST_IMPORT), save; delete the placement, save (the section must come back as shipped); undo, save.
#  B  start again: the edit list (N + P lines) read back, save again (the files must come out the same).
#  C  add a P line placing a model stored in another numbered section (newmodel_pick.py's copy candidate), save.
#  D  add a P line for a model whose textures the section does not load: the save must refuse and write nothing.
# Each written state is kept in $Out\<step> for verify_newmodel.py.
# Usage: test_newmodel.ps1 -Out <folder> [-Exe <bin folder>] [-Section 1012] [-Texture 926F5A52] [-Texture2 20F08EAB]
#        [-Copy 0139B912] [-Refuse 0000DC86]
param([Parameter(Mandatory = $true)][string]$Out, [string]$Exe = "$(Split-Path (Split-Path $PSScriptRoot))\build-new\bin\Release",
      [int]$Section = 1012, [string]$Texture = '926F5A52', [string]$Texture2 = '20F08EAB',
      [string]$Copy = '0139B912', [string]$Refuse = '0000DC86',
      [double]$X = 2110.74, [double]$Y = 783.60, [double]$Z = 154.04, [switch]$Replay, [string]$Start = 'A', [switch]$NewTextures, [switch]$OnlyA, [string]$Append = '')
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: not starting the editor"; return }
$tools = $PSScriptRoot
$log = "$Exe\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Nm {
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
    $script:main = [Nm]::MainOf([uint32]$p.Id)
    return $main -ne [IntPtr]::Zero
}
function Post([uint32]$id, [int]$wait = 4) { [Nm]::PostMessageW($main, 0x0111, [IntPtr]$id, [IntPtr]::Zero) | Out-Null; Start-Sleep -Seconds $wait }
# Save, and wait (3 minutes at most) for the log line it ends with.
function Save {
    $before = @(Select-String -Path $log -Pattern 'NFS World save:|save failed|Nothing was written').Count
    [Nm]::PostMessageW($main, 0x0111, [IntPtr]$save, [IntPtr]::Zero) | Out-Null
    $deadline = (Get-Date).AddMinutes(3)
    while ((Get-Date) -lt $deadline -and @(Select-String -Path $log -Pattern 'NFS World save:|save failed|Nothing was written').Count -le $before) { Start-Sleep -Seconds 2 }
    Start-Sleep -Seconds 2
}
function Shot($name) { try { & "$tools\cam.ps1" -ProcessId $p.Id -X ($X - 14) -Y ($Y - 14) -Z ($Z + 8) -Yaw 0.785 -Pitch -0.35 -Out "$Out\$name" -Command 1006 2>&1 | Out-Null } catch { "  (no picture: $_)" } }
function Lines($pattern, $n = 12) { Select-String -Path $log -Pattern $pattern | Select-Object -Last $n | ForEach-Object { "  " + $_.Line } }
function Keep($step) {   # the written state, for the verifier
    $dir = "$Out\$step"; if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    Copy-Item "$Out\tracks" $dir -Recurse
    Get-ChildItem $dir -Recurse -File | ForEach-Object { "  {0,10} {1}" -f $_.Length, $_.FullName.Substring($Out.Length + 1) }
    Get-Content "$dir\frs-original\L5RA-edits.txt" -ErrorAction SilentlyContinue | ForEach-Object { "  | " + $_.Substring(0, [Math]::Min(150, $_.Length)) }
}
$save = 40404; $delete = 40403; $undo = 40420; $import = 40121

# --- scratch folders: the game is a junction to Data2015\TRACKS and \GLOBAL (read only) ------------
Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"
$steps = if ($Start -eq 'A') { 'tracks', 'edit', 'A1', 'A2', 'A3', 'B', 'C', 'D', 'E' } else { 'tracks', 'B', 'C', 'D', 'E' }
foreach ($dir in ($steps | ForEach-Object { "$Out\$_" })) { if (Test-Path $dir) { Remove-Item $dir -Recurse -Force } }
New-Item -ItemType Directory -Force "$Out\game", "$Out\tracks", "$Out\edit" | Out-Null
cmd /c mklink /J "$Out\game\TRACKS" "$(& $PSScriptRoot\game.ps1)\TRACKS" | Out-Null
cmd /c mklink /J "$Out\game\GLOBAL" "$(& $PSScriptRoot\game.ps1)\GLOBAL" | Out-Null
if (-not (Test-Path "$Out\game\nfsw.exe")) { New-Item -ItemType File "$Out\game\nfsw.exe" | Out-Null }

# A Blender save of a new object: a 4 m box and a 2 m slab on it, Y up, two game textures (two effects).
$obj = "$Out\edit\L5RA-new.obj"
$v = @()
foreach ($b in @(@(-2, 0, -2, 2, 4, 2), @(-1, 4, -1, 1, 4.5, 1))) {
    for ($i = 0; $i -lt 8; $i++) {
        $v += "v {0} {1} {2}" -f $(if ($i -band 1) { $b[3] } else { $b[0] }), $(if ($i -band 2) { $b[4] } else { $b[1] }), $(if ($i -band 4) { $b[5] } else { $b[2] })
    }
}
$faces = @(@(1, 3, 4, 2), @(5, 6, 8, 7), @(1, 2, 6, 5), @(3, 7, 8, 4), @(1, 5, 7, 3), @(2, 4, 8, 6))
$mat1 = "tex_$Texture"; $mat2 = "tex_$Texture2"; $mtl = 'none.mtl'
if ($NewTextures) {
    # -NewTextures: materials that name no game texture -- a picture of its own (map_Kd, 300 x 200, so it is
    # brought to 256 x 256) and a flat Kd colour -- which must become new textures.
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap 300, 200
    for ($px = 0; $px -lt 300; $px++) { for ($py = 0; $py -lt 200; $py++) {
        $c = if ((([int]($px / 25) + [int]($py / 25)) % 2) -eq 0) { [System.Drawing.Color]::FromArgb(255, 230, 40, 200) } else { [System.Drawing.Color]::FromArgb(255, 20, 200, 60) }
        $bmp.SetPixel($px, $py, $c) } }
    $bmp.Save("$Out\edit\my wall.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    $mtl = 'L5RA-new.mtl'; $mat1 = 'Parede.001'; $mat2 = 'Tinta'
    Set-Content -Path "$Out\edit\$mtl" -Encoding ascii -Value @("newmtl $mat1", "Kd 1 1 1", "map_Kd my wall.png", "", "newmtl $mat2", "Kd 0.9 0.6 0.1")
}
$text = @("# test: new object", "mtllib $mtl") + $v + @("vt 0 0", "vt 1 0", "vt 1 1", "vt 0 1", "o Box", "usemtl $mat1")
foreach ($f in $faces) { $text += "f {0}/1 {1}/2 {2}/3 {3}/4" -f $f[0], $f[1], $f[2], $f[3] }
$text += @("usemtl $mat2")
foreach ($f in $faces) { $text += "f {0}/1 {1}/2 {2}/3 {3}/4" -f ($f[0] + 8), ($f[1] + 8), ($f[2] + 8), ($f[3] + 8) }
Set-Content -Path $obj -Value $text -Encoding ascii
Set-Content -Path "$Out\edit\L5RA-new.json" -Encoding ascii -Value @"
{
  "region": "L5RA",
  "space": "world",
  "axes": "obj is Y-up: obj(x,y,z) = game(x, -z, y)",
  "origin": [$X, $Y, $Z],
  "objects": [
  ]
}
"@

Remove-Item Env:FRS_TEST_TEXTURE, Env:FRS_TEST_EXPORT -ErrorAction SilentlyContinue
$env:FRS_GAME_FOLDER = "$Out\game"
$env:FRS_WRITE_TRACKS = "$Out\tracks"
$env:FRS_TEST_IMPORT = $obj
try {
  if ($Start -eq 'A') {
    "=== A: import a new object, save; delete, save; undo, save"
    if (-not (Start-Editor)) { "editor window not found"; return }
    Post $import 8
    Lines 'new model|new texture|mesh edit|left alone|refus|placed' 10
    Shot 'A-imported.png'
    Save
    Lines 'NFS World save|section .* written|new textures|failed|error' 8
    Keep 'A1'
    Post $delete 4; Save
    "--- after delete + save"
    Lines 'NFS World save|put back|failed|error' 4
    Keep 'A2'
    Post $undo 6
    Lines 'undo|Undo' 3
    Save
    "--- after undo + save"
    Lines 'NFS World save|section .* written|failed|error' 4
    Keep 'A3'
    if ($OnlyA) { Copy-Item $log "$Out\A.log"; return }
    Stop-Process -Id $p.Id -Force
  } elseif ($Start -ne 'E') {
    Copy-Item "$Out\A1\*" "$Out\tracks" -Recurse    # resume from the first save
  }

  if ($Start -ne 'E') {   # -Start E -Replay: only Gui's list
    "=== B: start again (the list read back), save"
    Remove-Item Env:FRS_TEST_IMPORT -ErrorAction SilentlyContinue
    if (-not (Start-Editor)) { "editor window not found"; return }
    Lines 'new model|new texture|NFS World edits|not applied' 8
    Shot 'B-reloaded.png'
    Save
    Lines 'NFS World save|section .* written|failed|error' 4
    Keep 'B'
    Stop-Process -Id $p.Id -Force

    "=== C: a model from another numbered section placed here (copy)"
    $list = "$Out\tracks\frs-original\L5RA-edits.txt"
    Add-Content -Path $list -Encoding ascii -Value ([string]::Format([cultureinfo]::InvariantCulture, "P $Section $Section $Copy 1 0 0 0 0 1 0 0 0 0 1 0 {0} {1} {2} 1", ($X + 12), $Y, $Z))
    if (-not (Start-Editor)) { "editor window not found"; return }
    Save
    Lines 'NFS World save|section .* written|failed|error' 4
    Keep 'C'
    Stop-Process -Id $p.Id -Force

    "=== D: a model whose textures the section does not load (its textures copied beside it, then the model)"
    Add-Content -Path $list -Encoding ascii -Value ([string]::Format([cultureinfo]::InvariantCulture, "P $Section $Section $Refuse 1 0 0 0 0 1 0 0 0 0 1 0 {0} {1} {2} 1", ($X - 12), $Y, $Z))
    if (-not (Start-Editor)) { "editor window not found"; return }
    Save
    Lines 'NFS World save|copied|new textures|failed' 8
    Keep 'D'
    Stop-Process -Id $p.Id -Force
  }

    if ($Replay) {
        "=== E: Gui's own edit list (copied, read only) replayed and saved"
        Remove-Item "$Out\tracks" -Recurse -Force
        New-Item -ItemType Directory -Force "$Out\tracks\frs-original" | Out-Null
        $real = "$(& $PSScriptRoot\game.ps1)\TRACKS\frs-original"
        Copy-Item "$real\L5RA-edits.txt" "$Out\tracks\frs-original\"
        if ($Append) { Get-Content $Append | Add-Content -Path "$Out\tracks\frs-original\L5RA-edits.txt" -Encoding ascii }
        foreach ($dir in 'textures', 'meshes') { if (Test-Path "$real\$dir") { Copy-Item "$real\$dir" "$Out\tracks\frs-original\" -Recurse } }
        if (-not (Start-Editor)) { "editor window not found"; return }
        Lines 'NFS World edits|not applied' 4
        [Nm]::PostMessageW($main, 0x0111, [IntPtr]$save, [IntPtr]::Zero) | Out-Null
        $deadline = (Get-Date).AddMinutes(5)
        while ((Get-Date) -lt $deadline -and -not (Select-String -Path $log -Pattern 'NFS World save:|save failed' -Quiet)) { Start-Sleep -Seconds 3 }
        Lines 'copied \(|not copied|new textures:' 40
        Lines 'NFS World save|failed' 3
        Keep 'E' | Select-Object -Last 12
    }
} finally {
    Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Exe*" } | Stop-Process -Force
    Remove-Item Env:FRS_GAME_FOLDER, Env:FRS_WRITE_TRACKS, Env:FRS_TEST_IMPORT -ErrorAction SilentlyContinue
    Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"
}
