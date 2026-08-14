# padkeys.ps1 — send PS2 pad button presses to PCSX2 via SendInput.
#
# PCSX2's pad uses SDL keyboard handling, so WM_POST-style messages don't work;
# SendInput injects at the OS input stream level and behaves like a real key.
# Foreground activation from a background process (WSL-spawned PowerShell)
# fails with plain SetForegroundWindow — AttachThreadInput + BringWindowToTop
# is the reliable steal. The window handle is verified before sending.
#
# Usage:
#   padkeys.ps1 enter              # Start (skip videos / confirm)
#   padkeys.ps1 l                  # Circle  (K=Cross J=Square I=Triangle)
#   padkeys.ps1 up,enter,l         # sequence, comma-separated
#   padkeys.ps1 enter --hold 150   # hold each key 150ms (default 80)
#   padkeys.ps1 --nofocus          # skip focusing the PCSX2 window
#
# Key map (Documents/PCSX2/inis/PCSX2.ini [Pad1]):
#   Start=Enter  Select=Backspace  Triangle=I  Circle=L  Cross=K  Square=J

param(
    [Parameter(Mandatory=$true)][string]$Keys,
    [int]$Hold = 80,
    [switch]$NoFocus
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PadInput {
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public InputUnion U; }
    [StructLayout(LayoutKind.Explicit)]
    public struct InputUnion { [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }

    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int cb);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);

    public static bool Focus(IntPtr h) {
        if (GetForegroundWindow() == h) return true;
        if (IsIconic(h)) ShowWindow(h, 9); // SW_RESTORE
        uint myTid = GetCurrentThreadId();
        uint tTid, tPid, fTid, fPid;
        tTid = GetWindowThreadProcessId(h, out tPid);
        IntPtr fg = GetForegroundWindow();
        fTid = GetWindowThreadProcessId(fg, out fPid);
        bool ok = false;
        if (AttachThreadInput(myTid, tTid, true)) {
            if (fTid != 0 && fTid != tTid) AttachThreadInput(myTid, fTid, true);
            BringWindowToTop(h);
            ok = SetForegroundWindow(h);
            if (fTid != 0 && fTid != tTid) AttachThreadInput(myTid, fTid, false);
            AttachThreadInput(myTid, tTid, false);
        }
        return ok;
    }

    public static void Key(ushort vk, bool up) {
        INPUT i = new INPUT();
        i.type = 1;
        i.U.ki.wVk = vk;
        i.U.ki.dwFlags = up ? 2u : 0u;
        SendInput(1, new INPUT[] { i }, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@

$vkMap = @{
    'enter' = 0x0D; 'return' = 0x0D; 'start' = 0x0D
    'backspace' = 0x08; 'select' = 0x08
    'l' = 0x4C; 'circle' = 0x4C
    'k' = 0x4B; 'cross' = 0x4B
    'j' = 0x4A; 'square' = 0x4A
    'i' = 0x49; 'triangle' = 0x49
    'up' = 0x26; 'down' = 0x28; 'left' = 0x25; 'right' = 0x27
    'space' = 0x20
    'f1' = 0x70; 'f2' = 0x71; 'f3' = 0x72; 'f4' = 0x73
    'f5' = 0x74; 'f6' = 0x75; 'f7' = 0x76; 'f8' = 0x77
    'f9' = 0x78; 'f10' = 0x79; 'f11' = 0x7A; 'f12' = 0x7B
}

if (-not $NoFocus) {
    $proc = Get-Process pcsx2-qt -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $proc) { Write-Error "pcsx2-qt not running"; exit 1 }
    $h = $proc.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { Write-Error "no pcsx2 window handle"; exit 1 }
    $focused = $false
    for ($try = 0; $try -lt 3 -and -not $focused; $try++) {
        [PadInput]::Focus($h) | Out-Null
        Start-Sleep -Milliseconds 300
        $focused = ([PadInput]::GetForegroundWindow() -eq $h)
    }
    if (-not $focused) { Write-Warning "could not focus pcsx2 window; sending anyway" }
}

foreach ($key in $Keys -split ',') {
    $key = $key.Trim().ToLower()
    $vk = $vkMap[$key]
    if (-not $vk -and $key -match '^0x[0-9a-f]+$') { $vk = [Convert]::ToUInt16($key, 16) }
    if (-not $vk) { Write-Error "unknown key: $key"; exit 2 }
    [PadInput]::Key($vk, $false) | Out-Null
    Start-Sleep -Milliseconds $Hold
    [PadInput]::Key($vk, $true) | Out-Null
    Start-Sleep -Milliseconds 60
}
Write-Host "sent: $Keys"
