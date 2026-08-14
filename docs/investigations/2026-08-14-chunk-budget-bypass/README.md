# Chunk-budget bypass — RESOLVED (2026-08-14)

The per-chunk size budget is **gone**. The wall was a stale table *inside
SCRIPT.UNI itself* — not a runtime bug, not an interpreter limit, not a
savestate artifact. Rebuilding that table on save makes arbitrarily long
scripts play correctly, on a fresh boot, with no debugger involved.

## The discovery

`docs/FORMAT.md` §1a claimed SCRIPT.UNI (the "no-TOC" UNI2 flavor) has "no
offset/length table in the file's own bytes" and that the 4096-byte header
region after magic/count is "zero padding". **That is wrong.** There IS a
per-chunk TOC at file offset `0x800` (the second 2048-byte sector), inside
that supposedly-empty header region:

```
offset  size  field
0x800   4     count? (unused — the real count is at 0x8)
0x800   35 × 16-byte entries, per chunk:
        +0    id        (0,1,2,3,4 for chunks 0-4, then 0x65..0x82 = index+0x60)
        +4    off       sector offset of the chunk from data start: (file_off - 0x1000) / 0x800
        +8    sector_len  padded chunk size in sectors: ceil(len / 0x800)
        +12   size      chunk length rounded up to 16
```

The TOC starts exactly at `0x800`; chunk 5's entry (the `sure1` scene) is at
`0x850`. `tools/unis_dump.txt` actually shows this table (its "sectors"
column = the `sector_len` field) — the dump was misattributed as a computed
walk.

## The mechanism (why the budget wall existed)

At boot the game reads SCRIPT.UNI's sector 2 and copies the TOC verbatim
into its runtime container registry (verified: registry at `0xD4F4B0` is
byte-for-byte identical to the file's `0x800` table, all 560 bytes). When a
scene loads, the game disc-reads the chunk with the TOC's `size` field as
the read length (Thread B: `reqUni2ReadCallBack` at `0x1573b8`,
`lw a1,12(a2)`).

Our rebuild tools preserved the header region **verbatim**, so the embedded
TOC kept the ORIGINAL chunk sizes forever. A translated chunk that grew past
its original size was read truncated → crash / black screen. That was the
whole "47-byte cap" / "size budget" saga.

## The fix (shipped)

Rebuild the embedded TOC whenever SCRIPT.UNI is saved:

- `native/core/uni2.cpp` `uni2_join()` rebuilds it (preserves ids,
  recomputes `off`/`sector_len`/`size` from the actual slot layout).
- `dotnet/DokuroScript.Core/Uni2.cs` `Join()` — same logic (reference impl).
- `tools/rebuild_toc.py` — standalone reference / test harness.
- The editor's budget rail became a *sector-padding* rail (a slot may grow
  freely within its padding; only growth past the padding grows the file,
  which the in-place ISO patcher cannot hold).

## Proof (all live on the patched emulator, fresh boots)

