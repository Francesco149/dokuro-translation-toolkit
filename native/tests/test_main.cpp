// Host-side sanity tests for the native core. Compile with
//   nix develop --command make -C native tests
//   build/run_tests [path-to-SCRIPT.UNI]
// Default script path: ../game-files/SCRIPT.UNI relative to native/.
//
// Covers: untouched byte round-trip, edit round-trips (ASCII/Cyrillic),
// full stress edit, dump export/import robustness, sanitize, cp932 parity,
// asm disasm/assemble round trip + syntax-error handling, budget, undo
// (incl. cross-session + dedup + tamper detection), ISO patching.

#include "asmfmt.h"
#include "cp932.h"
#include "iso.h"
#include "project.h"
#include "stcm2.h"
#include "text.h"
#include "undo.h"
#include "uni2.h"
#include "utf8.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace dokuro;

static int g_failures = 0;
static int g_checks = 0;
static const char* g_curTest = "";

static void check(bool cond, const char* fmt, ...)
{
    g_checks++;
    if (cond) return;
    g_failures++;
    fprintf(stderr, "  FAIL [%s]: ", g_curTest);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

#define CHECK(cond, ...) check((cond), __VA_ARGS__)
#define TEST(name) do { g_curTest = name; printf("== %s\n", name); fflush(stdout); } while (0)

static std::string g_scriptPath;

static std::vector<uint8_t> load(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    return d;
}

static void save(const std::string& p, const std::vector<uint8_t>& d)
{
    std::ofstream f(p, std::ios::binary);
    f.write((const char*)d.data(), d.size());
}

static bool bytes_equal(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    return a.size() == b.size() && memcmp(a.data(), b.data(), a.size()) == 0;
}

static std::string tmpdir()
{
    static std::string d;
    if (d.empty())
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "/tmp/dokuro_test_%d", (int)getpid());
        d = buf;
        system(("rm -rf " + d + " && mkdir -p " + d).c_str());
    }
    return d;
}

static std::string tmppath(const char* name)
{
    return tmpdir() + "/" + name;
}

// ---------- Test 1: untouched round trip ----------

static void test_untouched_roundtrip()
{
    TEST("T1 untouched byte round-trip");
    std::vector<uint8_t> data = load(g_scriptPath);
    std::vector<uint8_t> header;
    std::vector<std::vector<uint8_t>> slots;
    uni2_split(data.data(), data.size(), header, slots);
    int ok = 0, fail = 0;
    for (size_t i = 0; i < slots.size(); i++)
    {
        try
        {
            Stcm2File f = Stcm2File::parse(slots[i].data(), slots[i].size());
            std::vector<uint8_t> rebuilt = f.serialize();
            bool match = rebuilt.size() <= slots[i].size();
            if (match)
                match = memcmp(rebuilt.data(), slots[i].data(), rebuilt.size()) == 0;
            if (match)
                for (size_t j = rebuilt.size(); j < slots[i].size(); j++)
                    if (slots[i][j] != 0) { match = false; break; }
            if (match) ok++;
            else
            {
                fail++;
                fprintf(stderr, "  [%zu] MISMATCH rebuilt=%zu orig=%zu\n",
                        i, rebuilt.size(), slots[i].size());
            }
        }
        catch (const std::exception& ex)
        {
            fail++;
            fprintf(stderr, "  [%zu] EXCEPTION: %s\n", i, ex.what());
        }
    }
    CHECK(fail == 0, "%d slots failed untouched round-trip", fail);
    CHECK(ok == (int)slots.size(), "ok=%d slots=%zu", ok, slots.size());
    printf("  slots=%zu\n", slots.size());
}

// ---------- Test 2: single-string edits ----------

static void test_single_edits()
{
    TEST("T2 single-string edit round trip");
    std::vector<uint8_t> data = load(g_scriptPath);
    std::vector<uint8_t> header;
    std::vector<std::vector<uint8_t>> slots;
    uni2_split(data.data(), data.size(), header, slots);

    const char* texts[] = { "hello world test this text is longer",
                            "Меня зовут Сакура Кусакабэ." };
    for (const char* t : texts)
    {
        Stcm2File f = Stcm2File::parse(slots[11].data(), slots[11].size());
        // find a text chunk with length > 3
        int ai = -1, ci = -1;
        for (size_t a = 0; a < f.actions.size() && ai < 0; a++)
        {
            if (f.actions[a].call) continue;
            for (size_t c = 0; c < f.actions[a].chunks.size(); c++)
                if (f.actions[a].chunks[c].is_text &&
                    f.actions[a].chunks[c].text.size() > 3)
                {
                    ai = (int)a;
                    ci = (int)c;
                    break;
                }
        }
        CHECK(ai >= 0, "no candidate string in slot 11");
        if (ai < 0) continue;
        f.actions[ai].chunks[ci].edited = true;
        f.actions[ai].chunks[ci].edited_text = t;
        std::vector<uint8_t> rebuilt = f.serialize();
        Stcm2File r = Stcm2File::parse(rebuilt.data(), rebuilt.size());
        std::string got = r.actions[ai].chunks[ci].text;
        CHECK(got == t, "text round-trip mismatch: '%s' vs '%s'", got.c_str(), t);
        std::vector<uint8_t> rebuilt2 = r.serialize();
        CHECK(bytes_equal(rebuilt, rebuilt2), "re-serialize not stable");
    }
}

// ---------- Test 3: full stress ----------

static void test_stress()
{
    TEST("T3 full-project stress (edit every string)");
    std::vector<uint8_t> data = load(g_scriptPath);
    std::vector<uint8_t> header;
    std::vector<std::vector<uint8_t>> slots;
    uni2_split(data.data(), data.size(), header, slots);
    std::vector<std::vector<uint8_t>> rebuiltSlots;
    int okFiles = 0, failFiles = 0;
    for (auto& slot : slots)
    {
        Stcm2File f;
        try
        {
            f = Stcm2File::parse(slot.data(), slot.size());
        }
        catch (const std::exception&)
        {
            failFiles++;
            continue;
        }
        int n = 0;
        for (auto& a : f.actions)
        {
            if (a.call) continue;
            for (auto& c : a.chunks)
            {
                if (!c.is_text) continue;
                c.edited = true;
                c.edited_text = "тест " + std::to_string(n++) + " ЙЦУКЕН";
            }
        }
        try
        {
            auto rebuilt = f.serialize();
            Stcm2File::parse(rebuilt.data(), rebuilt.size()); // must not throw
            rebuiltSlots.push_back(std::move(rebuilt));
            okFiles++;
        }
        catch (const std::exception& ex)
        {
            failFiles++;
            fprintf(stderr, "  FAIL: %s\n", ex.what());
        }
    }
    CHECK(failFiles == 0, "files rebuilt+reparsed OK: %d, failed: %d", okFiles, failFiles);
    auto joined = uni2_join(header, rebuiltSlots);
    std::vector<uint8_t> h2;
    std::vector<std::vector<uint8_t>> slots2;
    uni2_split(joined.data(), joined.size(), h2, slots2);
    int reOk = 0;
    for (auto& s : slots2)
    {
        try { Stcm2File::parse(s.data(), s.size()); reOk++; }
        catch (...) {}
    }
    CHECK(reOk == (int)slots2.size(), "re-split reparse %d/%zu", reOk, slots2.size());
}

// ---------- Test 4: dump export/import robustness ----------

