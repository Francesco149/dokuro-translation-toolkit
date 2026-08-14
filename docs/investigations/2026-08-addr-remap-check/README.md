# Address-remap verification (2026-08)

Produced the finding written up in `FONT_AND_CRASH_INVESTIGATION.md` §6.3l: a from-scratch Python
port of `DokuroScript.Core`'s parser and `Stcm2File.Serialize()`, run directly against the real
`game-files/SCRIPT.UNI`, to check whether the C# remap logic correctly re-targets
`ActionRef`/export/call addresses when a translated line's length changes. Built because `dotnet`
wasn't available in that session's sandbox (no working SDK install path in the network allowlist)
— this is a way to re-check the *algorithm* without a .NET toolchain. It does NOT replace actually
running `TestHarness` (see `BUILD_AND_TEST.md`) — it can't catch a bug that's specific to the C#
and isn't reproduced in this Python port, only a logic error in the algorithm itself.

Requires only Python 3 stdlib. Run from this directory:

```
python3 verify_addrs.py
python3 verify_serialize.py       # slower — trims test count for sandbox time limits, see comments
```

## `verify_addrs.py`

Parses the real `SCRIPT.UNI` (all 35 STCM2 chunks) with an independent Python port of
`Stcm2.cs::Parse`, then checks a precondition the whole remap scheme depends on: **every**
`ActionRef` param, export `target_addr`, and `call`-action target must point to the exact file
offset where some action starts (never mid-action). Last run: **0 exceptions** across 1328
ActionRefs + 270 exports + 4 calls.

## `verify_serialize.py`

Imports from `verify_addrs.py`. Ports `Stcm2File.Serialize()` itself (including the `addrMap`/
`Resolve()` mechanism) to Python. For real dialogue (`0x118`) actions across all 35 files: edits
the action's text to Cyrillic strings of exactly 47 and 48 SJIS bytes (the crash-boundary bisection
from `FONT_AND_CRASH_INVESTIGATION.md` §6.3c), rebuilds, **re-parses the output from scratch**
(no assumptions carried over from the serialize step), and checks every export/ActionRef/call
target resolves to where its referenced action actually ended up. Also runs one whole-file stress
case per chunk (every text chunk in the file rewritten to 48 bytes simultaneously). Last run:
**0 mismatches across 383 tests** (6 dialogue actions × 2 lengths × 35 files + 35 whole-file cases;
reduced from the full action set only for sandbox time budget, not because failures appeared in a
larger run — increase the `tested_this_blob >= 6` cap in `verify_serialize.py::main` and the
`testlen` list if you want fuller coverage and have more time).

## If you're re-running this after a `Stcm2.cs` change

These scripts are a **snapshot port**, not auto-generated from the C#. If `Serialize()`'s remap
logic changes, re-diff `serialize_with_edit()` in `verify_serialize.py` against the current
`Stcm2.cs` by hand before trusting a rerun — it's easy for this Python copy to silently drift out
of sync with the real implementation.
