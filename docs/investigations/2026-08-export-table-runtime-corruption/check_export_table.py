"""
Independent re-verification of §6.3m: dumps chunk 5's export table from both the
pristine original SCRIPT.UNI and the confirmed-crashing SCRIPT.UNI.crashing, byte for
byte, and shows exactly where the 'sure1' export's target_addr field lives in each.

Result (see FONT_AND_CRASH_INVESTIGATION.md §6.3q for the write-up): both files contain
the byte-correct value (0x9c) at the field's *current* location -- the file on disk is
provably fine in both cases. This is what makes the runtime misread in §6.3p/6.3s
interesting: the game is reading a wrong value from a memory address whose on-disk
contents are proven correct.

Run from this directory: `python3 check_export_table.py`
"""
import struct
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "2026-08-addr-remap-check"))
from verify_addrs import split_uni, parse_stcm2  # noqa: E402

TOOLKIT_ROOT = os.path.join(os.path.dirname(__file__), "..", "..", "..")
ORIG = os.path.join(TOOLKIT_ROOT, "game-files", "SCRIPT.UNI")
CRASH = os.path.join(TOOLKIT_ROOT, "docs", "investigations",
                      "2026-08-crashing-script-sample", "SCRIPT.UNI.crashing")

CHUNK_IDX = 5  # "chunk 5" / the 6th embedded scene, per §6.3m


def main():
    for label, path in [("ORIGINAL", ORIG), ("CRASHING", CRASH)]:
        blobs = split_uni(path)
        blob = blobs[CHUNK_IDX]
        info = parse_stcm2(blob)
        print(f"=== {label} ({path}) ===")
        print(f"  blob length (total_len)     : {info['total_len']:#x} ({info['total_len']})")
        print(f"  header export_addr field    : {info['export_addr']:#x}")
        print(f"  header export_len field     : {info['export_len']}")
        for i, e in enumerate(info['exports']):
            name = e['name'].split(b'\x00')[0].decode('ascii', 'replace')
            ta = e['target_addr']
            entry_start = info['export_addr'] + i * 40
            ta_field_off = entry_start + 36
            raw = blob[ta_field_off:ta_field_off + 4]
            raw_u32 = struct.unpack('<I', raw)[0]
            print(f"  export[{i}] name={name!r:10} target_addr(parsed)={ta:#x}  "
                  f"entry_start={entry_start:#x}  target_addr_field_offset={ta_field_off:#x}  "
                  f"raw_bytes_at_field={raw.hex()}  raw_as_u32={raw_u32:#x}")
        print()


if __name__ == "__main__":
    main()