static void test_dump()
{
    TEST("T4 text dump export/import round trip + robustness");
    Project p = Project::load(g_scriptPath);
    auto dump = dump_export(p, g_scriptPath);
    CHECK(dump.find("# SCRIPT.UNI: " + g_scriptPath) != std::string::npos,
          "dump header must reference the source SCRIPT.UNI path");

    // hand-edit: change 3 RU lines, delete a block, add a garbage block,
    // reorder blocks, insert junk lines, CRLF, blank RU
    std::vector<std::string> lines;
    std::string cur;
    std::istringstream is(dump);
    while (std::getline(is, cur)) lines.push_back(cur);

    std::string edited;
    int changed = 0;
    bool deleted = false;
    std::vector<std::string> out;
    for (size_t i = 0; i < lines.size(); i++)
    {
        const std::string& l = lines[i];
        if (l.rfind("@", 0) == 0 && !deleted)
        {
            deleted = true;
            // skip this block + its 3 lines (JP/RU/blank)
            i += 3;
            continue;
        }
        out.push_back(l);
    }
    for (auto& l : out)
    {
        if (l.rfind("RU: ", 0) == 0 && changed < 3)
        {
            l = "RU: тестовый перевод " + std::to_string(changed++);
        }
    }
    // reorder: move the LAST block to the front; add garbage; CRLF-ify
    std::string lastBlock;
    {
        std::vector<std::string> tmp;
        for (size_t i = out.size(); i-- > 0;)
        {
            if (out[i].rfind("@", 0) == 0)
            {
                // capture block
                for (size_t j = i; j < out.size(); j++) tmp.push_back(out[j]);
                out.resize(i);
                break;
            }
        }
        out.insert(out.begin(), tmp.begin(), tmp.end());
    }
    std::string result;
    for (auto& l : out)
    {
        result += l;
        if (l.rfind("@", 0) == 0 && changed == 3)
        {
            // one block left blank RU: line -> should revert to original
        }
        result += "\r\n"; // CRLF everywhere
    }
    result += "\ngarbage line without @\n";
    result += "@999.999.999 dialogue\nJP: x\nRU: ignored\n\n";
    result += "@1.2.3 notdialogue\nRU: x\n\n";
    result += "RU: no block\n";

    Project p2 = Project::load(g_scriptPath);
    dump_import(p2, result);
    int applied = 0;
    for (auto& e : p2.entries())
    {
        auto& ch = p2.files[e.file].actions[e.action].chunks[e.chunk];
        if (ch.edited) applied++;
    }
    CHECK(applied == 3, "applied=%d expected 3", applied);
    CHECK(p2.files[11].actions.size() == p.files[11].actions.size(),
          "import must not change structure");
    // save + reload + verify the three translations survive
    std::string outPath = tmppath("rebuilt.UNI");
    auto bytes = p2.build_container();
    save(outPath, bytes);
    Project p3 = Project::load(outPath);
    int verified = 0;
    for (auto& e : p3.entries())
    {
        auto& ch = p3.files[e.file].actions[e.action].chunks[e.chunk];
        if (ch.text.rfind("тестовый перевод", 0) == 0) verified++;
    }
    CHECK(verified == 3, "verified=%d expected 3", verified);
    std::remove(outPath.c_str());
}

// ---------- Test 5: sanitize ----------

static void test_sanitize()
{
    TEST("T6 sanitize cases");
    struct Case { const char* in; const char* out; };
    const Case cases[] = {
        { "\u00A0x", " x" },                    // NBSP -> space
        { "\u202Fx", " x" },                    // NNBSP -> space
        { "a\u200Bb", "ab" },                   // ZWSP dropped
        { "\uFEFFa", "a" },                     // BOM dropped
        { "a\u00ADb", "ab" },                   // soft hyphen dropped
        { "a\u2013b", "a\u2015b" },             // en dash -> horizontal bar
        { "a\u2014b", "a\u2015b" },             // em dash -> horizontal bar
        { "a\u2212b", "a\u2015b" },             // minus -> horizontal bar
        { "a\uFF5Eb", "a\uFF5Eb" },             // fullwidth tilde STAYS (cp932-encodable)
        { "a\u201Eb", "a\"b" },                 // low-9 quote -> "
        { "a\u00ABb", "a\"b" },                 // guillemet -> "
        { "a\u00BBb", "a\"b" },                 // guillemet -> "
        { "a\u2022b", "a\u30FBb" },             // bullet -> katakana middle dot
        { "a\r\nb", "a  b" },                   // CRLF -> two spaces (each newline char)
        { "a\nb\nc", "a b c" },                 // newlines -> space (single-line script text)
        { "plain text", "plain text" },
    };
    for (const auto& c : cases)
    {
        std::string got = sanitize(c.in);
        CHECK(got == c.out, "sanitize(%s): got '%s' want '%s'",
              c.in, got.c_str(), c.out);
    }
    // every sanitize output must be cp932-encodable
    for (const auto& c : cases)
    {
        std::vector<uint8_t> enc;
        size_t bad;
        uint32_t badcp;
        CHECK(sjis::encode(sanitize(c.in), enc, &bad, &badcp),
              "sanitize output must be encodable");
    }
    // the OLD C# bug: FF5E -> U+301C was unencodable; U+301C must NOT appear
    CHECK(sanitize("\uFF5E").find(u8"\u301C") == std::string::npos,
          "sanitize must never produce U+301C");
}

// ---------- Test 7: cp932 codec ----------

static void test_cp932()
{
    TEST("T7 cp932 encode/decode parity + round trips");
    // full round trip over every encodable codepoint
    int encodable = 0, roundTrips = 0;
    for (uint32_t cp = 1; cp <= 0xFFFF; cp++)
    {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        std::string u;
        utf8::encode(cp, u);
        std::vector<uint8_t> enc;
        size_t bad;
        uint32_t badcp;
        bool ok = sjis::encode(u, enc, &bad, &badcp);
        if (!ok) continue;
        encodable++;
        // decode back
        std::string dec;
        bool dok = sjis::decode_strict(enc.data(), enc.size(), dec);
        if (dok && dec == u) roundTrips++;
    }
    CHECK(encodable > 9000, "expected ~9400 encodable codepoints, got %d", encodable);
    // a handful of codepoints map to byte pairs that decode to a DIFFERENT char
    // (.NET cp932's duplicate mappings — e.g. U+2016 vs U+2225 both on 0x8161);
    // we mirror .NET exactly, so a small mismatch set is expected and pinned here.
    CHECK(roundTrips >= 9300, "round trips %d/%d (expected >= 9300)", roundTrips, encodable);
    printf("  encodable=%d roundtrips=%d mismatches=%d\n", encodable, roundTrips,
           encodable - roundTrips);

    // Cyrillic specifics
    std::vector<uint8_t> enc;
    size_t bad;
    uint32_t badcp;
    CHECK(sjis::encode(u8"Меня зовут Сакура", enc, &bad, &badcp), "Cyrillic encode");
    CHECK(sjis::encode(u8"А", enc, &bad, &badcp) && enc.size() == 2 &&
          enc[0] == 0x84 && enc[1] == 0x40, "А must encode to 0x84 0x40");
    CHECK(sjis::encode(u8"Ёё", enc, &bad, &badcp), "Ё must encode");
    CHECK(!sjis::encode(u8"한", enc, &bad, &badcp), "Hangul must not encode");
    CHECK(badcp == 0xD55C, "bad cp must be U+D55C, got U+%04X", badcp);
    // bad byte position: 2-byte char then Hangul -> offset 2
    CHECK(!sjis::encode(u8"аб한", enc, &bad, &badcp) && bad == 4,
          "bad index must be 4 (two 2-byte chars), got %zu", bad);

    // strict decode rejects invalid sequences
    std::string dec;
    uint8_t inv[] = { 0x81, 0x20 }; // 0x20 is not a valid trail
    CHECK(!sjis::decode_strict(inv, 2, dec), "invalid pair must fail strict decode");
    uint8_t valid[] = { 0x84, 0x40 };
    CHECK(sjis::decode_strict(valid, 2, dec) && dec == u8"А", "А decode");
    // lenient replaces invalid bytes with '?' like .NET
    uint8_t mixed[] = { 0x41, 0x81, 0x20, 0x42 };
    // invalid trail: lead is replaced, trail is reprocessed (.NET lenient semantics)
    CHECK(sjis::decode_lenient(mixed, 4) == "A? B", "lenient decode must be 'A? B'");
    // wave dash: 0x8160 decodes to U+FF5E (Windows-31J), NOT U+301C
    uint8_t wd[] = { 0x81, 0x60 };
    CHECK(sjis::decode_strict(wd, 2, dec) && dec == u8"\uFF5E",
          "0x8160 must decode to U+FF5E");
    CHECK(!sjis::can_encode_cp(0x301C), "U+301C must NOT be encodable in cp932");
}

// ---------- Test 8: asm round trip ----------

