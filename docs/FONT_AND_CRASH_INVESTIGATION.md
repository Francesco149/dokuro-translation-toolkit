# Font System, Dialogue-Overflow Crash & Softlock — Current State

> Current RE state as of 2026-08-14: Thread A (font scale) shipped, Thread B
> (overflow crash) root cause CONFIRMED **and FIXED — the "size budget" wall
> is GONE** (the stale size came from SCRIPT.UNI's own embedded TOC at file
> offset 0x800, which is now rebuilt on save; arbitrarily long scripts play
> correctly). Thread C (extra-line softlock) RESOLVED (same root cause).
> Full story: `docs/investigations/2026-08-14-chunk-budget-bypass/`.
>
> **Tooling note:** the emulator is debugger-equipped (`emulator/` +
> `tools/ps2dbg.py`, patched PCSX2 build). Every "live check" below is
> directly executable from WSL — no human-in-the-loop needed.

## Thread A — Font scale patches (SHIPPING state)

**The dialogue-body patch is confirmed and is the one to ship**


| What | vaddr | Instruction | Patch (0.5625×) | Effect |
|---|---|---|---|---|
| Dialogue body scale | `0xb8794` | `lui v0, 0x3f80` | `80 3f` → `10 3f` | Dialogue only; nameplate/menu unchanged. CONFIRMED in emulator. |
| Global (all text) scale | `0x1653b0` | `lui a1, 0x3f80` | `80 3f` → `10 3f` | Nameplates/menus; NOT dialogue (separate scale mechanism). |
| Third site | `0x506b8` | — | — | Nameplate/menu family; independent of the above; stack multiplicatively. |

**Rules (learned the hard way, do not regress):**
- **NEVER hard-patch the ELF.** It changes the ELF CRC → PCSX2 game-specific
  patches/settings stop matching → breaks everything. Runtime patches only:
  pnach in the emulator's `patches/<crc>.pnach`, or debugger writes
  (`ps2dbg.py write_mem <vaddr> <instruction-bytes>`).
- Shrinking the font does **NOT** raise the 47-byte crash threshold — the
  crash is byte-count-based, not width-based . Font scale is
  purely a layout/readability tool.
