# View > Game look, off and on, from the same camera: two runs of test_view.ps1 (the second with
# FRS_GAMELOOK=1), then the game-look lines of the second run's log. Nothing is written into the game
# folder (test_view sets FRS_WRITE_TRACKS to a scratch folder) and the saved camera is put back.
# Do not run while nfsw.exe is running.
param([string]$Dir = 'D:\NFS World\work\nfsw-world-editor\build-look\bin\Release',
      [string]$OutDir = 'D:\NFS World\work\gamelook-research',
      [string]$Name = 'view',
      [single]$X = 2500, [single]$Y = 2000, [single]$Z = 220, [single]$Yaw = 0.8, [single]$Pitch = -0.25)
if (Get-Process nfsw -ErrorAction SilentlyContinue) { 'nfsw.exe is running: not started'; return }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$view = Join-Path $PSScriptRoot 'test_view.ps1'
Remove-Item Env:FRS_GAMELOOK -ErrorAction SilentlyContinue
& $view -Dir $Dir -Out (Join-Path $OutDir "$Name-off.png") -X $X -Y $Y -Z $Z -Yaw $Yaw -Pitch $Pitch
# test_view stops the editor with -Force, which returns before it exits: wait, or the next run finds the
# old log's 'region loaded' and shoots before the region is drawn.
for ($i = 0; $i -lt 30 -and (Get-Process FRSWorldEditor -ErrorAction SilentlyContinue |
         Where-Object { $_.Path -like "$Dir*" }); $i++) { Start-Sleep -Milliseconds 500 }
Start-Sleep -Seconds 1
$env:FRS_GAMELOOK = '1'
try {
    & $view -Dir $Dir -Out (Join-Path $OutDir "$Name-on.png") -X $X -Y $Y -Z $Z -Yaw $Yaw -Pitch $Pitch
} finally { Remove-Item Env:FRS_GAMELOOK -ErrorAction SilentlyContinue }
Select-String -Path (Join-Path $Dir 'FRSWorldEditor.log') -Pattern 'game look|Game look' | Select-Object -First 40 |
    ForEach-Object { $_.Line }
