"""
Pins down the EXACT byte boundary behind the known 47-safe/48-crash bisection (§6.3c),
for chunk 5's action at file-offset 0x304 (the exact action §6.3m found edited in the
confirmed-crashing file).

Uses the already-validated `serialize_with_edit` from ../2026-08-addr-remap-check
(proven correct in §6.3l) to build variant blobs at a range of replacement-text
lengths, without needing .NET or a live emulator.

Key result (see FONT_AND_CRASH_INVESTIGATION.md §6.3r): the shift is NOT linear in raw
SJIS byte count -- the data-chunk encoding pads to the next multiple of 4 bytes with a
minimum of 1 padding byte, so lengths 44-47 all produce an identical +8 shift, and
48-51 all produce +12. The 47-vs-48 crash boundary is exactly the edge of that padding
bucket. The resulting target_addr field address for the sole 'sure1' export lands at
chunk_base + 0x7420 in the 48-byte (crashing) case and chunk_base + 0x741c in the
47-byte (safe) case -- and 0x7420 is EXACTLY the original chunk's total_len (0x7418)
padded up with the same "round up, minimum 1 byte" convention already used for
individual text chunks in this format, just applied at the whole-blob level.

Run from this directory: `python3 boundary_check.py`
"""
import struct
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "2026-08-addr-remap-check"))
from verify_addrs import split_uni, parse_stcm2  # noqa: E402
from verify_serialize import serialize_with_edit, reparse_chunks  # noqa: E402

TOOLKIT_ROOT = os.path.join(os.path.dirname(__file__), "..", "..", "..")
ORIG = os.path.join(TOOLKIT_ROOT, "game-files", "SCRIPT.UNI")
CHUNK_IDX = 5
ACTION_ADDR = 0x304


def main():
    blobs = split_uni(ORIG)
    blob = blobs[CHUNK_IDX]
    info = parse_stcm2(blob)
    orig_total_len = info['total_len']
    print(f"original chunk5 total_len = {orig_total_len:#x} ({orig_total_len})")

    action = next(a for a in info['actions'] if a['addr'] == ACTION_ADDR)
    chunks = reparse_chunks(blob, action['data_start'], action['data_len'])
    text_chunks = [c for c in chunks if c['is_text']]
    orig_text = text_chunks[0]['text']
    orig_bytes = len(orig_text.encode('shift_jis'))
    print(f"original text at action {ACTION_ADDR:#x}: {orig_text!r} = {orig_bytes} SJIS bytes")
    print()

    for testlen in range(44, 53):
        # ASCII is 1 byte/char in SJIS, matching the person's "test"-repeated approach
        new_text = 'a' * testlen
        newblob = serialize_with_edit(info, blob, ACTION_ADDR, new_text)
        info2 = parse_stcm2(newblob)
        new_total_len = info2['total_len']
        shift = new_total_len - orig_total_len
        export_addr2 = info2['export_addr']
        entry_start = export_addr2  # export index 0, the only export ('sure1')
        ta_field_off = entry_start + 36
        raw = newblob[ta_field_off:ta_field_off + 4]
        raw_u32 = struct.unpack('<I', raw)[0]
        parsed_target = info2['exports'][0]['target_addr']
        past_orig_boundary_by = ta_field_off - orig_total_len
        print(f"replacement len={testlen:3d}B  shift={shift:+3d}  new_total_len={new_total_len:#x}  "
              f"export_addr={export_addr2:#x}  target_addr_field_off={ta_field_off:#x}  "
              f"(orig_total_len {'+' if past_orig_boundary_by >= 0 else ''}{past_orig_boundary_by})  "
              f"target_addr(parsed)={parsed_target:#x}  raw_bytes={raw.hex()}")


if __name__ == "__main__":
    main()
