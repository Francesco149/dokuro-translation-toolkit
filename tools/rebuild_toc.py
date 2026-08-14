#!/usr/bin/env python3
"""rebuild_toc.py — rebuild SCRIPT.UNI's embedded chunk TOC at file offset 0x800.

The no-TOC SCRIPT.UNI actually DOES have a TOC: 35 x 16-byte entries
{id, off_sectors, sector_len, size_rounded16} at file offset 0x800 (the
"zero padding" of the 4096-byte header — FORMAT.md §1a was wrong about it).
The game copies this table verbatim into its runtime container registry at
boot and uses the size field as the disc-read length. Translating a chunk
grows it; without rebuilding the TOC the game keeps the ORIGINAL size, reads
the chunk truncated, and crashes (Thread B).

Usage:
  rebuild_toc.py <in.SCRIPT.UNI> <out.SCRIPT.UNI>
  # preserves ids + sector_lens, recomputes off + size from actual chunk layout

Off = (chunk_offset - 0x1000) / 0x800  (sector offset from data start)
size = round_up16(chunk length)
sector_len = padded_chunk_len / 0x800   (kept from the original TOC unless
                                         the chunk outgrows its padding)
"""

import struct
import sys

TOC_OFF = 0x800          # TOC position inside the file
DATA_OFF = 0x1000        # first chunk starts here
STRIDE = 16
MAGIC = b"UNI2"
EXP_MAGIC = b"EXPORT_DATA\x00"


def round16(n):
    return (n + 15) // 16 * 16


def chunk_len(data, off):
    """Chunk length = export_addr + export_len*40 (export table ends the chunk)."""
    export_addr = struct.unpack_from("<I", data, off + 32)[0]
    export_len = struct.unpack_from("<I", data, off + 36)[0]
    return export_addr + export_len * 40


def rebuild(data):
    assert data[:4] == MAGIC, "not a UNI2 file"
    count = struct.unpack_from("<I", data, 8)[0]
    assert count * STRIDE + TOC_OFF <= DATA_OFF, "TOC does not fit in header"
    toc = []
    pos = DATA_OFF
    for i in range(count):
        if data[pos:pos + 5] != b"STCM2":
            raise SystemExit(f"chunk {i}: no STCM2 magic at 0x{pos:X}")
        cid, off_old, sec_len_old, size_old = struct.unpack_from("<IIII", data, TOC_OFF + i * STRIDE)
        length = chunk_len(data, pos)
        padded = ((pos + length + 0x7FF) // 0x800) * 0x800  # round end up
        sec_len = padded // 0x800 - pos // 0x800
        toc.append((cid, (pos - DATA_OFF) // 0x800, sec_len, round16(length)))
        pos = padded
    out = bytearray(data)
    for i, (cid, off, sec, size) in enumerate(toc):
        struct.pack_into("<IIII", out, TOC_OFF + i * STRIDE, cid, off, sec, size)
    return bytes(out), toc


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    data = open(sys.argv[1], "rb").read()
    out, toc = rebuild(data)
    open(sys.argv[2], "wb").write(out)
    print(f"wrote {sys.argv[2]} ({len(out)} bytes), {len(toc)} TOC entries:")
    for i, (cid, off, sec, size) in enumerate(toc):
        old = struct.unpack_from("<IIII", data, TOC_OFF + i * STRIDE)
        mark = "" if old == (cid, off, sec, size) else "  <- CHANGED"
        print(f"  {i:2d} id=0x{cid:X} off=0x{off:X} sec={sec} size=0x{size:X}{mark}")


if __name__ == "__main__":
    main()
