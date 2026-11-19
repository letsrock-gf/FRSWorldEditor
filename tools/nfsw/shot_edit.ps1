# Captures the same close view twice with the own build: once with the edit list of a scratch save (FRS_WRITE_TRACKS),
# once with an empty scratch list (the map as shipped). Scratch game folder as in test_mesh.ps1 (junctions, rmdir only).
# Usage: shot_edit.ps1 -Out <test_mesh folder> -X -Y -Z -Yaw -Pitch -Name <png prefix>
param([string]$Out, [single]$X, [single]$Y, [single]$Z, [single]$Yaw, [single]$Pitch, [string]$Name = 'close',
      [string]$Exe = 'D:\NFS World\work\nfsw-world-editor\build-mesh\bin\Release')
$tools = $PSScriptRoot
$log = "$Exe\FRSWorldEditor.log"
function Unlink($path) { if (Test-Path $path) { cmd /c rmdir "$path" | Out-Null } }
New-Item -ItemType Directory -Force "$Out\game", "$Out\empty" | Out-Null
Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"
cmd /c mklink /J "$Out\game\TRACKS" "D:\NFS World\Data2015\TRACKS" | Out-Null
cmd /c mklink /J "$Out\game\GLOBAL" "D:\NFS World\Data2015\GLOBAL" | Out-Null
$env:FRS_GAME_FOLDER = "$Out\game"
try {
    foreach ($pass in @(@{ Tracks = "$Out\tracks"; Tag = 'edited' }, @{ Tracks = "$Out\empty"; Tag = 'shipped' })) {
        $env:FRS_WRITE_TRACKS = $pass.Tracks
        Remove-Item $log -ErrorAction SilentlyContinue
        $p = Start-Process "$Exe\FRSWorldEditor.exe" -WorkingDirectory $Exe -PassThru
        $deadline = (Get-Date).AddMinutes(6)
        while ((Get-Date) -lt $deadline -and -not $p.HasExited -and -not ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet))) { Start-Sleep -Seconds 2 }
        Start-Sleep -Seconds 4
        & "$tools\cam.ps1" -ProcessId $p.Id -X $X -Y $Y -Z $Z -Yaw $Yaw -Pitch $Pitch -Out "$Out\$Name-$($pass.Tag).png" -Command 1006 | Out-Null
        "$($pass.Tag): " + ((Select-String -Path $log -Pattern 'NFS World edits' | ForEach-Object { $_.Line }) -join ' ')
        Stop-Process -Id $p.Id -Force
    }
} finally {
    Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Exe*" } | Stop-Process -Force
    Remove-Item Env:FRS_GAME_FOLDER, Env:FRS_WRITE_TRACKS -ErrorAction SilentlyContinue
    Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"
}
