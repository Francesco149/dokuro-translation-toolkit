// Headless UI test driver (see uitest.h). Runs the scripted regression inside
// the real render loop: app-level document ops, filter states, synthetic
// clicks via posted WM_ messages, framebuffer dumps, and invariant checks.
#include "app.h"
#include "uitest.h"
#include "core/vendor/miniz.h"
#include <imgui.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace dokuro;

int g_activeTab = 0;
std::string g_pendingDump;

namespace {

App* g_app = nullptr;
HWND g_hwnd = nullptr;
std::string g_outdir;

int g_checks = 0, g_fails = 0;
int g_step = 0;
int g_framesLeft = 0;
bool g_clickDown = false;
int g_clickX = 0, g_clickY = 0;
uint64_t g_perfStart = 0;
int g_perfLeft = 0;
int g_enterIdx = -1;       // row index before the Enter press
bool g_keyEnter = false;   // Enter press pending (feed down, then up next frame)
bool g_keyEnterHeld = false;

void UCHECK(bool ok, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s | %s\n", ok ? "PASS" : "FAIL", buf);
    g_checks++;
    if (!ok) g_fails++;
}

// ---------- step language ----------

struct Step
{
    enum Kind
    {
        Slot,             // i1 = slot index
        SlotFirstDialogue,// select the first slot containing dialogue lines
        Edit,             // i1=file i2=action i3=chunk, s1=text (-1 = entries().front())
        AddLine,          // i1=file i2=after, i3=opcode (-1 = front entry)
        Move,             // i1=file i2=from i3=to (-1 = front entry's file)
        Delete,           // i1=file i2=action (-1 = front entry's file)
        Undo, Redo,
        Asm,              // i1=file (-1 = front entry's file)
        Filter,           // i1=kind
        Frames,           // i1 = frames to pump
        Click,            // i1=x i2=y (client coords, io injection)
        ForceTab,         // i1 = tab to select via the public API (1 asm, 2 dialogue)
        CheckTab,         // i1 = expected active tab
        Dump,             // s1 = file name (PNG written to outdir)
        Budget,           // every slot within its size budget
        RowsCheck,        // i1 = kind filter value (rows must match)
        ExportImport,     // dump export -> import round trip (no-op on unchanged)
        AsmJpDump,        // prepend s1 to the asm buffer (JP font check)
        RuBufferSel,      // select the front entry and render (i1/i2 -1 resolve)
        RuBufferAdd,      // insert a dialogue line after the front entry and
                          //   select it (regression: panel must not keep the
                          //   previously selected line's text)
        RuBufferCheck,    // s1 = expected detail-panel RU buffer content
        RuClick,          // click the RU field at its recorded client coords
        RuEnterBegin,     // snapshot the current row index (Enter-advance check)
        RuType,           // s1 = chars to type into the focused RU field
        RuPendingCheck,   // s1: typing is live (pending set, model NOT committed)
        RuKeyEnter,       // press Enter in the RU field (save + next line)
        RuEnterCheck,     // Enter must commit s1 and advance one visible row
        AsmStale,         // i1 = expected stale flag (0/1) for the direct
                          //   script buffer vs. the model
        AsmEditCheck,     // the edited translation must survive an asm round trip
        Reopen,           // close + reopen the project; i1 = file (-1 = front)
                          //   — edits, custom flags, and undo must survive
        RuDebounce,       // simulate a typed RU edit (i1=file i2=action
                          //   i3=chunk, s1=text, -1s resolve to front entry):
                          //   sets the pending edit 500 ms in the past and
                          //   pumps 3 frames — app_tick must commit it
        RuCheck,          // i1/i2/i3/s1 like RuDebounce: edited_text must match
        Perf,             // pump 120 frames; avg frame time must be < 25 ms
                          //   (catches the per-frame slot-serialization 33fps)
    };
    Kind kind;
    int i1 = 0, i2 = 0, i3 = 0;
    std::string s1;
};

// Resolves -1 file/action indexes to the project's first text entry (the
// first dialogue line; slot indexes are NOT 0-based dialogue slots — the
// first dialogue slot in the real script is 5). Either pointer may be NULL
// (callers that only need the file).
static void front_entry(App& a, int* fi, int* ai)
{
    auto es = a.proj.entries();
    if (!es.empty())
    {
        if (fi && *fi < 0) *fi = es.front().file;
        if (ai && *ai < 0) *ai = (int)es.front().action;
    }
}

static Step S(Step::Kind k) { Step s; s.kind = k; return s; }

// The standard regression script. Runs against the pristine SCRIPT.UNI in an
// isolated project under outdir — every op is reversible or net-zero so the
// final budget invariant holds.
static std::vector<Step> make_script()
{
    std::vector<Step> v;
    v.push_back(S(Step::Frames)); v.back().i1 = 30;          // settle (fonts, layout)
    v.push_back(S(Step::Dump));   v.back().s1 = "01-editor";
    v.push_back(S(Step::SlotFirstDialogue));
    v.push_back(S(Step::Filter)); v.back().i1 = 1;           // dialogue (118+119+11A)
    v.push_back(S(Step::Frames)); v.back().i1 = 2;
    v.push_back(S(Step::RowsCheck)); v.back().i1 = 1;        // advance rows included
    v.push_back(S(Step::Dump));   v.back().s1 = "02-filter-dialogue";
    v.push_back(S(Step::Filter)); v.back().i1 = 0;
    v.push_back(S(Step::Frames)); v.back().i1 = 1;

    // tab switching via the public SetSelected API (headless mouse clicks
    // can't select unselected tabs in this vendored TabBar)
    v.push_back(S(Step::ForceTab)); v.back().i1 = 1;
    v.push_back(S(Step::Frames)); v.back().i1 = 2;
    v.push_back(S(Step::CheckTab)); v.back().i1 = 1;
    // put a JP line at the top of the asm buffer so the dump proves the mono
    // font renders Japanese (the real buffer's JP lines are far down)
    v.push_back(S(Step::AsmJpDump)); v.back().s1 =
        u8"chunk text \"――これは日常も僕もまとめてコナゴナにして…♪\"";
    v.push_back(S(Step::Dump));   v.back().s1 = "06-asm-jp";
    v.push_back(S(Step::ForceTab)); v.back().i1 = 2;
    v.push_back(S(Step::Frames)); v.back().i1 = 2;
    v.push_back(S(Step::CheckTab)); v.back().i1 = 0;
    v.push_back(S(Step::Dump));   v.back().s1 = "03-after-tab-switch";

    // document ops on the first text entry (slot 5 in the real script);
    // -1 file/action indexes resolve to entries().front()
    v.push_back(S(Step::Edit));   v.back().i1 = -1; v.back().i2 = -1; v.back().s1 = "Тест.";
    v.push_back(S(Step::AsmStale)); v.back().i1 = 1; // dialogue edit: buffer is stale
    v.push_back(S(Step::Dump));   v.back().s1 = "04-edited";
    v.push_back(S(Step::AddLine)); v.back().i1 = -1; v.back().i2 = -1; v.back().i3 = 0x118;
    // regression: after adding a line the detail panel must show the NEW
    // (empty) line, not the previously selected line's text
    v.push_back(S(Step::RuBufferSel)); v.back().i1 = -1; v.back().i2 = -1;
    v.push_back(S(Step::Dump));   v.back().s1 = "07-detail-panel"; // RU field visible
    v.push_back(S(Step::RuBufferCheck)); v.back().s1 = u8"Тест.";
    v.push_back(S(Step::RuBufferAdd)); v.back().i1 = -1; v.back().i2 = -1;
    v.push_back(S(Step::RuBufferCheck)); v.back().s1 = "";
    // headless drive of Enter = save + next line: click the RU field at its
    // recorded client coords (exact — no vision/desktop coordinates), type,
    // press Enter, verify commit + advance
    v.push_back(S(Step::RuEnterBegin));
    v.push_back(S(Step::RuClick));
    v.push_back(S(Step::RuType)); v.back().s1 = "abc";
    v.push_back(S(Step::RuPendingCheck)); v.back().s1 = "abc"; // live while typing
    v.push_back(S(Step::RuKeyEnter));
    v.push_back(S(Step::RuEnterCheck)); v.back().i1 = -1; v.back().s1 = "abc";
    v.push_back(S(Step::Undo));   // revert the "abc" edit
    v.push_back(S(Step::Undo));   // drop the temporary line; rest of the script
                                  // continues with [Тест.(0), custom(1)]
    v.push_back(S(Step::Move));   v.back().i1 = -1; v.back().i2 = 1; v.back().i3 = 0;
    v.push_back(S(Step::Undo));
    v.push_back(S(Step::Redo));
    // move again under the dialogue filter (visible-row subset)
    v.push_back(S(Step::Filter)); v.back().i1 = 1;
    v.push_back(S(Step::Frames)); v.back().i1 = 2;
    v.push_back(S(Step::Move));   v.back().i1 = -1; v.back().i2 = 0; v.back().i3 = 1;
    v.push_back(S(Step::Move));   v.back().i1 = -1; v.back().i2 = 1; v.back().i3 = 0;
    v.push_back(S(Step::Filter)); v.back().i1 = 0;
    // close + reopen: the text edit, the custom line (marked + visible), and
    // undo history must all survive the round trip
    v.push_back(S(Step::Reopen)); v.back().i1 = -1;
    // debounced detail-panel edit: set pending, app_tick must flush+commit
    // (action 1 = the edited line; the custom line sits at 0 after the moves)
    v.push_back(S(Step::RuDebounce)); v.back().i1 = -1; v.back().i2 = 1; v.back().i3 = 0;
    v.back().s1 = u8"тест RU debounce";
    v.push_back(S(Step::RuCheck)); v.back().i1 = -1; v.back().i2 = 1; v.back().i3 = 0;
    v.back().s1 = u8"тест RU debounce";
    v.push_back(S(Step::Delete)); v.back().i1 = -1; v.back().i2 = 0; // the custom line (back at 0)
    v.push_back(S(Step::Budget));
    v.push_back(S(Step::Asm));    v.back().i1 = -1;
    v.push_back(S(Step::AsmStale)); v.back().i1 = 0; // apply synced the buffer
    v.push_back(S(Step::AsmEditCheck)); v.back().i1 = -1;
    v.push_back(S(Step::ExportImport));
    v.push_back(S(Step::Budget));
    v.push_back(S(Step::Perf));   // 120 pumped frames; avg must be < 25 ms
    v.push_back(S(Step::Dump));   v.back().s1 = "05-final";
    return v;
}

std::vector<Step> g_script;

// ---------- step execution ----------

static void do_slot(App& a, const Step& st)
{
    if (st.i1 >= 0 && st.i1 < (int)a.proj.files.size())
    {
        a.sel_file = st.i1;
        a.sel_action = -1;
    }
    UCHECK(a.sel_file == st.i1, "select slot %d", st.i1);
}

static void do_edit(App& a, const Step& st)
{
    int fi = st.i1, ai = st.i2, ci = st.i3;
    if (fi < 0) // entries().front()
    {
        auto es = a.proj.entries();
        UCHECK(!es.empty(), "project has entries");
        if (es.empty()) return;
        fi = es.front().file;
        ai = (int)es.front().action;
        ci = (int)es.front().chunk;
    }
    else if (ai < 0)
    {
        // resolve the action index within the given file
        for (size_t i = 0; i < a.proj.files[fi].actions.size(); i++)
            for (size_t c = 0; c < a.proj.files[fi].actions[i].chunks.size(); c++)
                if (a.proj.files[fi].actions[i].chunks[c].is_text)
                {
                    ai = (int)i;
                    ci = (int)c;
                    i = a.proj.files[fi].actions.size();
                    break;
                }
    }
    bool nameplate = a.proj.files[fi].actions[ai].opcode == 0x11A;
    EntryRef e{ fi, ai, ci, nameplate ? EntryKind::Nameplate : EntryKind::Dialogue };
    UCHECK(app_set_translation(a, e, &st.s1), "edit slot %d action %d", fi, ai);
    UCHECK(a.proj.files[fi].actions[ai].chunks[ci].edited_text == st.s1,
           "edit text stored (%s)", st.s1.c_str());
}

static void do_addline(App& a, const Step& st)
{
    int fi = st.i1, ai = st.i2;
    front_entry(a, &fi, &ai);
    std::string warn;
    UCHECK(app_insert_action(a, fi, ai, (uint32_t)st.i3, "", &warn),
           "add line opcode %#x after %d (slot %d)", st.i3, ai, fi);
    // The padding guardrail fires exactly when the slot outgrew its sector
    // padding (a single line usually stays within the padding now that the
    // embedded TOC rebuild makes growth safe in-game).
    bool over = a.proj.file_over_budget(fi);
    UCHECK(over == !warn.empty(),
           "padding warning matches over-padding state (over=%d warn=%d)",
           over, !warn.empty());
    UCHECK(a.proj.files[fi].actions[ai + 1].custom,
           "added action is marked custom");
    UCHECK(a.proj.files[fi].actions[ai + 1].original_addr != 0,
           "added action has a real annotation address (0x%X)",
           a.proj.files[fi].actions[ai + 1].original_addr);
}

static void do_move(App& a, const Step& st)
{
    int fi = st.i1;
    front_entry(a, &fi, nullptr);
    size_t before = a.proj.files[fi].actions.size();
    UCHECK(app_move_action(a, fi, st.i2, st.i3), "move %d -> %d (slot %d)", st.i2, st.i3, fi);
    UCHECK(a.proj.files[fi].actions.size() == before, "move keeps action count");
}

static void do_delete(App& a, const Step& st)
{
    int fi = st.i1;
    front_entry(a, &fi, nullptr);
    bool wasCustom = a.proj.files[fi].actions[st.i2].custom;
    UCHECK(app_delete_action(a, fi, st.i2), "delete action %d (slot %d)", st.i2, fi);
    UCHECK((int)a.proj.files[fi].actions.size() > st.i2 &&
               !a.proj.files[fi].actions[st.i2].custom,
           "deleted action gone (was custom=%d)", wasCustom);
}

static void do_asm(App& a, const Step& st)
{
    int fi = st.i1;
    front_entry(a, &fi, nullptr);
    std::string text = asm_disasm(a.proj.files[fi], fi);
    Stcm2File assembled;
    AsmError err;
    bool ok = asm_assemble(text, fi, assembled, err);
    UCHECK(ok, "asm assemble slot %d%s", fi, ok ? "" : err.msg.c_str());
    if (ok)
    {
        UCHECK(app_apply_asm(a, fi, assembled), "asm apply slot %d", fi);
        a.asm_model_version = a.model_version; // mirrors apply_now
    }
}

static void do_click(int x, int y)
{
    // Inject straight into ImGui's input queue. Window-message injection was
    // tried first but the backend's TrackMouseEvent on the hidden window
    // triggers WM_MOUSELEAVE, which queues AddMousePosEvent(-FLT_MAX) and
    // wipes the click position before the frame drains. Direct io events
    // bypass that; the win32 backend is a no-op while the window is
    // unfocused, so nothing overwrites them.
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent((float)x, (float)y);
    io.AddMouseButtonEvent(0, true);
    g_clickDown = true;
    g_clickX = x;
    g_clickY = y;
}

static void do_export_import(App& a)
{
    std::string dump = dump_export(a.proj, a.paths.orig_uni);
    std::string path = g_outdir + "/dump.txt";
    FILE* f = fopen(path.c_str(), "wb");
    if (f)
    {
        fwrite(dump.data(), 1, dump.size(), f);
        fclose(f);
    }
    UCHECK(dump.size() > 1000, "dump export (%zu bytes)", dump.size());
    std::vector<size_t> before;
    for (size_t i = 0; i < a.proj.files.size(); i++)
        before.push_back(a.proj.file_serialized_len(i));
    try
    {
        dump_import(a.proj, dump);
        std::vector<size_t> after;
        for (size_t i = 0; i < a.proj.files.size(); i++)
            after.push_back(a.proj.file_serialized_len(i));
        UCHECK(after == before, "import of unchanged dump is a no-op");
    }
    catch (const Stcm2Error& e)
    {
        UCHECK(false, "dump import threw: %s", e.what());
    }
}

static void exec_step(App& a, const Step& st)
{
    switch (st.kind)
    {
    case Step::Slot: do_slot(a, st); break;
    case Step::SlotFirstDialogue:
    {
        int target = 0;
        for (size_t i = 0; i < a.proj.files.size(); i++)
        {
            for (const auto& act : a.proj.files[i].actions)
                if (act.opcode == 0x118) { target = (int)i; i = a.proj.files.size(); break; }
        }
        a.sel_file = target;
        a.sel_action = -1;
        UCHECK(target < (int)a.proj.files.size(), "project has a dialogue slot");
        break;
    }
    case Step::Edit: do_edit(a, st); break;
    case Step::AddLine: do_addline(a, st); break;
    case Step::Move: do_move(a, st); break;
    case Step::Delete: do_delete(a, st); break;
    case Step::Undo: UCHECK(app_undo(a), "undo"); break;
    case Step::Redo: UCHECK(app_redo(a), "redo"); break;
    case Step::Asm: do_asm(a, st); break;
    case Step::Filter: a.kind_filter = st.i1; break;
    case Step::Frames: g_framesLeft += st.i1; break;
    case Step::Click: do_click(st.i1, st.i2); g_framesLeft += 2; break;
    case Step::ForceTab:
        if (st.i1 >= 1 && st.i1 <= 2)
        {
            g_forceTab = st.i1;
            g_framesLeft += 2; // let the selection take effect
        }
        else
            UCHECK(false, "ForceTab: bad tab %d", st.i1);
        break;
    case Step::CheckTab:
        UCHECK(g_activeTab == st.i1, "active tab is %d (want %d)", g_activeTab, st.i1);
        break;
    case Step::Dump:
        g_pendingDump = g_outdir + "/" + st.s1 + ".png";
        break;
    case Step::Budget:
    {
        int over = 0;
        for (size_t i = 0; i < a.slot_lens.size(); i++)
            if (a.slot_lens[i] > a.proj.file_budget(i)) over++;
        UCHECK(over == 0, "no slot over budget (%d over)", over);
        break;
    }
    case Step::RowsCheck:
    {
        int n = ui_visible_row_count(a);
        UCHECK(n > 0, "filter %d shows %d rows", st.i1, n);
        UCHECK(ui_visible_rows_match_kind(a, st.i1), "filter %d rows match kind", st.i1);
        if (st.i1 == 1)
            UCHECK(ui_visible_advance_count(a) > 0,
                   "dialogue filter shows advance rows (%d)",
                   ui_visible_advance_count(a));
        break;
    }
    case Step::ExportImport: do_export_import(a); break;
    case Step::AsmJpDump:
        a.asm_text = st.s1 + "\n" + a.asm_text;
        g_framesLeft += 2;
        break;
    case Step::RuBufferSel:
    {
        int fi = st.i1, ai = st.i2;
        front_entry(a, &fi, &ai);
        a.sel_file = fi;
        a.sel_action = ai;
        g_framesLeft += 3; // let the panel load its buffer
        break;
    }
    case Step::RuBufferAdd:
    {
        int fi = st.i1, ai = st.i2;
        front_entry(a, &fi, &ai);
        std::string warn;
        UCHECK(app_insert_action(a, fi, ai, 0x118, "", &warn),
               "ru buffer: add line after %d", ai);
        a.sel_file = fi;
        a.sel_action = ai + 1; // select the new line (do_add_line behavior)
        g_framesLeft += 3;
        break;
    }
    case Step::RuClick:
    {
        int x, y;
        UCHECK(ui_ru_field(&x, &y), "ru click: RU field rect recorded");
        do_click(x, y);
        g_framesLeft += 2;
        break;
    }
    case Step::RuEnterBegin:
        g_enterIdx = ui_sel_row_index(a);
        UCHECK(g_enterIdx >= 0, "enter drive: selection is a visible row");
        break;
    case Step::RuType:
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddInputCharactersUTF8(st.s1.c_str());
        g_framesLeft += 2;
        break;
    }
    case Step::RuPendingCheck:
    {
        UCHECK(a.ru_pending && a.ru_text == st.s1,
               "typing is live (pending '%s' buf='%s' ver=%llu)",
               a.ru_text.c_str(), ui_ru_buffer(), (unsigned long long)a.model_version);
        bool committed = false;
        for (const auto& f : a.proj.files)
            for (const auto& act : f.actions)
                for (const auto& c : act.chunks)
                    if (c.is_text && c.edited && c.edited_text == st.s1) committed = true;
        UCHECK(!committed, "typed text not yet committed (debounced autosave)");
        break;
    }
    case Step::RuKeyEnter:
        g_keyEnter = true;
        g_framesLeft += 3;
        break;
    case Step::RuEnterCheck:
    {
        int fi = st.i1;
        front_entry(a, &fi, nullptr);
        bool committed = false;
        for (const auto& act : a.proj.files[fi].actions)
            for (const auto& c : act.chunks)
                if (c.is_text && c.edited && c.edited_text == st.s1) committed = true;
        UCHECK(committed, "enter committed '%s'", st.s1.c_str());
        UCHECK(!a.ru_pending, "enter flushed the pending edit");
        UCHECK(ui_sel_row_index(a) == g_enterIdx + 1,
               "enter moved to the next line (row %d -> %d)",
               g_enterIdx, ui_sel_row_index(a));
        break;
    }
    case Step::AsmStale:
    {
        bool stale = !a.asm_dirty && a.asm_model_version != a.model_version;
        UCHECK(stale == (st.i1 != 0), "asm buffer stale=%d (want %d)", stale, st.i1);
        break;
    }
    case Step::AsmEditCheck:
    {
        int fi = st.i1;
        front_entry(a, &fi, nullptr);
        bool found = false;
        for (const auto& act : a.proj.files[fi].actions)
            for (const auto& c : act.chunks)
                if (c.edited && c.edited_text == u8"тест RU debounce") found = true;
        UCHECK(found, "asm round trip preserves the edited translation");
        break;
    }
    case Step::RuBufferCheck:
    {
        const char* got = ui_ru_buffer();
        UCHECK(got != nullptr && st.s1 == got, "ru buffer is '%s' (want '%s')",
               got ? got : "(null)", st.s1.c_str());
        break;
    }
    case Step::Reopen:
    {
        int fi = st.i1;
        front_entry(a, &fi, nullptr);
        // snapshot what must survive the restart
        bool edited = false;
        std::string editedText;
        int customAction = -1;
        for (size_t i = 0; i < a.proj.files[fi].actions.size(); i++)
        {
            const auto& act = a.proj.files[fi].actions[i];
            if (act.custom) customAction = (int)i;
            for (const auto& c : act.chunks)
                if (c.is_text && c.edited) { edited = true; editedText = c.edited_text; }
        }
        UCHECK(edited, "reopen: an edited line exists before restart");
        UCHECK(customAction >= 0, "reopen: a custom line exists before restart");
        std::string projTxt = a.paths.root + "/project.txt";
        app_close_project(a);
        std::string err;
        UCHECK(app_open_project(a, projTxt, &err), "reopen project: %s", err.c_str());
        UCHECK(a.undo.steps() > 0, "reopen: undo history survives (%zu steps)",
               a.undo.steps());
        // verify the restored model
        bool edited2 = false;
        bool restored = false;
        int customAction2 = -1;
        bool customTextVisible = false;
        bool customEdited = false;
        for (size_t i = 0; i < a.proj.files[fi].actions.size(); i++)
        {
            const auto& act = a.proj.files[fi].actions[i];
            if (act.custom)
            {
                customAction2 = (int)i;
                for (const auto& c : act.chunks)
                    if (c.is_text)
                    {
                        customTextVisible = true;
                        if (c.edited) customEdited = true;
                    }
            }
            for (const auto& c : act.chunks)
                if (c.is_text && c.edited)
                {
                    edited2 = true;
                    if (c.edited_text == editedText) restored = true;
                }
        }
        UCHECK(edited2 && restored,
               "reopen: edited line restored ('%s')", editedText.c_str());
        UCHECK(customAction2 >= 0, "reopen: custom flag survives");
        UCHECK(customTextVisible, "reopen: custom line's text chunk is visible");
        UCHECK(customEdited, "reopen: custom line stays edited (RU side, not JP)");
        break;
    }
    case Step::RuDebounce:
    {
        int fi = st.i1, ai = st.i2, ci = st.i3;
        if (fi < 0 || ai < 0)
        {
            auto es = a.proj.entries();
            if (es.empty()) { UCHECK(false, "ru debounce: no entries"); break; }
            if (fi < 0) fi = es.front().file;
            if (ai < 0) ai = (int)es.front().action;
        }
        a.ru_file = fi;
        a.ru_action = ai;
        a.ru_chunk = ci;
        a.ru_text = st.s1;
        a.ru_pending = true;
        a.ru_at = (double)app_now_ms() - 500.0; // typed 500 ms ago: flush next tick
        g_framesLeft += 3;
        break;
    }
    case Step::RuCheck:
    {
        int fi = st.i1, ai = st.i2, ci = st.i3;
        if (fi < 0 || ai < 0)
        {
            auto es = a.proj.entries();
            if (es.empty()) { UCHECK(false, "ru check: no entries"); break; }
            if (fi < 0) fi = es.front().file;
            if (ai < 0) ai = (int)es.front().action;
        }
        const auto& ch = a.proj.files[fi].actions[ai].chunks[ci];
        UCHECK(ch.edited && ch.edited_text == st.s1,
               "ru debounce committed ('%s')", ch.edited_text.c_str());
        UCHECK(!a.ru_pending, "ru debounce cleared the pending flag");
        break;
    }
    case Step::Perf:
        g_perfStart = app_now_ms();
        g_perfLeft = 120;
        g_framesLeft += 120;
        break;
    }
}

} // namespace

