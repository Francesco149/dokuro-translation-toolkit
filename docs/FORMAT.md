# STCM2/UNI2 File Format Reference

Detailed byte-level format spec for the container and script bytecode formats this project
reverse engineered. Pulled out of the main orientation doc to keep that one skimmable — this
is the reference to open when you need exact field offsets/sizes, not a narrative.

## 1. The container format(s): UNI2

Every `UNION\*.UNI` file starts `UNI2`. There are **two different flavors** depending on which
file:

### 1a. No-TOC flavor (SCRIPT.UNI only, as far as we've seen)
Header: magic(4) + u32 unk(seen `0x00010000`) + u32 count, then — contrary to what this section
claimed until 2026-08-14 — the 4096-byte header region is NOT zero padding: it holds a
**per-chunk TOC at file offset `0x800`**:

```
0x800  count × 16-byte entries, one per chunk (35 for SCRIPT.UNI):
       +0   id          (0..4 for the first chunks, then 0x65..0x82 = index + 0x60)
       +4   off         sector offset from data start: (file_off - 0x1000) / 0x800
       +8   sector_len  padded chunk size in sectors: ceil(len / 0x800)
       +12  size        chunk length rounded up to 16
```

The game copies this table verbatim into its runtime container registry at
boot and uses the `size` field as the disc-read length for each chunk
(Thread B; registry base `*(0x365a6c)`, see
`FONT_AND_CRASH_INVESTIGATION.md`). The 35 `STCM2` blobs themselves are
concatenated back to back after the 0x1000-byte header, each padded up to the
next 0x800 (2048-byte) sector boundary.