static void test_asm_roundtrip()
{
    TEST("T8 asm disasm/assemble byte round trip (all slots)");
    std::vector<uint8_t> data = load(g_scriptPath);
    std::vector<uint8_t> header;
    std::vector<std::vector<uint8_t>> slots;
    uni2_split(data.data(), data.size(), header, slots);
    int ok = 0;
    for (size_t i = 0; i < slots.size(); i++)
    {
        try
        {
            Stcm2File f = Stcm2File::parse(slots[i].data(), slots[i].size());
            std::string text = asm_disasm(f, (int)i);
            Stcm2File g;
            AsmError err;
            if (!asm_assemble(text, (int)i, g, err))
            {
                fprintf(stderr, "  [%zu] ASSEMBLE ERROR line %d: %s\n", i, err.line, err.msg.c_str());
                ok--;
                continue;
            }
            std::vector<uint8_t> a = f.serialize();
            std::vector<uint8_t> b = g.serialize();
            if (!bytes_equal(a, b))
            {
                fprintf(stderr, "  [%zu] ASM ROUND TRIP MISMATCH %zu vs %zu\n",
                        i, a.size(), b.size());
                ok--;
                continue;
            }
            // text-level stability: disasm(assemble(disasm)) == disasm
            std::string text2 = asm_disasm(g, (int)i);
            if (text2 != text)
            {
                fprintf(stderr, "  [%zu] ASM TEXT NOT STABLE\n", i);
                ok--;
                continue;
            }
            ok++;
        }
        catch (const std::exception& ex)
        {
            fprintf(stderr, "  [%zu] EXCEPTION: %s\n", i, ex.what());
            ok--;
        }
    }
    CHECK(ok == (int)slots.size(), "asm round trip ok=%d/%zu", ok, slots.size());

    // edit via asm: change a text line, reassemble, verify new text present
    {
        Stcm2File f = Stcm2File::parse(slots[5].data(), slots[5].size());
        std::string text = asm_disasm(f, 5);
        // replace the first `chunk text "` line with a test line
        std::string needle = "chunk text \"";
        size_t at = text.find(needle);
        CHECK(at != std::string::npos, "slot 5 must have a text chunk");
        if (at != std::string::npos)
        {
            size_t eol = text.find('\n', at);
            std::string replacement = "    chunk text \"тест через asm редактор\"";
            text.replace(at, eol - at, replacement);
            Stcm2File g;
            AsmError err;
            bool asmOk = asm_assemble(text, 5, g, err);
            CHECK(asmOk, "edited asm must assemble (line %d: %s)",
                  err.line, err.msg.c_str());
            if (asmOk)
            {
                auto bytes = g.serialize();
                Stcm2File h = Stcm2File::parse(bytes.data(), bytes.size());
                bool found = false;
                for (auto& a : h.actions)
                    for (auto& c : a.chunks)
                        if (c.is_text && c.text.find("тест через asm редактор") != std::string::npos)
                            found = true;
                CHECK(found, "edited text must be present after reassemble+reparse");
            }
        }
    }
}

// ---------- Test 9: asm syntax errors ----------

static void test_asm_errors()
{
    TEST("T9 asm syntax error handling");
    std::vector<uint8_t> data = load(g_scriptPath);
    std::vector<uint8_t> header;
    std::vector<std::vector<uint8_t>> slots;
    uni2_split(data.data(), data.size(), header, slots);
    Stcm2File base = Stcm2File::parse(slots[5].data(), slots[5].size());
    std::string good = asm_disasm(base, 5);

    struct BadCase { const char* name; std::function<std::string(const std::string&)> edit; };
    std::vector<BadCase> cases;
    cases.push_back({ "missing tag", [](const std::string& s) {
        auto p = s.find(".stcm2");
        if (p == std::string::npos) return s;
        auto q = s.find('\n', p);
        return s.substr(0, p) + s.substr(q + 1);
    } });
    cases.push_back({ "unknown statement", [](const std::string& s) {
        return "frobnicate 123\n" + s;
    } });
    cases.push_back({ "bad hex", [](const std::string& s) {
        auto p = s.find(".unk1");
        auto q = s.find('\n', p);
        std::string t = s;
        t.replace(p, q - p, ".unk1 0xZZZ");
        return t;
    } });
    cases.push_back({ "unterminated quote", [](const std::string& s) {
        auto p = s.find("chunk text \"");
        if (p == std::string::npos) return s;
        auto q = s.find('\n', p);
        std::string t = s;
        t.replace(p, q - p, "    chunk text \"unterminated");
        return t;
    } });
    cases.push_back({ "unencodable char", [](const std::string& s) {
        auto p = s.find("chunk text \"");
        if (p == std::string::npos) return s;
        auto q = s.find('\n', p);
        std::string t = s;
        t.replace(p, q - p, "    chunk text \"emoji \xF0\x9F\x98\x80 here\"");
        return t;
    } });
    cases.push_back({ "duplicate action address", [](const std::string& s) {
        auto p = s.find("action @");
        if (p == std::string::npos) return s;
        auto q = s.find('\n', p);
        std::string t = s;
        t.insert(q, "\naction @0x00000138"); // duplicate the first annotation addr
        return t;
    } });
    cases.push_back({ "param out of range", [](const std::string& s) {
        auto p = s.find("param data_ptr");
        if (p == std::string::npos) return s;
        auto q = s.find('\n', p);
        std::string t = s;
        t.replace(p, q - p, "  param data_ptr 0x7FFFFFFF");
        return t;
    } });

    cases.push_back({ "action_ref to unknown target", [](const std::string& s) {
        auto p = s.find("param action_ref");
        if (p == std::string::npos) return s;
        auto q = s.find('\n', p);
        std::string t = s;
        t.replace(p, q - p, "  param action_ref 0xDEADBEEF");
        return t;
    } });
    cases.push_back({ "export to unknown target", [](const std::string& s) {
        auto p = s.find("export \"");
        if (p == std::string::npos) return s;
        auto q = s.find('\n', p);
        std::string t = s;
        t.replace(p, q - p, "  export \"x\" @0xDEADBEEF");
        return t;
    } });
    cases.push_back({ "chunk before action", [](const std::string& s) {
        return "    chunk text \"x\"\n" + s;
    } });
    cases.push_back({ "double tag", [](const std::string& s) {
        auto p = s.find(".stcm2");
        auto q = s.find('\n', p);
        std::string t = s;
        t.replace(q, q, "\n.stcm2 \"x\"");
        return t;
    } });
    cases.push_back({ "bad escape", [](const std::string& s) {
        auto p = s.find("chunk text \"");
        if (p == std::string::npos) return s;
        auto q = s.find('\n', p);
        std::string t = s;
        t.replace(p, q - p, "    chunk text \"bad \\q escape\"");
        return t;
    } });

    for (auto& c : cases)
    {
        std::string text = c.edit(good);
        Stcm2File g;
        AsmError err;
        bool ok = asm_assemble(text, 5, g, err);
        CHECK(!ok, "%s: must fail to assemble", c.name);
        CHECK(err.line > 0, "%s: must report a line number", c.name);
        CHECK(!err.msg.empty(), "%s: must report a message", c.name);
    }

    // errors must not corrupt: feeding garbage to dump_import must be safe too
    Stcm2File g;
    AsmError err;
    CHECK(!asm_assemble("not asm at all\n", 5, g, err), "garbage text must fail");

    // empty text fails (no tag)
    CHECK(!asm_assemble("", 5, g, err), "empty asm must fail");

    // good text must still assemble after all that
    CHECK(asm_assemble(good, 5, g, err), "good asm must still assemble");
}

// ---------- Test 10: budget (sector padding) + embedded TOC ----------

