# Isolated test (build-test copy, scratch folder, no input, nothing saved) of the Edits tab on NFS World: loads an
# edit list with its kept textures and models, and prints the tab's own title ("Edits in force in L5RA: N...")
# beside what the list holds, by kind. The tab should list every object, texture and model edit of the list.
param([string]$Out, [string]$Kept = "$(& $PSScriptRoot\game.ps1)\TRACKS\frs-original", [switch]$Revert)
# -Revert: also takes every listed row back (FRS_TEST_REVERT), which saves into the scratch folder; the list
# written there should keep only what the tab cannot take back (edited models, collision).
$d = "$(Split-Path (Split-Path $PSScriptRoot))\build-test\bin\Release"
$log = "$d\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class E {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder b, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public static List<string> Texts(uint pid, string starts) {
        var found = new List<string>();
        EnumWindows((top, l) => { uint owner; GetWindowThreadProcessId(top, out owner); if (owner != pid) return true;
            EnumChildWindows(top, (h, l2) => { var t = new StringBuilder(512); SendMessageW(h, 0x000D, (IntPtr)512, t);
                if (t.ToString().StartsWith(starts)) {
                    found.Add(t.ToString()); }
                var c = new StringBuilder(64); GetClassNameW(h, c, 64);
                if (c.ToString() == "SysListView32") found.Add("listview items: " + SendMessageW(h, 0x1004, IntPtr.Zero, IntPtr.Zero));
                return true; }, IntPtr.Zero);
            return true; }, IntPtr.Zero);
        return found;
    }
}
'@
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: not started"; return }
Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force "$Out\frs-original" | Out-Null
Copy-Item "$Kept\L5RA-edits.txt" "$Out\frs-original\L5RA-edits.txt"
foreach ($sub in 'textures', 'meshes') { if (Test-Path "$Kept\$sub") { Copy-Item "$Kept\$sub" "$Out\frs-original\$sub" -Recurse } }
Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item Env:FRS_TEST_TEXTURE, Env:FRS_TEST_EXPORT, Env:FRS_TEST_SAVE -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = $Out
if ($Revert) { $env:FRS_TEST_REVERT = '1' } else { Remove-Item Env:FRS_TEST_REVERT -ErrorAction SilentlyContinue }
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(8)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    $done = if ($Revert) { 'NFS World save|save failed' } else { 'region loaded' }
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern $done -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if ($p.HasExited) { "editor exited: $($p.ExitCode)"; return }
Start-Sleep -Seconds 4
Select-String -Path $log -Pattern 'NFS World edits' | ForEach-Object { $_.Line }
"list by kind: " + ((Get-Content "$Out\frs-original\L5RA-edits.txt" | Group-Object { $_.Substring(0, 1) } | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ' ')
[E]::Texts([uint32]$p.Id, 'Edits in force')
if ($Revert) {
    Select-String -Path $log -Pattern 'edits tab:|NFS World save|save failed' | ForEach-Object { $_.Line }
    "list after:   " + ((Get-Content "$Out\frs-original\L5RA-edits.txt" | Group-Object { $_.Substring(0, 1) } | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ' ')
}
Stop-Process -Id $p.Id -Force
