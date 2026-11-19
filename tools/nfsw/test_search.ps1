# Isolated test (build-test copy, scratch folder, nothing saved) of the texture browser's search field: opens the
# browser, types names into the field and prints, for each, the browser's title, the rows listed and the name of
# the texture left selected. A name in another case should list the same rows; a name no texture has, none.
param([string]$Out, [string[]]$Names = @('', 'road', 'ROAD', 'zzqqzz', ''), [int]$WaitMinutes = 0)
# -WaitMinutes: waits that long for nfsw.exe to stay closed for 60 s before starting, instead of giving up at once.
$d = 'D:\NFS World\work\nfsw-world-editor\build-test\bin\Release'
$log = "$d\FRSWorldEditor.log"
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class S {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, StringBuilder l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, string l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public static string Text(IntPtr h) { var t = new StringBuilder(512); SendMessageW(h, 0x000D, (IntPtr)512, t); return t.ToString(); }
    public static IntPtr Browser(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((top, l) => { uint owner; GetWindowThreadProcessId(top, out owner);
            if (owner == pid && Text(top).StartsWith("Texture browser")) found = top;
            return true; }, IntPtr.Zero);
        return found;
    }
}
'@
$deadline = (Get-Date).AddMinutes($WaitMinutes)
$closedSince = $null
while ($true) {
    if (Get-Process nfsw -ErrorAction SilentlyContinue) { $closedSince = $null }
    elseif (-not $closedSince) { $closedSince = Get-Date }
    if ($WaitMinutes -eq 0 -and $closedSince) { break }
    if ($closedSince -and ((Get-Date) - $closedSince).TotalSeconds -ge 60) { break }
    if ((Get-Date) -ge $deadline) { "nfsw.exe is running: not started"; return }
    Start-Sleep -Seconds 5
}
Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
New-Item -ItemType Directory -Force $Out | Out-Null
Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item Env:FRS_TEST_TEXTURE, Env:FRS_TEST_EXPORT, Env:FRS_TEST_SAVE, Env:FRS_TEST_REVERT -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = $Out
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(8)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'region loaded' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
if ($p.HasExited) { "editor exited: $($p.ExitCode)"; return }
Start-Sleep -Seconds 4
$p.Refresh()
[S]::PostMessageW($p.MainWindowHandle, 0x0111, [IntPtr]40209, [IntPtr]::Zero) | Out-Null   # View > Texture Browser
$browser = [IntPtr]::Zero
for ($i = 0; $i -lt 60 -and $browser -eq [IntPtr]::Zero; ++$i) { Start-Sleep -Seconds 1; $browser = [S]::Browser([uint32]$p.Id) }
if ($browser -eq [IntPtr]::Zero) { "texture browser did not open"; Stop-Process -Id $p.Id -Force; return }
$search = [S]::GetDlgItem($browser, 2108)
if ($search -eq [IntPtr]::Zero) { "no search field in the browser"; Stop-Process -Id $p.Id -Force; return }
foreach ($name in $Names) {
    [S]::SendMessageW($search, 0x000C, [IntPtr]::Zero, [string]$name) | Out-Null   # WM_SETTEXT: the field tells the dialog
    Start-Sleep -Seconds 2
    $rows = [S]::SendMessageW([S]::GetDlgItem($browser, 2101), 0x1004, [IntPtr]::Zero, [IntPtr]::Zero)
    $chosen = ([S]::Text([S]::GetDlgItem($browser, 2103)) -split "`n")[0]
    "'$name': rows=$rows chosen='$chosen' title='$([S]::Text($browser))'"
}
Stop-Process -Id $p.Id -Force