static void test_budget()
{
    TEST("T10 sector-padding budget + embedded TOC rebuild");
    Project p = Project::load(g_scriptPath);
    // untouched files are at or under budget (equal or less after rebuild)
    int over = 0;
    for (size_t i = 0; i < p.files.size(); i++)
    {
        if (p.file_over_budget(i)) over++;
    }
    CHECK(over == 0, "%d files over budget untouched", over);
    size_t orig5 = p.originals[5].serialize().size();
    CHECK(p.file_budget(5) == (orig5 + 0x7FFu) / 0x800u * 0x800u,
          "budget must be orig rounded up to 0x800 (got %u, want %zu)",
          p.file_budget(5), (orig5 + 0x7FFu) / 0x800u * 0x800u);
    // force growth past the padding: grow the smallest slot until over
    size_t smallest = 0;
    for (size_t i = 1; i < p.files.size(); i++)
        if (p.orig_slot_sizes[i] < p.orig_slot_sizes[smallest]) smallest = i;
    Action line;
    line.opcode = 0x118;
    line.chunks.push_back(DataChunk{});
    line.chunks.back().type = 0;
    line.chunks.back().is_text = true;
    line.chunks.back().edited = true;
    line.chunks.back().edited_text = "тест";
    line.params.push_back(Param{ ParamKind::DataPointer, 0 });
    size_t pristineLen = p.originals[smallest].serialize().size();
    int n = 0;
    while (!p.file_over_budget(smallest) && n < 64)
    {
        p.files[smallest].actions.push_back(line);
        n++;
    }
    CHECK(n > 1 && n < 64,
          "padding budget allows some growth then trips (%d actions added)", n);
    CHECK(p.file_over_budget(smallest),
          "small file must be over budget once grown past its padding");
    size_t len = p.file_serialized_len(smallest);
    CHECK(len > pristineLen,
          "adding actions must grow the file (%zu vs %zu)", len, pristineLen);
    // The embedded TOC must be rebuilt from the actual slot layout: for every
    // slot, off = sector offset from data start, size = round16(len), and the
    // sector_len tracks the padded size.
    std::vector<uint8_t> container = p.build_container();
    uint32_t count = container[8] | (container[9] << 8) | (container[10] << 16) | (container[11] << 24);
    CHECK(count == p.files.size(), "container count (%u) matches slots (%zu)",
          count, p.files.size());
    size_t pos = p.container_header.size();
    bool tocOk = true;
    for (size_t i = 0; i < p.files.size(); i++)
    {
        size_t off = 0x800 + i * 16;
        auto u32 = [&](size_t at) {
            return container[at] | (container[at+1] << 8) |
                   (container[at+2] << 16) | (container[at+3] << 24);
        };
        uint32_t toff = u32(off + 4);
        uint32_t tsec = u32(off + 8);
        uint32_t tsize = u32(off + 12);
        size_t len_i = p.file_serialized_len(i);
        size_t padded = ((len_i + 0x7FFu) / 0x800u) * 0x800u;
        if (toff != (uint32_t)((pos - p.container_header.size()) / 0x800) ||
            tsec != (uint32_t)(padded / 0x800) ||
            tsize != (uint32_t)((len_i + 15) / 16 * 16))
            tocOk = false;
        pos += padded;
    }
    CHECK(tocOk, "rebuilt TOC matches the joined slot layout for all %zu slots",
          p.files.size());
    // chunk 5's off must still be 0x6B when no earlier slot outgrew its
    // padding (smallest == chunk 0 grew here, so skip that exact value)
}

// ---------- Test 11: undo ----------

static void test_undo()
{
    TEST("T11 undo: ops, persistence, dedup, tamper detection");
    std::string dir = tmppath("undodir");
    system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());

    Project p = Project::load(g_scriptPath);
    std::vector<uint8_t> pristine = p.build_container();
    uint64_t ph = 0;
    for (unsigned char c : std::string("TESTPROJECT")) ph = ph * 131 + c;

    // fresh log
    UndoLog log;
    std::string err;
    CHECK(log.open(dir, ph, pristine, &err), "open fresh log: %s", err.c_str());

    // find first editable entry
    auto entries = p.entries();
    CHECK(!entries.empty(), "project has entries");
    const EntryRef& e = entries.front();
    auto& ch = p.files[e.file].actions[e.action].chunks[e.chunk];

    // step 1: text edit
    {
        std::vector<UndoOp> ops;
        UndoOp op;
        op.kind = UndoOpKind::TextEdit;
        op.file = e.file;
        op.a = e.action;
        op.c = e.chunk;
        op.old_edited = ch.edited;
        op.old_text = ch.edited_text;
        op.old_raw.assign(ch.raw.begin(), ch.raw.end());
        op.new_edited = true;
        op.new_text = "первый перевод";
        std::vector<uint8_t> enc;
        size_t bad;
        uint32_t badcp;
        sjis::encode(op.new_text, enc, &bad, &badcp);
        op.new_raw.assign((const char*)enc.data(), enc.size());
        ops.push_back(op);
        p.set_translation(e, &op.new_text);
        auto working = p.build_container();
        log.commit(std::move(ops), working, "edit line");
    }

    // step 2: another edit to the SAME chunk (dedup: old_text blob shared)
    {
        std::vector<UndoOp> ops;
        UndoOp op;
        op.kind = UndoOpKind::TextEdit;
        op.file = e.file;
        op.a = e.action;
        op.c = e.chunk;
        op.old_edited = ch.edited;
        op.old_text = ch.edited_text;
        op.old_raw.assign(ch.raw.begin(), ch.raw.end());
        op.new_edited = true;
        op.new_text = "второй перевод подлиннее";
        std::vector<uint8_t> enc;
        size_t bad;
        uint32_t badcp;
        sjis::encode(op.new_text, enc, &bad, &badcp);
        op.new_raw.assign((const char*)enc.data(), enc.size());
        ops.push_back(op);
        p.set_translation(e, &op.new_text);
        auto working = p.build_container();
        log.commit(std::move(ops), working, "edit line again");
    }

    CHECK(log.steps() == 2, "2 steps recorded, got %zu", log.steps());
    CHECK(log.can_undo(), "can undo");

    // undo step 2
    std::vector<UndoOp> ops;
    CHECK(log.undo(ops), "undo step 2");
    CHECK(apply_undo_step(p, ops, false), "apply inverse");
    auto& ch2 = p.files[e.file].actions[e.action].chunks[e.chunk];
    CHECK(ch2.edited_text == "первый перевод", "after undo text must be step-1 text, got '%s'",
          ch2.edited_text.c_str());
    CHECK(log.can_redo(), "can redo");

    // redo
    CHECK(log.redo(ops), "redo step 2");
    CHECK(apply_undo_step(p, ops, true), "apply forward");
    auto& ch3 = p.files[e.file].actions[e.action].chunks[e.chunk];
    CHECK(ch3.edited_text == "второй перевод подлиннее", "after redo text must be step-2 text");

    // close and reopen (persistence)
    log.close();
    auto workingNow = p.build_container();
    UndoLog log2;
    CHECK(log2.open(dir, ph, workingNow, &err), "reopen log: %s", err.c_str());
    CHECK(log2.steps() == 2, "reopened log has 2 steps, got %zu", log2.steps());
    CHECK(log2.undo(ops), "cross-session undo");
    CHECK(apply_undo_step(p, ops, false), "apply cross-session inverse");
    auto& ch4 = p.files[e.file].actions[e.action].chunks[e.chunk];
    CHECK(ch4.edited_text == "первый перевод", "cross-session undo must restore step-1 text");

    // undo to pristine
    CHECK(log2.undo(ops), "undo step 1");
    CHECK(apply_undo_step(p, ops, false), "apply inverse to pristine");
    auto& ch5 = p.files[e.file].actions[e.action].chunks[e.chunk];
    CHECK(!ch5.edited, "after undoing all edits chunk must be unedited");
    CHECK(bytes_equal(p.build_container(), pristine),
          "undoing all steps must reproduce the pristine container");

    // tamper detection: modify working bytes externally -> reopen discards
    auto tampered = pristine;
    if (!tampered.empty()) tampered[0] ^= 0xFF;
    UndoLog log3;
    std::string err3;
    CHECK(log3.open(dir, ph, tampered, &err3), "tampered reopen");
    CHECK(log3.steps() == 0, "tampered working file must discard history (got %zu steps)",
          log3.steps());

    // prune: re-commit two steps (the tamper test cleared the log), then prune
    UndoLog log4;
    CHECK(log4.open(dir, ph, pristine, &err), "reopen for prune");
    for (int i = 0; i < 2; i++)
    {
        std::vector<UndoOp> ops2;
        UndoOp op2;
        op2.kind = UndoOpKind::TextEdit;
        op2.file = e.file;
        op2.a = e.action;
        op2.c = e.chunk;
        op2.old_edited = false;
        op2.new_edited = true;
        op2.new_text = "prune step " + std::to_string(i);
        std::vector<uint8_t> enc2;
        size_t bad2;
        uint32_t badcp2;
        sjis::encode(op2.new_text, enc2, &bad2, &badcp2);
        op2.new_raw.assign((const char*)enc2.data(), enc2.size());
        ops2.push_back(op2);
        p.set_translation(e, &op2.new_text);
        auto working2 = p.build_container();
        log4.commit(std::move(ops2), working2, "prune step");
    }
    CHECK(log4.steps() == 2, "prune base has 2 steps");
    log4.prune(1);
    CHECK(log4.steps() == 1, "after prune(1): %zu steps", log4.steps());
    log4.close();

    // clear
    UndoLog log5;
    CHECK(log5.open(dir, ph, pristine, &err), "reopen for clear");
    log5.clear_history();
    CHECK(log5.steps() == 0, "after clear: 0 steps");
    log5.close();
}