// ---------- PNG writer (miniz-backed) ----------
// (outside the anonymous namespace: main.cpp links it for framebuffer dumps)

void uitest_save_png(const char* path, const uint8_t* bgra, uint32_t w, uint32_t h,
                     size_t rowPitch)
{
    std::vector<uint8_t> raw((size_t)h * (1 + (size_t)w * 4));
    for (uint32_t y = 0; y < h; y++)
    {
        uint8_t* dst = &raw[(size_t)y * (1 + (size_t)w * 4)];
        dst[0] = 0; // filter: none
        const uint8_t* src = bgra + (size_t)y * rowPitch;
        dst++;
        for (uint32_t x = 0; x < w; x++)
        {
            dst[x * 4 + 0] = src[x * 4 + 2]; // BGRA -> RGBA
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = 255;
        }
    }
    mz_ulong compLen = mz_compressBound((mz_ulong)raw.size());
    std::vector<uint8_t> comp(compLen);
    if (mz_compress(comp.data(), &compLen, raw.data(), (mz_ulong)raw.size()) != MZ_OK)
        return;
    FILE* f = fopen(path, "wb");
    if (!f) return;
    auto put32 = [&](uint32_t v) {
        uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
        fwrite(b, 1, 4, f);
    };
    auto chunk = [&](const char* type, const uint8_t* data, size_t n) {
        put32((uint32_t)n);
        fwrite(type, 1, 4, f);
        uint32_t crc = mz_crc32(MZ_CRC32_INIT, (const mz_uint8*)type, 4);
        if (n)
        {
            crc = mz_crc32(crc, data, n);
            fwrite(data, 1, n, f);
        }
        put32(crc);
    };
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {
        (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
        8, 6, 0, 0, 0,
    };
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", comp.data(), compLen);
    chunk("IEND", nullptr, 0);
    fclose(f);
    printf("uitest: frame saved: %s\n", path);
}

// ---------- public driver API ----------

void uitest_begin(App& a, const std::string& outdir)
{
    g_app = &a;
    g_outdir = outdir;
    g_step = 0;
    g_framesLeft = 0;
    g_clickDown = false;
    g_checks = 0;
    g_fails = 0;
    g_script = make_script();
    printf("uitest: %zu steps scripted\n", g_script.size());
}

bool uitest_finished()
{
    return g_step >= (int)g_script.size() && g_framesLeft <= 0 && !g_clickDown;
}

void uitest_on_frame(App& a)
{
    if (g_keyEnter)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiKey_Enter, true);
        g_keyEnter = false;
        g_keyEnterHeld = true;
        return; // let this frame process the press
    }
    if (g_keyEnterHeld)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiKey_Enter, false);
        g_keyEnterHeld = false;
        return; // let this frame process the release
    }
    if (g_clickDown)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMouseButtonEvent(0, false);
        g_clickDown = false;
        return; // let this frame process the release
    }
    if (g_perfLeft > 0)
    {
        g_perfLeft--;
        if (g_perfLeft == 0)
        {
            double avg = (double)(app_now_ms() - g_perfStart) / 120.0;
            UCHECK(avg < 25.0,
                   "perf: avg frame time %.1f ms over 120 frames (< 25)",
                   avg);
        }
    }
    if (g_framesLeft > 0)
    {
        g_framesLeft--;
        return;
    }
    if (g_step >= (int)g_script.size()) return;
    exec_step(a, g_script[g_step]);
    g_step++;
    // a step that only sets state needs one rendered frame to take effect
    // before the next step runs; dump steps are consumed by the frame hook
    if (g_framesLeft == 0 && g_pendingDump.empty())
        g_framesLeft = 1;
}

int uitest_summary()
{
    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

void uitest_request_dump(const std::string& path) { g_pendingDump = path; }

// main.cpp calls this with the app HWND so clicks can be posted to it.
void uitest_set_hwnd(void* h) { g_hwnd = (HWND)h; }
