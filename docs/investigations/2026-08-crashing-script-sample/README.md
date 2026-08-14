# Confirmed crashing `SCRIPT.UNI` sample (2026-08)

`SCRIPT.UNI.crashing` is the actual file the person built with the current `WinFormsApp` and
confirmed crashes the game in PCSX2. Kept here as a ground-truth reproducer — this is what's
referenced throughout `FONT_AND_CRASH_INVESTIGATION.md` §6.3m.

## What's been checked against it

Full write-up: `FONT_AND_CRASH_INVESTIGATION.md` §6.3m. Summary: it's byte-for-byte structurally
correct. Every `ActionRef`/export/call target in the file resolves to an exact action start (1328 +
270 + 4 references, zero exceptions), including in the specific scene that crashes. The one edit in
the file — diffed against `game-files/SCRIPT.UNI` — is chunk 5 (the 6th embedded scene), action at
file-offset `0x304`, an `0x118` dialogue action, Japanese text replaced with `test` × 24 (48 SJIS
bytes, matching the known 47-safe/48-crash bisection from §6.3c exactly). 488 of that chunk's 500
actions shifted position as a result; every reference to a shifted action still resolves correctly.

## Reproducing the check

From this directory:

```python
import sys
sys.path.insert(0, "../2026-08-addr-remap-check")
import verify_addrs as va

blobs = va.split_uni("SCRIPT.UNI.crashing")
# ... see verify_addrs.py's main() for the exact invariant check, or FONT_AND_CRASH_INVESTIGATION.md §6.3m
```

Or diff it against the pristine original to relocate the edit if it's ever unclear:

```python
a = open("../../../game-files/SCRIPT.UNI", "rb").read()
b = open("SCRIPT.UNI.crashing", "rb").read()
# first differing byte run marks the start of the edited region
```

## What this means for next steps

The address-remap question is closed (§6.3m) — don't re-open it without new evidence, the check
above is exhaustive, not a sample. The open question is entirely inside the game's own
`GetFunctionVariable` (`0x168d70`-`0x1693b4`) — see `ORIENTATION.md` TODO #1 for the current
concrete next step (trace it live against this exact file, in PCSX2 or another MIPS-capable
debugger, without assuming an upstream bad pointer).