- **KNOWN LIMITATION (2026-08-14, translator's finding): the dialogue-scale
  patch at `0xb8794` only affects the FIRST line of each dialogue box.** Later
  lines in the same box render at the original scale. The likely cause is that
  the scale is applied once when the box is opened (first line), not per
  line/page — the per-line layout path (`MkFontPrint` 0x164400 / `RunFontSystem`
  0x162de0) is a separate mechanism. Making the font smaller for ALL lines
  needs either finding where later lines get their scale (trace the text-box
  page-advance path), or replacing the font/atlas entirely — deferred to a
  future session. Nameplate/global scale sites are unaffected by this
  limitation (they are not per-line).
- Scale table for the `lui` upper halfword (low 16 bits are `0x0000` for all
  multiples of 1/16 in [0.5, 1.0], so only the upper halfword changes):
  `0x3f80`=1.0, `0x3f70`=0.9375, `0x3f60`=0.875, `0x3f40`=0.75, `0x3f10`=0.5625,
  `0x3f00`=0.5. Cyrillic renders ~1.57× wider than the Latin glyphs it
  piggybacks on (FORMAT.md §4) — ~0.7-0.8 cancels that.
- Glyph atlas source (where master glyph bitmaps live) still unsolved, low
  priority. Font replacement = bigger project, separate thread.

## Font workaround & replacement research (2026-08-14)

**Workaround to fit more text — effectively SOLVED by Threads B/C:** the
"47-byte cap" is a per-chunk size budget (chunk ≤ original size rounded to
16). Text can GROW as long as other lines in the same chunk shrink to
compensate — proven live with `tools/appendaction/` (appended a full
dialogue line while keeping the chunk at budget). Combine with the dialogue
font-scale patch (0xb8794) for layout. The practical translator constraint
is now: per-chunk budget, not per-line length.

**Font replacement path:** the glyph atlas is a runtime-built texture
(DWARF names: `FontTex0_1`, `FontImageRenewal`, `FontImageDat`,
`FontClatImage`). Bitmap source is embedded in the executable (open
question). Replacement approaches, in order of practicality:
1. Runtime glyph patching: once the atlas texture address is pinned (trace
   `FontImageRenewal`/`FontImageDat` at boot), overwrite glyph bitmaps via
   the DebugServer (or a pnach) — no ELF edit, no CRC break.
2. ELF-level: prohibited (CRC) unless the atlas lives in a non-CRC'd region
   (unlikely).
3. Full font swap (new atlas with Cyrillic glyphs): needs (1) + glyph
   re-mapping in the SJIS range — the big project.

**Engine family (STCM2/STCM2L = Otomate/Idea Factory VM script):** confirmed
shared format across titles — Edel Blume (PS2, 2005, same era),
ROOT∞REXX (Vita), AMNESIA World (Switch), Cendrillon palikA (Switch),
ColLar × MalicE (Switch, USA) — per robbie01/stcm2-asm's README; format
spec + RSpec corpus at mchubby/ideaf_script_format. Any font/format
solutions from those games' translation scenes may transfer.

## Thread B — Dialogue overflow crash (47-byte cap)

**✅ ROOT CAUSE CONFIRMED LIVE (2026-08-14) AND FIXED — the stale size came
from SCRIPT.UNI's OWN embedded TOC.** Full capture:

- Clean file live capture: break at `0x1573b8` while the `sure1` scene loads
  → TOC entry `a2=0xD4F500`, size field **`0x7420`** (= clean chunk size
  `0x7418` rounded up to 16 — the table stores sizes padded to 16).
- Crashing file (in-place-patched ISO) live capture: same breakpoint, same
  scene → entry `[id=0x65 off=0x6B sec=0xF size=0x7420]`, read sector
  `a0=0x74380` (= container LBA `0x74313` + 2 + off `0x6B`) — **size still
  `0x7420` while the translated chunk is really `0x7424`** → the game
  disc-reads only `0x7420` bytes → chunk truncated by 4 bytes in RAM.
- The truncated script then crashes exactly where the docs predicted: TLB-miss
  storm at `pc=0x169300` (inside `GetFunctionVariable`, 0x168d70-0x1693b4) →
  PCSX2 itself aborts (CrashHandler).

**Why the size was "stale" (the mystery resolved):** the runtime registry is
a byte-for-byte COPY of a table that lives INSIDE SCRIPT.UNI at file offset
`0x800` — 35 × 16-byte entries `{id, off_sectors, sector_len, size_round16}`
in the header region FORMAT.md §1a wrongly called "zero padding". Our rebuild
tools preserved the header verbatim, so the embedded TOC (and thus the
registry) kept the ORIGINAL sizes forever. Not a runtime bug, not a
savestate artifact — verified on a fresh boot with a grown file: registry
shows the stale size until the TOC is rebuilt, then the correct size.

**✅ THE FIX (file-side, shipped): rebuild the embedded TOC on save.**
`uni2_join()` (native) / `Uni2Script.Join` (C#) recompute `off`/`sector_len`/
`size` from the actual slot layout. Live-verified: a chunk 96 bytes past the
old budget plays its appended dialogue in-game; 10 appended lines play in
sequence and the scene continues; even a chunk grown PAST its sector padding
(SCRIPT.UNI grows a sector) loads correctly when the file is relocated with
`tools/surgical_iso_move.py`. Full story + recipes:
`docs/investigations/2026-08-14-chunk-budget-bypass/`.

**Remaining constraint — the ISO, not the game:** a slot growing past its
0x800 padding grows SCRIPT.UNI's file size, which the in-place ISO patcher
(`tools/patch_iso.py`) cannot hold. The native editor now handles this
automatically (Update ISO does a full rebuild with the path tables at LBA
257/258 when SCRIPT.UNI outgrows its original extent). Manual options for
hand-built ISOs: (1) rebuild with mkisofs and relocate the path table to
LBA 257 (`tools/iso_path_table_fix.py` — the game's cdvdman hardcodes that
position for DVDs; without the fix the IOP TLB-storms at pc=0x4 during
boot), verified booting + playing; or (2) the surgical relocation
(`tools/surgical_iso_move.py` — moves SCRIPT.UNI into the disc-end free
space and patches its directory record), also verified. The game finds
SCRIPT.UNI via the ISO9660 directory record at boot, so no hardcoded LBA is
involved on the game's side.

**Key replay facts:** first dialogue line = scene id 5 / sure1; chunk 5 =
file offset `0x36800`, clean size `0x7418`, translated `0x7424`. The
container registry: `table_base = *(0x365a6c)` = `0xD4F4B0`; container 3 =
SCRIPT.UNI (`cdrom0:\UNION\SCRIPT.UNI`, 35 chunks, LBA `0x74313`); per-chunk
entries are the file's embedded TOC copied verbatim.

## Thread C — Extra dialogue line / box-advance softlock (0x118/0x119)

**RESOLVED: same root cause as Thread B** (size budget), see below. The
earlier "separate investigations" framing predates the live
evidence; the interpreter itself has no append limit.

**Input/attract findings (2026-08-14, patched emulator):** virtual pad input
(`set_pad`) works at interactive screens (main menu → first dialogue scene
confirmed; the scene even crashed on the crashing file = input reached it).
The game does NOT poll the controller during intro videos / the title
attract loop — presses during those do nothing. This is expected game
behavior, not an input bug.

**T6 handler decoded (2026-08-14, live):** on a fresh boot, driving to the
first dialogue line with bps at the T6 dispatch (0x166cd4) and index-4
handler (0x166e40): both fire exactly once per dialogue line, cursor
`680(s1)` = 0 on line 1. Handler disassembly:

```
0x166e40: lui at,0x176; lw v1,-0x2960(at); lw a1,(v1)   # a1 = *g1 (g1=0x1736A0)
          lui at,0x176; lw v1,-0x2958(at); lw v1,(v1)   # v1 = *g2 (g2=0x1736A8)
          bne a1,v1 -> 0x166E78
          lw a1,4(s1); lw v1,0xC(a0); addu v1,a1
          sw v1,4(s1); b -> 0x168D00                    # 4(s1) += action.data[0xC]
0x166e78: lui at,0x176; lw v1,-0x2954(at); sw v1,4(s1)   # else: 4(s1) = *g3 (0x1736AC)
          b -> 0x168D00
```

**✅ RESOLVED (2026-08-14, live): appended actions work when the chunk stays
in budget.** Built a test SCRIPT.UNI with `tools/appendaction/`: appended a
0x118 ("テストの追加台詞！") + 0x119 pair after the first dialogue line of
sure1, shortening two other lines to keep chunk 5 at 0x7418 (≤ the runtime
table's 0x7420 budget). Booted via in-place-patched ISO (menu savestate
route: loadstate slot 1 → circle): line 1 displayed (T6 cursor 0) →
advance → **the appended line displayed** → advance → **the original line 2
followed normally**. No softlock, no black screen. The T6 handler fired only
for the first line (the per-frame state machine handles subsequent lines
without re-dispatching — consistent with the "tokenized action" model).
**Conclusion: the "extra-dialogue-line softlock" is the Thread-B size-budget
bug in another guise** — a chunk grown past its runtime-table size gets read
truncated and the garbage tail manifests as a black screen / softlock (or a
crash, depending on where the truncation lands). Fixing Thread B's budget
(fit chunks ≤ original padded size, or patch the runtime table) resolves
both. The appended pair's 0x119-advance semantics are fine (line → advance →
next line) — the earlier "naive append" failures were budget overruns, not
interpreter chain limits.

Handler interpretation (from the disassembly above): the handler advances
the script pointer (`4(s1)`) by the action's `0xC(a0)` offset while two
pointer globals (0x1736A0/0x1736A8, dereferenced) match; when they diverge
it snaps `4(s1)` to a third global (0x1736AC). Those globals are per-scene
chain bounds — the natural follow-up if the budget fix ever misbehaves.
NOTE: STCM2_Run only runs at scene-start action bursts and on state changes,
NOT per frame, and traces must start from a fresh boot or the menu savestate
route (loadstate → circle) — the interpreter does not re-fire on
mid-scene savestate loads.

Known:
- `STCM2_Run` = 0x1661a0-0x168d6c (~2800 insns), flag-driven cooperative loop
  (flag `-31228(gp)`), not a bounded counter.
- Action cursor = `680(s1)` (offset `0x2A8`), 84-byte stride entries —
  stride confirmed twice, earlier "28" was wrong.
- 8 internal jump tables mapped (bases `0x362e90/60/30/00/db0/d90/d70/d30`);
  the `0x100-0x109` one at `0x166420` confirmed; the dialogue-relevant one is
  the T6 table at `0x166cd4` (bound `<6`) — **index 4 (`0x166E40`) fires for a
  normal dialogue line** (). The adjacent bracket-command string data
  (`Color[`, `Speed[`, ...) means part of the cluster is the inner
  `[...]`-token dispatch — the T6 table is on the real dialogue path anyway.
- `0x118` never appears as a literal in the instruction stream → dispatch is
  table-index based; tracing it live is faster than static.
- On a CRASHING run the code never reaches `0x166200` (dispatch top) — that's
  Thread B's territory, not evidence about this thread's softlock.

## Key addresses (quick reference)

| Address | What |
|---|---|
| `0xb8794` | Dialogue scale `lui v0,0x3f80` (runtime patch site) |
| `0x1653b0` / `0x506b8` | Global + nameplate scale sites |
| `0x164400` | `MkFontPrint` — main text-layout engine |
| `0x162de0` | `RunFontSystem` — per-frame font tick |
| `0x168d70` | `GetFunctionVariable` — crash site (Thread B) |
| `0x1573b8` | `reqUni2ReadCallBack` size read — THE Thread B check |
| `0x1572d0` | `reqUni2ReadCallBack` entry |
| `0x157be0` | `initUni2FileSystem` (registry alloc) |
| `0x1C2F820` | fallback write-watch target (Thread B) |
| `0x1661a0`-`0x168d6c` | `STCM2_Run` (Thread C) |
| `0x166cd4` | T6 jump table (dialogue dispatch, bound `<6`) |
| `0x166e40` | T6 index 4 — normal dialogue line handler |
| `0x166200` | per-action dispatch top (`lw a0,680(s1)`) |
| `0x36d6f0` | `$gp` value (from `.reginfo`) |
| `0x365a6c` | container registry `table_base` (BSS) |

## Ruled-out / do-not-revisit

- `680(a0)` write in `GetFunctionVariable` as crash cause (identical in safe
  & crash runs).
- Glyph-slot pool exhaustion (`GetFreeFONT_WORD_DAT`) — handled gracefully,
  returns 9999 sentinel, not a crash.
- Our tooling corrupting `target_addr` on rebuild — provably correct; the
  file is byte-verified (the crashing sample's file itself is fine; the
  truncation is runtime-side).
- `capstone`/plain `objdump` for EE disasm — silently wrong; use
  `mipsel-linux-gnu-objdump -m mips:5900` (reveng/od.sh).
- `InitFontSystem` (0x1230f8-region helpers) as a size source — it's COLOR.
