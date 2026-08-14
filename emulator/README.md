# Patched PCSX2 for autonomous game debugging

Fork of PCSX2 (v2.6.3, nixpkgs source) with the PS2Recomp-style DebugServer
bridge compiled in, extended with agentic input injection and framebuffer
capture. This replaces the hkmodd/pcsx2-mcp prebuilt for OUR agent workflow:
no window focus needed for input, screenshots work headless, everything goes
over one TCP port (21512) that `tools/ps2dbg.py` speaks natively.

## Why (history, 2026-08-14)

SendInput-based key injection (win/padkeys.ps1) never reached PCSX2's SDL pad
layer even with a verified foreground steal (AttachThreadInput), and the
hkmodd DebugServer has no input commands (verified: full command list in
DebugServer.cpp, plus upstream PINE.cpp — memory/savestates only). So the
DebugServer itself was patched:

- `set_pad` / `clear_pad` / `get_pad` — virtual pad state, applied inside
  `PadDualshock2::GetButtons()`/`GetPressure()` (the exact point the game
  reads the controller via SIO2). Active-low masks, bit layout:
  up=0,right=1,down=2,left=3,triangle=4,circle=5,cross=6,square=7,
  select=8,start=9,l1=10,l2=11,r1=12,r2=13,l3=14,r3=15.
  `hold_ms` auto-releases via a detached timer thread; otherwise the mask
  stays until `clear_pad` (pausing at a breakpoint then pressing works).
- `screenshot` — `MTGS::SaveMemorySnapshot()` (GS framebuffer readback, no
  window needed, works paused or running) returned as base64 BGRA +
  dimensions; `ps2dbg.py shot out.png` decodes to PNG client-side.

## Build

```
nix build .#pcsx2-dbg --print-out-paths
```

The derivation overrides nixpkgs' `pcsx2` (same v2.6.3 source) with
`emulator/patches/0001-debugserver-input-screenshot.patch`. Result is a
`pcsx2-qt` binary in the store. Wrap or symlink it as `pcsx2-dbg`.

## Run

```
pcsx2-dbg <iso>                    # windowed (WSLg); keep it VISIBLE — a human
                                   #   sets the game up (boot, main menu, save
                                   #   state) while the agent drives the rest
tools/pcsx2ctl.sh start            # with PCSX2_EXE=pcsx2-dbg, ISO on Linux side
```

**`--nogui` is NOT a valid CLI flag for this PCSX2 build** (discovered twice:
2026-08-14, 2026-08-14). Passing it makes the process exit 0 silently with no
VM start; `QT_QPA_PLATFORM=offscreen` hangs at GUI init with no DebugServer.
Launch windowed with the ISO path — that is the only working invocation.
The DebugServer port 21512 comes up ~60-120 s after launch, once the VM boots.

Linux PCSX2 keeps its config in `~/.config/PCSX2` (game settings, BIOS, etc.
are independent of the Windows `Documents\PCSX2` used by the MCP build).
BIOS files live in `~/.config/PCSX2/bios` — copy `C:\Users\headpats\Documents\PCSX2\bios\*` there once.

## DebugServer protocol (v1 — hkmodd upstream)

Newline-delimited JSON on TCP 21512, 127.0.0.1 only (no relay needed on
Linux). Commands: status, read_registers, write_register, set_pc,
read_memory, write_memory, read_string, disassemble, evaluate,
set_breakpoint, remove_breakpoint, list_breakpoints, set_memcheck,
remove_memcheck, list_memchecks, pause, resume, step, step_over,
get_threads, get_modules, get_backtrace, is_valid_address,
clear_breakpoints — plus our set_pad/clear_pad/get_pad/screenshot.
Response: `{"ok":true,...}` per line.

## Files

- `patches/0001-debugserver-input-screenshot.patch` — everything:
  DebugServer.cpp/.h added to pcsx2/DebugTools/ (adapted from
  https://github.com/hkmodd/pcsx2-mcp, MIT), virtual-pad hook in
  PadDualshock2.cpp, Start/Stop in VMManager.cpp, CMakeLists entry.
- Upstream reference: `/opt/src/pcsx2-mcp` (hkmodd fork, node MCP server +
  original DebugServer). Not needed at runtime; ps2dbg.py is our client.
