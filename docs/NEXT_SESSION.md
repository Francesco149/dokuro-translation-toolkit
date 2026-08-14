# Next-session handoff — budget bypass + editor ISO rebuild DONE

Read `docs/ORIENTATION.md` first (project state, tools, workflow rule). This
file carries ONLY the unfinished work for the next session: nothing else lives
here anymore.

## Repo layout (2026-08-14)

- `public` (GitHub, default branch): single squashed snapshot, **no game
  binaries** (only `game-files/SLPM_661.85.sym`); work continues here. The
  game files are gitignored; `tests/test_main.cpp` degrades to the synthetic
  suite (T6/T7/T12) without `game-files/SCRIPT.UNI`, so CI stays green.
- `master` (local archive): full history incl. the game files and the
  WinForms removal commit. Run the FULL test suite here
  (`cd native && ../build/run_tests`).

## Session delta (2026-08-14): chunk budget bypass + auto ISO rebuild

**The size source was SCRIPT.UNI's own embedded TOC at file offset `0x800`**
(35 × 16-byte entries `{id, off_sectors, sector_len, size_round16}` inside
the header region FORMAT.md §1a wrongly called "zero padding"). The game
copies it verbatim into its runtime registry at boot and uses the `size`
field as the disc-read length. Our tools preserved it verbatim → grown
chunks were read truncated → the "47-byte cap" / budget crash. **Fixed: the TOC is rebuilt on save** (`uni2_join` native +
`Uni2Script.Join` C#, `tools/rebuild_toc.py` reference). Live-verified on
fresh boots: over-budget chunks load, appended dialogue lines play, the
scene continues. Past-padding growth (SCRIPT.UNI grows a sector) works two
ways — a full mkisofs rebuild with the path table relocated to LBA 257
(`tools/iso_path_table_fix.py`) **and** the surgical relocation
(`tools/surgical_iso_move.py`) — both user-verified in-game: all 15
appended test lines played, then the rest of the chunk's dialogue
continued. Full story:
`docs/investigations/2026-08-14-chunk-budget-bypass/`.

**The editor now rebuilds the ISO itself when SCRIPT.UNI grows.**
`iso_rebuild` / `IsoRebuilder` (`native/core/iso.cpp`) write a fresh
ISO9660 image: path tables at LBA 257/258 (SONY's hardcoded DVD position),
PVD + root/dir records re-extented, every other file byte-identical.
`app_begin_iso_patch` picks the in-place patch within padding and the full
rebuild past it; the UI pumps the rebuild chunked behind the busy modal
(`busy_mode 3`). Tested: T12b in the host + ASan suites covers
grow/shrink/missing-file/chunked-ownership; 5455 checks green; Win32 PE
cross-build links.

## Unfinished work

1. Defensive fix, low effort: add `[`, `]`, and `#` to the editor's
   sanitize guard list (warn or escape). A real unbounded buffer overflow
   was proven in `MkFontPrint`'s `[...]` name-placeholder handling if text
   ever contains an unmatched `[` (FONT doc, ruled-out section).
2. Confirm the `0x118`/`0x11A` opcode assumption (`FORMAT.md` §4) holds
   across ALL 35 script files, not just the ones spot-checked (no
   contradicting evidence yet).
3. Consider whether any OTHER opcodes carry player-facing text we haven't
   classified (everything seen so far fits 0x118/0x119/0x11A).
4. Font, low priority: per-line dialogue scale source (the 0xb8794 patch
   only affects the FIRST line per box) and the glyph-atlas source. Full
   font swap is a separate larger project; the runtime patch sites in
   Thread A are the stepping stone.

## Key facts (do not re-derive)

- Embedded TOC: file offset `0x800`, count at `0x8`, entries `{id, off,
  sector_len, size}` — off = (file_off − 0x1000)/0x800, size = round16(len).
- Runtime registry = byte-for-byte copy: base `*(0x365a6c)` = `0xD4F4B0`,
  chunk 5 entry `0xD4F500`, size at `+0xC`. Read length read at `0x1573b8`
  (`lw a1,12(a2)`).
- ISO rules: Update ISO in-place patches within a slot's padding; past it
  (SCRIPT.UNI grows) it does a full native rebuild with the path tables at
  LBA 257/258 — the position SONY's cdvdman hardcodes for DVDs. The game
  resolves SCRIPT.UNI's LBA from the ISO9660 directory record at boot (no
  hardcoded LBA). Manual recipes for hand-built ISOs:
  `tools/iso_path_table_fix.py`, `tools/surgical_iso_move.py`.
- Emulator launch: `pcsx2-qt <iso>` windowed ONLY (`--nogui` is not a real
  flag — see emulator/README.md); a human skips videos to the main menu;
  agent drives the rest via `tools/ps2dbg.py` (set_pad/shot/read_mem).