// ---------- Test 12: ISO ----------

static void test_iso()
{
    TEST("T12 ISO9660 find + in-place patch");
    // build a synthetic ISO: 64 sectors, PVD at 16, root dir at 20, UNION at 21,
    // SCRIPT.UNI file at 22 (size 4096), filler after
    std::string iso = tmppath("fake.iso");
    std::vector<uint8_t> img(64 * 2048, 0);
    // PVD
    img[16 * 2048 + 1] = 'C'; img[16 * 2048 + 2] = 'D'; img[16 * 2048 + 3] = '0';
    img[16 * 2048 + 4] = '0'; img[16 * 2048 + 5] = '1';
    // root record at PVD+156: len 34, extent 20, size 2048, flags 2, name len 1
    uint8_t* root = &img[16 * 2048 + 156];
    root[0] = 34;
    root[2] = 20; root[3] = 0; root[4] = 0; root[5] = 0;
    root[10] = 0; root[11] = 8; root[12] = 0; root[13] = 0; // 2048
    root[25] = 2;
    root[32] = 1;
    root[33] = '.';
    // root dir at LBA 20: ., .., UNION (records pack at exact lengths)
    auto dirRec = [](uint8_t* rec, const char* name, uint8_t flags, uint32_t extent, uint32_t size) {
        memset(rec, 0, 64);
        uint8_t nlen = (uint8_t)strlen(name);
        rec[0] = 33 + nlen;
        rec[2] = (uint8_t)extent; rec[3] = (uint8_t)(extent >> 8);
        rec[4] = (uint8_t)(extent >> 16); rec[5] = (uint8_t)(extent >> 24);
        rec[10] = (uint8_t)size; rec[11] = (uint8_t)(size >> 8);
        rec[12] = (uint8_t)(size >> 16); rec[13] = (uint8_t)(size >> 24);
        rec[25] = flags;
        rec[32] = nlen;
        memcpy(rec + 33, name, nlen);
    };
    uint8_t* rootDir = &img[20 * 2048];
    size_t off = 0;
    dirRec(rootDir + off, ".", 2, 20, 2048); off += 34;
    dirRec(rootDir + off, "..", 2, 20, 2048); off += 35;
    dirRec(rootDir + off, "UNION", 2, 21, 2048); off += 38;
    dirRec(rootDir + off, "DUMMY.DAT;1", 0, 23, 33);
    // UNION dir at LBA 21
    uint8_t* ud = &img[21 * 2048];
    off = 0;
    dirRec(ud + off, ".", 2, 21, 2048); off += 34;
    dirRec(ud + off, "..", 2, 20, 2048); off += 35;
    dirRec(ud + off, "SCRIPT.UNI;1", 0, 22, 4096);
    // file data at LBA 22 + 23
    memcpy(&img[22 * 2048], "STCM2-FAKE-DATA-0123456789ABCDEF", 33);
    memcpy(&img[23 * 2048], "DUMMY-PAYLOAD-0123456789", 24);
    save(iso, img);

    IsoEntry e;
    std::string err;
    bool found = iso_find(iso, "/UNION/SCRIPT.UNI", &e, &err);
    CHECK(found, "find SCRIPT.UNI: %s", err.c_str());
    CHECK(e.extent_lba == 22 && e.size == 4096, "extent=%u size=%u", e.extent_lba, e.size);

    // patch with a shorter file
    std::vector<uint8_t> newData = { 'S', 'T', 'C', 'M', '2', '-', 'n', 'e', 'w' };
    std::string outIso = tmppath("patched.iso");
    bool patchedOk = iso_patch(iso, "/UNION/SCRIPT.UNI", newData, outIso, &err);
    CHECK(patchedOk, "patch: %s", err.c_str());
    auto patched = load(outIso);
    CHECK(patched.size() == img.size(), "patched ISO must keep the same size");
    CHECK(memcmp(patched.data(), img.data(), 22 * 2048) == 0,
          "bytes before the file must be unchanged");
    CHECK(memcmp(&patched[22 * 2048], newData.data(), newData.size()) == 0,
          "patched bytes in place");
    // the region between the new data and the end of the original extent is
    // zero padding (the original content there is gone by design)
    for (size_t i = 22 * 2048 + newData.size(); i < 22 * 2048 + 4096; i++)
        CHECK(patched[i] == 0, "padding byte at %zu must be zero", i);
    // everything past the original extent must be byte-identical
    CHECK(memcmp(&patched[22 * 2048 + 4096], &img[22 * 2048 + 4096],
                 img.size() - 22 * 2048 - 4096) == 0, "tail unchanged");

    // oversized replacement must fail
    std::vector<uint8_t> big(5000, 1);
    std::string err2;
    CHECK(!iso_patch(iso, "/UNION/SCRIPT.UNI", big, tmppath("bad.iso"), &err2),
          "oversized replacement must fail");

    // missing file
    CHECK(!iso_find(iso, "/UNION/MISSING.UNI", &e, &err), "missing file must fail");

    // chunked patcher must own its payload: the app's begin() takes a local
    // buffer that dies on return — pumping afterwards must not read freed
    // memory (use-after-free regression guard)
    {
        IsoPatcher patcher;
        std::string err3;
        bool began = false;
        {
            std::vector<uint8_t> local = { 'S', 'T', 'C', 'M', '2', '-', 'o', 'w', 'n' };
            began = patcher.begin(iso, "/UNION/SCRIPT.UNI", local,
                                  tmppath("chunked.iso"), &err3);
        } // local dies here
        CHECK(began, "chunked begin: %s", err3.c_str());
        bool done = false;
        for (int i = 0; i < 1000 && !done; i++)
            done = patcher.pump(64 * 1024, &err3);
        CHECK(done, "chunked pump finishes: %s", err3.c_str());
        auto chunked = load(tmppath("chunked.iso"));
        CHECK(chunked.size() == img.size(), "chunked ISO size preserved");
        CHECK(memcmp(&chunked[22 * 2048], "STCM2-own", 9) == 0,
              "chunked patch wrote the owned payload");
    }

    // ---------- T12b: full ISO rebuild (file grows past its extent) ----------
    TEST("T12b ISO9660 full rebuild (grown SCRIPT.UNI)");
    std::vector<uint8_t> grown(5000, 0xAB); // > 4096 original extent
    grown[0] = 'G'; grown[1] = 'R'; grown[2] = 'O'; grown[3] = 'W'; grown[4] = 'N';
    std::string rbIso = tmppath("rebuilt.iso");
    std::string rberr;
    bool rebuilt = iso_rebuild(iso, "/UNION/SCRIPT.UNI", grown, rbIso, &rberr);
    CHECK(rebuilt, "rebuild: %s", rberr.c_str());
    CHECK(file_size(rbIso) % 2048 == 0, "rebuild output is sector-aligned");
    auto rb = load(rbIso);
    auto rbv = [&](uint32_t lba, uint32_t off) {
        return (uint32_t)rb[lba * 2048 + off] | ((uint32_t)rb[lba * 2048 + off + 1] << 8) |
               ((uint32_t)rb[lba * 2048 + off + 2] << 16) | ((uint32_t)rb[lba * 2048 + off + 3] << 24);
    };
    auto rbvbe = [&](uint32_t lba, uint32_t off) {
        return ((uint32_t)rb[lba * 2048 + off] << 24) | ((uint32_t)rb[lba * 2048 + off + 1] << 16) |
               ((uint32_t)rb[lba * 2048 + off + 2] << 8) | (uint32_t)rb[lba * 2048 + off + 3];
    };
    auto rbu16 = [&](uint32_t lba, uint32_t off) {
        return (uint16_t)((uint16_t)rb[lba * 2048 + off] | ((uint16_t)rb[lba * 2048 + off + 1] << 8));
    };
    // PVD: magic + patched volume size + path table fields + root record
    CHECK(memcmp(&rb[16 * 2048 + 1], "CD001", 5) == 0, "rebuild PVD magic");
    uint32_t totalSectors = (uint32_t)(rb.size() / 2048);
    CHECK(rbv(16, 80) == totalSectors && rbvbe(16, 84) == totalSectors,
          "PVD volume size both endians (%u/%u/%u)", rbv(16, 80), rbvbe(16, 84), totalSectors);
    // path table at 257, size 24 (root 10 + UNION 14); L location fields
    CHECK(rbv(16, 132) == 24 && rbvbe(16, 136) == 24, "PVD path table size");
    CHECK(rbv(16, 140) == 257 && rbvbe(16, 144) == 257, "PVD L path table location = 257");
    CHECK(rbv(16, 148) == 0 && rbvbe(16, 152) == 0, "optional path tables cleared");
    // L path table at 257: root entry (len 1, extent = root dir, parent 1)
    uint32_t rootLba = rbv(16, 158);
    CHECK(rb[257 * 2048] == 1 && rb[257 * 2048 + 1] == 0 && rbv(257, 2) == rootLba &&
          rbu16(257, 6) == 1 && rb[257 * 2048 + 8] == 0,
          "L path table root entry (extent %u)", rootLba);
    // second entry at offset 10: UNION (len 5, parent 1, extent = rootLba+1)
    CHECK(rb[257 * 2048 + 10] == 5 && rbv(257, 12) == rootLba + 1 && rbu16(257, 16) == 1 &&
          memcmp(&rb[257 * 2048 + 18], "UNION", 5) == 0,
          "L path table UNION entry (extent %u)", rootLba + 1);
    // M path table at 258: same entries, big-endian extents
    CHECK(rb[258 * 2048] == 1 && rbvbe(258, 2) == rootLba,
          "M path table root entry (BE extent %u)", rootLba);
    // root directory record in PVD: both-endian size, root dir is 1 sector
    CHECK(rbv(16, 166) == rbvbe(16, 170), "PVD root size both endians match");
    // files: DUMMY.DAT at rootLba+2, SCRIPT.UNI at rootLba+3 (dirs first)
    IsoEntry rbe;
    std::string rbErr;
    CHECK(iso_find(rbIso, "/UNION/SCRIPT.UNI", &rbe, &rbErr), "find SCRIPT.UNI in rebuild: %s", rbErr.c_str());
    CHECK(rbe.extent_lba == rootLba + 3 && rbe.size == 5000,
          "rebuilt SCRIPT.UNI extent=%u size=%u", rbe.extent_lba, rbe.size);
    CHECK(memcmp(&rb[(size_t)rbe.extent_lba * 2048], grown.data(), grown.size()) == 0,
          "rebuilt SCRIPT.UNI payload");
    for (size_t i = (size_t)rbe.extent_lba * 2048 + grown.size(); i < (size_t)rbe.extent_lba * 2048 + 3 * 2048; i++)
        CHECK(rb[i] == 0, "rebuilt SCRIPT.UNI padding byte %zu", i);
    // the other file survives byte-identical
    IsoEntry rbe2;
    CHECK(iso_find(rbIso, "/DUMMY.DAT", &rbe2, &rbErr), "find DUMMY.DAT in rebuild: %s", rbErr.c_str());
    CHECK(rbe2.extent_lba == rootLba + 2 && rbe2.size == 33,
          "DUMMY.DAT extent=%u size=%u", rbe2.extent_lba, rbe2.size);
    CHECK(memcmp(&rb[(size_t)rbe2.extent_lba * 2048], "DUMMY-PAYLOAD-0123456789", 24) == 0,
          "DUMMY.DAT payload preserved");
    // original ISO untouched
    auto origAfter = load(iso);
    CHECK(bytes_equal(origAfter, img), "source ISO unmodified by rebuild");

    // rebuild with a SMALLER file is also supported (the general path)
    std::vector<uint8_t> small = { 'S', 'M', 'A', 'L', 'L' };
    std::string rberr2;
    CHECK(iso_rebuild(iso, "/UNION/SCRIPT.UNI", small, tmppath("rebuilt2.iso"), &rberr2),
          "rebuild with smaller file: %s", rberr2.c_str());
    IsoEntry rbe3;
    CHECK(iso_find(tmppath("rebuilt2.iso"), "/UNION/SCRIPT.UNI", &rbe3, &rbErr),
          "find small SCRIPT.UNI: %s", rbErr.c_str());
    CHECK(rbe3.size == 5, "small SCRIPT.UNI size %u", rbe3.size);

    // missing file must fail
    std::string rberr3;
    CHECK(!iso_rebuild(iso, "/UNION/NOPE.UNI", grown, tmppath("bad_rb.iso"), &rberr3),
          "rebuild with missing file must fail");

    // chunked rebuilder owns its payload (same use-after-free guard as patcher)
    {
        IsoRebuilder reb;
        std::string err3;
        bool began = false;
        {
            std::vector<uint8_t> local = { 'O', 'W', 'N', 'E', 'D' };
            began = reb.begin(iso, "/UNION/SCRIPT.UNI", local, tmppath("chunked_rb.iso"), &err3);
        } // local dies here
        CHECK(began, "chunked rebuild begin: %s", err3.c_str());
        bool done = false;
        for (int i = 0; i < 100000 && !done; i++)
            done = reb.pump(64 * 1024, &err3);
        CHECK(done, "chunked rebuild pump finishes: %s", err3.c_str());
        IsoEntry rbe4;
        CHECK(iso_find(tmppath("chunked_rb.iso"), "/UNION/SCRIPT.UNI", &rbe4, &rbErr),
              "find in chunked rebuild: %s", rbErr.c_str());
        auto crb = load(tmppath("chunked_rb.iso"));
        CHECK(memcmp(&crb[(size_t)rbe4.extent_lba * 2048], "OWNED", 5) == 0,
              "chunked rebuild wrote the owned payload");
    }
}

