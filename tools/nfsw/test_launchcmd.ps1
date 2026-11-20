# Isolated check of Launch Game's own command on NFS World (build-test copy, posted messages, no focus, no game
# started): a scratch game folder (junctions to the real TRACKS and GLOBAL, an empty nfsw.exe) gets an
# editor-launch.cmd that only leaves a note. Launch Game must run it from the game folder and leave the request
# for editor-fast-test.asi; without the file, and with no launcher beside the folder, it must say how to set one.
param([Parameter(Mandatory = $true)][string]$Out)
$d = "$(Split-Path (Split-Path $PSScriptRoot))\build-test\bin\Release"
$log = "$d\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class Launch {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, System.Text.StringBuilder l);
    public static List<IntPtr> Tops(uint pid) {
        var found = new List<IntPtr>();
        EnumWindows((h, l) => { uint owner; GetWindowThreadProcessId(h, out owner);
            if (owner == pid && IsWindowVisible(h)) found.Add(h); return true; }, IntPtr.Zero);
        return found;
    }
    public static List<string> Texts(IntPtr top) {
        var texts = new List<string>();
        EnumChildWindows(top, (h, l) => { var t = new System.Text.StringBuilder(1024);
            SendMessageW(h, 0x000D, (IntPtr)1024, t); if (t.Length > 0) texts.Add(t.ToString()); return true; }, IntPtr.Zero);
        return texts;
    }
}
'@
function Unlink([string]$path) { if (Test-Path $path) { cmd /c rmdir "$path" | Out-Null } }   # the junction only
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: not started"; return }
Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force

$game = "$Out\game"
Unlink "$game\TRACKS"; Unlink "$game\GLOBAL"
if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force $game, "$Out\tracks" | Out-Null
cmd /c mklink /J "$game\TRACKS" "$(& $PSScriptRoot\game.ps1)\TRACKS" | Out-Null
cmd /c mklink /J "$game\GLOBAL" "$(& $PSScriptRoot\game.ps1)\GLOBAL" | Out-Null
New-Item -ItemType File "$game\nfsw.exe" | Out-Null
[IO.File]::WriteAllText("$game\editor-launch.cmd", "@echo started in %CD%> `"%~dp0launched.txt`"`r`n")

Remove-Item Env:FRS_TEST_TEXTURE, Env:FRS_TEST_EXPORT, Env:FRS_TEST_SAVE, Env:FRS_TEST_REVERT, Env:FRS_TEST_LAUNCH_POSE -ErrorAction SilentlyContinue
$env:FRS_GAME_FOLDER = $game
$env:FRS_WRITE_TRACKS = "$Out\tracks"
Remove-Item $log -ErrorAction SilentlyContinue
try {
    $p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
    $deadline = (Get-Date).AddMinutes(6)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited -and
           -not ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet))) { Start-Sleep 2 }
    if ($p.HasExited) { "the editor exited with $($p.ExitCode)"; return }
    Start-Sleep 3
    $main = @([Launch]::Tops([uint32]$p.Id))

    # 1. with editor-launch.cmd
    [Launch]::PostMessageW($main[0], 0x111, [IntPtr]1007, [IntPtr]::Zero) | Out-Null   # IDC_LAUNCH_GAME
    $deadline = (Get-Date).AddSeconds(90)
    while ((Get-Date) -lt $deadline -and -not (Test-Path "$game\launched.txt")) { Start-Sleep 1 }
    $note = if (Test-Path "$game\launched.txt") { (Get-Content "$game\launched.txt" -Raw).Trim() } else { '' }
    $request = if (Test-Path "$game\scripts\editor-fast-test.txt") { (Get-Content "$game\scripts\editor-fast-test.txt" -Raw).Trim() } else { '' }
    "own command: '$note'"
    "request for the ASI: '$request'"
    Select-String -Path $log -Pattern 'launch' | ForEach-Object { "  $($_.Line)" }
    $one = ($note -eq "started in $game") -and ($request -match '^(pose |menus$)')

    # 2. without it, and with no launcher beside the folder
    Remove-Item "$game\editor-launch.cmd", "$game\launched.txt" -Force
    Remove-Item "$game\scripts\editor-fast-test.txt" -ErrorAction SilentlyContinue
    [Launch]::PostMessageW($main[0], 0x111, [IntPtr]1007, [IntPtr]::Zero) | Out-Null
    Start-Sleep 4
    $box = @([Launch]::Tops([uint32]$p.Id) | Where-Object { $main -notcontains $_ })
    $said = if ($box) { ([Launch]::Texts($box[0]) -join ' | ') } else { '' }
    "without a command: '$($said.Substring(0, [Math]::Min(230, $said.Length)))'"
    $two = ($said -match 'editor-launch\.cmd') -and -not (Test-Path "$game\scripts\editor-fast-test.txt")
    if ($one -and $two) { 'PASSED' } else { "FAILED (own command $one, message $two)" }
} finally {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Start-Sleep 1
    Unlink "$game\TRACKS"; Unlink "$game\GLOBAL"
    Remove-Item Env:FRS_GAME_FOLDER, Env:FRS_WRITE_TRACKS -ErrorAction SilentlyContinue
}
