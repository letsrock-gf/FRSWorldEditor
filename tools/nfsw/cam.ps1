# Moves the editor camera without focus: writes the saved camera and posts "go to saved camera".
# Usage: cam.ps1 <pid> <x> <y> <z> <yaw> <pitch> <out.png> [commandId]
param([int]$ProcessId, [single]$X, [single]$Y, [single]$Z, [single]$Yaw, [single]$Pitch, [string]$Out, [int]$Command = 0)
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Post {
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
'@
$bytes = New-Object byte[] 20
[Buffer]::BlockCopy([single[]]@($X, $Y, $Z, $Yaw, $Pitch), 0, $bytes, 0, 20)
# Only when the key is missing: New-Item -Force on an existing registry key replaces it, which wiped Gui's
# saved view settings, panel sizes and region cameras on 22/11.
if (-not (Test-Path 'HKCU:\Software\FRSWorldEditorNFSW')) { New-Item -Path 'HKCU:\Software\FRSWorldEditorNFSW' | Out-Null }
Set-ItemProperty -Path 'HKCU:\Software\FRSWorldEditorNFSW' -Name Camera -Value $bytes -Type Binary
$h = (Get-Process -Id $ProcessId).MainWindowHandle
[Post]::PostMessageW($h, 0x0111, [IntPtr]$Command, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 2500
& "$PSScriptRoot\shot.ps1" -ProcessId $ProcessId -Out $Out