**Tooling implication: the embedded TOC MUST be rebuilt whenever a slot's
size changes** — `uni2_join()` (native) / `Uni2Script.Join` (C#) now do this
automatically (preserve ids, recompute off/sector_len/size from the actual
layout). Without it, a grown chunk is read truncated → crash (the old
"47-byte cap"). `tools/rebuild_toc.py` is the standalone reference.

### 1b. TOC flavor (SYSTEM.UNI, CHARA.UNI, ETC.UNI, RUN.UNI, MINI.UNI, etc.)
Same 4-byte magic + count header, but there IS a table: at file offset `0x800`, `count` entries
of `(index:u32, start_sector:u32, length_sectors:u32, flags:u32)`, all still in 2048-byte sector
units, with actual resource data starting at file offset `0x1000`. Sub-resources are tagged
`ART2` (graphics) or occasionally other magics (`TIM2` in `MEMORY.UNI`, `GLOB`/`IECS` in
`SOUND.UNI` — not investigated). `list_uni_tags.py` dumps these tables; see `docs/unis_dump.txt`
for the full listing across every UNI file in the ISO (gathered during the font hunt, see `FONT_AND_CRASH_INVESTIGATION.md`).

## 2. The STCM2 script bytecode format

This is the payload inside each `SCRIPT.UNI` slot. One file = one scene/chunk of dialogue script.
Tag inside every one reads `" File Make By Minku 06.0"` — this is a known third-party toolchain
("Minku") that `stcm2-asm` already targets, which is why that tool works here unmodified.

All addresses in this format are **plain absolute byte offsets from the start of the STCM2 blob**
(offset 0 = the `S` of `STCM2`). There is no separate address space translation.

### 2.1 Header (fixed 92 bytes = `GlobalDataOffset` constant)
```
offset  size  field
0       5     magic "STCM2"
5       27    tag (ASCII, null-padded)
32      4     export_addr  (u32 LE) - filled in AFTER export table position is known; see 2.5
36      4     export_len   (u32 LE) - count of export table entries
40      4     unk1         - preserved verbatim, meaning unknown
44      4     collection_addr - preserved verbatim, meaning unknown
48      32    unk32        - preserved verbatim, meaning unknown
80      12    "GLOBAL_DATA\0" magic
92      ...   global_data starts here (this offset = GlobalDataOffset constant, sanity-checked on parse)
```

### 2.2 global_data
Opaque blob, length determined by scanning forward in 4-byte steps until the bytes
`"CODE_START_\0"` (12 bytes) are found. We never interpret global_data's contents — copied
verbatim on rebuild. Pointers into it ("GlobalDataPointer" parameters) are encoded as absolute
addresses in range `[92, 92+len(global_data))` — i.e. as if global_data conceptually starts at
file offset 92, which it does.

### 2.3 Actions (the actual bytecode, back to back after `CODE_START_\0`)
Each action:
```
offset from action start   size   field
0                           4     global_call (u32): 0 = normal, 1 = call (opcode field is
                                  actually a target action ADDRESS in this case, not a real opcode)
4                           4     opcode (u32) - e.g. 0x118 = dialogue line, 0x119 = box-advance
                                  boundary (no params), 0x11A = nameplate name. See §4 below.
8                           4     nparams (u32)
12                          4     length (u32) - TOTAL size of this action incl. this header
16                          12*nparams   params (see 2.4)
16+12*nparams               length-16-12*nparams   data (see 2.5 chunk format)
```
Actions are simply laid out sequentially — no padding between them. Their addresses (used for
`ActionRef`/`call` target resolution and export table entries) are just their file position.
**Rebuilding requires recomputing every subsequent action's address whenever an earlier one
changes size** (see `Stcm2File.Serialize()` — it does a full linear layout pass).

### 2.4 Parameters (12 bytes = 3× u32 LE each)
Four kinds, disambiguated purely from the 3-word pattern (see `Param.Parse` in `Stcm2.cs`):
- **ActionRef**: `[0xffffff41, target_addr, 0x40000000|0xff000000]` — a jump/reference to another
  action's address. Must be remapped on rebuild if the target moved.
- **DataPointer**: `[addr, tag, tag]` where `addr` falls inside this action's own trailing `data`
  blob → `addr - data_start` is the offset within `data` where a self-describing chunk lives (2.5).
- **GlobalDataPointer**: `[addr, tag, tag]` where `addr` falls inside `[92, 92+global_data_len)`.
- **Value**: anything else with a valid `tag` — a plain inline literal (numbers, flags).
  (`tag` is always `0x40000000` or `0xff000000` — both seen, meaning unclear, preserved as-is.)

### 2.5 Data chunks (what DataPointer params point at) — this is where the actual TEXT lives
Self-describing "boxed value", found by scanning: try to parse a chunk header at the current
position; on success, the *next* chunk (if any) MUST start immediately (byte 0 of what's left) —
only the very first chunk in a data blob may have leading junk bytes before it. This exactly
mirrors what `stcm2-asm`'s disassembler does; ported faithfully in `ScanDataChunks`/
`TryDecodeChunk` in `Stcm2.cs`.
```
offset  size  field
0       4     type (u32): 0 or 1
4       4     qlen (u32): len/4
8       4     magic, must be 1
12      4     len (u32): must equal qlen*4
16      len   content
```
Classification of `content` (mirrors `stcm2-asm`'s `decode_string`/`four_byte_heuristic`):
- `type==1 && len==4` → always a packed **u32 number**, never text.
- `type==0 && len==4` → ambiguous 4-byte slot; heuristic: strip trailing zero bytes, and if what's
  left is ≥3 bytes, isn't the literal `"op"`, decodes as valid Shift-JIS, and has no control
  characters → it's **text**. Otherwise it's a **u32 number**.
- `type==0 && len!=4` → always **text**, with 1–4 trailing zero bytes as padding (must decode
  cleanly after trimming those).

**We only ever touch chunks classified as text, and only the ones belonging to dialogue (0x118)
or nameplate (0x11A) actions** (§4 below). Numbers/flags and non-dialogue text (e.g. background
filenames like `"bg38_a"` which also happens to be a text-classified chunk!) are left completely
untouched — this is why the tool filters by the *owning action's opcode*, not just "is this a
string", or it would expose internal identifiers as if they were translatable dialogue.

### 2.6 Export table
After the last action: `"EXPORT_DATA\0"` magic, then `export_len` entries of
`(zero:u32, name:32 bytes, target_addr:u32)`. `export_addr` in the header equals the file offset
of the first entry (i.e. right after the magic). Purpose of exports not fully explored (probably
scene entry points referenced from elsewhere/other files) — we preserve the list and just remap
`target_addr` through the same old-addr→new-addr table used for ActionRef, in original order.

**🔶 This field is the center of the dialogue-overflow-crash investigation — see
`FONT_AND_CRASH_INVESTIGATION.md` §3 for the full story.** Live in-emulator tracing found the
crash comes from a bad `target_addr` read here (`STCM2_GetExportPointer` does nothing but
`script_base + *(matched_entry + 36)` — no runtime computation, so a wrong value here means it was
already wrong in the file). The natural suspect was our own remap-on-rebuild logic (same
old-addr→new-addr table used for `ActionRef`, see 2.4) not accounting for the byte-shift a longer
translated line introduces. **That specific theory was checked directly against `Serialize()` and
does NOT hold up** — the remap table is rebuilt fresh from real positions on every call and
survived 383 stress tests at the exact 47/48-SJIS-byte crash boundary with zero mismatches. See
`FONT_AND_CRASH_INVESTIGATION.md` §3 (search "6.3l") for the full reasoning, the ruled-out
alternate theory (magic-byte collision in `Uni2Script.Split()`), and the concrete next step.

### Why we reimplemented this in C# instead of shelling out to stcm2-asm
Two reasons: (1) Win7 can't run a binary built with the modern Rust this repo requires (see the
tools table above), so the *shipped* Win7 tool structurally cannot depend on it. (2) Working
directly on the parsed binary model (rather than round-tripping through stcm2-asm's human-
readable `.asm` text format) let us skip reimplementing a full text grammar/assembler and just
do targeted string substitution + relayout, which is simpler and was fully verified in this repo
(see `BUILD_AND_TEST.md`) — whereas we have no way to test-compile/run stcm2-asm itself in most of
our dev environments here.

## 3. Text encoding: Cyrillic already works, by a lucky coincidence

**Shift-JIS (Windows codepage 932, `cp932`) natively includes the full Cyrillic alphabet**
(JIS X 0208 row 7: А-Я, а-я, **including Ё**) at bytes `0x84xx`. Verified byte-identical between
Python's `str.encode('shift_jis')` and .NET's `Encoding.GetEncoding(932)` for the same Cyrillic
string — this is a real standard, not a hack.

This means: **no custom character mapping/codec table is needed.** A translator can type
Cyrillic directly; `Stcm2Text.EncodeSjis` handles it via the OS's normal cp932 support (built
into Windows natively — no extra package needed on the shipped net48 app; on Linux/.NET Core dev
environments you need `System.Text.CodePagesEncodingProvider` registered first, see
`Stcm2Text.cs`'s static constructor).

**This is a coincidence about the character encoding, not a font solution.** The bytes decode
correctly and *some* pre-existing glyph gets displayed for them — but per `FONT_AND_CRASH_INVESTIGATION.md`, that glyph's
shape/width is whatever the original artist drew for that barely-used JIS row, and we don't yet
know how to change it or add missing glyphs (e.g. guillemets «», curly quotes „", etc. — not
confirmed present).

### 3a. Sanitization + validation layer (added after a real bug: pasted NBSP crashed Save)
Real-world text (pasted from a browser/Word/chat) routinely contains characters that LOOK like
plain punctuation but aren't in cp932 - non-breaking spaces, em/en dashes, curly low quotes,
guillemets, zero-width spaces, BOM, etc. The strict cp932 encoder throws on these, and letting
that raw .NET exception surface (as it originally did) produces a useless error naming a byte
index with no indication of *which line* or *which character* caused it.

Fixed with two layers in `Stcm2Text.cs` / `Stcm2Project.cs`:
- `Stcm2Text.Sanitize()` runs on every `SetTranslation` call (both grid edits and text-dump
  import - single choke point) and silently normalizes the common cases (NBSP→space, em/en
  dash→JIS horizontal bar U+2015, guillemets/low-quote→straight quote, zero-width chars dropped,
  etc. - see the switch statement for the full, tested-against-real-cp932 list). The translator
  never sees this happen for the common cases.
- Whatever's left (genuinely unencodable, e.g. Hangul/other-script paste-in accidents) is caught
  by `Stcm2Project.Validate()` / `TryEncodeSjis`, which `Save()` now calls FIRST, before touching
  any file, collecting **every** bad entry (not just the first) with exact file/kind/text/
  character/position, thrown as `Stcm2EncodingException`. Affected rows are also live-highlighted
  red in the editor grid.

If a new "smart punctuation" bug report comes in, the fix is almost always: add one more `case`
to `Stcm2Text.Sanitize`'s switch statement, after confirming the intended replacement actually
encodes under cp932 (don't guess - test it, e.g. via Python `char.encode('shift_jis')`, several
plausible-looking substitutes silently fail too, like plain em/en dash themselves).

**Correction (2026-08-14):** the old `U+FF5E → U+301C` case was WRONG. Windows cp932 maps byte
`0x8160` to **U+FF5E** (FULLWIDTH TILDE); U+301C (WAVE DASH) is **not encodable** in .NET's
cp932. The sanitizer's own output therefore failed `TryEncodeSjis` and flagged every pasted `～`
as a bad character. Fixed in both the C# core and the native port: U+FF5E is now left as-is.
Verified against .NET `Encoding.GetEncoding(932)` with ExceptionFallback (2026-08-14).




## 4. In-game behavior found via manual crash-testing (hex-editing + PCSX2)

These directly shaped the tool's safety rails — **do not relax them without re-testing in an
emulator**:

- **Dialogue box (opcode `0x118`) overflow CRASHES the game.** Confirmed empirically. The tool
  must never let a translator save text past the limit without an explicit, scary warning.
- English fits ~36 characters: `"hello world test this text is longer"` barely fit.
- Russian fits fewer: `"Меня зовут Сакура Кусак"` (23 chars) fit — i.e. **Cyrillic glyphs render
  ~1.57× wider than Latin ones** in this font (36/23). `OverflowChecker` in `Stcm2Project.cs`
  encodes this ratio as a per-character weight heuristic. **This is an approximation pending real
  glyph metrics from `FONT_AND_CRASH_INVESTIGATION.md`** — good enough to flag risk, not a substitute for testing
  borderline lines in an emulator.
- **Adding a new `0x118` action (extra line) does NOT crash but SOFTLOCKS** (black screen, "auto"
  icon shows in corner on Start press → game is alive but stuck). Same result adding a `0x119`
  (box-advance) marker too. **Conclusion: the number of dialogue-line/box-advance actions per
  scene is hardcoded/expected elsewhere and must never change.** The tool must only ever edit
  TEXT within existing actions, never add/remove/reorder actions. (`Stcm2File.Serialize()` as
  written can't add actions anyway — it only re-lays-out the existing list — but be careful if
  this is ever extended.)
- **Nameplate (`0x11A`) overflow does NOT crash** — extra text past ~22 characters (English:
  `"this is a very long na"`) is silently truncated instead. Lower risk than dialogue overflow,
  still flagged by the tool but not a blocker.

## 5. The native editor's asm text format (`native/core/asmfmt.cpp`)

The direct script editor tab works on a human-editable text representation of one STCM2 slot.
Grammar (one statement per line, `;` comments):

```
.stcm2 "<tag27>"                    required first statement
.unk1 <hex>  .collection <hex>  .unk32 <hex>×8
.global_data / .bytes <hex>… / .end
.code_start
action @<hex>                       address = the annotation from the disassembly
  call 0|1
  opcode <hex>                      (call=0)
  target <hex>                      (call=1: target action address)
  param action_ref|data_ptr|global_ptr|value <hex>
  data
    chunk text "<escaped>"          type-0 text; content = sjis(text) + 1..4 zero pad
    chunk text_raw <hex>…           type-0 text kept as exact bytes (non-round-trippable content)
    chunk num <hex>                 type-1 u32
    chunk val <hex>                 type-0 u32 numeric slot
    chunk raw <hex>…                type-0 unclassified bytes
    bytes <hex>…                    raw data bytes (leading junk / unparsed tail)
  end_action
exports
  export "<name32>" @<hex>
.end
```

**Round-trip contract:** `disasm → assemble → serialize` reproduces the original serialize()
output byte-for-byte for every slot of the real SCRIPT.UNI (verified by the host test suite).
Text chunks whose content cannot be losslessly re-encoded (invalid cp932 bytes, 4-byte text
without zero padding) are emitted as `chunk text_raw` so nothing is silently corrupted.

**References are symbolic through the annotations:** `action @0x…` lines carry the addresses
from the disassembly; `param action_ref` / `call target` / `export @…` values are resolved
through an annotation→layout remap table at assemble time. This is what makes length edits
safe: changing a text line shifts later actions, and every reference follows automatically.
A reference to an address that no action carries is a syntax error (with line number).

Strings escape `\" \\ \n \r \t \xNN`; `\xNN` produces a raw byte in the content (used by the
disassembler for non-cp932 bytes). `.stcm2` tags and export names are byte-exact via the same
escaping.

## 6. Per-slot size: the embedded TOC is rebuilt, so the old budget is gone

Thread B's "size budget" was a stale-table artifact: the game reads each
chunk with the `size` from SCRIPT.UNI's own embedded TOC (see §1a), and our
tools used to preserve that TOC verbatim — a grown chunk was read truncated
→ crash. **Fixed 2026-08-14**: `uni2_join()` rebuilds the embedded TOC on
save, so slots may grow freely and the game reads them correctly (verified
in-game: 10+ appended dialogue lines play, scene continues).

The only remaining constraint is the **ISO file size**: a slot may grow up
to its 0x800 boundary without changing SCRIPT.UNI's file size (in-place ISO
patch works). Growing past the padding grows the file — the native editor's
Update ISO then does a full rebuild with the path tables at LBA 257/258
(SONY's cdvdman reads the path table at hardcoded LBA 257 for DVDs).
Manual recipes for hand-built ISOs: mkisofs +
`tools/iso_path_table_fix.py` (relocates a plain rebuild's path table
there), or the surgical relocation (`tools/surgical_iso_move.py`, verified
booting + playing).

The editor's `file_budget()` is this padding limit (pristine slot size
rounded up to 0x800); slots past it are flagged because the fast in-place
patch no longer applies and a full rebuild is used instead — not because
the game would crash.
