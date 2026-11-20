# Starts the ported editor on Data2015, waits for the region, captures the given views. Never touches nfsw.exe.
# Usage: run_views.ps1 <tag> "x,y,z,yaw,pitch" ["x,y,z,yaw,pitch" ...]
param([string]$Tag, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Views)
$s = Split-Path $PSScriptRoot -Parent
$d = "$(Split-Path (Split-Path $PSScriptRoot))\build\bin\Release"
$log = "$d\FRSWorldEditor.log"
Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
Remove-Item $log -ErrorAction SilentlyContinue
$env:FRS_GAME_FOLDER = "$(& $PSScriptRoot\game.ps1)"
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(6)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region L5RA:' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if ($p.HasExited) { "editor exited with code $($p.ExitCode)"; Get-Content $log -Tail 5; return }
Start-Sleep -Seconds 4
"pid=$($p.Id) mem=$([int]($p.WorkingSet64 / 1MB)) MB"
Select-String -Path $log -Pattern 'region L5RA:|unresolved|instances' | Select-Object -First 6 | ForEach-Object { $_.Line }
$i = 0
foreach ($v in $Views) {
    $i++
    $f = $v.Split(',') | ForEach-Object { [single]$_ }
    & "$PSScriptRoot\cam.ps1" -ProcessId $p.Id -X $f[0] -Y $f[1] -Z $f[2] -Yaw $f[3] -Pitch $f[4] -Out "$s\$Tag-$i.png" -Command 1006
}
