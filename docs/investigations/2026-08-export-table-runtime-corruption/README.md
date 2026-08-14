# Export-table runtime corruption — investigation artifacts

Supporting scripts for `FONT_AND_CRASH_INVESTIGATION.md` §6.3o-§6.3s (the session that
picked up where §6.3n left off, using the person's live trace of `STCM2_GetExportPointer`
itself). Read that section for the narrative; this folder is just the reproducible
byte-level and disassembly evidence behind it.

## What's here

- **`check_export_table.py`** — re-verifies §6.3m independently: dumps chunk 5's export
  table from both the pristine original `SCRIPT.UNI` and the confirmed-crashing
  `SCRIPT.UNI.crashing` (from `../2026-08-crashing-script-sample/`), byte for byte.
  Confirms the `sure1` export's `target_addr` field holds the correct value (`0x9c`) in
  **both** files, at its correctly-shifted location. The file on disk is provably fine —
  the corruption in §6.3p is a *runtime* misread, not a bad rebuild.

- **`boundary_check.py`** — the more interesting one. Builds a sweep of replacement-text
  lengths (44-52 bytes) for chunk 5's action at file-offset `0x304` (the exact action
  §6.3m found edited) using the already-validated `serialize_with_edit` from
  `../2026-08-addr-remap-check/`, and shows exactly where the export table's
  `target_addr` field lands for each. Confirms the empirical 47-safe/48-crash threshold
  from §6.3c corresponds to an exact, computable byte address: the field crosses from
  `orig_total_len + 4` to `orig_total_len + 8` right at the 47→48 boundary, because the
  data-chunk padding (round up to a multiple of 4, minimum 1 padding byte) buckets
  44-47 and 48-51 identically. `orig_total_len + 8` (`0x7420` for chunk 5) is the precise
  address the live trace in §6.3p shows being misread.

Both scripts import from `../2026-08-addr-remap-check/verify_addrs.py` and
`verify_serialize.py` — no new parsing logic, just new questions asked of the existing,
already-validated (§6.3l) reimplementation.

## Why this matters (one-paragraph version)

`GetExportPointer`'s own logic is now fully disassembly-confirmed correct (§6.3p), and
the file bytes it should be reading are fully confirmed correct (§6.3m, and again here).
So the wrong value it returns at runtime has to come from the *runtime memory* at that
address disagreeing with the file — i.e. something about how the chunk gets loaded into
RAM doesn't reflect its true (possibly-grown) size. `boundary_check.py` shows that
"something" tracks the **original**, pre-translation chunk size almost exactly (plus a
small, consistent padding slack) — independent of how the file was actually edited. That
pattern is the signature of a **fixed/stale size** being used somewhere in the loading
path, not a pointer-arithmetic bug in either the game's export-table walk or our own
rebuild tool. §6.3s traces the actual load call chain in the executable
(`AD_ScrirptLoad` → `iuRequestAccess` → `reqUni2ReadCallBack`) and finds a plausible
concrete mechanism: a runtime container registry whose per-entry `size` field is used
directly as the read length for the disc/HDD request. Whether that field is itself
re-derived from `SCRIPT.UNI`'s current bytes at boot (in which case this is a dead end)
or comes from something our tool doesn't currently touch (in which case this is the
root cause) is the one open question — see §6.3s for the exact, cheap live check that
settles it.

## Re-running

```
cd docs/investigations/2026-08-export-table-runtime-corruption
python3 check_export_table.py
python3 boundary_check.py
```

No .NET needed, matching the sibling `2026-08-addr-remap-check/` folder's approach.
