using System;
using System.Collections.Generic;
using System.IO;

namespace DokuroScript.Core
{
    /// <summary>
    /// Handles the SCRIPT.UNI-style UNI2 container: sequential STCM2 blobs
    /// padded to 2048-byte sectors, with a per-chunk TOC hidden at file
    /// offset 0x800 inside the "header" ({id, off_sectors, sector_len,
    /// size_round16}, 35 x 16 bytes). The game copies that TOC verbatim
    /// into its runtime registry at boot and uses the size field as the
    /// disc-read length (Thread B), so Join() REBUILDS it from the actual
    /// slot layout — without that, grown chunks get read truncated.
    /// C# port of uni2_split.py / uni2_join.py - see those for the format
    /// writeup.
    /// </summary>
    public static class Uni2Script
    {
        private const int Sector = 0x800;
        private const int TocOffset = 0x800;
        private const int TocStride = 16;
        private static readonly byte[] StcmMagic = System.Text.Encoding.ASCII.GetBytes("STCM2");

        private static int ReadLe32(byte[] b, int off)
        {
            return b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24);
        }

        private static void WriteLe32(byte[] b, int off, int v)
        {
            b[off] = (byte)(v & 0xFF);
            b[off + 1] = (byte)((v >> 8) & 0xFF);
            b[off + 2] = (byte)((v >> 16) & 0xFF);
            b[off + 3] = (byte)((v >> 24) & 0xFF);
        }

        /// <summary>
        /// Rebuilds the embedded per-chunk TOC in-place in `joined`
        /// (header + padded slots). Preserves ids; recomputes off
        /// (sector offset from data start), sector_len (padded size in
        /// sectors) and size (round16 of the real length) from the actual
        /// slot layout. No-op when the header has no populated TOC.
        /// </summary>
        private static void RebuildEmbeddedToc(byte[] joined, int headerLen, IReadOnlyList<byte[]> slots)
        {
            if (headerLen < 12 || joined[0] != (byte)'U' || joined[1] != (byte)'N' ||
                joined[2] != (byte)'I' || joined[3] != (byte)'2')
                return;
            int count = ReadLe32(joined, 8);
            if (count <= 0 || count > 128) return;
            if (headerLen < TocOffset + count * TocStride) return;
            bool populated = false;
            for (int i = 0; i < count && !populated; i++)
                for (int f = 0; f < 4; f++)
                    if (ReadLe32(joined, TocOffset + i * TocStride + f * 4) != 0) { populated = true; break; }
            if (!populated) return;
            int pos = headerLen;
            for (int i = 0; i < count && i < slots.Count; i++)
            {
                int len = slots[i].Length;
                int padded = ((len + Sector - 1) / Sector) * Sector;
                WriteLe32(joined, TocOffset + i * TocStride + 4, (pos - headerLen) / Sector);
                WriteLe32(joined, TocOffset + i * TocStride + 8, padded / Sector);
                WriteLe32(joined, TocOffset + i * TocStride + 12, (len + 15) / 16 * 16);
                pos += padded;
            }
        }

        public static (byte[] header, List<byte[]> slots) Split(byte[] data)
        {
            var offsets = new List<int>();
            int idx = 0;
            while (true)
            {
                idx = IndexOf(data, StcmMagic, idx);
                if (idx < 0) break;
                offsets.Add(idx);
                idx += 1;
            }
            if (offsets.Count == 0) throw new Stcm2FormatException("no STCM2 entries found in container");

            byte[] header = new byte[offsets[0]];
            Array.Copy(data, header, header.Length);

            var slots = new List<byte[]>();
            for (int i = 0; i < offsets.Count; i++)
            {
                int start = offsets[i];
                int end = (i + 1 < offsets.Count) ? offsets[i + 1] : data.Length;
                byte[] slot = new byte[end - start];
                Array.Copy(data, start, slot, 0, slot.Length);
                slots.Add(slot);
            }
            return (header, slots);
        }

        public static byte[] Join(byte[] header, List<byte[]> entries)
        {
            using var ms = new MemoryStream();
            ms.Write(header, 0, header.Length);
            foreach (var e in entries)
            {
                ms.Write(e, 0, e.Length);
                int pad = (Sector - (e.Length % Sector)) % Sector;
                if (pad > 0) ms.Write(new byte[pad], 0, pad);
            }
            byte[] joined = ms.ToArray();
            RebuildEmbeddedToc(joined, header.Length, entries);
            return joined;
        }

        private static int IndexOf(byte[] haystack, byte[] needle, int from)
        {
            for (int i = from; i <= haystack.Length - needle.Length; i++)
            {
                bool ok = true;
                for (int j = 0; j < needle.Length; j++)
                {
                    if (haystack[i + j] != needle[j]) { ok = false; break; }
                }
                if (ok) return i;
            }
            return -1;
        }
    }
}