1. **File TOC is the size source.** Grown chunk 5 (0x7418 → 0x7478) with the
   OLD TOC: fresh boot → registry still shows 0x7420 → the stale-size crash
   would follow. (Thread B's "stale size" was never a savestate artifact.)
2. **Rebuilt TOC is read.** Same grown file, TOC rebuilt (chunk 5 size →
   0x7480): fresh boot → registry shows 0x7480.
3. **Over-budget text plays.** The over-budget chunk (96 bytes past the old
   budget) loads; the first dialogue line + an appended line display in-game;
   advancing continues into the original script. No crash, no softlock.
4. **Many lines.** 10 appended dialogue lines (chunk 0x76EC, ~880 bytes over
   the old budget): all 10 display in sequence, then the scene continues into
   the original dialogue.
5. **Past the padding (file growth).** 15 appended lines push chunk 5 to
   0x7868, past its 0x800 padding → SCRIPT.UNI grows by one sector. The
   surgical relocation below boots and registers the grown chunk (size
   0x7870, sector_len 16, later chunks shifted +1 sector — all correct).
6. **Relocated file plays in-game (user-verified).** On the relocated ISO,
   all 15 appended test lines display in sequence and the rest of the
   chunk's original dialogue continues correctly. The line order matched the
   insertion order exactly (11-15 then 1-10 — the appendaction chains each
   inserted after the first dialogue line).
7. **FULL REBUILD works (path table at 0x101).** A mkisofs rebuild of the
   disc (all files relocated) failed until the path table was moved to LBA
   257 — the position SONY's cdvdman hardcodes for DVD. With the fix: boots
   clean (0 TLB misses), registry registers the grown chunk (0x7870, sec
   16), and **user-verified in-game: all 15 appended lines play, then the
   rest of the chunk's dialogue continues.** Proper rebuilds need no
   relocation tricks — just the path-table fix.

## Growing the FILE: the ISO constraint (solved — path table at 0x101)

A chunk growing past its sector padding grows SCRIPT.UNI itself. Then:

- **In-place patch (`tools/patch_iso.py`)**: impossible (new file > original
  extent).
- **Full ISO rebuild (mkisofs)**: **WORKS — with one required fix.** A plain
  mkisofs rebuild fails: the game's IOP cdvdman reads the PATH TABLE at
  HARDCODED LBA **257 (0x101)** for DVD discs (disassembled: the game's
  CDVDMAN.IRX, `li v0, 257` → `0x33fc` read → path-table cache build) — a
  SONY mastering convention. Generic builders put the path table right after
  the volume descriptors (~LBA 19), so cdvdman reads garbage → empty
  directory cache → every file lookup fails → IOP TLB-miss storm during
  boot. **Moving the path table to LBA 257 + fixing the PVD fields fixes it:
  the rebuilt ISO boots clean (0 TLB misses), registers the grown chunk, and
  plays it in-game.** `tools/iso_path_table_fix.py` does the relocation;
  the full recipe is in its docstring. (Not UDF-related: zeroing the
  original's UDF bridge descriptors boots fine.)
- **Surgical relocation (`tools/surgical_iso_move.py`)**: still works and is
  the fallback — keeps the original disc byte-for-byte, writes the grown
  SCRIPT.UNI into the trailing free space (~21 MB free at the disc end),
  patches SCRIPT.UNI's directory record (extent + size, both endians).
  Verified booting + playing.

Practical rule: keep edits within each slot's sector padding (≈ 0x3E8 bytes ≈
15-20 extra lines for the sure1 scene; the editor flags the limit) and the
in-place patch works. For bigger scenes, rebuild the ISO (path table fix) or
use the surgical tool.

## Files

- `tools/rebuild_toc.py` — rebuild the embedded TOC (reference impl).
- `tools/iso_path_table_fix.py` — move an ISO's path table to LBA 257
  (makes any rebuild PS2-DVD-compatible; full rebuild recipe in its
  docstring).
- `tools/surgical_iso_move.py` — fallback: relocate a grown SCRIPT.UNI in
  the ISO without rebuilding.
- Test ISOs (this session): `/tmp/grown1_fixed.SCRIPT.UNI` (1 appended line,
  in-padding), `/tmp/stress_fixed.SCRIPT.UNI` (10 appended lines),
  `/tmp/stress2_fixed.SCRIPT.UNI` (15 appended lines, past padding),
  `/tmp/dokuro-surgical.iso` (stress2 relocated; boots),
  `/tmp/rebuild-pt257.iso` (stress2 in a full mkisofs rebuild + path-table
  fix; boots and plays).

## Key addresses (unchanged)

| Address | What |
|---|---|
| `0x800` | embedded TOC in SCRIPT.UNI (file offset) |
| `0xD4F4B0` | runtime registry = copy of the embedded TOC |
| `0xD4F500` | registry entry for chunk 5 (`sure1`), size at +0xC |
| `0x1573b8` | `reqUni2ReadCallBack` — `lw a1,12(a2)` = the size read |