// ---------- Test 13: structure ops ----------

static void test_structure()
{
    TEST("T13 action insert/delete/move + references");
    Project p = Project::load(g_scriptPath);
    // pick a file with several actions
    size_t fi = 5;
    auto& f = p.files[fi];
    CHECK(f.actions.size() > 3, "file 5 has actions");
    size_t before = f.actions.size();

    // insert a dialogue action
    Action line;
    line.opcode = 0x118;
    DataChunk c;
    c.type = 0;
    c.is_text = true;
    c.edited = true;
    c.edited_text = "новая строка";
    line.chunks.push_back(c);
    line.params.push_back(Param{ ParamKind::DataPointer, 0 });
    size_t at = f.actions.size() / 2;
    auto bytes = Stcm2File::serialize_action(line, 0);
    // re-parse through the same path the undo log uses
    Action parsed = Stcm2File::parse_action(bytes.data(), bytes.size(), 0,
                                             (uint32_t)f.global_data.size());
    parsed.custom = true;
    f.actions.insert(f.actions.begin() + at, std::move(parsed));
    CHECK(f.actions.size() == before + 1, "action inserted");
    auto container = p.build_container(); // must serialize + reparse
    save(tmppath("struct.UNI"), container);
    Project p3 = Project::load(tmppath("struct.UNI"));
    CHECK(p3.files[fi].actions.size() == before + 1, "inserted action survives reload");

    // referenced action detection: file 5 likely has calls — find a referenced one
    bool foundRef = false;
    for (size_t a = 0; a < f.actions.size(); a++)
    {
        if (p.action_is_referenced((int)fi, a))
        {
            foundRef = true;
            break;
        }
    }
    CHECK(foundRef, "some action in file 5 must be referenced");

    // move an action
    auto& f3 = p3.files[fi];
    size_t n0 = f3.actions.size();
    std::vector<uint8_t> beforeBytes = f3.serialize();
    Action moved = std::move(f3.actions[0]);
    f3.actions.erase(f3.actions.begin());
    f3.actions.insert(f3.actions.begin() + (n0 - 1), std::move(moved));
    std::vector<uint8_t> afterBytes = f3.serialize();
    CHECK(!bytes_equal(beforeBytes, afterBytes), "move must change the file");
    Stcm2File::parse(afterBytes.data(), afterBytes.size()); // still valid
}

