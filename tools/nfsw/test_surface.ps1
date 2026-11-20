# Isolated test (own build tree, scratch folder): Gui's whole edit list saved again with the current writer.
# Added ground must never get the null surface (hash 0), which makes nfsw.exe reset the player car
# (0x85c7c0 -> 0x858a20). verify_surface.py reads the result; verify_table.py checks the section table.
param([string]$Out, [string]$Tree = 'build-reset', [string]$Extra)
$d = "$(Split-Path (Split-Path $PSScriptRoot))\$Tree\bin\Release"
$kept = "$(& $PSScriptRoot\game.ps1)\TRACKS\frs-original"
$log = "$d\FRSWorldEditor.log"
if (Get-Process nfsw -ErrorAction SilentlyContinue) { 'nfsw.exe is running: no scripted editor beside the game'; return }

Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force "$Out\frs-original" | Out-Null
Copy-Item "$kept\L5RA-edits.txt" "$Out\frs-original\"
# -Extra <file>: more edit lines appended (e.g. shipped null ground given a surface, as Set Surface writes it).
if ($Extra) { Add-Content "$Out\frs-original\L5RA-edits.txt" (Get-Content $Extra) }
foreach ($sub in 'textures', 'meshes') { if (Test-Path "$kept\$sub") { Copy-Item "$kept\$sub" "$Out\frs-original\$sub" -Recurse } }

Remove-Item $log -ErrorAction SilentlyContinue
foreach ($v in 'FRS_TEST_EXPORT', 'FRS_TEST_TEXTURE', 'FRS_TEST_COLLISION_ADD', 'FRS_TEST_COLLISION_CUT') { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SurfW {
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
'@
$env:FRS_WRITE_TRACKS = $Out
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(8)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
Start-Sleep -Seconds 6
# Save (menu command 40404), posted: no focus taken, written to $Out by FRS_WRITE_TRACKS.
[SurfW]::PostMessageW((Get-Process -Id $p.Id).MainWindowHandle, 0x0111, [IntPtr]40404, [IntPtr]::Zero) | Out-Null
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if (Select-String -Path $log -Pattern 'NFS World save|save failed|Could not' -Quiet) { break }
    Start-Sleep -Seconds 2
}
Start-Sleep -Seconds 2
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Select-String -Path $log -Pattern 'NFS World edits|NFS World save|ollision|\[error\]' | ForEach-Object { $_.Line }
