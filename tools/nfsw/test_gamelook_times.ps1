# Game look captures of one camera at several time-of-day positions (FRS_GAMELOOK_TIME, 0..8 along the
# attributes.bin cycle day1_1 .. day1_6). Uses test_view.ps1 (scratch FRS_WRITE_TRACKS, camera restored).
# Do not run while nfsw.exe is running.
param([string]$Dir = 'D:\NFS World\work\nfsw-world-editor\build-look\bin\Release',
      [string]$OutDir = 'D:\NFS World\work\gamelook-research',
      [string]$Name = 'time',
      [string[]]$Times = @('1.5', '3.15', '5.5', '7.5'),
      [single]$X = 2500, [single]$Y = 2000, [single]$Z = 220, [single]$Yaw = 0.8, [single]$Pitch = -0.25)
if (Get-Process nfsw -ErrorAction SilentlyContinue) { 'nfsw.exe is running: not started'; return }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$view = Join-Path $PSScriptRoot 'test_view.ps1'
$env:FRS_GAMELOOK = '1'
try {
    foreach ($t in $Times) {
        $env:FRS_GAMELOOK_TIME = $t
        & $view -Dir $Dir -Out (Join-Path $OutDir "$Name-$t.png") -X $X -Y $Y -Z $Z -Yaw $Yaw -Pitch $Pitch
        Select-String -Path (Join-Path $Dir 'FRSWorldEditor.log') -Pattern 'FRS_GAMELOOK_TIME' | Select-Object -First 1 |
            ForEach-Object { $_.Line }
        for ($i = 0; $i -lt 30 -and (Get-Process FRSWorldEditor -ErrorAction SilentlyContinue |
                 Where-Object { $_.Path -like "$Dir*" }); $i++) { Start-Sleep -Milliseconds 500 }
        Start-Sleep -Seconds 1
    }
} finally { Remove-Item Env:FRS_GAMELOOK, Env:FRS_GAMELOOK_TIME -ErrorAction SilentlyContinue }
