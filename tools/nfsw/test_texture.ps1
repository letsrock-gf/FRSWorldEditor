# Isolated test of texture replacement: the build-test editor replaces one texture with a test image and saves
# into a scratch folder (FRS_WRITE_TRACKS). Never writes to the game.
param([string]$Out, [string]$Hash, [string]$Image)
$d = 'D:\NFS World\work\nfsw-world-editor\build-test\bin\Release'
$log = "$d\FRSWorldEditor.log"
New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -Recurse -File | Remove-Item -Force
Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
Remove-Item $log -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = $Out
$env:FRS_TEST_TEXTURE = "$Hash;$Image"
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(6)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'NFS World save|Could not' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if ($p.HasExited) { "editor exited: $($p.ExitCode)"; Get-Content $log -Tail 5; return }
Start-Sleep -Seconds 3
& "$PSScriptRoot\cam.ps1" -ProcessId $p.Id -X 2500 -Y 2000 -Z 220 -Yaw 0.8 -Pitch -0.25 -Out "$Out\replaced.png" -Command 1006 | Out-Null
Select-String -Path $log -Pattern 'texture [0-9A-F]{8}:|NFS World save|read:' | Select-Object -Last 6 | ForEach-Object { $_.Line }
"files written: " + (Get-ChildItem $Out -File -Filter '*.BUN').Count + ", kept copies: " + (Get-ChildItem "$Out\frs-original" -File -Filter '*.BUN').Count
Get-ChildItem "$Out\frs-original" -Recurse -File -Exclude *.BUN | ForEach-Object { "{0,9} {1}" -f $_.Length, $_.FullName.Substring($Out.Length + 1) }
Get-Content "$Out\frs-original\L5RA-edits.txt"
Stop-Process -Id $p.Id -Force
