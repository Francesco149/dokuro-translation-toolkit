# win/guiauto.ps1 — generic SendInput mouse/keyboard driver for WSL-driven
# Windows GUI verification (dokuro-editor.exe and its common dialogs).
# Coordinates are screen pixels (virtual screen of this process; matches
# win/screenshot.ps1 captures from the same session).
#
# Usage (powershell -NoProfile -ExecutionPolicy Bypass -File guiauto.ps1 ...):
#   guiauto.ps1 click X Y [-Title "substr"]        left-click (focus first)
#   guiauto.ps1 dblclick X Y [-Title "substr"]
#   guiauto.ps1 rclick X Y
#   guiauto.ps1 drag X1 Y1 X2 Y2 [-Steps 20] [-Hold 10]
#   guiauto.ps1 type "some text"                    Unicode-safe typing
#   guiauto.ps1 key enter|esc|tab|up|down|left|right|home|end|del|backspace|pgup|pgdn|f2
#   guiauto.ps1 key ctrl+s / ctrl+z / ctrl+y / ctrl+a / ctrl+c / ctrl+v / alt+o ...
#   guiauto.ps1 wait 500                            sleep ms
#
# Focus stealing uses the padkeys.ps1 AttachThreadInput trick (works when the
# target window is foreground-capable; reliable for the editor itself).

param()

