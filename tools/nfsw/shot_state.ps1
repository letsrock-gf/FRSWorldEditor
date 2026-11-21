# Opens the editor (own build) on a written state kept by a test (its TRACKS folder as FRS_WRITE_TRACKS, the game a
# read-only junction) and takes pictures from given camera poses, without focus. Never while nfsw.exe runs.
# Usage: shot_state.ps1 -State <kept folder> -Out <folder> -Poses "x,y,z,yaw,pitch,name;..." [-Exe <bin folder>]
param([Parameter(Mandatory = $true)][string]$State, [Parameter(Mandatory = $true)][string]$Out, [Parameter(Mandatory = $true)][string]$Poses,
      [string]$Exe = "$(Split-Path (Split-Path $PSScriptRoot))\build-roads\bin\Release")
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: not starting the editor"; return }
$log = "$Exe\FRSWorldEditor.log"
$work = "$Out\shot-tracks"; $game = "$Out\shot-game"
foreach ($j in "$game\TRACKS", "$game\GLOBAL") { if (Test-Path $j) { cmd /c rmdir "$j" | Out-Null } }
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
Copy-Item $State $work -Recurse
New-Item -ItemType Directory -Force $game | Out-Null
cmd /c mklink /J "$game\TRACKS" "$(& $PSScriptRoot\game.ps1)\TRACKS" | Out-Null
cmd /c mklink /J "$game\GLOBAL" "$(& $PSScriptRoot\game.ps1)\GLOBAL" | Out-Null
if (-not (Test-Path "$game\nfsw.exe")) { New-Item -ItemType File "$game\nfsw.exe" | Out-Null }
$env:FRS_GAME_FOLDER = $game; $env:FRS_WRITE_TRACKS = $work
try {
    Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Exe*" } | Stop-Process -Force
    Remove-Item $log -ErrorAction SilentlyContinue
    $p = Start-Process "$Exe\FRSWorldEditor.exe" -WorkingDirectory $Exe -PassThru
    $deadline = (Get-Date).AddMinutes(8)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited -and
           -not ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet))) { Start-Sleep 2 }
    Start-Sleep 6
    foreach ($pose in $Poses -split ';') {
        $v = $pose -split ','
        & "$PSScriptRoot\cam.ps1" -ProcessId $p.Id -X $v[0] -Y $v[1] -Z $v[2] -Yaw $v[3] -Pitch $v[4] -Out "$Out\$($v[5]).png" -Command 1006 2>&1 | Out-Null
        "  $($v[5]).png"
    }
} finally {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Remove-Item Env:FRS_WRITE_TRACKS, Env:FRS_GAME_FOLDER -ErrorAction SilentlyContinue
    foreach ($j in "$game\TRACKS", "$game\GLOBAL") { if (Test-Path $j) { cmd /c rmdir "$j" | Out-Null } }
}
