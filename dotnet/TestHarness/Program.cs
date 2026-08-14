// Regression test suite for DokuroScript.Core. Run this after ANY change to
// the Core library before trusting it - see docs/ORIENTATION.md section 8.
// Point scriptPath at a real UNION/SCRIPT.UNI extracted from the game ISO.

using System;
using System.IO;
using DokuroScript.Core;

var scriptPath = args.Length > 0 ? args[0] : "/mnt/user-data/uploads/SCRIPT.UNI";
if (!File.Exists(scriptPath))
{
    Console.WriteLine($"SCRIPT.UNI not found at {scriptPath}. Pass its path as the first argument.");
    return 1;
}

var data = File.ReadAllBytes(scriptPath);
var (header, slots) = Uni2Script.Split(data);
Console.WriteLine($"Split into {slots.Count} slots, header={header.Length} bytes");

// --- Test 1: every chunk parses and re-serializes byte-identical when untouched ---
int ok = 0, fail = 0, totalStrings = 0;
for (int i = 0; i < slots.Count; i++)
{
    try
    {
        var stcm2 = Stcm2File.Parse(slots[i]);
        var rebuilt = stcm2.Serialize();

        bool match = rebuilt.Length <= slots[i].Length;
        if (match)
            for (int j = 0; j < rebuilt.Length; j++)
                if (rebuilt[j] != slots[i][j]) { match = false; break; }
        if (match)
            for (int j = rebuilt.Length; j < slots[i].Length; j++)
                if (slots[i][j] != 0) { match = false; break; } // trailing sector padding must be zero

        foreach (var _ in stcm2.EnumerateStrings()) totalStrings++;

        if (match) ok++;
        else { fail++; Console.WriteLine($"  [{i:D2}] MISMATCH rebuilt={rebuilt.Length} original_slot={slots[i].Length}"); }
    }
    catch (Exception ex)
    {
        fail++;
        Console.WriteLine($"  [{i:D2}] EXCEPTION: {ex.GetType().Name}: {ex.Message}");
    }
}
Console.WriteLine($"\n[Test 1] Untouched round-trip: {ok} OK, {fail} FAIL out of {slots.Count}");
Console.WriteLine($"[Test 1] Total translatable (any-opcode) string chunks found: {totalStrings}");

// --- Test 2: edit one string (ASCII, then Cyrillic), rebuild, reparse, verify text + stability ---
Console.WriteLine("\n[Test 2] Single-string edit round trip");
{
    foreach (var (label, newText) in new[] { ("ASCII", "hello world test this text is longer"), ("Cyrillic", "Меня зовут Сакура Кусакабэ.") })
    {
        var stcm2 = Stcm2File.Parse(slots[11]);
        Stcm2File.TranslatableString? target = null;
        foreach (var s in stcm2.EnumerateStrings())
            if (s.Original.Length > 3) { target = s; break; }
        if (target == null) throw new Exception("no candidate string found in slot 11");
        var t = target.Value;

        stcm2.SetTranslation(t.ActionIndex, t.ChunkIndex, newText);
        var rebuilt = stcm2.Serialize();
        var reparsed = Stcm2File.Parse(rebuilt);
        string roundtripped = reparsed.Actions[t.ActionIndex].Chunks[t.ChunkIndex].Text ?? "<null>";
        bool textOk = roundtripped == newText;

        var rebuilt2 = reparsed.Serialize();
        bool stable = rebuilt.Length == rebuilt2.Length;
        if (stable) for (int i = 0; i < rebuilt.Length; i++) if (rebuilt[i] != rebuilt2[i]) { stable = false; break; }

        Console.WriteLine($"  {label}: text round-trip {(textOk ? "OK" : "MISMATCH")}, re-serialize stable {(stable ? "OK" : "UNSTABLE")}");
    }
}

