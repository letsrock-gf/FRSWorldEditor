# Attaches to a running editor as a debugger and reports where it dies: exception code, fast-fail code and the
# symbol (from the build's PDB) of the faulting address and of the return addresses found on the stack.
# Usage: catch_crash.ps1 -ProcessId <pid> [-Seconds 300]. Detaches when the process exits or the time runs out.
param([int]$ProcessId, [int]$Seconds = 300)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Dbg {
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool DebugActiveProcess(int pid);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool DebugActiveProcessStop(int pid);
    [DllImport("kernel32.dll")] public static extern bool DebugSetProcessKillOnExit(bool k);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool WaitForDebugEvent(byte[] ev, int ms);
    [DllImport("kernel32.dll")] public static extern bool ContinueDebugEvent(int pid, int tid, uint status);
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(uint access, bool inherit, int tid);
    [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr h, ulong addr, byte[] buf, int n, out IntPtr read);
    [DllImport("kernel32.dll")] public static extern bool GetThreadContext(IntPtr h, byte[] ctx);
    [DllImport("dbghelp.dll", CharSet=CharSet.Unicode)] public static extern bool SymInitializeW(IntPtr h, string path, bool invade);
    [DllImport("dbghelp.dll", CharSet=CharSet.Unicode)] public static extern bool SymFromAddrW(IntPtr h, ulong addr, out ulong disp, byte[] info);
    [DllImport("dbghelp.dll")] public static extern uint SymSetOptions(uint o);
    public static string Sym(IntPtr h, ulong a) {
        byte[] info = new byte[88 + 2 * 512];
        BitConverter.GetBytes(88).CopyTo(info, 0);      // SizeOfStruct
        BitConverter.GetBytes(512).CopyTo(info, 80);    // MaxNameLen
        ulong d;
        if (!SymFromAddrW(h, a, out d, info)) return null;
        int len = BitConverter.ToInt32(info, 76);
        return Encoding.Unicode.GetString(info, 84, Math.Min(len, 511) * 2) + "+0x" + d.ToString("X");
    }
}
'@
$deadline = (Get-Date).AddSeconds($Seconds)
if (-not [Dbg]::DebugActiveProcess($ProcessId)) { "attach failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"; return }
[Dbg]::DebugSetProcessKillOnExit($false) | Out-Null
$proc = [Dbg]::OpenProcess(0x1F0FFF, $false, $ProcessId)
[Dbg]::SymSetOptions(0x2 -bor 0x10) | Out-Null   # deferred loads, line info
[Dbg]::SymInitializeW($proc, (Split-Path (Get-Process -Id $ProcessId).Path), $true) | Out-Null
"attached to $ProcessId"
$ev = New-Object byte[] 256
while ((Get-Date) -lt $deadline) {
    if (-not [Dbg]::WaitForDebugEvent($ev, 1000)) { continue }
    $code = [BitConverter]::ToInt32($ev, 0); $pidE = [BitConverter]::ToInt32($ev, 4); $tid = [BitConverter]::ToInt32($ev, 8)
    $cont = 0x00010002   # DBG_CONTINUE
    if ($code -eq 1) {   # EXCEPTION_DEBUG_EVENT: EXCEPTION_RECORD64 at +16 (x64 layout)
        $exc = [BitConverter]::ToUInt32($ev, 16); $addr = [BitConverter]::ToUInt64($ev, 32)
        $first = [BitConverter]::ToUInt32($ev, 16 + 152)
        $p0 = [BitConverter]::ToUInt64($ev, 48)
        if ($exc -ne 0x80000003 -and $exc -ne 0x406D1388 -and $exc -ne 0xE06D7363) {
            "exception 0x{0:X8} at 0x{1:X} {2} first={3} param0=0x{4:X}" -f $exc, $addr, [Dbg]::Sym($proc, $addr), $first, $p0
            if ($exc -eq 0xC0000409 -or $exc -eq 0xC0000005 -or $first -eq 0) {
                # Walk the raw stack for return addresses inside known symbols.
                $th = [Dbg]::OpenThread(0x1FFFFF, $false, $tid)
                $ctx = New-Object byte[] 1232
                [BitConverter]::GetBytes(0x10000B).CopyTo($ctx, 48)   # CONTEXT_FULL, ContextFlags at +0x30
                $handle = [Runtime.InteropServices.GCHandle]::Alloc($ctx, 'Pinned')
                [Dbg]::GetThreadContext($th, $ctx) | Out-Null
                $rsp = [BitConverter]::ToUInt64($ctx, 0x98)
                $buf = New-Object byte[] 0x8000; $read = [IntPtr]::Zero
                [Dbg]::ReadProcessMemory($proc, $rsp, $buf, $buf.Length, [ref]$read) | Out-Null
                $seen = 0
                for ($i = 0; $i -lt $buf.Length -and $seen -lt 25; $i += 8) {
                    $v = [BitConverter]::ToUInt64($buf, $i)
                    if ($v -lt 0x10000) { continue }
                    $s = [Dbg]::Sym($proc, $v)
                    if ($s -and $s -notmatch '^\+') { "   [rsp+0x{0:X}] {1}" -f $i, $s; $seen++ }
                }
                $handle.Free()
                if ($exc -eq 0xC0000409) { break }
            }
            $cont = 0x80010001   # DBG_EXCEPTION_NOT_HANDLED
        }
    } elseif ($code -eq 5) { "process exited"; break }
    [Dbg]::ContinueDebugEvent($pidE, $tid, $cont) | Out-Null
}
[Dbg]::DebugActiveProcessStop($ProcessId) | Out-Null
