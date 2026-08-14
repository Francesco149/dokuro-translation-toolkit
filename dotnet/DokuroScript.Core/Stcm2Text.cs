using System;
using System.Text;

namespace DokuroScript.Core
{
    public static class Stcm2Text
    {
        // cp932 = Windows Shift-JIS superset of JIS X0208, which is what the game
        // itself uses. Crucially, cp932 already maps the full Cyrillic alphabet
        // (А-Я, а-я, incl. Ё) onto the same bytes as JIS X0208 row 7 - this is a
        // real, standard part of Shift-JIS, not a hack. So a translator can type
        // Cyrillic directly and it will encode to bytes the game's existing font
        // cache can already render, with no custom codec table needed.
        private static readonly Encoding Sjis;
        private static readonly Encoding SjisLenient;

        static Stcm2Text()
        {
            TryRegisterCodePagesProvider();
            Sjis = Encoding.GetEncoding(932, EncoderFallback.ExceptionFallback, DecoderFallback.ExceptionFallback);
            SjisLenient = Encoding.GetEncoding(932, EncoderFallback.ReplacementFallback, DecoderFallback.ReplacementFallback);
        }

        // On .NET Framework 4.8 (the shipped WinForms app), cp932 is provided by
        // Windows itself - this is a no-op there. On .NET Core (used only for our
        // own dev/test harness on Linux), it's needed to unlock cp932 support.
        private static void TryRegisterCodePagesProvider()
        {
            try
            {
                var t = Type.GetType("System.Text.CodePagesEncodingProvider, System.Text.Encoding.CodePages");
                var instance = t?.GetProperty("Instance")?.GetValue(null) as EncodingProvider;
                if (instance != null) Encoding.RegisterProvider(instance);
            }
            catch { /* best effort */ }
        }

        public static byte[] EncodeSjis(string s) => Sjis.GetBytes(s);

        /// <summary>
        /// Fixes up characters that LOOK like plain punctuation (curly quotes typed
        /// by autocorrect, a pasted non-breaking space, an em dash, etc.) but aren't
        /// in cp932/Shift-JIS, so a translator never has to know or care that these
        /// are technically "different" characters. Anything left over after this
        /// that still can't encode is a genuine "please use a different character"
        /// case, reported precisely by TryEncodeSjis rather than left to throw a
        /// raw .NET exception at save time.
        /// </summary>
        public static string Sanitize(string input)
        {
            if (string.IsNullOrEmpty(input)) return input;
            var sb = new StringBuilder(input.Length);
            foreach (var ch in input)
            {
                switch (ch)
                {
                    case '\u00A0': // NO-BREAK SPACE
                    case '\u202F': // NARROW NO-BREAK SPACE
                        sb.Append(' ');
                        break;
                    case '\u200B': // ZERO WIDTH SPACE
                    case '\uFEFF': // BOM / ZERO WIDTH NO-BREAK SPACE
                    case '\u00AD': // SOFT HYPHEN
                        break; // drop entirely, invisible either way
                    case '\u2013': // EN DASH
                    case '\u2014': // EM DASH
                    case '\u2212': // MINUS SIGN
                        sb.Append('\u2015'); // -> JIS X0208 horizontal bar (the "real" dash in this charset)
                        break;
                    case '\uFF5E': // FULLWIDTH TILDE
                        // NOTE: kept as U+FF5E — Windows cp932 maps byte 0x8160 to U+FF5E
                        // (not U+301C WAVE DASH, which cp932 CANNOT encode — verified
                        // against .NET Encoding.GetEncoding(932) with ExceptionFallback
                        // 2026-08-14). Earlier code "normalized" U+FF5E -> U+301C which
                        // then failed TryEncodeSjis and flagged the line BAD CHARACTER.
                        // U+FF5E itself encodes fine, so leave it alone.
                        break;
                    case '\u201E': // DOUBLE LOW-9 QUOTATION MARK („)
                    case '\u00AB': // LEFT GUILLEMET («)
                    case '\u00BB': // RIGHT GUILLEMET (»)
                        sb.Append('"');
                        break;
                    case '\u2022': // BULLET
                        sb.Append('\u30FB'); // -> katakana middle dot, closest equivalent in this charset
                        break;
                    default:
                        sb.Append(ch);
                        break;
                }
            }
            return sb.ToString();
        }

        /// <summary>Non-throwing encode check. Returns true and the encoded bytes on
        /// success; on failure returns the exact character and its position in the
        /// (already-sanitized, as displayed) string so the caller can point the
        /// translator at precisely what to fix.</summary>
        public static bool TryEncodeSjis(string s, out byte[]? bytes, out int badIndex, out char badChar)
        {
            for (int i = 0; i < s.Length; i++)
            {
                if (!CanEncodeChar(s[i]))
                {
                    bytes = null;
                    badIndex = i;
                    badChar = s[i];
                    return false;
                }
            }
            bytes = Sjis.GetBytes(s);
            badIndex = -1;
            badChar = '\0';
            return true;
        }

        private static bool CanEncodeChar(char c)
        {
            try { Sjis.GetBytes(new[] { c }); return true; }
            catch { return false; }
        }

        public static bool TryDecodeSjisStrict(byte[] bytes, out string? s)
        {
            try
            {
                s = Sjis.GetString(bytes);
                return true;
            }
            catch
            {
                s = null;
                return false;
            }
        }

        public static string DecodeSjisLenient(byte[] bytes) => SjisLenient.GetString(bytes);
    }

}
