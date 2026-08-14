# SLPM_661.85 reverse engineering toolkit

Companion tooling for reading the game's MIPS executable (`SLPM_661.85`). See
`docs/ORIENTATION.md` section 6 for the full writeup — this README is just "how to run it".

## Setup (do this first, every fresh sandbox/session)

```bash
pip install capstone --break-system-packages   # not actually used anymore, harmless to skip
apt-get update && apt-get install -y binutils-mipsel-linux-gnu
```

The second command is the important one — it gives you `mipsel-linux-gnu-objdump`, which
understands the PS2 EE's `mips:5900` machine type (MMI instructions, 128-bit `sq`/`lq`, etc).
**Do not use `capstone` or plain `objdump` for disassembly** — capstone's generic MIPS engine
misdecodes EE-specific instructions as bogus MIPS-DSP-ASE opcodes (silently wrong, not just
missing), and stock `objdump` doesn't recognize the architecture at all. This cost a lot of
back-and-forth to discover; don't re-discover it. (`reveng/disasm.py` is the old capstone-based
script from before this was figured out — it's still checked in but has its own loud warning at
the top now; don't use it.)

**Sandbox gotcha (2026-08):** `apt-get`-installed packages, and anything written outside this
repo checkout (e.g. a separate `/home/claude/work/investigate/`-style scratch dir), were
observed to vanish partway through a single session — `mipsel-linux-gnu-objdump` disappeared
and had to be reinstalled mid-session, and a whole scratch directory of Python scripts was lost
outright, while edits made directly inside this repo checkout survived fine. If a tool that
should be installed suddenly isn't, don't assume something's more broken than it is — just
re-run the setup command above. But don't rely on anything outside this repo checkout
persisting across a session; write scratch scripts inside `docs/investigations/<dated-folder>/`
(or wherever they'll actually be committed) rather than a separate working directory, or you
may have to redo the work.

## Disassembling a function

```bash
./od.sh 0x00164400 0x00165058   # low/high addresses from all_functions.txt
```

This just wraps `mipsel-linux-gnu-objdump -d -m mips:5900 -M no-aliases`.

## Symbol table (`all_functions.txt`)

1733 real function names with exact `[low_pc, high_pc)` address ranges, recovered from the
executable's own embedded debug info. Format: `low_pc  high_pc  name` per line, sorted by
address. This is checked in because regenerating it takes a couple minutes; regenerate with:

```bash
python3 dwarf1_parse.py    # writes dwarf1_entries.pkl (all 31274 raw DIEs)
python3 extract_funcs.py   # reads that, writes funcs.pkl + prints all_functions.txt-ish output
```

Both scripts have `/mnt/user-data/uploads/SLPM_661.85` hardcoded as the binary path — fix if
your sandbox mounts it elsewhere.

### Why this works / what's actually in the binary

`readelf -S` shows two `MIPS_DEBUG`-typed sections, `.debug` and `.line`. This looks like it
should be the classic SGI/IRIX ECOFF symbol table format (that's what `MIPS_DEBUG` normally
means, and what GNU binutils' `include/coff/sym.h` describes) — **it is not**. The compiler
producer string at the very start of `.debug` reads `"MW MIPS C Compiler"` (Metrowerks
CodeWarrior), and MWCC's PS2 toolchain emits **DWARF 1** (the 1992 UI/PLSIG spec) into those
same section names instead. DWARF1 predates DWARF2's renaming to `.debug_info`/`.debug_line`,
so it still uses the old SGI section names — which is what caused the initial confusion.

DWARF1 struct layout, confirmed byte-for-byte against this exact binary (see
`elf_dwarf.h`, fetched from binutils' `include/elf/dwarf.h` — that header is the original
1992 spec transcribed, and is the ground truth for tag/attribute/form numbers):

```
DIE:
  u32 length      (total bytes, including this field)
  u16 tag         (enum dwarf_tag)
  attributes until `length` bytes consumed:
    u16 attr      (enum dwarf_attribute — LOW 4 BITS are the form! see gotcha below)
    <form-dependent data>
      FORM_ADDR   (1): u32
      FORM_REF    (2): u32 (offset elsewhere in .debug)
      FORM_BLOCK2 (3): u16 len, then len bytes
      FORM_BLOCK4 (4): u32 len, then len bytes
      FORM_DATA2  (5): u16
      FORM_DATA4  (6): u32
      FORM_DATA8  (7): 8 bytes
      FORM_STRING (8): null-terminated
length==4 (no tag) is a "null" DIE — marks end of a sibling/children list.
```

**Gotcha that cost real time:** the form is encoded in the attribute code's low **4** bits
(`attr & 0xF`), not 3. `AT_name = 0x30|FORM_STRING = 0x38`; `0x38 & 0x7 == 0`, which silently
looks like a plausible-but-wrong form and swallows the rest of the DIE's attributes. Always
mask with `0xF`.

Walking is trivially robust: you don't need to correctly parse every attribute to find the next
DIE — `length` alone tells you where it starts, so a parser can walk the *entire* `.debug`
section linearly root-to-end without any recursion/sibling-chasing, picking out whatever tags
you care about (here: `TAG_global_subroutine=0x6` and `TAG_subroutine=0x14`, pulling
`AT_name` (0x38) + `AT_low_pc` (0x111) + `AT_high_pc` (0x121)). 31274 DIEs total, 1733 of them
named functions with addresses. No recursion, no sibling-pointer following needed for this use
case — attribute-parse errors (if any existed) can't desync the walk.

Verified the whole file parses cleanly (tag histogram looks exactly like real C++ debug info:
9954 `member`, 3194 `formal_parameter`, 2811 `local_variable`, 1307 `class_type`, etc. — no
garbage/desync).

### Runtime string caveat

The debug info's own strings (source paths like `FontLib.cpp`, mangled names like
`RunFontSystem__Fv`) are **not** present anywhere in the game's *loaded* runtime image — only
in the on-disk `.debug` section, which isn't part of either ELF `LOAD` segment. Don't try to
cross-reference them against runtime `lui`/`addiu` address-formation pairs in `.text`; they
simply aren't there. (Learned this the hard way before finding the DWARF1 angle — worth noting
so it's not re-attempted.)
