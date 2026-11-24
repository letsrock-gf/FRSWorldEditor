# Opens the game as installed (its edit list) in the editor of <Exe>, saves into a scratch folder (FRS_WRITE_TRACKS),
# closes. Never writes to the game. Refuses while nfsw.exe runs.
# Usage: scratch_save.ps1 -Out <folder> [-Exe <bin folder>] [-Game <Data2015>]
param([Parameter(Mandatory = $true)][string]$Out, [string]$Exe = "D:\NFS World\work\nfsw-world-editor\build-new\bin\Release",
      [string]$Game = "D:\NFS World\Data2015", [int]$Minutes = 20)
if (Get-Process nfsw -ErrorAction SilentlyContinue) { 'nfsw.exe is running: nothing done'; exit 2 }
if (Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Exe*" }) { 'that editor is open: nothing done'; exit 2 }
Add-Type 'using System; using System.Runtime.InteropServices; public static class SS { [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l); }' -ErrorAction SilentlyContinue
$log = "$Exe\FRSWorldEditor.log"
if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force $Out | Out-Null
# The list, kept meshes/textures and shipped originals: read from FRS_WRITE_TRACKS\frs-original, so a copy (never a
# junction: the save writes the list there).
robocopy "$Game\TRACKS\frs-original" "$Out\frs-original" /E /NFL /NDL /NJH /NJS /NP | Out-Null
try {
    Remove-Item $log -ErrorAction SilentlyContinue
    $env:FRS_GAME_FOLDER = $Game
    $env:FRS_WRITE_TRACKS = $Out
    $p = Start-Process "$Exe\FRSWorldEditor.exe" -WorkingDirectory $Exe -PassThru
    $dl = (Get-Date).AddMinutes($Minutes)
    while ((Get-Date) -lt $dl -and -not $p.HasExited -and -not ((Test-Path $log) -and (Select-String -Path $log -Pattern 'uploaded \d+ sections' -Quiet))) { Start-Sleep 2 }
    Start-Sleep 10; $p.Refresh()
    [SS]::PostMessageW($p.MainWindowHandle, 0x0111, [IntPtr]40404, [IntPtr]::Zero) | Out-Null
    while ((Get-Date) -lt $dl -and -not $p.HasExited -and -not (Select-String -Path $log -Pattern 'NFS World save|save failed|Could not' -Quiet)) { Start-Sleep 2 }
    Start-Sleep 3
} finally {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Remove-Item Env:FRS_GAME_FOLDER, Env:FRS_WRITE_TRACKS -ErrorAction SilentlyContinue
}
Copy-Item $log "$Out\editor.log"
Select-String -Path $log -Pattern 'NFS World save|\[error\]|save failed' | Select-Object -Last 3 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(230, $_.Line.Length)) }
