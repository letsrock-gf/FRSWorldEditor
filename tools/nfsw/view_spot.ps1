# Opens a copy of an editor build in a scratch folder (never Gui's folder, never writing to the game: FRS_WRITE_TRACKS),
# with FRS_DIAGNOSTICS=1 (frs-placements.txt), moves the camera to a spot and captures it.
# Usage: view_spot.ps1 -Out <folder> -X -Y -Z -Yaw -Pitch [-Source <bin folder>]
param([Parameter(Mandatory = $true)][string]$Out, [single]$X, [single]$Y, [single]$Z, [single]$Yaw, [single]$Pitch,
      [string]$Source = 'D:\NFS World\work\nfsw-world-editor\build\bin\Release', [string]$Name = 'spot.png', [int[]]$Commands = @())
if (Get-Process nfsw -ErrorAction SilentlyContinue) { "nfsw.exe is running: not starting the editor"; return }
$tools = $PSScriptRoot
$bin = "$Out\bin"
New-Item -ItemType Directory -Force $bin, "$Out\game", "$Out\tracks" | Out-Null
Copy-Item "$Source\FRSWorldEditor.exe" $bin -Force
function Unlink($path) { if (Test-Path $path) { cmd /c rmdir "$path" | Out-Null } }
Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"
cmd /c mklink /J "$Out\game\TRACKS" "D:\NFS World\Data2015\TRACKS" | Out-Null
cmd /c mklink /J "$Out\game\GLOBAL" "D:\NFS World\Data2015\GLOBAL" | Out-Null
if (-not (Test-Path "$Out\game\nfsw.exe")) { New-Item -ItemType File "$Out\game\nfsw.exe" | Out-Null }
$log = "$bin\FRSWorldEditor.log"
Remove-Item $log -ErrorAction SilentlyContinue
$env:FRS_GAME_FOLDER = "$Out\game"; $env:FRS_WRITE_TRACKS = "$Out\tracks"; $env:FRS_DIAGNOSTICS = '1'
try {
    $p = Start-Process "$bin\FRSWorldEditor.exe" -WorkingDirectory $bin -PassThru
    $deadline = (Get-Date).AddMinutes(8)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }
        Start-Sleep -Seconds 2
    }
    Start-Sleep -Seconds 6
    # A useful size without taking focus from whatever window Gui is in.
    Add-Type -Namespace Vs -Name W -MemberDefinition '[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c); [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint f);' -ErrorAction SilentlyContinue
    $p.Refresh(); $h = $p.MainWindowHandle
    [Vs.W]::ShowWindow($h, 4) | Out-Null                                            # SW_SHOWNOACTIVATE
    [Vs.W]::SetWindowPos($h, [IntPtr]1, 0, 0, 1600, 900, 0x0010) | Out-Null           # HWND_BOTTOM, SWP_NOACTIVATE
    Start-Sleep -Seconds 3
    Add-Type -Namespace Vs -Name P -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);' -ErrorAction SilentlyContinue
    foreach ($c in $Commands) { [Vs.P]::PostMessageW($h, 0x0111, [IntPtr]$c, [IntPtr]::Zero) | Out-Null; Start-Sleep -Seconds 2 }
    & "$tools\cam.ps1" -ProcessId $p.Id -X $X -Y $Y -Z $Z -Yaw $Yaw -Pitch $Pitch -Out "$Out\$Name" -Command 1006
    Start-Sleep -Seconds 3
    Select-String -Path $log -Pattern 'camera \(' | Select-Object -Last 3 | ForEach-Object { "  " + $_.Line }
} finally {
    Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$bin*" } | Stop-Process -Force
    Remove-Item Env:FRS_GAME_FOLDER, Env:FRS_WRITE_TRACKS, Env:FRS_DIAGNOSTICS -ErrorAction SilentlyContinue
    Unlink "$Out\game\TRACKS"; Unlink "$Out\game\GLOBAL"
}
