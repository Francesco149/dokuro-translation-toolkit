using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace DokuroScript.Core
{
    /// <summary>
    /// Human-editable plain text interchange format, one block per translatable
    /// string, e.g.:
    ///
    ///   @11.5.0 dialogue
    ///   JP: とにもかくにも、僕はソフトボール投げに参加
    ///   RU: 
    ///
    ///   @11.5.1 dialogue
    ///   JP: することに決めました。
    ///   RU: 
    ///
    /// "@file.action.chunk" is a stable ID (matches Stcm2Project indices for THIS
    /// export - do not reorder/reimport against a different SCRIPT.UNI). Only the
    /// RU: line is read back in; everything else is context. Leave RU blank to
    /// keep the Japanese text as-is. This is intentionally dead simple so it can
    /// be edited in Notepad, diffed in git, or batch find-and-replaced.
    /// </summary>
    public static class TextDump
    {
        public static string Export(Stcm2Project project)
        {
            var sb = new StringBuilder();
            sb.AppendLine("# Dokuro-chan script translation dump");
            sb.AppendLine("# Only edit the RU: lines. Do not add or remove @blocks.");
            sb.AppendLine("# Overflowing the dialogue box CRASHES the game - the tool will warn you,");
            sb.AppendLine("# but always test anything close to the limit in an emulator.");
            sb.AppendLine();

            foreach (var e in project.EnumerateEntries())
            {
                sb.Append('@').Append(e.FileIndex).Append('.').Append(e.ActionIndex).Append('.').Append(e.ChunkIndex);
                sb.Append(' ').Append(e.Kind == EntryKind.Dialogue ? "dialogue" : "nameplate");
                sb.AppendLine();
                sb.Append("JP: ").AppendLine(Escape(e.Original));
                sb.Append("RU: ").AppendLine(Escape(e.Translation ?? ""));
                sb.AppendLine();
            }
            return sb.ToString();
        }

        public static void Import(Stcm2Project project, string text)
        {
            // index entries by id for quick lookup
            var byId = new Dictionary<(int, int, int), TranslatableEntry>();
            foreach (var e in project.EnumerateEntries())
                byId[(e.FileIndex, e.ActionIndex, e.ChunkIndex)] = e;

            (int, int, int)? current = null;
            using var reader = new StringReader(text);
            string? line;
            while ((line = reader.ReadLine()) != null)
            {
                if (line.StartsWith("@"))
                {
                    var spaceIdx = line.IndexOf(' ');
                    var idPart = spaceIdx >= 0 ? line.Substring(1, spaceIdx - 1) : line.Substring(1);
                    var parts = idPart.Split('.');
                    if (parts.Length == 3 &&
                        int.TryParse(parts[0], out var fi) &&
                        int.TryParse(parts[1], out var ai) &&
                        int.TryParse(parts[2], out var ci))
                    {
                        current = (fi, ai, ci);
                    }
                    else
                    {
                        current = null;
                    }
                }
                else if (line.StartsWith("RU:") && current != null)
                {
                    var value = Unescape(line.Substring(3).TrimStart(' '));
                    if (byId.TryGetValue(current.Value, out var entry))
                    {
                        project.SetTranslation(entry, value.Length == 0 ? null : value);
                    }
                }
            }
        }

        // Dialogue text can't contain literal newlines in this format's line-based
        // grammar (JP:/RU: are always one line) - the game encodes multi-line
        // boxes as separate 0x118 actions anyway (see EntryKind.Dialogue docs),
        // so a single chunk's text is always one line already. This escaping only
        // guards against stray \r/\n sneaking in from a paste.
        private static string Escape(string s) => s.Replace("\r", "\\r").Replace("\n", "\\n");
        private static string Unescape(string s) => s.Replace("\\r", "\r").Replace("\\n", "\n");
    }
}