// --- Test 3: edit EVERY string across ALL files, rebuild, reparse, repack, re-split ---
Console.WriteLine("\n[Test 3] Full-project stress test (edit every string in every file)");
{
    int okFiles = 0, failFiles = 0;
    var rebuiltSlots = new System.Collections.Generic.List<byte[]>();
    foreach (var slot in slots)
    {
        var f = Stcm2File.Parse(slot);
        int n = 0;
        foreach (var s in f.EnumerateStrings())
            f.SetTranslation(s.ActionIndex, s.ChunkIndex, "тест " + (n++) + " ЙЦУКЕН");
        try
        {
            var rebuilt = f.Serialize();
            Stcm2File.Parse(rebuilt); // must not throw
            rebuiltSlots.Add(rebuilt);
            okFiles++;
        }
        catch (Exception ex)
        {
            failFiles++;
            Console.WriteLine("  FAIL: " + ex.Message);
        }
    }
    Console.WriteLine($"  files rebuilt+reparsed OK: {okFiles}, failed: {failFiles}");

    var joined = Uni2Script.Join(header, rebuiltSlots);
    var (h2, slots2) = Uni2Script.Split(joined);
    int reOk = 0;
    foreach (var s in slots2) { Stcm2File.Parse(s); reOk++; }
    Console.WriteLine($"  re-split+re-parsed rebuilt container: {reOk}/{slots2.Count} OK");
}

// --- Test 4: Stcm2Project + TextDump export -> hand-edit -> import -> save -> reload ---
Console.WriteLine("\n[Test 4] Project-level dump/reimport round trip");
{
    var proj = Stcm2Project.Load(scriptPath);
    int total = 0, dialogue = 0, nameplate = 0;
    foreach (var e in proj.EnumerateEntries())
    {
        total++;
        if (e.Kind == EntryKind.Dialogue) dialogue++; else nameplate++;
    }
    Console.WriteLine($"  entries (dialogue+nameplate only): total={total} dialogue={dialogue} nameplate={nameplate}");

    var dump = TextDump.Export(proj);
    var lines = dump.Split('\n');
    int edited = 0;
    for (int i = 0; i < lines.Length && edited < 3; i++)
        if (lines[i].StartsWith("RU: ")) { lines[i] = "RU: тестовый перевод " + edited; edited++; }
    var editedDump = string.Join('\n', lines);

    var proj2 = Stcm2Project.Load(scriptPath);
    TextDump.Import(proj2, editedDump);
    int applied = 0;
    foreach (var e in proj2.EnumerateEntries()) if (e.Translation != null) applied++;

    var outPath = Path.Combine(Path.GetTempPath(), "dokuro_test_rebuilt_SCRIPT.UNI");
    proj2.Save(outPath);
    var verify = Stcm2Project.Load(outPath);
    int verified = 0;
    foreach (var e in verify.EnumerateEntries()) if (e.Original.StartsWith("тестовый")) verified++;
    File.Delete(outPath);

    Console.WriteLine($"  applied {applied} translations from edited dump (expected 3)");
    Console.WriteLine($"  verified {verified} present after full save+reload (expected 3)");
}

// --- Test 5: overflow heuristic sanity check against real calibration data ---
Console.WriteLine("\n[Test 5] Overflow heuristic vs. real crash-test calibration data");
{
    Console.WriteLine($"  English 36ch dialogue (should be ~= limit {OverflowChecker.DialogueLimit}): " +
        OverflowChecker.Score("hello world test this text is longer"));
    Console.WriteLine($"  Russian 23ch dialogue (should be ~= limit {OverflowChecker.DialogueLimit}): " +
        OverflowChecker.Score("Меня зовут Сакура Кусак"));
    Console.WriteLine($"  English 22ch nameplate (should be ~= limit {OverflowChecker.NameplateLimit}): " +
        OverflowChecker.Score("this is a very long na"));
}

Console.WriteLine("\nAll tests completed.");
return 0;
