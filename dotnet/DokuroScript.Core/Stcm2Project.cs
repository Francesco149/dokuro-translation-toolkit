using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace DokuroScript.Core
{
    public enum EntryKind { Dialogue, Nameplate }

    /// <summary>
    /// The two opcodes that carry player-facing text, per in-game testing:
    ///   0x118 = one line of dialogue text inside the current text box.
    ///           Two 0x118 actions back to back = two lines in one box.
    ///           0x119 (no params) = "box advance" boundary between beats.
    ///   0x11A = speaker nameplate text.
    /// IMPORTANT: never add/remove 0x118/0x119/0x11A actions - the game
    /// hardcodes how many lines a box expects and will softlock (black
    /// screen) if that's changed, even though the byte format itself would
    /// happily parse. Only ever edit the TEXT inside existing actions.
    /// </summary>
    public static class Opcodes
    {
        public const uint DialogueLine = 0x118;
        public const uint BoxAdvance = 0x119;
        public const uint Nameplate = 0x11A;
    }

    public class TranslatableEntry
    {
        public int FileIndex;
        public int ActionIndex;
        public int ChunkIndex;
        public EntryKind Kind;
        public string Original = "";
        public string? Translation;

        public string EffectiveText => Translation ?? Original;

        // See OverflowChecker for where these numbers come from - they are an
        // approximation calibrated from manual crash-testing, NOT real glyph
        // metrics (the game's font renderer hasn't been reverse engineered
        // yet). Treat "near/over limit" as "test this one in an emulator",
        // not as gospel.
        public double WidthScore => OverflowChecker.Score(EffectiveText);
        public double LimitScore => Kind == EntryKind.Dialogue ? OverflowChecker.DialogueLimit : OverflowChecker.NameplateLimit;
        public bool OverLimit => WidthScore > LimitScore;
        public bool NearLimit => WidthScore > LimitScore * 0.9 && !OverLimit;
    }

    public static class OverflowChecker
    {
        // Calibration data points (manual in-game testing, 2026-08):
        //   dialogue box: "hello world test this text is longer" (36 ASCII chars) barely fits
        //   dialogue box: "Меня зовут Сакура Кусак" (23 Cyrillic chars) fits
        //   -> Cyrillic glyphs are ~36/23 = 1.565x the width of ASCII glyphs in this font
        //   nameplate box: "this is a very long na" (22 ASCII chars) fits, overflow is
        //   truncated silently (NOT a crash, unlike the dialogue box)
        public const double AsciiWeight = 1.0;
        public const double CyrillicWeight = 36.0 / 23.0;
        public const double DefaultWeight = 1.3; // unverified charset guess, be conservative

        public const double DialogueLimit = 36.0;
        public const double NameplateLimit = 22.0 / AsciiWeight; // same units, nameplate box is smaller

        public static double Score(string s)
        {
            double total = 0;
            foreach (var ch in s)
            {
                if (ch <= 0x7F) total += AsciiWeight;
                else if (ch >= 0x0400 && ch <= 0x04FF) total += CyrillicWeight;
                else total += DefaultWeight;
            }
            return total;
        }
    }

    /// <summary>One entry that can't be encoded into the game's charset, even after
    /// Stcm2Text.Sanitize ran. Carries everything needed to point a translator at
    /// exactly what to fix, rather than a raw .NET encoding exception.</summary>
    public class EncodingIssue
    {
        public int FileIndex;
        public int ActionIndex;
        public int ChunkIndex;
        public EntryKind Kind;
        public string Text = "";
        public int BadCharIndex;
        public char BadChar;

        public string Describe()
        {
            string charDesc = char.IsControl(BadChar) || char.IsWhiteSpace(BadChar)
                ? $"U+{(int)BadChar:X4}"
                : $"'{BadChar}' (U+{(int)BadChar:X4})";
            string kindStr = Kind == EntryKind.Dialogue ? "Dialogue" : "Nameplate";
            return $"File {FileIndex}, {kindStr}: character {charDesc} at position {BadCharIndex} in \"{Text}\"";
        }
    }

    public class Stcm2EncodingException : Exception
    {
        public List<EncodingIssue> Issues { get; }

        public Stcm2EncodingException(List<EncodingIssue> issues)
            : base(BuildMessage(issues))
        {
            Issues = issues;
        }

        private static string BuildMessage(List<EncodingIssue> issues) =>
            $"{issues.Count} line(s) contain a character that can't be encoded into the game's text format:\n\n" +
            string.Join("\n", issues.Select(i => i.Describe()));
    }

    public class Stcm2Project
    {
        public byte[] ContainerHeader = Array.Empty<byte>();
        public List<Stcm2File> Files = new();
        public string? SourcePath;

        public static Stcm2Project Load(string scriptUniPath)
        {
            var data = File.ReadAllBytes(scriptUniPath);
            var (header, slots) = Uni2Script.Split(data);
            var proj = new Stcm2Project { ContainerHeader = header, SourcePath = scriptUniPath };
            foreach (var slot in slots)
                proj.Files.Add(Stcm2File.Parse(slot));
            return proj;
        }

        /// <summary>Checks every translated (not just changed-this-session) entry
        /// for characters that can't survive a round trip through the game's
        /// Shift-JIS-based text format. Call this before Save() to get a full,
        /// precise list of problems instead of an exception from deep inside
        /// Serialize() naming only the first one it happens to hit.</summary>
        public List<EncodingIssue> Validate()
        {
            var issues = new List<EncodingIssue>();
            foreach (var e in EnumerateEntries())
            {
                if (e.Translation == null) continue; // untouched original JP text is already known-valid
                if (!Stcm2Text.TryEncodeSjis(e.Translation, out _, out var badIndex, out var badChar))
                {
                    issues.Add(new EncodingIssue
                    {
                        FileIndex = e.FileIndex,
                        ActionIndex = e.ActionIndex,
                        ChunkIndex = e.ChunkIndex,
                        Kind = e.Kind,
                        Text = e.Translation,
                        BadCharIndex = badIndex,
                        BadChar = badChar
                    });
                }
            }
            return issues;
        }

        public void Save(string outPath)
        {
            var issues = Validate();
            if (issues.Count > 0) throw new Stcm2EncodingException(issues);

            var rebuilt = new List<byte[]>();
            foreach (var f in Files) rebuilt.Add(f.Serialize());
            // validate before touching disk: re-parse everything first
            foreach (var b in rebuilt) Stcm2File.Parse(b);
            var joined = Uni2Script.Join(ContainerHeader, rebuilt);
            File.WriteAllBytes(outPath, joined);
        }

        public IEnumerable<TranslatableEntry> EnumerateEntries()
        {
            for (int fi = 0; fi < Files.Count; fi++)
            {
                var file = Files[fi];
                for (int ai = 0; ai < file.Actions.Count; ai++)
                {
                    var action = file.Actions[ai];
                    if (action.Call) continue;
                    EntryKind? kind = action.Opcode switch
                    {
                        Opcodes.DialogueLine => EntryKind.Dialogue,
                        Opcodes.Nameplate => EntryKind.Nameplate,
                        _ => (EntryKind?)null
                    };
                    if (kind == null) continue;

                    for (int ci = 0; ci < action.Chunks.Count; ci++)
                    {
                        var chunk = action.Chunks[ci];
                        if (!chunk.IsText) continue;
                        yield return new TranslatableEntry
                        {
                            FileIndex = fi,
                            ActionIndex = ai,
                            ChunkIndex = ci,
                            Kind = kind.Value,
                            Original = chunk.Text ?? "",
                            Translation = chunk.EditedText
                        };
                    }
                }
            }
        }

        public void SetTranslation(TranslatableEntry entry, string? text)
        {
            var sanitized = text == null ? null : Stcm2Text.Sanitize(text);
            Files[entry.FileIndex].SetTranslation(entry.ActionIndex, entry.ChunkIndex, sanitized);
        }
    }
}
