# Isolated test of placed cars (ui/CarsExtension, io/WorldMesh StageCarModel): never writes to the game, never starts
# while nfsw.exe or another editor runs.
#  A  an edit list with a K line (car, paint) and its P line: the editor stages the car, draws it; picture, save.
#  B  the same with another paint (blue, metallic, matte): picture, save -- the viewport colour and the 0x135200 change.
#  C  FRS_TEST_CAR places a second car through the Cars shelf's own Place (as a double click), shows the shelf and
#     the Paint rows of the selected placement; picture, save.
# Each written state is kept in $Out\<step>; verify_cars.py reads them.
# Usage: test_cars.ps1 -Out <folder> [-Exe <bin folder>] [-Car 240SX] [-Section 5445]
param([Parameter(Mandatory = $true)][string]$Out, [string]$Exe = "$(Split-Path (Split-Path $PSScriptRoot))\build-col\bin\Release",
      [string]$Car = '240SX', [int]$Section = 5445, [double]$X = 6765.0, [double]$Y = -1293.0, [double]$Z = 116.0,
      [switch]$OnlyC)
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: not starting the editor"; return }
if (Get-Process FRSWorldEditor -ErrorAction SilentlyContinue) { "another FRSWorldEditor is running: not starting"; return }
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
public static class Drag {
    [StructLayout(LayoutKind.Sequential)] public struct PT { public int x, y; }
    [StructLayout(LayoutKind.Sequential)] public struct RC { public int l, t, r, b; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RC r);
    [DllImport("user32.dll")] static extern bool ScreenToClient(IntPtr h, ref PT p);
    [DllImport("user32.dll")] static extern IntPtr ChildWindowFromPointEx(IntPtr p, PT pt, uint flags);
    [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    static IntPtr Deepest(IntPtr h, int sx, int sy) {
        while (true) {
            var p = new PT { x = sx, y = sy }; ScreenToClient(h, ref p);
            IntPtr c = ChildWindowFromPointEx(h, p, 1 | 4);   // skip invisible, transparent
            if (c == IntPtr.Zero || c == h) return h;
            h = c;
        }
    }
    static IntPtr Lp(IntPtr h, int sx, int sy) { var p = new PT { x = sx, y = sy }; ScreenToClient(h, ref p); return (IntPtr)((p.y << 16) | (p.x & 0xFFFF)); }
    // A left-button drag from (x0, y) to (x1, y), in pixels of the window picture (window rectangle), as window
    // messages to the control under it: the real cursor does not move.
    public static string Run(IntPtr main, int x0, int x1, int y) {
        RC r; GetWindowRect(main, out r);
        int sx0 = r.l + x0, sx1 = r.l + x1, sy = r.t + y;
        IntPtr h = Deepest(main, sx0, sy);
        PostMessageW(h, 0x0200, IntPtr.Zero, Lp(h, sx0, sy)); System.Threading.Thread.Sleep(150);
        PostMessageW(h, 0x0201, (IntPtr)1, Lp(h, sx0, sy)); System.Threading.Thread.Sleep(150);
        for (int i = 1; i <= 10; ++i) { int sx = sx0 + (sx1 - sx0) * i / 10; PostMessageW(h, 0x0200, (IntPtr)1, Lp(h, sx, sy)); System.Threading.Thread.Sleep(60); }
        System.Threading.Thread.Sleep(150);
        PostMessageW(h, 0x0202, IntPtr.Zero, Lp(h, sx1, sy));
        return "dragged " + x0 + "->" + x1 + " at y " + y + " on window " + h.ToString("X");
    }
}
public static class CarShelf {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc proc, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder b, int n);
    [DllImport("user32.dll")] static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, System.Text.StringBuilder l);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr h);
    // Picks the shelf whose text starts with `name` in the Library's shelf combo box, as a click would.
    public static string Pick(IntPtr main, string name) {
        string result = "no shelf combo holds " + name;
        EnumChildWindows(main, (h, l) => {
            var c = new System.Text.StringBuilder(64); GetClassNameW(h, c, 64);
            if (c.ToString() != "ComboBox") return true;
            int n = (int)SendMessageW(h, 0x146, IntPtr.Zero, IntPtr.Zero);
            var seen = new System.Text.StringBuilder();
            for (int i = 0; i < n; ++i) {
                var t = new System.Text.StringBuilder(256); SendMessageW(h, 0x148, (IntPtr)i, t);
                if (i < 40) seen.Append("|" + t.ToString());
                if (!t.ToString().TrimStart().StartsWith(name)) { if (i == n - 1) result += "; combo " + n + ": " + seen; continue; }
                SendMessageW(h, 0x14E, (IntPtr)i, IntPtr.Zero);
                SendMessageW(GetParent(h), 0x0111, (IntPtr)((1 << 16) | (GetDlgCtrlID(h) & 0xFFFF)), h);
                result = "picked shelf '" + t.ToString() + "' (" + n + " shelves)";
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
'@
function Unlink($path) { if (Test-Path $path) { cmd /c rmdir "$path" | Out-Null } }
function Start-Editor {
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
function Stop-Editor { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 } }
function Save {
    $before = @(Select-String -Path $log -Pattern 'NFS World save:|save failed|Nothing was written').Count
    [Nm]::PostMessageW($main, 0x0111, [IntPtr]40404, [IntPtr]::Zero) | Out-Null
    $deadline = (Get-Date).AddMinutes(3)
    while ((Get-Date) -lt $deadline -and @(Select-String -Path $log -Pattern 'NFS World save:|save failed|Nothing was written').Count -le $before) { Start-Sleep -Seconds 2 }
    Start-Sleep -Seconds 2
}
function Shot($name) { try { & "$tools\cam.ps1" -ProcessId $p.Id -X ($X - 6) -Y ($Y - 6) -Z ($Z + 3) -Yaw 0.785 -Pitch -0.3 -Out "$Out\$name" -Command 1006 2>&1 | Out-Null } catch { "  (no picture: $_)" } }
function Lines($pattern, $n = 12) { Select-String -Path $log -Pattern $pattern | Select-Object -Last $n | ForEach-Object { "  " + $_.Line } }
function Keep($step) {
    $dir = "$Out\$step"; if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    Copy-Item "$Out\tracks" $dir -Recurse
    Get-ChildItem $dir -Recurse -File | ForEach-Object { "  {0,10} {1}" -f $_.Length, $_.FullName.Substring($Out.Length + 1) }
    Get-Content "$dir\frs-original\L5RA-edits.txt" -ErrorAction SilentlyContinue | ForEach-Object { "  | " + $_.Substring(0, [Math]::Min(150, $_.Length)) }
}
function List($paint) {
    $name = "FRS_$($Car)_1"
    $h = [uint32]4294967295; foreach ($c in [char[]]$name) { $h = [uint32](([uint64]$h * 33 + [byte]$c) % 4294967296) }
    $hex = '{0:X8}' -f $h
    New-Item -ItemType Directory -Force "$Out\tracks\frs-original" | Out-Null
    Set-Content -Path "$Out\tracks\frs-original\L5RA-edits.txt" -Encoding ascii -Value @(
        "K $Section $hex $name $Car $paint",
        "P $Section $Section $hex 1 0 0 0 0 1 0 0 0 0 1 0 $X $Y $Z 1")
}

$game = & "$tools\game.ps1"
Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"; Unlink "$Out\game\CARS"
foreach ($dir in 'tracks', 'A1', 'B1', 'C1') { if (Test-Path "$Out\$dir") { Remove-Item "$Out\$dir" -Recurse -Force } }
New-Item -ItemType Directory -Force "$Out\game", "$Out\tracks" | Out-Null
foreach ($d in 'TRACKS', 'GLOBAL', 'CARS') { cmd /c mklink /J "$Out\game\$d" "$game\$d" | Out-Null }
if (-not (Test-Path "$Out\game\nfsw.exe")) { New-Item -ItemType File "$Out\game\nfsw.exe" | Out-Null }
$env:FRS_GAME_FOLDER = "$Out\game"
$env:FRS_WRITE_TRACKS = "$Out\tracks"
try {
  if (-not $OnlyC) {
    "=== A: red gloss, not metallic"
    List '0 0.45 0.9 0 1'
    if (-not (Start-Editor)) { "editor window not found"; return }
    Lines 'car .* staged|car texture|car .* not applied|stock parts|did not unpack' 8
    Shot 'A-red.png'
    Save
    Lines 'NFS World save|section .* written|failed|error' 6
    Keep 'A1'
    Copy-Item $log "$Out\A.log"
    Stop-Editor

    "=== B: blue metallic matte"
    List '230 0.4 0.85 1 0'
    if (-not (Start-Editor)) { "editor window not found"; return }
    Lines 'car .* staged|not applied' 4
    Shot 'B-blue.png'
    Save
    Lines 'NFS World save|section .* written|failed|error' 6
    Keep 'B1'
    Copy-Item $log "$Out\B.log"
    Stop-Editor
  }

    "=== C: FRS_TEST_CAR places a second car the way a double click on the Cars shelf does"
    Remove-Item "$Out\tracks\frs-original\L5RA-edits.txt" -ErrorAction SilentlyContinue
    $env:FRS_TEST_CAR = "$Car $X $Y $Z"
    if (-not (Start-Editor)) { "editor window not found"; return }
    Remove-Item Env:FRS_TEST_CAR
    Lines 'car .* staged|placed FRS_|car texture' 8
    Start-Sleep -Seconds 4
    "  " + [CarShelf]::Pick($main, 'Cars')
    Start-Sleep -Seconds 8
    Shot 'C-placed.png'
    # The Paint sliders as the 1400 x 860 picture shows them: saturation to ~0.9, then hue to ~200.
    "  " + [Drag]::Run($main, 1147, 1330, 471); Start-Sleep -Seconds 2
    "  " + [Drag]::Run($main, 1147, 1270, 371); Start-Sleep -Seconds 3
    Shot 'C-slid.png'
    Save
    Lines 'NFS World save|section .* written|failed|error' 6
    Keep 'C1'
    Copy-Item $log "$Out\C.log"
} finally {
    Stop-Editor
    Remove-Item Env:FRS_GAME_FOLDER, Env:FRS_WRITE_TRACKS, Env:FRS_TEST_CAR -ErrorAction SilentlyContinue
    Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"; Unlink "$Out\game\CARS"
}
