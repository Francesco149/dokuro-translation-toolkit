#!/usr/bin/env python3
"""surgical_iso_move.py — relocate a grown SCRIPT.UNI inside an existing PS2 ISO.

Background (2026-08-14): the chunk-budget wall is gone (see
docs/investigations/2026-08-14-chunk-budget-bypass/). A translated chunk may
grow freely as long as SCRIPT.UNI's embedded TOC at 0x800 is rebuilt (the
editor does this on save; tools/rebuild_toc.py is the standalone reference).
When a chunk outgrows its 0x800 sector padding, SCRIPT.UNI itself grows and
the in-place patcher (tools/patch_iso.py) cannot hold it.

PREFERRED: full ISO rebuild. A mkisofs-style rebuild DOES work once the
path table is relocated to LBA 257 (0x101) — the PS2's cdvdman hardcodes
that position for DVD discs (see tools/iso_path_table_fix.py and the
chunk-budget-bypass investigation). Recipe: extract the ISO, swap in the
grown SCRIPT.UNI, mkisofs rebuild, run iso_path_table_fix.py.

THIS TOOL (fallback): keeps the original disc byte-for-byte and only
  1. writes the grown SCRIPT.UNI into the trailing free space of the disc, and
  2. patches SCRIPT.UNI's directory record (extent + size, both endians).
The game locates SCRIPT.UNI through the ISO9660 directory record at boot
(dynamic lookup), so the moved file is found regardless of the path table.

Usage:
  surgical_iso_move.py <in.iso> <grown-SCRIPT.UNI> <out.iso>
"""

import struct
import sys
import os

def read_sector(f, lba):
    f.seek(lba * 2048)
    return f.read(2048)

def parse_rec(rec, base_off):
    ln = rec[0]
    if ln == 0:
        return None
    name_len = rec[32]
    name = rec[33:33 + name_len].decode("ascii", "replace")
    ext = struct.unpack_from("<I", rec, 2)[0]
    size = struct.unpack_from("<I", rec, 10)[0]
    flags = rec[25]
    return dict(len=ln, name=name, ext=ext, size=size, flags=flags, rec_off=base_off)

def walk_dir(f, ext, size):
    entries = []
    pos = 0
    while pos < size:
        data = read_sector(f, ext + pos // 2048)
        rec = data[pos % 2048:]
        e = parse_rec(rec, ext * 2048 + pos)
        if e is None:
            pos = (pos // 2048 + 1) * 2048
            continue
        pos += e["len"]
        if e["name"] in (".", "..", "\x00", "\x01"):
            continue  # current/parent dir entries
        entries.append(e)
    return entries

def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    src, new_script, out = sys.argv[1:4]
    new = open(new_script, "rb").read()
    new_sectors = (len(new) + 2047) // 2048
    with open(src, "rb") as f:
        pvd = read_sector(f, 16)
        if pvd[1:6] != b"CD001":
            raise SystemExit("not an ISO9660 image")
        root = pvd[156:190]
        root_ext = struct.unpack_from("<I", root, 2)[0]
        root_size = struct.unpack_from("<I", root, 10)[0]
        root_entries = walk_dir(f, root_ext, root_size)
        union = [e for e in root_entries
                 if e["name"].split(";")[0].upper() == "UNION"][0]
        union_entries = walk_dir(f, union["ext"], union["size"])
        script = [e for e in union_entries
                  if e["name"].split(";")[0].upper() == "SCRIPT.UNI"][0]
        # highest used sector = max(file extent + file sectors) over all dirs
        last = 0
        for d in walk_dir(f, root_ext, root_size):
            if d["flags"] & 2:
                for e in walk_dir(f, d["ext"], d["size"]):
                    last = max(last, e["ext"] + (e["size"] + 2047) // 2048)
            else:
                last = max(last, d["ext"] + (d["size"] + 2047) // 2048)
        disc = os.path.getsize(src) // 2048
        free = disc - last
        if new_sectors > free:
            raise SystemExit(f"grown SCRIPT.UNI needs {new_sectors} sectors, "
                             f"only {free} free at disc end")
        print(f"SCRIPT.UNI record: ext 0x{script['ext']:X} size 0x{script['size']:X} "
              f"-> moving to 0x{last:X} (0x{len(new):X} bytes, {new_sectors} sectors)")
        with open(src, "rb") as fin, open(out, "wb") as fout:
            fin.seek(0)
            fout.write(fin.read())
            fout.seek(last * 2048)
            fout.write(new)
            rec_off = script["rec_off"]
            fout.seek(rec_off + 2)
            fout.write(struct.pack("<I", last))
            fout.write(struct.pack(">I", last))
            fout.write(struct.pack("<I", len(new)))
            fout.write(struct.pack(">I", len(new)))
        print(f"wrote {out}: SCRIPT.UNI relocated, directory record patched, "
              "all other bytes identical")

if __name__ == "__main__":
    main()
