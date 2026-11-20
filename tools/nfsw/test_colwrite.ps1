# Collision save, start to end, without the game and without touching Data2015 (posted messages, no focus):
#   a) deletes collision polygons (FRS_TEST_COLLISION_CUT, the Delete key's own path), saves into <Out>\a and
#      verifies the written file; the writer's self-test runs on all packs in the same start;
#   b) opens the editor again on a copy of that save (<Out>\b): the edit list has to delete them again;
#   c) opens it once more, takes the saved deletions back the way the Edits tab does
#      (FRS_TEST_COLLISION_RESTORE) and saves: the shipped file has to come back.
# Usage: test_colwrite.ps1 -Out <scratch folder> [-Build build-col]
param([string]$Out, [string]$Build = 'build-col')
$tools = $PSScriptRoot
$d = "$(Split-Path (Split-Path $PSScriptRoot))\$Build\bin\Release"
$log = "$d\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ColW {
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
'@
function Start-Editor($tracks, $variables) {
    Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
    Start-Sleep -Seconds 1
    Remove-Item $log -ErrorAction SilentlyContinue
    foreach ($name in 'FRS_TEST_COLLISION_CUT', 'FRS_TEST_COLLISION_RESTORE', 'FRS_WORLD_COLLISION_SELFTEST') {
        Remove-Item "Env:$name" -ErrorAction SilentlyContinue
    }
    $env:FRS_WRITE_TRACKS = $tracks
    foreach ($name in $variables.Keys) { Set-Item "Env:$name" $variables[$name] }
    $started = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
    $deadline = (Get-Date).AddMinutes(6)
    while ((Get-Date) -lt $deadline -and -not $started.HasExited) {
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }
        Start-Sleep -Seconds 2
    }
    Start-Sleep -Seconds 6
    return $started.Id
}
function Send-Command([int]$id, [int]$command, [int]$wait) {
    [ColW]::PostMessageW((Get-Process -Id $id).MainWindowHandle, 0x0111, [IntPtr]$command, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Seconds $wait
}
function Show-Log($pattern) { Select-String -Path $log -Pattern $pattern | ForEach-Object { $_.Line } }

Remove-Item $Out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$Out\a" | Out-Null
$spec = (python "$tools\col_cutspec.py" | Select-Object -First 1)
"spec: $spec"

"=== a) delete and save"
$id = Start-Editor "$Out\a" @{ FRS_TEST_COLLISION_CUT = $spec; FRS_WORLD_COLLISION_SELFTEST = '7' }
Send-Command $id 40404 8                # save (redirected to the scratch folder)
Show-Log 'self-test|collision test|NFS World save|save failed|collision: deleted'
Stop-Process -Id $id -Force
Get-ChildItem "$Out\a" -Recurse -File | ForEach-Object { "{0,10} {1}" -f $_.Length, $_.FullName.Substring($Out.Length + 1) }
Get-Content "$Out\a\frs-original\L5RA-edits.txt" -ErrorAction SilentlyContinue
python "$tools\verify_collision.py" "$Out\a"
Copy-Item "$Out\a" "$Out\b" -Recurse

"=== b) open again on the saved copy"
$id = Start-Editor "$Out\b" @{}
Show-Log 'NFS World edits|collision: \d+ sections'
Send-Command $id 40404 5                # nothing changed: the save has nothing to write
Show-Log 'NFS World save|save failed'
Stop-Process -Id $id -Force
python "$tools\verify_collision.py" "$Out\b"

"=== c) take the saved deletions back and save"
$id = Start-Editor "$Out\b" @{ FRS_TEST_COLLISION_RESTORE = '1' }
Show-Log 'NFS World edits|collision test|collision: \d+ log'
Send-Command $id 40404 8
Show-Log 'NFS World save|save failed'
Stop-Process -Id $id -Force
python "$tools\verify_collision.py" "$Out\b" --restored
Remove-Item Env:FRS_WRITE_TRACKS -ErrorAction SilentlyContinue