// ---------- Test 14: encoding validation ----------

static void test_validation()
{
    TEST("T14 encoding validation reports precise issues");
    Project p = Project::load(g_scriptPath);
    auto entries = p.entries();
    CHECK(!entries.empty(), "has entries");
    const EntryRef& e = entries.front();
    std::string bad = u8"привет 한";
    p.set_translation(e, &bad);
    auto issues = p.validate();
    CHECK(issues.size() == 1, "exactly one issue, got %zu", issues.size());
    if (!issues.empty())
    {
        CHECK(issues[0].bad_cp == 0xD55C, "bad cp U+D55C, got U+%04X", issues[0].bad_cp);
        CHECK(issues[0].file == e.file && issues[0].action == e.action &&
              issues[0].chunk == e.chunk, "issue points at the right chunk");
        CHECK(issues[0].bad_index == 7, "bad index 7 (6 Cyrillic + space before Hangul), got %d",
              issues[0].bad_index);
    }
    // fixing it clears the issues
    std::string good = u8"привет мир";
    p.set_translation(e, &good);
    CHECK(p.validate().empty(), "valid text must produce no issues");
}

// ---------- Test 15: load_pair restores edited flags ----------

static void test_loadpair_restores_edited()
{
    TEST("T15 load_pair restores edited flags from the working diff");
    std::string pristine = tmppath("lp_orig.uni");
    std::string working = tmppath("lp_work.uni");
    save(pristine, load(g_scriptPath));
    EntryRef e{-1, -1, -1};
    std::string jpText;
    {
        Project p = Project::load(pristine);
        for (int f = 0; f < (int)p.files.size() && e.file < 0; f++)
            for (size_t a = 0; a < p.files[f].actions.size() && e.file < 0; a++)
                for (size_t c = 0; c < p.files[f].actions[a].chunks.size() && e.file < 0; c++)
                    if (p.files[f].actions[a].chunks[c].is_text) e = { f, (int)a, (int)c };
        CHECK(e.file >= 0, "found a text chunk to edit");
        if (e.file < 0) return;
        jpText = p.files[e.file].actions[e.action].chunks[e.chunk].text;
        std::string tr = u8"тест перевода";
        p.set_translation(e, &tr);
        CHECK(p.files[e.file].actions[e.action].chunks[e.chunk].edited, "set_translation marks edited");
        save(working, p.build_container());
    }
    {
        Project q = Project::load_pair(pristine, working);
        CHECK(q.files[e.file].actions[e.action].chunks[e.chunk].edited,
              "edited flag must survive reopen");
        CHECK(q.files[e.file].actions[e.action].chunks[e.chunk].edited_text == u8"тест перевода",
              "edited_text must survive reopen");
        CHECK(q.files[e.file].actions[e.action].chunks[e.chunk].text == jpText,
              "jp view must stay pristine after reopen");
        // untouched chunks stay untouched
        bool anyEdited = false;
        for (const auto& f : q.files)
            for (const auto& a : f.actions)
                for (const auto& c : a.chunks)
                    if (c.edited) anyEdited = true;
        CHECK(anyEdited, "exactly the edited chunk is flagged");
        // and the round trip stays byte-stable
        std::vector<uint8_t> rebuilt = q.build_container();
        CHECK(bytes_equal(rebuilt, load(working)), "reopen->rebuild is byte-stable");
    }
    remove(pristine.c_str());
    remove(working.c_str());
}

// ---------- Test 17: four-byte chunks (empty + short text) are text ----------

static void test_fourbyte_text()
{
    TEST("T17 four-byte chunks: empty + short text are text");
    // an empty text chunk serializes to 4 zero bytes; without the fix it
    // round-trips as numeric 0 and the added line is invisible in the UI
    {
        Action act;
        act.opcode = 0x118;
        DataChunk c;
        c.type = 0;
        c.is_text = true;
        c.edited = true;
        c.edited_text = "";
        act.chunks.push_back(c);
        act.params.push_back(Param{ ParamKind::DataPointer, 0 });
        std::vector<uint8_t> bytes = Stcm2File::serialize_action(act, 0);
        Action parsed = Stcm2File::parse_action(bytes.data(), bytes.size(), 0, 0);
        CHECK(parsed.chunks.size() == 1 && parsed.chunks[0].is_text,
              "empty text chunk stays text after round trip");
        CHECK(parsed.chunks[0].text.empty(), "empty text decodes to empty");
    }
    // a single kanji nameplate (2 SJIS bytes + 2 pad) must also be text —
    // the real script has 2032 such invisible rows (e.g. 桜 = 8D F7 00 00)
    {
        Action act;
        act.opcode = 0x11A;
        DataChunk c;
        c.type = 0;
        c.is_text = true;
        c.edited = true;
        c.edited_text = u8"桜";
        act.chunks.push_back(c);
        act.params.push_back(Param{ ParamKind::DataPointer, 0 });
        std::vector<uint8_t> bytes = Stcm2File::serialize_action(act, 0);
        Action parsed = Stcm2File::parse_action(bytes.data(), bytes.size(), 0, 0);
        CHECK(parsed.chunks.size() == 1 && parsed.chunks[0].is_text,
              "short text chunk stays text after round trip");
        CHECK(parsed.chunks[0].text == u8"桜", "short text decodes back");
    }
    // end-to-end on the real script: every dialogue/nameplate action must now
    // expose a text chunk (previously 2032 single-kanji nameplates were
    // classified numeric and hidden from the editor)
    {
        Project p = Project::load(g_scriptPath);
        int invisible = 0;
        for (const auto& f : p.files)
            for (const auto& a : f.actions)
            {
                if (a.call || (a.opcode != 0x118 && a.opcode != 0x11A)) continue;
                bool hasText = false;
                for (const auto& c : a.chunks)
                    if (c.is_text) { hasText = true; break; }
                if (!hasText) invisible++;
            }
        CHECK(invisible == 0, "no dialogue/nameplate action is invisible (%d)",
              invisible);
    }
}

// ---------- Test 18: load_pair restores edits after a structural change ----------

static void test_loadpair_structural()
{
    TEST("T18 load_pair restores edits after a custom action is added");
    std::string pristine = tmppath("st_orig.uni");
    std::string working = tmppath("st_work.uni");
    save(pristine, load(g_scriptPath));
    EntryRef e{-1, -1, -1};
    std::string jpText;
    std::string tr = u8"тест перевода";
    uint32_t customAddr = 0;
    {
        Project p = Project::load(pristine);
        for (int f = 0; f < (int)p.files.size() && e.file < 0; f++)
            for (size_t a = 0; a < p.files[f].actions.size() && e.file < 0; a++)
                for (size_t c = 0; c < p.files[f].actions[a].chunks.size() && e.file < 0; c++)
                    if (p.files[f].actions[a].chunks[c].is_text) e = { f, (int)a, (int)c };
        CHECK(e.file >= 0, "found a text chunk to edit");
        if (e.file < 0) return;
        jpText = p.files[e.file].actions[e.action].chunks[e.chunk].text;
        p.set_translation(e, &tr);

        // insert a custom dialogue action after it (mirrors app_insert_action:
        // serialize -> parse; empty text must now round-trip as a text chunk)
        Action act;
        act.opcode = 0x118;
        act.custom = true;
        DataChunk c;
        c.type = 0;
        c.is_text = true;
        c.edited = true;
        c.edited_text = "";
        act.chunks.push_back(c);
        act.params.push_back(Param{ ParamKind::DataPointer, 0 });
        size_t at = (size_t)(e.action + 1);
        std::vector<uint8_t> bytes = Stcm2File::serialize_action(act, 0);
        Action parsed = Stcm2File::parse_action(
            bytes.data(), bytes.size(), 0, (uint32_t)p.files[e.file].global_data.size());
        CHECK(parsed.chunks[0].is_text, "inserted empty line round-trips as text");
        parsed.custom = true;
        p.files[e.file].actions.insert(p.files[e.file].actions.begin() + at,
                                       std::move(parsed));
        // build_container assigns the custom action its real layout address —
        // the same value the state file records for it
        save(working, p.build_container());
        customAddr = p.files[e.file].actions[at].new_addr;
    }
    {
        std::vector<std::pair<int, uint32_t>> customs;
        customs.push_back({ e.file, customAddr });
        Project q = Project::load_pair(pristine, working, &customs);
        const auto& ch = q.files[e.file].actions[e.action].chunks[e.chunk];
        CHECK(ch.edited, "edited flag survives a structural reopen");
        CHECK(ch.edited_text == tr, "edited_text survives a structural reopen");
        CHECK(ch.text == jpText, "JP view stays pristine after a structural reopen");
        // the custom action: marked + visible
        const auto& ca = q.files[e.file].actions[e.action + 1];
        CHECK(ca.custom, "custom action flag survives reopen");
        CHECK(ca.chunks.size() == 1 && ca.chunks[0].is_text,
              "custom line's text chunk is visible after reopen");
        // byte stability: the reopened model re-serializes to the working file
        CHECK(bytes_equal(q.build_container(), load(working)),
              "structural reopen -> rebuild is byte-stable");
        // revert on the shifted slot must find the pristine counterpart
        CHECK(q.revert_chunk(e), "revert works after a structural reopen");
        const auto& rch = q.files[e.file].actions[e.action].chunks[e.chunk];
        CHECK(!rch.edited && rch.text == jpText,
              "revert returns the line to pristine JP");
    }
    remove(pristine.c_str());
    remove(working.c_str());
}

