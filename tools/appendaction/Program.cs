// appendaction — build a SCRIPT.UNI with an extra 0x118/0x119 dialogue pair
// appended to a chosen chunk, keeping the chunk within its original size
// budget (the runtime container table's size field; see
// docs/FONT_AND_CRASH_INVESTIGATION.md Thread B). Long lines in the chunk are
// shortened to make room, so the ONLY change under test is the appended
// actions — the Thread-B size crash must NOT trigger.
//
// Usage: dotnet run -- <in.SCRIPT.UNI> <out.SCRIPT.UNI> <chunkIndex> <afterActionIndex> <"test text">
// Example: dotnet run -- /tmp/clean-SCRIPT.UNI /tmp/append.SCRIPT.UNI 5 11 "【テスト】追加台詞！"
//
// The appended pair: 0x118 (dialogue line with the given SJIS text) then
// 0x119 (box advance) — the naive translation-tool insertion the docs call
// "the softlock repro". If the game handles it, the test text displays and
// the scene continues; if it softlocks (black screen) the interpreter's
// action-chain handling breaks on appended actions.

using DokuroScript.Core;

if (args.Length < 6)
{
    Console.Error.WriteLine("usage: appendaction <in> <out> <chunkIdx> <afterActionIdx> <text>");
    return 1;
}
string inPath = args[0], outPath = args[1];
int chunkIdx = int.Parse(args[2]);
int afterActionIdx = int.Parse(args[3]);
string testText = args[4];
// action indexes in chunkIdx to shorten (space separated)
var shrinkActions = args[5].Split(' ', StringSplitOptions.RemoveEmptyEntries).Select(int.Parse).ToHashSet();

var proj = Stcm2Project.Load(inPath);
if (chunkIdx >= proj.Files.Count) { Console.Error.WriteLine("chunk out of range"); return 1; }
var file = proj.Files[chunkIdx];
var origSize = file.Serialize().Length;

// 1) shorten designated dialogue lines to free size budget
foreach (var ai in shrinkActions)
{
    var act = file.Actions[ai];
    var chunk = act.Chunks.FirstOrDefault(c => c.IsText);
    if (chunk == null) { Console.Error.WriteLine($"action {ai} has no text chunk"); return 1; }
    chunk.EditedText = "…"; // tiny placeholder; frees most of the line's bytes
    Console.WriteLine($"shortened action {ai} ({chunk.Text?.Length ?? 0} chars -> 1)");
}

// 2) append the test dialogue pair after afterActionIdx
var line = new ActionModel { Opcode = Opcodes.DialogueLine };
line.Chunks.Add(new DataChunk { Type = 0, IsText = true, EditedText = testText });
line.Params.Add(new Param { Kind = ParamKind.DataPointer, Value = 0 });
var adv = new ActionModel { Opcode = Opcodes.BoxAdvance };
file.Actions.InsertRange(afterActionIdx + 1, new[] { line, adv });
Console.WriteLine($"inserted 0x118+0x119 after action {afterActionIdx}");

// 3) save
proj.Save(outPath);

// 4) report sizes
var newSize = file.Serialize().Length;
Console.WriteLine($"chunk {chunkIdx}: 0x{origSize:X} -> 0x{newSize:X} (budget: <= 0x{(origSize + 15) / 16 * 16:X})");
if (newSize > (origSize + 15) / 16 * 16)
{
    Console.Error.WriteLine("WARNING: chunk grew past its padded budget — Thread-B crash will mask the softlock test");
    return 2;
}
Console.WriteLine("ok");
return 0;
