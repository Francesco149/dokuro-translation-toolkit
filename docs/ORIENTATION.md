# Dokuro-chan Fan Translation — Orientation

Game: **Game ni Natta yo! Dokuro-chan - Kenkou Shindan Daisakusen** (PS2, serial `SLPM-66185`).
Goal: Russian fan translation. Target for the editing tool: **must run on Windows 7** (translator
is staying on Win7; non-negotiable).

**Read this whole doc first, every fresh session — it's the dense index.** It took a long
investigation to get here and re-deriving it wastes turns. The full detail lives in dedicated docs
this one points to; open those on demand rather than up front, they're long. This doc should stay
short enough to read in full every time; if it starts creeping back up in length, that's a sign
something belongs in a dedicated doc instead.

If you're an AI picking this up fresh: the person you're talking to already knows all of this.
Don't re-explain it back to them, just use it.

## 0. WORKFLOW RULE — headless verification is the DEFAULT (user-mandated)

Always prefer headless screenshots and reproducible headless drives over
desktop screenshots and SendInput automation. The editor exe has two headless
modes (window hidden off-screen; nothing appears on the user's desktop):

- `dokuro-editor.exe --screenshot <out.png> [project]` — pump ~90 frames, save
  one framebuffer dump.
- `dokuro-editor.exe --uitest <SCRIPT.UNI|project.txt> <outdir>` — scripted
  regression through the REAL render loop; framebuffer PNGs in outdir, checks
  in `outdir/uitest.log`. Steps include Click (io-injected mouse), Type/key
  input, tab forcing, document ops, reopen, perf.

1. **Screenshots**: use headless framebuffer dumps (swapchain at client size,
   coordinate-stable). Do NOT capture the desktop (`win/screenshot.ps1`) — it
   grabs the user's whole screen (privacy) and adds window-frame offsets.
   Vision-inspect the PNGs for layout/glyph verification.
2. **Driving the UI**: extend the uitest (io injection: `io.AddMousePosEvent` /
   `AddMouseButtonEvent` / `AddInputCharactersUTF8` / `AddKeyEvent`) instead of
   `win/guiauto.ps1` SendInput. Headless drives are deterministic, don't steal
   focus, don't depend on the Windows session staying unlocked, and re-run in
   CI. `win/guiauto.ps1` + `win/screenshot.ps1` are LAST-RESORT only (e.g. OS
   dialogs that only exist interactively); the Windows session auto-locks
   (~3h), so plan around it.
3. When a UI interaction can't be driven headless, add a small deterministic
   hook or use coordinates recorded from ImGui itself (`GetItemRect*`, like
   the uitest's RU-field click) — never hand-wave with a desktop screenshot.

## 1. Where things stand (as of this writing)

- **Authoritative front = the native C++ code.** `native/` (editor + `core/`
  + host test suite) is the current, authoritative implementation of the
  format handling and tooling. The .NET and Python trees (`dotnet/`,
  `reveng/`) are **POTENTIALLY OUT OF DATE** — they are kept specifically so
  free reverse-engineering efforts can run in the Claude.ai website sandbox
  (plain .NET/Python, no nix toolchain needed). Trust `native/` and
  `FORMAT.md` for current behavior, not the ported code.
- ✅ **Live debugging stack (new since 2026-08-14).** The emulator is now
  fully agent-drivable from WSL: `tools/ps2dbg.py` speaks the DebugServer
  protocol (breakpoints, registers, memory, disasm, stepping, watchpoints)
  over TCP 21512 to a **patched PCSX2 build** (`nix build .#pcsx2-dbg`,
  see `emulator/README.md`) that adds `set_pad`/`clear_pad`/`get_pad`
  (virtual pad input — no window focus needed) and `screenshot` (GS
  framebuffer readback, works headless). Game start sequence: press `start`
  (Enter) a few times to skip videos → title screen → `start` again →
  main menu → `circle` (L) on the first option → first dialogue line.
  See `tools/pcsx2ctl.sh` (launcher), `win/padkeys.ps1` (SendInput
  fallback, focus-stealing — unreliable against SDL, prefer `set_pad`).
- ✅ **Script format fully reverse engineered.** The native C++ engine
  (`native/core/`) parses and rebuilds `SCRIPT.UNI` losslessly (a C# port
  lives in `dotnet/`, potentially out of date). Verified: all 35
  embedded script chunks round-trip byte-for-byte unmodified, and a stress test editing **every
  one of the 36,672 translatable string chunks** with real Cyrillic text rebuilds and re-parses
  cleanly. Full byte-level spec: `FORMAT.md`. This is solid ground to build on.
- ✅ **Native ImGui editor is the primary tool now** (`native/`, C++17 + Dear ImGui + DX11,
  i686 mingw cross, Win7 32/64-bit). Full rewrite of the dialogue editor around the tested
  Core semantics: project folders with original/working copies (+ optional ISO patching),
  drag-to-reorder lines/events with original-vs-custom tracking, revert, add/delete with
  warnings, per-slot sector-padding enforcement (the embedded TOC is rebuilt
  on save — no more size budget), a direct script editor tab with
  live re-assemble, hardened txt export/import, and an op-based undo log that persists
  across sessions with disk pruning. Host test suite: 4282 checks green; the
  exe passes its `--selftest` (18/18) and `--uitest` (55 checks — scripted
  headless regression through the real render loop with framebuffer PNG dumps;
  per §0, headless verification is the default). The only items not covered by
  headless tests are drag-reorder and the Update ISO click (need a human mouse
  / an unlocked Windows session). The superseded WinForms GUI was removed from
  the tree (recoverable from git history); the native `core/` is the
  authoritative implementation of the format semantics (the C# port is
  potentially out of date).
- ✅ **Font system: dialogue-specific scale patch CONFIRMED working.** Three independent hardcoded
  `1.0f` scale constants found and isolated; the dialogue-body one (`0xb8794`, inside
  `AD_AdvMainWinOpen`'s call to `AD_WinTxtStateSet`) was tested in-emulator and confirmed correct
  with no other visible side effects — **this is the patch to ship for font-size mitigation.**
  **IMPORTANT: never hard-patch the ELF — it changes the ELF CRC and breaks PCSX2 game-specific
  patches. Apply runtime patches only** (pnach in `patches/<crc>.pnach`, or
  `ps2dbg.py write_mem`). Full trace, byte-level patch recipe, and all ruled-out candidates:
  `FONT_AND_CRASH_INVESTIGATION.md` Thread A.
  Still have NOT found/edited the actual glyph bitmap source (so no new glyphs, no narrower
  spacing on its own).
- ✅ **Overflow crash root cause CONFIRMED AND FIXED** (Thread B): the
  game's runtime container registry is a byte-for-byte copy of a TOC that
  lives INSIDE SCRIPT.UNI at file offset `0x800`; the size field there is
  used directly as the disc-read length, and our tools used to preserve that
  table verbatim — a grown chunk was read truncated → crash (TLB storm in
  `GetFunctionVariable`) or black screen / softlock depending on where the
  truncation lands. Deterministic registry addresses: chunk 5 (`sure1`)
  entry `0xD4F500`, table base `0xD4F4B0`, `table_base = *(0x365a6c)`.
  **Fixed 2026-08-14: the embedded TOC is rebuilt on save** — the old
  per-chunk budget (`ceil(orig_size/16)*16`) is gone.
- ✅ **Chunk-budget bypass DONE — the size wall is GONE.** The stale size
  came from SCRIPT.UNI's own embedded TOC at file offset `0x800` (inside the
  header region FORMAT.md §1a wrongly called "zero padding"): 35 × 16-byte
  entries `{id, off_sectors, sector_len, size_round16}` that the game copies
  verbatim into its runtime registry at boot and uses as the disc-read
  length. The editor now rebuilds that TOC on save (`uni2_join`), so chunks
  may grow freely — verified in-game: 10+ appended dialogue lines play and
  the scene continues. The editor's Update ISO covers both cases
  automatically: in-place patch within a slot's 0x800 padding, full native
  ISO9660 rebuild when a slot grows past it (fresh image with the path
  tables at LBA 257/258 — the position SONY's cdvdman hardcodes for DVDs,
  the same fix mkisofs rebuilds needed, proven in-game; all other files
  byte-identical). The manual recipes (`tools/iso_path_table_fix.py`,
  `tools/surgical_iso_move.py`) remain for hand-built ISOs. Full story:
  `docs/investigations/2026-08-14-chunk-budget-bypass/`.
- ✅ **Extra dialogue line / box-advance softlock RESOLVED** (Thread C):
  appended 0x118 + 0x119 actions play correctly when the chunk stays in
  budget (proven live) — the earlier "softlock" was the Thread-B truncation
  in another guise. There is no interpreter limit on appended actions.

## 2. Document map

| Doc | What's in it | Open it when... |
|---|---|---|
| `FORMAT.md` | Byte-level spec: UNI2 container (both flavors), STCM2 bytecode (header/actions/params/data chunks/export table), text encoding, in-game opcode/overflow behavior confirmed by crash-testing. | You need an exact field offset/size, or "what does opcode/param-kind X mean". Reference doc, not narrative. |
| `FONT_AND_CRASH_INVESTIGATION.md` | Latest RE state: font scale patches (shipped, runtime-only), overflow-crash root cause (RESOLVED — SCRIPT.UNI's embedded TOC, rebuilt on save), extra-line behavior (RESOLVED, Thread C), key addresses, ruled-out theories. | Continuing the RE / font work. |
| `NEXT_SESSION.md` | ONLY the unfinished work to carry over (currently: the standing polish backlog — sanitize guard chars, opcode audit, font per-line scale). | Starting a session — read this right after this file. |
| `BUILD_AND_TEST.md` | Build/test/sign the native editor (host suite, selftest, uitest, signing); the dotnet Core/TestHarness toolchain notes (reference only). | Building or verifying the editor after a change. |
| `docs/investigations/2026-08-14-chunk-budget-bypass/` | The budget-bypass story: embedded TOC at 0x800 discovery + live proof (in-padding AND past-padding growth), the ISO constraint (rebuilds need the path table at LBA 257 — `tools/iso_path_table_fix.py`; surgical relocation as fallback), recipes. | Re-deriving the TOC mechanism or the ISO rebuild/relocation flow. |
| `docs/investigations/2026-08-crashing-script-sample/` | The confirmed-crashing `SCRIPT.UNI` (chunk grown past its registry size) — ground-truth reproducer for the budget RE. | Re-running the budget / truncation work. |
| `docs/investigations/2026-08-export-table-runtime-corruption/` | Byte-level + disassembly evidence for the confirmed stale-read-size mechanism (Thread B). | Re-deriving the registry math or the patch recipe. |
| `docs/unis_dump.txt` | Raw dump of every UNI container's TOC (gathered during the font hunt). The `SCRIPT.UNI` section IS a real on-disk TOC: its "sectors" column is the embedded table's `sector_len` field (see the chunk-budget-bypass investigation — FORMAT.md §1a's old "no TOC in the file" claim was wrong). | Looking for a specific named sub-resource across the whole ISO. |

**When you finish a session:** fold the result into the current-state docs —
update this file's §1 status bullets and §4 TODOs, move completed work out of
`NEXT_SESSION.md`, and delete anything that was a session log rather than
current state. Docs must stay one unified front: the latest state of the RE and
the project, not a history of findings.

## 3. Tools in this project

| Path | What it is |
|---|---|
| `tools/ps2dbg.py` | **The live-debug client.** Speaks the DebugServer JSON protocol directly (no node/MCP needed): breakpoints, watchpoints, registers, memory R/W, disasm, stepping, backtrace — plus `set_pad`/`clear_pad`/`get_pad` (virtual controller input) and `shot` (headless framebuffer capture → PNG). Requires the patched emulator (`emulator/`). |
| `tools/pcsx2ctl.sh` | Launcher/status for the emulator stack (Windows MCP build via relay, or Linux `pcsx2-dbg` directly). `start`/`status`/`stop`. |
| `win/debugserver-relay.ps1` | Non-admin TCP bridge 0.0.0.0 → 127.0.0.1 so WSL can reach the Windows-side DebugServer (loopback-only). Embedded C# — do not port back to scriptblock pumps (PSInvalidOperation crash). |
| `win/padkeys.ps1` | SendInput pad presses (focus-stealing). **Unreliable against PCSX2's SDL layer — prefer `set_pad` via the patched emulator.** |
| `emulator/` | Patched PCSX2 fork: DebugServer + virtual pad + framebuffer screenshot. `nix build .#pcsx2-dbg`. See `emulator/README.md`. |
| `tools/rebuild_toc.py` | Rebuilds SCRIPT.UNI's embedded per-chunk TOC at file offset 0x800 (the editor's `uni2_join` does this natively; this is the standalone reference/CLI). | Testing or hand-building grown SCRIPT.UNI files. |
| `tools/iso_path_table_fix.py` | Moves an ISO's path table to LBA 257 (0x101) — the position the PS2's cdvdman hardcodes for DVD discs. Makes any mkisofs-style rebuild PS2-compatible; full rebuild recipe in its docstring. | Rebuilding an ISO with a grown SCRIPT.UNI (or any PS2 DVD rebuild). |
| `tools/surgical_iso_move.py` | Fallback for a grown SCRIPT.UNI without a rebuild: relocates it into the trailing free space of the ISO and patches its ISO9660 directory record (both endians). The game resolves SCRIPT.UNI's LBA from the directory at boot, so the move works. | Growing SCRIPT.UNI past its original size without rebuilding the disc. |
| `tools/uni2_split.py`, `tools/uni2_join.py` | Split/rejoin `SCRIPT.UNI` (the UNI2 flavor). Reference implementation / quick CLI use. **Not present in this checkout** (dropped from the last compiled zip) — trivial to recreate from `FORMAT.md` §1a if needed, or see `docs/investigations/2026-08-addr-remap-check/verify_addrs.py` for a working from-scratch Python split implementation. |
| `tools/list_uni_tags.py` | Lists named sub-resources in a TOC-flavor UNI2 file (`SYSTEM.UNI`, `CHARA.UNI`, etc.) Also not present in this checkout — see note above. |
| `dotnet/DokuroScript.Core/` | **POTENTIALLY OUT OF DATE** — .NET engine: `Stcm2.cs` (binary parse/rebuild), `Uni2.cs` (container split/join), `Stcm2Text.cs` (SJIS codec), `Stcm2Project.cs` (project model), `TextDump.cs` (plain-text export/import). Kept so RE experiments run free in the Claude.ai website sandbox; the native `core/` is authoritative. |
| `dotnet/TestHarness/` | **POTENTIALLY OUT OF DATE** — net8.0 console app exercising Core against a real `SCRIPT.UNI`. Same sandbox purpose; the native host test suite (`native/tests/`) is authoritative. |
| `native/` | **The current translator tool**: Dear ImGui + DX11 editor (Win7-compatible i686 PE), engine core (`core/`), direct-script editor, undo log, ISO patching, host test suite, `--selftest`. Build/test/sign via `nix develop --command make -C native [tests|sign|selftest]`. |
| `tools/gen_cp932/` | Generates `native/core/cp932_tables.h` from .NET's cp932 codec (the cp932 byte mapping is a fixed standard — this is about the encoding tables, not the game-format code). Regenerate after touching encoding behavior. |
| `stcm2-asm/` (upstream, in `stcm2-asm-master.zip`) | Third-party Rust disassembler/assembler for the same STCM2 format. Confirmed working against this game's script. NOT used by our tool (see `FORMAT.md` for why we reimplemented instead of shelling out to it), but useful as an independent cross-check / for manual `.asm`-text-level hacking if ever needed. **Building it requires a modern Rust (edition 2024 + let-chains, ~1.88+); binaries built with modern Rust will NOT run on Windows 7** (Rust dropped Win7 support at 1.75/1.76). So it's a dev-machine-only tool (WSL2/Win11), never shipped to the Win7 translator. |
| `reveng/` | **POTENTIALLY OUT OF DATE** — MIPS/DWARF1 reverse-engineering scripts for `SLPM_661.85` itself (the game executable, not the script format): recovers a real function symbol table and enables real EE disassembly. Sandbox-oriented (plain Python, runs in the Claude.ai website sandbox); findings are folded into `FONT_AND_CRASH_INVESTIGATION.md`. See `reveng/README.md`. |
| `docs/investigations/` | Dated, self-contained investigation artifacts (scripts + README) that don't belong in the main tooling but are worth keeping runnable. See §2 above. |

## 4. Open TODOs, roughly in priority order

1. ~~Final in-game check of the relocated-script ISO~~ **DONE (user-verified,
   2026-08-14):** the relocated `/tmp/dokuro-surgical.iso` (grown chunk 5
   past its padding, SCRIPT.UNI at the disc end) played all 15 appended
   test lines and continued into the rest of the chunk's dialogue — the
   budget-bypass proof is complete end-to-end.
2. ~~**Editor: implement full ISO rebuild for grown scripts.**~~ **DONE
   2026-08-14:** Update ISO rebuilds the whole disc image natively when
   SCRIPT.UNI outgrows its original extent — fresh ISO9660 with path tables
   at LBA 257/258 (SONY's hardcoded DVD position), PVD/root/dir records
   re-extented, all other files byte-identical, other files' LBAs resolved
   via the directory records at boot. In-place patch stays the default
   within padding; the busy modal pumps the rebuild chunked. Verified: T12b
   in the host + ASan suites (5455 checks), Win32 PE cross-build links.
3. **Defensive fix, low effort:** add `[`, `]`, and `#` to the editor's
   sanitize guard list (warn or escape). A real unbounded buffer overflow was
   proven in `MkFontPrint`'s `[...]` name-placeholder handling if text ever
   contains an unmatched `[` (FONT doc, ruled-out section).
4. Confirm the `0x118`/`0x11A` opcode assumption (`FORMAT.md` §4) holds across
   ALL 35 script files, not just the ones spot-checked (no contradicting
   evidence yet).
5. Consider whether any OTHER opcodes carry player-facing text we haven't
   classified (everything seen so far fits 0x118/0x119/0x11A).
6. Font, low priority: per-line dialogue scale source (the 0xb8794 patch only
   affects the FIRST line per box) and the glyph-atlas source. Full font swap
   is a separate larger project; the runtime patch sites in Thread A are the
   stepping stone.

**Standing note:** the overflow crash (Thread B) is RESOLVED — the stale size
was SCRIPT.UNI's own embedded TOC at `0x800`, rebuilt on save now; the
"softlock" (Thread C) was the same bug and is RESOLVED. Don't re-debate the
file-vs-runtime contradiction — it was the embedded TOC all along. Chunks grow
freely; Update ISO picks in-place patch (within padding) or full rebuild
(grown file) automatically — no manual tools needed — and the game's loader
is not a constraint.
