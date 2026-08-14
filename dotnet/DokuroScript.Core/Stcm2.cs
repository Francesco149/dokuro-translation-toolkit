using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace DokuroScript.Core
{
    // ---------- low level helpers ----------

    internal static class Bin
    {
        public static uint ReadU32LE(byte[] b, int off)
        {
            if (off < 0 || off + 4 > b.Length) throw new Stcm2FormatException($"read past end at {off}");
            return (uint)(b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24));
        }

        public static void WriteU32LE(BinaryWriter w, uint v) => w.Write(v);

        public static bool StartsWith(byte[] b, int off, byte[] pat)
        {
            if (off < 0 || off + pat.Length > b.Length) return false;
            for (int i = 0; i < pat.Length; i++)
                if (b[off + i] != pat[i]) return false;
            return true;
        }
    }

    public class Stcm2FormatException : Exception
    {
        public Stcm2FormatException(string msg) : base(msg) { }
    }

    // ---------- data model ----------

    public enum ParamKind { ActionRef, DataPointer, Value, GlobalDataPointer }

    /// <summary>
    /// One action parameter (12 bytes on disk: 3x u32 LE).
    /// For ActionRef/DataPointer, "Value" holds the ORIGINAL address/offset at parse
    /// time; DataPointer's Value is already relative to the owning action's data
    /// blob (matches stcm2.rs semantics). ActionRef's Value is the target action's
    /// ORIGINAL address and must be remapped through Stcm2File's address table when
    /// re-serializing after any action changes size.
    /// </summary>
    public class Param
    {
        public ParamKind Kind;
        public uint Value;

        public static Param Parse(uint[] triple, uint dataAddr, uint dataLen, uint globalDataLen)
        {
            const uint GDO = Stcm2File.GlobalDataOffset;
            uint a = triple[0], b = triple[1], c = triple[2];
            bool bcTag = (b == 0x40000000 || b == 0xff000000) && (c == 0x40000000 || c == 0xff000000);

            if (a == 0xffffff41 && (c == 0x40000000 || c == 0xff000000))
                return new Param { Kind = ParamKind.ActionRef, Value = b };

            if (bcTag && a >= dataAddr && a < dataAddr + dataLen)
                return new Param { Kind = ParamKind.DataPointer, Value = a - dataAddr };

            if (bcTag && a >= GDO && a < GDO + globalDataLen)
                return new Param { Kind = ParamKind.GlobalDataPointer, Value = a - GDO };

            if (bcTag)
                return new Param { Kind = ParamKind.Value, Value = a };

            throw new Stcm2FormatException($"bad parameter: {a:X8} {b:X8} {c:X8}");
        }

        /// <summary>Encode back to the 3xu32 on-disk form. actionRefResolver maps an
        /// ORIGINAL action address to its (possibly new) address.</summary>
        public uint[] Encode(uint dataAddr, Func<uint, uint> actionRefResolver)
        {
            const uint GDO = Stcm2File.GlobalDataOffset;
            switch (Kind)
            {
                case ParamKind.ActionRef:
                    return new[] { 0xffffff41u, actionRefResolver(Value), 0xff000000u };
                case ParamKind.DataPointer:
                    return new[] { dataAddr + Value, 0xff000000u, 0xff000000u };
                case ParamKind.GlobalDataPointer:
                    return new[] { GDO + Value, 0xff000000u, 0xff000000u };
                case ParamKind.Value:
                    return new[] { Value, 0xff000000u, 0xff000000u };
                default:
                    throw new InvalidOperationException();
            }
        }
    }

    /// <summary>
    /// One self-describing "boxed value" found inside an action's data blob.
    /// Mirrors disasm.rs's StringType: Type0U32/Type1U32 are opaque 4-byte numbers
    /// we must NOT touch; Text is real, editable string content.
    /// </summary>
    public class DataChunk
    {
        public int Offset;              // offset within the owning Action's Data
        public uint Type;               // 0 or 1 (on-disk "type" field)
        public bool IsText;
        public byte[] RawContent = Array.Empty<byte>(); // content bytes (post header, pre-truncation semantics below)
        public uint NumericValue;       // valid when !IsText
        public string? Text;            // decoded text when IsText (Shift-JIS)
        public string? EditedText;      // set by the UI/editor; null = unedited

        public int OnDiskLength => 16 + PayloadLen(); // header + content incl its own padding

        private int PayloadLen()
        {
            if (!IsText || EditedText == null) return RawContent.Length;
            var enc = Stcm2Text.EncodeSjis(EditedText);
            int pad = 4 - (enc.Length % 4);
            if (pad == 0) pad = 4; // always at least 1 null pad byte, matches original format's 1..=4 rule
            return enc.Length + pad;
        }

        /// <summary>Serialize this chunk (header + content) to a writer.
        /// When untouched, writes back the ORIGINAL bytes verbatim (no re-encode
        /// round trip) so an unmodified file reproduces byte-for-byte.</summary>
        public void Write(BinaryWriter w)
        {
            if (!IsText || EditedText == null)
            {
                w.Write(Type);
                w.Write((uint)(RawContent.Length / 4));
                w.Write(1u); // magic
                w.Write((uint)RawContent.Length);
                w.Write(RawContent);
                return;
            }

            var enc = Stcm2Text.EncodeSjis(EditedText);
            int pad = 4 - (enc.Length % 4);
            if (pad == 0) pad = 4;
            int len = enc.Length + pad;

            w.Write(0u);              // type
            w.Write((uint)(len / 4)); // qlen
            w.Write(1u);              // magic
            w.Write((uint)len);       // len
            w.Write(enc);
            for (int i = 0; i < pad; i++) w.Write((byte)0);
        }
    }

    public class ActionModel
    {
        public uint OriginalAddr;
        public bool Call;
        public uint Opcode; // if Call, this is the ORIGINAL address of the target action
        public List<Param> Params = new();
        public byte[] LeadingJunk = Array.Empty<byte>(); // bytes before the first DataChunk (rare)
        public List<DataChunk> Chunks = new();
        public byte[]? UnparsedData; // fallback: null unless the data blob couldn't be chunk-parsed;
                                       // when set, this is written back verbatim and Chunks/LeadingJunk are ignored.

        public uint NewAddr; // filled in during Serialize()

        public byte[] BuildData(Func<uint, uint> unusedResolverPlaceholder)
        {
            using var ms = new MemoryStream();
            using var w = new BinaryWriter(ms);
            if (UnparsedData != null)
            {
                w.Write(UnparsedData);
                return ms.ToArray();
            }
            w.Write(LeadingJunk);
            foreach (var c in Chunks) c.Write(w);
            return ms.ToArray();
        }
    }

    public class ExportEntry
    {
        public byte[] Name = new byte[32];
        public uint OriginalTargetAddr;
    }

    public class Stcm2File
    {
        public const int MagicLen = 5; // "STCM2"
        public const int TagLen = 32 - MagicLen; // 27
        public static readonly byte[] Magic = Encoding.ASCII.GetBytes("STCM2");
        public static readonly byte[] GlobalDataMagic = Encoding.ASCII.GetBytes("GLOBAL_DATA\0");
        public static readonly byte[] CodeStartMagic = Encoding.ASCII.GetBytes("CODE_START_\0");
        public static readonly byte[] ExportDataMagic = Encoding.ASCII.GetBytes("EXPORT_DATA\0");

        public const uint GlobalDataOffset = MagicLen + TagLen + 12 * 4 + 12; // 92, matches stcm2.rs

        public byte[] Tag = new byte[TagLen];
        public uint Unk1;
        public uint CollectionAddr;
        public byte[] Unk32 = new byte[32];
        public byte[] GlobalData = Array.Empty<byte>();
        public List<ActionModel> Actions = new(); // in on-disk order
        public List<ExportEntry> Exports = new(); // in on-disk order

        // ---------------- PARSE ----------------

        public static Stcm2File Parse(byte[] file)
        {
            int pos = 0;
            if (!Bin.StartsWith(file, 0, Magic)) throw new Stcm2FormatException("missing STCM2 magic");
            pos += MagicLen;

            var result = new Stcm2File();
            Array.Copy(file, pos, result.Tag, 0, TagLen); pos += TagLen;

            uint exportAddr = Bin.ReadU32LE(file, pos); pos += 4;
            uint exportLen = Bin.ReadU32LE(file, pos); pos += 4;
            result.Unk1 = Bin.ReadU32LE(file, pos); pos += 4;
            result.CollectionAddr = Bin.ReadU32LE(file, pos); pos += 4;
            Array.Copy(file, pos, result.Unk32, 0, 32); pos += 32;

            if (!Bin.StartsWith(file, pos, GlobalDataMagic)) throw new Stcm2FormatException("missing GLOBAL_DATA magic");
            pos += GlobalDataMagic.Length;
            if (pos != GlobalDataOffset) throw new Stcm2FormatException("global data offset mismatch");

            int globalLen = 0;
            while (!Bin.StartsWith(file, pos + globalLen, CodeStartMagic)) globalLen += 4;
            result.GlobalData = new byte[globalLen];
            Array.Copy(file, pos, result.GlobalData, 0, globalLen);
            pos += globalLen;

            if (!Bin.StartsWith(file, pos, CodeStartMagic)) throw new Stcm2FormatException("missing CODE_START magic");
            pos += CodeStartMagic.Length;

            var actionsByAddr = new Dictionary<uint, ActionModel>();
            int exportEntriesStart = checked((int)exportAddr) - ExportDataMagic.Length;

            while (pos < exportEntriesStart)
            {
                uint addr = (uint)pos;
                uint globalCall = Bin.ReadU32LE(file, pos); pos += 4;
                uint opcode = Bin.ReadU32LE(file, pos); pos += 4;
                uint nparams = Bin.ReadU32LE(file, pos); pos += 4;
                uint length = Bin.ReadU32LE(file, pos); pos += 4;

                bool call = globalCall switch
                {
                    0 => false,
                    1 => true,
                    _ => throw new Stcm2FormatException($"global_call = {globalCall:X8}")
                };

                var action = new ActionModel { OriginalAddr = addr, Call = call, Opcode = opcode };

                uint dataAddr = addr + 16 + 12 * nparams;
                uint dataLen = length - 16 - 12 * nparams;

                for (int i = 0; i < nparams; i++)
                {
                    var triple = new[] { Bin.ReadU32LE(file, pos), Bin.ReadU32LE(file, pos + 4), Bin.ReadU32LE(file, pos + 8) };
                    pos += 12;
                    action.Params.Add(Param.Parse(triple, dataAddr, dataLen, (uint)globalLen));
                }

                int ndata = checked((int)dataLen);
                byte[] data = new byte[ndata];
                Array.Copy(file, pos, data, 0, ndata);
                pos += ndata;

                ScanDataChunks(data, action);

                actionsByAddr[addr] = action;
                result.Actions.Add(action);
            }

            if (!Bin.StartsWith(file, pos, ExportDataMagic)) throw new Stcm2FormatException("missing EXPORT_DATA magic");
            pos += ExportDataMagic.Length;

            for (int i = 0; i < exportLen; i++)
            {
                uint zero = Bin.ReadU32LE(file, pos); pos += 4;
                if (zero != 0) throw new Stcm2FormatException("expected zero in export entry");
                var name = new byte[32];
                Array.Copy(file, pos, name, 0, 32); pos += 32;
                uint targetAddr = Bin.ReadU32LE(file, pos); pos += 4;
                result.Exports.Add(new ExportEntry { Name = name, OriginalTargetAddr = targetAddr });
            }

            return result;
        }

        // Mirrors disasm.rs's scanning loop + decode_string/four_byte_heuristic.
        private static void ScanDataChunks(byte[] data, ActionModel action)
        {
            int windowStart = 0;
            bool atBeginning = true;
            var chunks = new List<DataChunk>();
            byte[] leadingJunk = Array.Empty<byte>();

            while (windowStart < data.Length)
            {
                int pos = 0;
                bool found = false;
                DataChunk? chunk = null;

                while (windowStart + pos < data.Length)
                {
                    if (TryDecodeChunk(data, windowStart + pos, out chunk))
                    {
                        found = true;
                        break;
                    }
                    pos++;
                }

                if (!found) break; // remainder becomes trailing junk below

                if (pos != 0)
                {
                    if (!atBeginning)
                    {
                        // Shouldn't happen in well-formed files; be lenient and just
                        // fold the gap into UnparsedData fallback instead of crashing.
                        action.UnparsedData = data;
                        return;
                    }
                    leadingJunk = new byte[pos];
                    Array.Copy(data, windowStart, leadingJunk, 0, pos);
                }

                atBeginning = false;
                chunk!.Offset = windowStart + pos;
                chunks.Add(chunk);
                windowStart = windowStart + pos + chunk.OnDiskLength;
            }

            if (windowStart < data.Length)
            {
                if (chunks.Count == 0)
                {
                    // whole blob is junk (e.g. opcodes with no string args)
                    action.LeadingJunk = data;
                    action.Chunks = chunks;
                    return;
                }
                else
                {
                    // trailing bytes after the last chunk that aren't a valid chunk -
                    // preserve verbatim via fallback rather than risk corrupting them.
                    action.UnparsedData = data;
                    return;
                }
            }

            action.LeadingJunk = leadingJunk;
            action.Chunks = chunks;
        }

        private static bool TryDecodeChunk(byte[] data, int addr, out DataChunk? chunk)
        {
            chunk = null;
            try
            {
                if (data.Length - addr <= 16) return false;
                uint type_ = Bin.ReadU32LE(data, addr);
                if (type_ != 0 && type_ != 1) return false;
                uint qlen = Bin.ReadU32LE(data, addr + 4);
                uint magic1 = Bin.ReadU32LE(data, addr + 8);
                if (magic1 != 1) return false;
                uint len = Bin.ReadU32LE(data, addr + 12);
                if (len != qlen * 4) return false;
                int ilen = checked((int)len);
                if (ilen < 0 || data.Length - (addr + 16) < ilen) return false;

                byte[] content = new byte[ilen];
                Array.Copy(data, addr + 16, content, 0, ilen);

                if (type_ == 1)
                {
                    if (ilen != 4) return false; // unsupported per original format
                    chunk = new DataChunk { Type = 1, IsText = false, NumericValue = Bin.ReadU32LE(content, 0), RawContent = content };
                    return true;
                }

                // type_ == 0
                if (ilen == 4)
                {
                    var (isText, num, text) = FourByteHeuristic(content);
                    chunk = isText
                        ? new DataChunk { Type = 0, IsText = true, Text = text, RawContent = content }
                        : new DataChunk { Type = 0, IsText = false, NumericValue = num, RawContent = content };
                    return true;
                }

                // long-form: always text (per original format's own rules)
                int nzero = 0;
                for (int i = content.Length - 1; i >= 0 && content[i] == 0; i--) nzero++;
                if (nzero < 1 || nzero > 4) return false;
                byte[] trimmed = new byte[content.Length - nzero];
                Array.Copy(content, trimmed, trimmed.Length);
                string decoded = Stcm2Text.DecodeSjisLenient(trimmed);
                chunk = new DataChunk { Type = 0, IsText = true, Text = decoded, RawContent = content };
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static (bool isText, uint num, string? text) FourByteHeuristic(byte[] v /* len 4 */)
        {
            uint n = Bin.ReadU32LE(v, 0);
            if (n > 0xFFFFFF) return (false, n, null);

            int nzero = 0;
            for (int i = v.Length - 1; i >= 0 && v[i] == 0; i--) nzero++;
            byte[] trimmed = new byte[v.Length - nzero];
            Array.Copy(v, trimmed, trimmed.Length);

            if (trimmed.Length < 3 || (trimmed.Length == 2 && trimmed[0] == (byte)'o' && trimmed[1] == (byte)'p'))
                return (false, n, null);

            if (!Stcm2Text.TryDecodeSjisStrict(trimmed, out string? s)) return (false, n, null);
            if (s!.Any(char.IsControl)) return (false, n, null);
            return (true, n, s);
        }

        // ---------------- SERIALIZE ----------------

        public byte[] Serialize()
        {
            // Pass 1: compute new data bytes + lengths for every action, and assign new addresses.
            uint pos = GlobalDataOffset + (uint)GlobalData.Length + (uint)CodeStartMagic.Length;
            var addrMap = new Dictionary<uint, uint>();
            var built = new List<(ActionModel action, byte[] data, uint newAddr, uint length)>();

            foreach (var action in Actions)
            {
                byte[] data = action.BuildData(x => x);
                uint nparams = (uint)action.Params.Count;
                uint length = 16 + 12 * nparams + (uint)data.Length;
                addrMap[action.OriginalAddr] = pos;
                built.Add((action, data, pos, length));
                pos += length;
            }

            uint Resolve(uint originalAddr)
            {
                if (!addrMap.TryGetValue(originalAddr, out var na))
                    throw new Stcm2FormatException($"action ref to unknown address {originalAddr:X}");
                return na;
            }

            using var ms = new MemoryStream();
            using var w = new BinaryWriter(ms);

            w.Write(Magic);
            w.Write(Tag);

            long headerFixupPos = ms.Position; // export_addr, export_len
            w.Write(0u); w.Write(0u);
            w.Write(Unk1);
            w.Write(CollectionAddr);
            w.Write(Unk32);
            w.Write(GlobalDataMagic);
            w.Write(GlobalData);
            w.Write(CodeStartMagic);

            foreach (var (action, data, newAddr, length) in built)
            {
                uint dataAddr = newAddr + 16 + 12 * (uint)action.Params.Count;
                w.Write(action.Call ? 1u : 0u);
                w.Write(action.Call ? Resolve(action.Opcode) : action.Opcode);
                w.Write((uint)action.Params.Count);
                w.Write(length);
                foreach (var p in action.Params)
                {
                    var enc = p.Encode(dataAddr, Resolve);
                    w.Write(enc[0]); w.Write(enc[1]); w.Write(enc[2]);
                }
                w.Write(data);
            }

            uint exportEntriesStart = (uint)ms.Position + (uint)ExportDataMagic.Length;
            w.Write(ExportDataMagic);
            foreach (var exp in Exports)
            {
                w.Write(0u);
                w.Write(exp.Name);
                w.Write(Resolve(exp.OriginalTargetAddr));
            }

            byte[] final = ms.ToArray();
            // fix up export_addr/export_len now that we know them
            BitConverterLE(final, (int)headerFixupPos, exportEntriesStart);
            BitConverterLE(final, (int)headerFixupPos + 4, (uint)Exports.Count);
            return final;
        }

        private static void BitConverterLE(byte[] buf, int off, uint v)
        {
            buf[off] = (byte)v;
            buf[off + 1] = (byte)(v >> 8);
            buf[off + 2] = (byte)(v >> 16);
            buf[off + 3] = (byte)(v >> 24);
        }

        // ---------------- editor convenience ----------------

        public struct TranslatableString
        {
            public int ActionIndex;
            public int ChunkIndex;
            public string Original;
            public string? Translated;
        }

        public IEnumerable<TranslatableString> EnumerateStrings()
        {
            for (int ai = 0; ai < Actions.Count; ai++)
            {
                var a = Actions[ai];
                for (int ci = 0; ci < a.Chunks.Count; ci++)
                {
                    var c = a.Chunks[ci];
                    if (c.IsText)
                        yield return new TranslatableString { ActionIndex = ai, ChunkIndex = ci, Original = c.Text ?? "", Translated = c.EditedText };
                }
            }
        }

        public void SetTranslation(int actionIndex, int chunkIndex, string? text)
        {
            Actions[actionIndex].Chunks[chunkIndex].EditedText = text;
        }
    }
}
