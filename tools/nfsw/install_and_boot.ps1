# Installs a scratch TRACKS output (e.g. test_roadtransfer.ps1's W1) into Data2015 and boots to the world; or puts
# the backup back and boots (the A/B). The exact steps of the 27/11 road-transfer run (crash at boot with W1,
# world in 30.5 s after restore). Needs the game lock held by -Owner; never runs while nfsw.exe runs.
#   install_and_boot.ps1 -Owner <lock owner> -Install <W1 folder> -Backup <new backup folder> [-Pose "7372 -2687.4 73.0 0"]
#   install_and_boot.ps1 -Owner <lock owner> -Restore <backup folder>
# Boot: Data2015\scripts\editor-fast-test.txt = "pose x y z heading" (car put there) or "menus"; the game is started
# through explorer (a child of this script would die with it). Waits up to 200 s; prints the fast-test log lines
# and any new dump in %LOCALAPPDATA%\CrashDumps. Leaves the game running if it reached the world: close it yourself.
param([Parameter(Mandatory = $true)][string]$Owner, [string]$Install, [string]$Backup, [string]$Restore,
      [string]$Pose = 'menus', [string]$Game = 'D:\NFS World\Data2015')
$lock = Get-Content 'D:\NFS World\work\game-session.lock' -Raw -ErrorAction SilentlyContinue
if ($lock -notmatch [regex]::Escape($Owner)) { "lock not held by $Owner"; exit 1 }
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: stop"; exit 1 }
$tr = "$Game\TRACKS"
if ($Install) {
    if (-not $Backup) { "-Backup is required with -Install"; exit 1 }
    New-Item -ItemType Directory -Force "$Backup\TRACKS" | Out-Null
    $files = Get-ChildItem $Install -Recurse -File | ForEach-Object { $_.FullName.Substring($Install.Length + 1) }
    $new = @()
    foreach ($f in $files) {
        $src = Join-Path $tr $f
        if (Test-Path $src) { $d = Join-Path "$Backup\TRACKS" $f; New-Item -ItemType Directory -Force (Split-Path $d) | Out-Null; Copy-Item $src $d }
        else { $new += $f }
    }
    $files | Set-Content "$Backup\files.txt"; $new | Set-Content "$Backup\new-files.txt"
    foreach ($f in $files) { $d = Join-Path $tr $f; New-Item -ItemType Directory -Force (Split-Path $d) | Out-Null; Copy-Item (Join-Path $Install $f) $d -Force }
    "installed $($files.Count) files ($($new.Count) new); backup $Backup"
}
if ($Restore) {
    Get-ChildItem "$Restore\TRACKS" -Recurse -File | ForEach-Object { Copy-Item $_.FullName (Join-Path $tr $_.FullName.Substring("$Restore\TRACKS".Length + 1)) -Force }
    foreach ($f in Get-Content "$Restore\new-files.txt") { if ($f -and (Test-Path (Join-Path $tr $f))) { Remove-Item (Join-Path $tr $f) } }
    $bad = @(Get-ChildItem "$Restore\TRACKS" -Recurse -File | Where-Object {
        (Get-FileHash $_.FullName).Hash -ne (Get-FileHash (Join-Path $tr $_.FullName.Substring("$Restore\TRACKS".Length + 1))).Hash })
    "restored; files differing from the backup: $($bad.Count)"
}
Set-Content -Path "$Game\scripts\editor-fast-test.txt" -Encoding ascii -Value ($(if ($Pose -eq 'menus') { 'menus' } else { "pose $Pose" }))
$t0 = Get-Date
Start-Process explorer.exe "`"$(Split-Path $Game)\Jogar NFS World Offline.cmd`""
$log = "$Game\scripts\editor-fast-test.log"; $n0 = @(Get-Content $log -ErrorAction SilentlyContinue).Count
while (((Get-Date) - $t0).TotalSeconds -lt 200) {
    Start-Sleep 5
    $l = @(Get-Content $log -ErrorAction SilentlyContinue)
    if ($l.Count -gt $n0 -and ($l[$n0..($l.Count - 1)] -match 'world reached|released at')) { break }
    if (((Get-Date) - $t0).TotalSeconds -gt 40 -and -not (Get-Process nfsw -ErrorAction SilentlyContinue)) { "nfsw gone"; break }
}
"elapsed $([int]((Get-Date) - $t0).TotalSeconds) s"
$l = @(Get-Content $log); if ($l.Count -gt $n0) { $l[$n0..($l.Count - 1)] }
Get-Process nfsw -ErrorAction SilentlyContinue | ForEach-Object { "nfsw running, pid $($_.Id)" }
Get-ChildItem "$env:LOCALAPPDATA\CrashDumps" -ErrorAction SilentlyContinue | Where-Object LastWriteTime -gt $t0 | ForEach-Object { "dump $($_.FullName)" }
