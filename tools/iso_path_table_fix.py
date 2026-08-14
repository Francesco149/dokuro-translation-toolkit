#!/usr/bin/env python3
"""iso_path_table_fix.py — move an ISO's path table to LBA 257 (PS2 DVD).

The PS2's cdvdman (BIOS and game modules) builds its directory cache from
the PATH TABLE at HARDCODED LBA 257 (0x101) for DVD discs — a SONY mastering
convention. Generic ISO builders (mkisofs/genisoimage) place the path table
right after the volume descriptors (~LBA 19), so their output boots on a PC
emulator's own parser but the PS2 guest's cdvdman reads garbage at 257 and
fails (IOP TLB-miss storm during boot, before the game even loads its
modules). Verified live 2026-08-14: a mkisofs rebuild of the Dokuro-chan
disc with the path table relocated to 257 boots clean and plays the script.

This script makes any ISO9660 image PS2-DVD-compatible:
  1. reads the current path table (LBA + size from the PVD),
  2. writes it at LBA 257,
  3. fixes the PVD's path-table size/location fields (both endians).

Usage:
  iso_path_table_fix.py <in.iso> [out.iso]
  # out.iso defaults to in-place when omitted.

Full rebuild recipe for a grown SCRIPT.UNI (see
docs/investigations/2026-08-14-chunk-budget-bypass/):
  7z x in.iso -oiso_root          # extract
  cp grown-SCRIPT.UNI iso_root/UNION/SCRIPT.UNI
  mkisofs -iso-level 1 -V LABEL -o rebuilt.iso iso_root
  python3 iso_path_table_fix.py rebuilt.iso
"""

import struct
import sys

PVD_LBA = 16
PATH_TABLE_LBA = 257  # SONY's fixed DVD path-table position (0x101)


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) == 3 else src
    with open(src, "rb") as f:
        f.seek(PVD_LBA * 2048)
        pvd = f.read(2048)
        if pvd[1:6] != b"CD001":
            raise SystemExit("not an ISO9660 image (no CD001 PVD)")
        pt_size = struct.unpack_from("<I", pvd, 132)[0]
        pt_lba = struct.unpack_from("<I", pvd, 140)[0]
        print(f"path table: LBA {pt_lba} size {pt_size}")
        if pt_size == 0 or pt_size > 0x10000:
            raise SystemExit(f"implausible path table size {pt_size}")
        if pt_lba == PATH_TABLE_LBA:
            print("already at LBA 257 — nothing to do")
            return
        f.seek(pt_lba * 2048)
        pt = f.read(pt_size)
        # sanity: first entry length byte + parent field
        print(f"first entry: len {pt[0]} extent {struct.unpack_from('<I', pt, 2)[0]}")
    with open(src, "rb") as fin, open(out, "r+b" if out == src else "wb") as fout:
        if out != src:
            fin.seek(0)
            fout.write(fin.read())
        fout.seek(PATH_TABLE_LBA * 2048)
        fout.write(pt)
        fout.seek(PVD_LBA * 2048 + 132)
        fout.write(struct.pack("<I", pt_size))  # size LE
        fout.write(struct.pack(">I", pt_size))  # size BE
        fout.write(struct.pack("<I", PATH_TABLE_LBA))  # L path table LE
        fout.write(struct.pack(">I", PATH_TABLE_LBA))  # L path table BE
        fout.write(struct.pack("<I", 0))  # optional L path table
        fout.write(struct.pack(">I", 0))
    print(f"path table relocated to LBA {PATH_TABLE_LBA} (0x{PATH_TABLE_LBA:X}), PVD updated")


if __name__ == "__main__":
    main()
