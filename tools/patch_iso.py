#!/usr/bin/env python3
"""patch_iso.py — in-place ISO9660 file replacement for test ISOs.

Replacing a file in a PS2 ISO by REBUILDING the image breaks the disc layout
(LBAs shift; some games read fixed LBAs for streaming and die — Dokuro-chan
does, TLB-miss storm at IOP pc=0x4). Patching the file's bytes in place at
its original extent keeps the layout identical, which keeps the game booting.

Replacement file must be <= the original file's size (padded with zeros;
PS2 games rarely care about trailing padding in script files).

Usage:
  patch_iso.py original.iso /UNION/SCRIPT.UNI new-script.uni output.iso
"""

import struct
import sys

def read_sector(f, lba):
    f.seek(lba * 2048)
    return f.read(2048)

def walk_dir(f, extent, size):
    entries = []
    pos = 0
    while pos < size:
        f.seek(extent * 2048 + (pos // 2048) * 2048)
        data = f.read(2048)
        rec = data[pos % 2048:]
        ln = rec[0]
        if ln == 0:
            pos = (pos // 2048 + 1) * 2048
            continue
        name = rec[33:33 + rec[32]].decode("ascii", "replace")
        entries.append((name, rec[25], struct.unpack_from("<I", rec, 2)[0],
                        struct.unpack_from("<I", rec, 10)[0]))
        pos += ln
    return entries

def find(iso_path, iso_path_in_image):
    parts = [p for p in iso_path_in_image.split("/") if p]
    with open(iso_path, "rb") as f:
        pvd = read_sector(f, 16)
        assert pvd[1:6] == b"CD001", "not an ISO9660 image"
        root = pvd[156:190]
        extent = struct.unpack_from("<I", root, 2)[0]
        size = struct.unpack_from("<I", root, 10)[0]
        for part in parts:
            match = None
            for name, flags, e, s in walk_dir(f, extent, size):
                if name.split(";")[0].upper() == part.upper():
                    match = (name, flags, e, s)
                    break
            if match is None:
                raise SystemExit(f"{part!r} not found in ISO")
            name, flags, extent, size = match
        return name, extent, size

def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    src_iso, path, new_file, out_iso = sys.argv[1:5]
    name, extent, orig_size = find(src_iso, path)
    new_data = open(new_file, "rb").read()
    if len(new_data) > orig_size:
        raise SystemExit(
            f"replacement {len(new_data)} bytes > original {orig_size} "
            f"({name}) — cannot patch in place")
    print(f"patching {name}: extent={extent} (0x{extent:X}) size={orig_size} -> {len(new_data)}")
    with open(src_iso, "rb") as fin, open(out_iso, "wb") as fout:
        start = extent * 2048
        fin.seek(0)
        fout.write(fin.read(start))          # everything before the file
        fout.write(new_data)                  # replacement bytes
        fout.write(b"\x00" * (orig_size - len(new_data)))  # pad to original size
        fin.seek(start + orig_size)
        while True:
            chunk = fin.read(1 << 20)
            if not chunk:
                break
            fout.write(chunk)                 # everything after
    print(f"wrote {out_iso}")

if __name__ == "__main__":
    main()