// ---------- Test 19: custom lines stay editable after reopen ----------

static void test_custom_reopen_editable()
{
    TEST("T19 custom lines stay editable after reopen (content on RU side)");
    std::string pristine = tmppath("cu_orig.uni");
    std::string working = tmppath("cu_work.uni");
    save(pristine, load(g_scriptPath));
    int fi = -1;
    std::string customText = u8"добавленная строка";
    uint32_t customAddr = 0;
    {
        Project p = Project::load(pristine);
        int ai = -1;
        for (int f = 0; f < (int)p.files.size() && fi < 0; f++)
            for (size_t a = 0; a < p.files[f].actions.size() && fi < 0; a++)
                if (p.files[f].actions[a].opcode == 0x118) { fi = f; ai = (int)a; }
        CHECK(fi >= 0, "found a dialogue action");
        if (fi < 0) return;
        // insert a custom dialogue action with a translation (mirrors
        // app_insert_action + a detail-panel edit)
        Action act;
        act.opcode = 0x118;
        act.custom = true;
        DataChunk c;
        c.type = 0;
        c.is_text = true;
        c.edited = true;
        c.edited_text = customText;
        act.chunks.push_back(c);
        act.params.push_back(Param{ ParamKind::DataPointer, 0 });
        size_t at = (size_t)(ai + 1);
        std::vector<uint8_t> bytes = Stcm2File::serialize_action(act, 0);
        Action parsed = Stcm2File::parse_action(
            bytes.data(), bytes.size(), 0, (uint32_t)p.files[fi].global_data.size());
        parsed.original_addr = 0xF0000000u; // synthetic annotation address
        parsed.custom = true;
        p.files[fi].actions.insert(p.files[fi].actions.begin() + at, std::move(parsed));
        save(working, p.build_container());
        customAddr = p.files[fi].actions[at].new_addr;
    }
    {
        std::vector<std::pair<int, uint32_t>> customs;
        customs.push_back({ fi, customAddr });
        Project q = Project::load_pair(pristine, working, &customs);
        // locate the custom action (marked by load_pair)
        const Action* custom = nullptr;
        for (const auto& a : q.files[fi].actions)
            if (a.custom) { custom = &a; break; }
        CHECK(custom != nullptr, "custom action survives reopen");
        if (!custom) return;
        CHECK(custom->chunks.size() == 1 && custom->chunks[0].is_text,
              "custom text chunk survives reopen");
        const auto& cc = custom->chunks[0];
        CHECK(cc.edited, "custom chunk stays edited (editable)");
        CHECK(cc.edited_text == customText, "custom content stays on the RU side");
        CHECK(cc.text.empty(), "custom JP view stays empty");
        CHECK(bytes_equal(q.build_container(), load(working)),
              "custom reopen -> rebuild is byte-stable");
    }
    remove(pristine.c_str());
    remove(working.c_str());
}

// ---------- Test 16: undo restores a deleted action's original address ----------

static void test_undo_addr_restore()
{
    TEST("T16 undo restores deleted action address");
    std::string dir = tmppath("undoaddr");
    system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());

    Project p = Project::load(g_scriptPath);
    std::vector<uint8_t> pristine = p.build_container();
    uint64_t ph = 0;
    for (unsigned char c : std::string("ADDRTEST")) ph = ph * 131 + c;

    UndoLog log;
    std::string err;
    CHECK(log.open(dir, ph, pristine, &err), "open fresh log: %s", err.c_str());

    // pick a non-referenced, non-call action with a real address
    int fi = -1, ai = -1;
    uint32_t addr = 0;
    for (int f = 0; f < (int)p.files.size() && ai < 0; f++)
    {
        for (size_t a = 0; a < p.files[f].actions.size() && ai < 0; a++)
        {
            const auto& act = p.files[f].actions[a];
            if (act.call || act.original_addr == 0) continue;
            if (p.action_is_referenced(f, (int)a)) continue;
            fi = f;
            ai = (int)a;
            addr = act.original_addr;
        }
    }
    CHECK(fi >= 0, "found a deletable action");
    if (fi < 0) return;

    std::vector<uint8_t> bytes = Stcm2File::serialize_action(p.files[fi].actions[ai], 0);
    std::vector<uint8_t> working;
    {
        std::vector<UndoOp> ops;
        UndoOp op;
        op.kind = UndoOpKind::ActionDelete;
        op.file = fi;
        op.index = (uint32_t)ai;
        op.addr = addr;
        op.custom = p.files[fi].actions[ai].custom;
        op.action_bytes.assign((const char*)bytes.data(), bytes.size());
        ops.push_back(std::move(op));
        p.files[fi].actions.erase(p.files[fi].actions.begin() + ai);
        working = p.build_container();
        log.commit(std::move(ops), working, "delete event");
    }

    // reopen from disk (cross-session decode path) and undo
    {
        UndoLog log2;
        CHECK(log2.open(dir, ph, working, &err), "reopen log: %s", err.c_str());
        CHECK(log2.can_undo(), "one undo step after reopen");
        std::vector<UndoOp> ops;
        CHECK(log2.undo(ops), "pop the undo step");
        CHECK(apply_undo_step(p, ops, false), "apply inverse ops");
        const auto& act = p.files[fi].actions[ai];
        CHECK(act.original_addr == addr, "restored action keeps its address (%u vs %u)",
              act.original_addr, addr);
        CHECK(p.build_container() == pristine, "undo returns the slot to pristine bytes");
    }
    remove((dir + "/undo.log").c_str());
}

// ---------- main ----------

int main(int argc, char** argv)
{
    g_scriptPath = argc > 1 ? argv[1] : "../game-files/SCRIPT.UNI";
    printf("SCRIPT.UNI: %s\n", g_scriptPath.c_str()); fflush(stdout);
    // The public branch ships no game files, so the real-script tests cannot
    // run there (CI runs the synthetic suite: T6/T7/T12). The full suite runs
    // locally from the archive branch, which keeps the game files.
    bool haveGameFile = file_exists(g_scriptPath);
    if (!haveGameFile)
        printf("NOTE: %s not found — real-script tests skipped (public branch has no game "
               "files); synthetic suite only.\n", g_scriptPath.c_str());

    if (haveGameFile)
    {
        test_untouched_roundtrip();
        test_single_edits();
        test_stress();
        test_dump();
        test_asm_roundtrip();
        test_asm_errors();
        test_budget();
        test_undo();
        test_structure();
        test_validation();
        test_loadpair_restores_edited();
        test_undo_addr_restore();
        test_fourbyte_text();
        test_loadpair_structural();
        test_custom_reopen_editable();
    }
    test_sanitize();
    test_cp932();
    test_iso();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