# Manual arg parsing: -File positional binding is order-sensitive and
# positional ints (e.g. click 160 575) sneak into declared params. Parse
# everything from $args: first token = action, rest = free args, with
# -Title/-Hold/-Steps consumed as named pairs wherever they appear.
$all = @($args)
if ($all.Count -lt 1) { Write-Host "usage: guiauto.ps1 <action> [args...] [-Title sub] [-Hold ms] [-Steps n]"; exit 1 }
$Action = [string]$all[0]
$Title = ""
$Hold = 10
$Steps = 20
$rest = @()
$i = 1
while ($i -lt $all.Count) {
    $a = [string]$all[$i]
    if ($a -eq '-Title' -or $a -eq '-title') { $Title = [string]$all[$i + 1]; $i += 2 }
    elseif ($a -eq '-Hold' -or $a -eq '-hold') { $Hold = [int]$all[$i + 1]; $i += 2 }
    elseif ($a -eq '-Steps' -or $a -eq '-steps') { $Steps = [int]$all[$i + 1]; $i += 2 }
    else { $rest += $a; $i += 1 }
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Threading;

public class GuiInput {
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public InputUnion U; }
    [StructLayout(LayoutKind.Explicit)]
    public struct InputUnion {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT {
        public int dx; public int dy; public uint mouseData; public uint dwFlags;
        public uint time; public IntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }

    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int cb);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x; public int y; }
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    public static IntPtr FindByTitle(string sub) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            var sb = new System.Text.StringBuilder(256);
            GetWindowText(h, sb, 256);
            if (sb.ToString().IndexOf(sub, StringComparison.OrdinalIgnoreCase) >= 0 &&
                IsWindowVisible(h)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);

    public static bool Focus(IntPtr h) {
        if (GetForegroundWindow() == h) return true;
        if (IsIconic(h)) ShowWindow(h, 9);
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

    public delegate bool EnumChildProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumChildProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder sb, int max);

    // Reports screen rects of child Edit / Button controls of the window whose
    // title contains `sub` (for driving common file dialogs).
    public static string ChildRects(string sub) {
        IntPtr h = FindByTitle(sub);
        if (h == IntPtr.Zero) return "window not found: " + sub;
        var sb = new System.Text.StringBuilder();
        EnumChildWindows(h, (ch, l) => {
            var cls = new System.Text.StringBuilder(64);
            GetClassName(ch, cls, 64);
            var txt = new System.Text.StringBuilder(64);
            GetWindowText(ch, txt, 64);
            RECT r; GetWindowRect(ch, out r);
            string s = cls.ToString();
            if (s == "Edit" || s == "Button" || s == "ComboBox" || s == "ComboBoxEx32" ||
                s == "Static" && txt.Length > 0)
                sb.Append(cls + "|'" + txt + "'|" + r.L + "," + r.T + "," + r.R + "," + r.B + "\n");
            return true;
        }, IntPtr.Zero);
        return sb.ToString();
    }

    public static void Mouse(uint flags, int dx, int dy) {
        INPUT i = new INPUT(); i.type = 0;
        i.U.mi.dx = dx; i.U.mi.dy = dy; i.U.mi.mouseData = 0;
        i.U.mi.dwFlags = flags; i.U.mi.time = 0; i.U.mi.dwExtraInfo = IntPtr.Zero;
        SendInput(1, new INPUT[] { i }, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Move(int x, int y) { SetCursorPos(x, y); Thread.Sleep(30); }
    public static void Wheel(int x, int y, int delta) {
        Move(x, y);
        INPUT i = new INPUT(); i.type = 0;
        i.U.mi.dx = 0; i.U.mi.dy = 0;
        i.U.mi.mouseData = (uint)((delta << 16) & 0xFFFF0000);
        i.U.mi.dwFlags = 0x0800; // MOUSEEVENTF_WHEEL
        i.U.mi.time = 0; i.U.mi.dwExtraInfo = IntPtr.Zero;
        SendInput(1, new INPUT[] { i }, Marshal.SizeOf(typeof(INPUT)));
        Thread.Sleep(20);
    }
    public static void Key(ushort vk, bool up) {
        INPUT i = new INPUT(); i.type = 1;
        i.U.ki.wVk = vk; i.U.ki.wScan = 0; i.U.ki.dwFlags = up ? 2u : 0u;
        i.U.ki.time = 0; i.U.ki.dwExtraInfo = IntPtr.Zero;
        SendInput(1, new INPUT[] { i }, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Unicode(char c, bool up) {
        INPUT i = new INPUT(); i.type = 1;
        i.U.ki.wVk = 0; i.U.ki.wScan = (ushort)c; i.U.ki.dwFlags = (up ? 2u : 0u) | 4u; // KEYEVENTF_UNICODE
        i.U.ki.time = 0; i.U.ki.dwExtraInfo = IntPtr.Zero;
        SendInput(1, new INPUT[] { i }, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Tap(ushort vk) { Key(vk, false); Thread.Sleep(20); Key(vk, true); Thread.Sleep(20); }
    public static void TapWithMods(ushort mod, ushort vk) {
        Key(mod, false); Thread.Sleep(15); Key(vk, false); Thread.Sleep(15);
        Key(vk, true); Thread.Sleep(15); Key(mod, true); Thread.Sleep(20);
    }
}
"@

# ---- focus helper (from padkeys.ps1) ----
function Focus-Title([string]$sub) {
    if ($sub -eq "") { return $true }
    $h = [GuiInput]::FindByTitle($sub)
    if ($h -eq [IntPtr]::Zero) { return $false }
    [GuiInput]::Focus($h) | Out-Null
    Start-Sleep -Milliseconds 120
    return $true
}

$action = $Action.ToLowerInvariant()

switch ($action) {
    "click" {
        if ($Title) { Focus-Title $Title }
        $x = [int]$rest[0]; $y = [int]$rest[1]
        [GuiInput]::Move($x, $y)
        [GuiInput]::Mouse(0x0002, 0, 0)  # LEFTDOWN
        Start-Sleep -Milliseconds $Hold
        [GuiInput]::Mouse(0x0004, 0, 0)  # LEFTUP
        Write-Host "click: $x,$y"
    }
    "dblclick" {
        if ($Title) { Focus-Title $Title }
        $x = [int]$rest[0]; $y = [int]$rest[1]
        [GuiInput]::Move($x, $y)
        [GuiInput]::Mouse(0x0002, 0, 0); [GuiInput]::Mouse(0x0004, 0, 0)
        Start-Sleep -Milliseconds 60
        [GuiInput]::Mouse(0x0002, 0, 0); [GuiInput]::Mouse(0x0004, 0, 0)
        Write-Host "dblclick: $x,$y"
    }
    "rclick" {
        if ($Title) { Focus-Title $Title }
        $x = [int]$rest[0]; $y = [int]$rest[1]
        [GuiInput]::Move($x, $y)
        [GuiInput]::Mouse(0x0008, 0, 0)
        Start-Sleep -Milliseconds $Hold
        [GuiInput]::Mouse(0x0010, 0, 0)
        Write-Host "rclick: $x,$y"
    }
    "drag" {
        if ($Title) { Focus-Title $Title }
        $x1 = [int]$rest[0]; $y1 = [int]$rest[1]; $x2 = [int]$rest[2]; $y2 = [int]$rest[3]
        [GuiInput]::Move($x1, $y1)
        Start-Sleep -Milliseconds 60
        [GuiInput]::Mouse(0x0002, 0, 0)
        for ($i = 1; $i -le $Steps; $i++) {
            $cx = $x1 + ($x2 - $x1) * $i / $Steps
            $cy = $y1 + ($y2 - $y1) * $i / $Steps
            [GuiInput]::Move([int]$cx, [int]$cy)
            Start-Sleep -Milliseconds 12
        }
        Start-Sleep -Milliseconds $Hold
        [GuiInput]::Mouse(0x0004, 0, 0)
        Write-Host "drag: $x1,$y1 -> $x2,$y2"
    }
    "type" {
        if ($Title) { Focus-Title $Title }
        $text = $rest -join " "
        foreach ($ch in $text.ToCharArray()) {
            [GuiInput]::Unicode($ch, $false)
            Start-Sleep -Milliseconds 8
            [GuiInput]::Unicode($ch, $true)
            Start-Sleep -Milliseconds 8
        }
        Write-Host "typed: $text"
    }
    "key" {
        if ($Title) { Focus-Title $Title }
        $combo = ($rest -join "").ToLowerInvariant()
        $map = @{
            "enter"="\n"; "esc"="\e"; "tab"="\t"; "space"=" ";
            "up"=0x26; "down"=0x28; "left"=0x25; "right"=0x27;
            "home"=0x24; "end"=0x23; "del"=0x2E; "delete"=0x2E;
            "backspace"=0x08; "pgup"=0x21; "pgdn"=0x22; "f2"=0x71; "f5"=0x74;
        }
        $parts = $combo -split '\+'
        $mods = @{ "ctrl"=0x11; "shift"=0x10; "alt"=0x12 }
        $modVks = @()
        $keyPart = ""
        foreach ($p in $parts) {
            if ($mods.ContainsKey($p)) { $modVks += $mods[$p] } else { $keyPart = $p }
        }
        $vk = 0
        if ($keyPart -eq "enter") { $vk = 0x0D } elseif ($keyPart -eq "esc") { $vk = 0x1B }
        elseif ($keyPart -eq "tab") { $vk = 0x09 } elseif ($keyPart -eq "space") { $vk = 0x20 }
        elseif ($keyPart -eq "a") { $vk = 0x41 } elseif ($keyPart -eq "s") { $vk = 0x53 }
        elseif ($keyPart -eq "z") { $vk = 0x5A } elseif ($keyPart -eq "y") { $vk = 0x59 }
        elseif ($keyPart -eq "c") { $vk = 0x43 } elseif ($keyPart -eq "v") { $vk = 0x56 }
        elseif ($keyPart -eq "x") { $vk = 0x58 } elseif ($keyPart -eq "o") { $vk = 0x4F }
        elseif ($keyPart -eq "f") { $vk = 0x46 } elseif ($keyPart -eq "n") { $vk = 0x4E }
        elseif ($keyPart -eq "w") { $vk = 0x57 } elseif ($keyPart -eq "q") { $vk = 0x51 }
        elseif ($keyPart -eq "r") { $vk = 0x52 } elseif ($keyPart -eq "p") { $vk = 0x50 }
        elseif ($keyPart -eq "t") { $vk = 0x54 } elseif ($keyPart -eq "g") { $vk = 0x47 }
        elseif ($keyPart -eq "h") { $vk = 0x48 } elseif ($keyPart -eq "i") { $vk = 0x49 }
        elseif ($keyPart -eq "j") { $vk = 0x4A } elseif ($keyPart -eq "k") { $vk = 0x4B }
        elseif ($keyPart -eq "l") { $vk = 0x4C } elseif ($keyPart -eq "m") { $vk = 0x4D }
        elseif ($keyPart -eq "d") { $vk = 0x44 } elseif ($keyPart -eq "b") { $vk = 0x42 }
        elseif ($keyPart -eq "e") { $vk = 0x45 } elseif ($keyPart -eq "u") { $vk = 0x55 }
        elseif ($keyPart -eq "0") { $vk = 0x30 } elseif ($keyPart -eq "1") { $vk = 0x31 }
        elseif ($keyPart -eq "2") { $vk = 0x32 } elseif ($keyPart -eq "3") { $vk = 0x33 }
        elseif ($keyPart -eq "4") { $vk = 0x34 } elseif ($keyPart -eq "5") { $vk = 0x35 }
        elseif ($keyPart -eq "6") { $vk = 0x36 } elseif ($keyPart -eq "7") { $vk = 0x37 }
        elseif ($keyPart -eq "8") { $vk = 0x38 } elseif ($keyPart -eq "9") { $vk = 0x39 }
        elseif ($map.ContainsKey($keyPart)) { $vk = $map[$keyPart] }
        if ($vk -eq 0) { Write-Host "unknown key: $combo"; exit 1 }
        if ($modVks.Count -eq 0) { [GuiInput]::Tap($vk) }
        elseif ($modVks.Count -eq 1) { [GuiInput]::TapWithMods($modVks[0], $vk) }
        else { Write-Host "too many mods: $combo"; exit 1 }
        Write-Host "key: $combo"
    }
    "wait" {
        Start-Sleep -Milliseconds ([int]$rest[0])
    }
    "mdown" {
        $x = [int]$rest[0]; $y = [int]$rest[1]
        [GuiInput]::Move($x, $y)
        [GuiInput]::Mouse(0x0002, 0, 0)
        Write-Host "mdown: $x,$y"
    }
    "mmove" {
        $x = [int]$rest[0]; $y = [int]$rest[1]
        [GuiInput]::Move($x, $y)
        Write-Host "mmove: $x,$y"
    }
    "mup" {
        [GuiInput]::Mouse(0x0004, 0, 0)
        Write-Host "mup"
    }
    "wheel" {
        # wheel X Y [notches]  — positive = scroll up, negative = down
        $x = [int]$rest[0]; $y = [int]$rest[1]
        $n = if ($rest.Count -ge 3) { [int]$rest[2] } else { -3 }
        for ($i = 0; $i -lt [Math]::Abs($n); $i++) {
            $dir = if ($n -lt 0) { -120 } else { 120 }
            [GuiInput]::Wheel($x, $y, $dir)
            Start-Sleep -Milliseconds 30
        }
        Write-Host "wheel: $x,$y x$n"
    }
    "focus" {
        $h = [GuiInput]::FindByTitle($Title)
        if ($h -eq [IntPtr]::Zero) { Write-Host "window not found: $Title"; exit 1 }
        [GuiInput]::Focus($h) | Out-Null
        Start-Sleep -Milliseconds 200
        Write-Host "focused: $Title"
    }
    "move" {
        # move [x y]  -> top-left to x,y;  move [x y w h] -> also resize
        $h = [GuiInput]::FindByTitle($Title)
        if ($h -eq [IntPtr]::Zero) { Write-Host "window not found: $Title"; exit 1 }
        $x = [int]$rest[0]; $y = [int]$rest[1]
        $w = 0; $hh = 0; $flags = 0x0015  # SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE
        if ($rest.Count -ge 4) { $w = [int]$rest[2]; $hh = [int]$rest[3]; $flags = 0x0014 } # SWP_NOZORDER|SWP_NOACTIVATE (allows resize)
        [GuiInput]::SetWindowPos($h, [IntPtr]::Zero, $x, $y, $w, $hh, $flags) | Out-Null
        Start-Sleep -Milliseconds 200
        Write-Host "moved: $Title to $x,$y"
    }
    "rect" {
        $h = [GuiInput]::FindByTitle($Title)
        if ($h -eq [IntPtr]::Zero) { Write-Host "window not found: $Title"; exit 1 }
        $r = New-Object GuiInput+RECT
        [GuiInput]::GetWindowRect($h, [ref]$r) | Out-Null
        Write-Host ("rect: " + $r.L + "," + $r.T + " " + $r.R + "," + $r.B + " (" + ($r.R-$r.L) + "x" + ($r.B-$r.T) + ")")
    }
    "childrect" {
        Write-Host ([GuiInput]::ChildRects($Title))
    }
    default { Write-Host "unknown action: $Action"; exit 1 }
}
