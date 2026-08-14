// Headless self-test mode: --selftest <script.uni> [tmpdir]
// Exercises the core through the REAL mingw-built binary (CRT, filesystem,
// UTF-8 paths, miniz, cp932 tables) without a UI. Exits 0 on success.
#include "app.h"
#include "core/asmfmt.h"
#include "core/cp932.h"
#include "core/project.h"
#include "core/text.h"
#include "core/uni2.h"
#include "core/utf8.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

using PerfClock = std::chrono::steady_clock;
static double ms_since(PerfClock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(PerfClock::now() - t0).count();
}
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace dokuro;

namespace {

int g_fails = 0;
int g_checks = 0;

void check(bool cond, const char* fmt, ...)
{
    g_checks++;
    if (cond) return;
    g_fails++;
    fprintf(stderr, "  FAIL: ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

bool equal(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    return a.size() == b.size() && memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace

int run_selftest(int argc, char** argv)
{
    const char* scriptPath = argc > 1 ? argv[1] : nullptr;
    const char* tmp = argc > 2 ? argv[2] : "selftest_tmp";
    if (!scriptPath)
    {
        fprintf(stderr, "usage: dokuro-editor.exe --selftest <SCRIPT.UNI> [tmpdir]\n");
        return 2;
    }
    // GUI-subsystem binary: attach stdout/stderr to a log file in the tmp dir
    std::string logPath = std::string(tmp) + "/selftest.log";
    {
        FILE* marker = fopen(logPath.c_str(), "ab");
        if (marker)
        {
            fputs("started\n", marker);
            fclose(marker);
        }
        FILE* f = freopen(logPath.c_str(), "w", stdout);
        if (!f) f = freopen("selftest.log", "w", stdout); // fallback: cwd
        if (f) *stderr = *stdout;
    }
    printf("selftest: %s\n", scriptPath);
    printf("argc=%d tmp='%s'\n", argc, tmp);
    fflush(stdout);

    // 1. parse + untouched round trip
    try
    {
        PerfClock::time_point tLoad0 = PerfClock::now();
        Project p = Project::load(scriptPath);
        double tLoad = ms_since(tLoad0);
        printf("perf: Project::load=%.1fms\n", tLoad);
        check(tLoad < 10000.0, "perf: load %.0fms exceeds 10s threshold", tLoad);
        check(p.files.size() == 35, "expected 35 slots, got %zu", p.files.size());
        // byte-exact: re-serialize from a fresh parse equals the slot bytes
        int ok = 0;
        std::vector<uint8_t> data = read_file(scriptPath);
        std::vector<uint8_t> hdr;
        std::vector<std::vector<uint8_t>> slots;
        uni2_split(data.data(), data.size(), hdr, slots);
        for (size_t i = 0; i < slots.size(); i++)
        {
            Stcm2File f = Stcm2File::parse(slots[i].data(), slots[i].size());
            std::vector<uint8_t> rebuilt = f.serialize();
            bool match = rebuilt.size() <= slots[i].size() &&
                         memcmp(rebuilt.data(), slots[i].data(), rebuilt.size()) == 0;
            if (match)
                for (size_t j = rebuilt.size(); j < slots[i].size(); j++)
                    if (slots[i][j] != 0) { match = false; break; }
            if (match) ok++;
            else fprintf(stderr, "  slot %zu round trip MISMATCH\n", i);
        }
        check(ok == (int)slots.size(), "untouched round trip %d/%zu", ok, slots.size());

        // 1b. perf: the UI hot paths (still pristine model). These ran PER
        // FRAME before the slot_lens cache — serialization is far too slow
        // for that. Thresholds are generous (slow CI / Win7) but catch
        // O(n^2)-style regressions.
        {
            PerfClock::time_point t1 = PerfClock::now();
            std::vector<uint8_t> container = p.build_container();
            double tContainer = ms_since(t1);
            printf("perf: build_container=%.1fms (%zu bytes)\n", tContainer, container.size());
            check(tContainer < 5000.0, "perf: build_container %.0fms exceeds 5s", tContainer);

            PerfClock::time_point t2 = PerfClock::now();
            size_t totalLen = 0;
            for (size_t i = 0; i < p.files.size(); i++)
                totalLen += p.file_serialized_len(i);
            double tLens = ms_since(t2);
            printf("perf: %zu x file_serialized_len=%.1fms (%zu bytes)\n",
                   p.files.size(), tLens, totalLen);
            check(tLens < 3000.0, "perf: slot lengths %.0fms exceeds 3s", tLens);

            PerfClock::time_point t3 = PerfClock::now();
            std::string dump = dump_export(p, scriptPath);
            double tDump = ms_since(t3);
            printf("perf: dump_export=%.1fms (%zu bytes)\n", tDump, dump.size());
            check(tDump < 5000.0, "perf: dump %.0fms exceeds 5s", tDump);
        }

        // 2. budget (pristine model — must run before any edits mutate slots)
        {
            int over = 0;
            for (size_t i = 0; i < p.files.size(); i++)
                if (p.file_over_budget(i)) over++;
            check(over == 0, "%d slots over budget untouched", over);
        }

        // 3. asm round trip on the PRISTINE model (first 6 slots + slot 5)
        int asmOk = 0;
        for (int i = 0; i < (int)p.files.size() && i < 6; i++)
        {
            Stcm2File f = p.files[i];
            std::string text = asm_disasm(f, i);
            Stcm2File g;
            AsmError err;
            if (!asm_assemble(text, i, g, err))
            {
                fprintf(stderr, "  asm slot %d assemble error line %d: %s\n", i, err.line, err.msg.c_str());
                continue;
            }
            if (equal(f.serialize(), g.serialize())) asmOk++;
            else fprintf(stderr, "  asm slot %d round trip MISMATCH\n", i);
        }
        {
            Stcm2File f = p.files[5];
            std::string text = asm_disasm(f, 5);
            Stcm2File g;
            AsmError err;
            if (asm_assemble(text, 5, g, err) && equal(f.serialize(), g.serialize())) asmOk++;
            else fprintf(stderr, "  asm slot 5 (2nd) round trip MISMATCH\n");
        }
        check(asmOk == 7, "asm round trip %d/7", asmOk);

        // 4. Cyrillic edit round trip
        auto entries = p.entries();
        check(!entries.empty(), "project has entries");
        if (!entries.empty())
        {
            const EntryRef& e = entries.front();
            std::string t = "Меня зовут Сакура Кусакабэ.";
            p.set_translation(e, &t);
            std::vector<uint8_t> rebuilt = p.files[e.file].serialize();
            Stcm2File r = Stcm2File::parse(rebuilt.data(), rebuilt.size());
            check(r.actions[e.action].chunks[e.chunk].text == t,
                  "Cyrillic round trip mismatch");
        }

        // 4. dump header + import
        std::string dump = dump_export(p, scriptPath);
        check(dump.find(std::string("# SCRIPT.UNI: ") + scriptPath) != std::string::npos,
              "dump header must reference the script path");
        Project p2 = Project::load(scriptPath);
        dump_import(p2, dump); // no-op import must not crash


        // 6. undo round trip through a real log
        std::string dir = std::string(tmp);
#ifdef _WIN32
        _mkdir(dir.c_str()); // ignore "already exists"
        remove((dir + "/undo.log").c_str());
#else
        mkdir(dir.c_str(), 0755);
        remove((dir + "/undo.log").c_str());
#endif
        UndoLog log;
        std::string err;
        std::vector<uint8_t> pristine = p.build_container();
        uint64_t ph = 0;
        for (unsigned char c : std::string("SELFTEST")) ph = ph * 131 + c;
        check(log.open(dir, ph, pristine, &err), "undo open: %s", err.c_str());
        if (!entries.empty())
        {
            const EntryRef& e = entries.front();
            auto& ch = p.files[e.file].actions[e.action].chunks[e.chunk];
            UndoOp op;
            op.kind = UndoOpKind::TextEdit;
            op.file = e.file;
            op.a = e.action;
            op.c = e.chunk;
            op.old_edited = ch.edited;
            op.old_text = ch.edited_text;
            op.old_raw.assign(ch.raw.begin(), ch.raw.end());
            op.new_edited = true;
            op.new_text = "селфтест строка";
            std::vector<uint8_t> enc;
            size_t bad;
            uint32_t badcp;
            sjis::encode(op.new_text, enc, &bad, &badcp);
            op.new_raw.assign((const char*)enc.data(), enc.size());
            p.set_translation_raw(e, true, op.new_text);
            std::vector<UndoOp> ops;
            ops.push_back(std::move(op));
            log.commit(std::move(ops), p.build_container(), "selftest edit");
            std::vector<UndoOp> popped;
            check(log.undo(popped), "undo pop");
            check(apply_undo_step(p, popped, false), "undo apply");
            check(p.files[e.file].actions[e.action].chunks[e.chunk].edited_text == "Меня зовут Сакура Кусакабэ.",
                  "undo must restore the pre-commit text");
        }
        log.close();

        // 7. sanitize + encoding validation
        check(sanitize("\u00A0") == " ", "NBSP sanitize");
        check(sanitize("\u2014") == "\u2015", "em dash sanitize");
        {
            std::string badText = u8"привет 한";
            p.set_translation(entries.front(), &badText);
            auto issues = p.validate();
            check(issues.size() == 1 && issues[0].bad_cp == 0xD55C,
                  "validation must catch Hangul (got %zu issues)", issues.size());
        }
        printf("selftest: %d checks, %d failures\n", g_checks, g_fails);
    }
    catch (const std::exception& ex)
    {
        fprintf(stderr, "selftest EXCEPTION: %s\n", ex.what());
        return 1;
    }
    return g_fails == 0 ? 0 : 1;
}
