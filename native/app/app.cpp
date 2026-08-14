// Document operations — every mutation builds undo ops, applies them, and
// autosaves. See app.h.
#include "app.h"
#include "win32_util.h"
#include "core/asmfmt.h"
#include "core/cp932.h"
#include "core/text.h"
#include "core/uni2.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#endif

using namespace dokuro;

static uint64_t fnv1a64(const uint8_t* b, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++)
    {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

uint64_t app_now_ms()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void app_set_status(App& a, const std::string& msg, double seconds)
{
    a.status = msg;
    a.status_until = (double)app_now_ms() + seconds * 1000.0;
}

static std::string path_dirname(const std::string& p)
{
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? "." : p.substr(0, s);
}

static std::string path_basename(const std::string& p)
{
    size_t s = p.find_last_of("/\\");
    std::string b = s == std::string::npos ? p : p.substr(s + 1);
    size_t dot = b.find_last_of('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

static std::string path_join(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "/" + b;
}

static bool make_dirs(const std::string& path)
{
    std::string cur;
    for (size_t i = 0; i < path.size(); i++)
    {
        cur += path[i];
        if (path[i] == '/' || path[i] == '\\' || i + 1 == path.size())
        {
            if (!cur.empty())
            {
#ifdef _WIN32
                // file_exists() (ifstream) cannot see directories; check with
                // the filesystem API so existing components are skipped.
                DWORD attrs = GetFileAttributesA(cur.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES) continue;
                if (CreateDirectoryA(cur.c_str(), nullptr) == 0 &&
                    GetLastError() != ERROR_ALREADY_EXISTS)
                    return false;
#else
                struct stat st;
                if (stat(cur.c_str(), &st) == 0) continue;
                if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
#endif
            }
        }
    }
    return true;
}

// ---------- project folder location ----------

// Standard place for new project folders: <My Documents>\dokuro\projects
// (settings.projects_dir overrides; falls back to the exe dir).
static std::string projects_root(const AppSettings& s)
{
    if (!s.projects_dir.empty()) return s.projects_dir;
    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, buf)))
        return win32util::wide_to_utf8(buf) + "/dokuro/projects";
    return win32util::exe_dir() + "/projects";
}

// Unique project folder name: Dokuro_<uni basename>_<YYYYMMDD-HHMMSS>
// (caller appends -2, -3 on same-second collisions).
static std::string unique_project_name(const std::string& uniPath)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char name[128];
    snprintf(name, sizeof(name), "Dokuro_%s_%04u%02u%02u-%02u%02u%02u",
             path_basename(uniPath).c_str(), st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    std::string clean;
    // iterate to the null terminator — range-for over the raw array would
    // also process the uninitialized tail (stack garbage in the dir name)
    for (const char* p = name; *p; p++)
    {
        char c = *p;
        clean += (isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
    }
    return clean;
}

// First slot containing dialogue (0x118) lines; 0 if none.
static int first_dialogue_slot(const Project& p)
{
    for (size_t i = 0; i < p.files.size(); i++)
        for (const auto& act : p.files[i].actions)
            if (act.opcode == 0x118) return (int)i;
    return 0;
}

// Highest-index slot that has any edited chunk; -1 if none.
static int last_edited_slot(const Project& p)
{
    for (size_t i = p.files.size(); i-- > 0;)
        for (const auto& act : p.files[i].actions)
            for (const auto& ch : act.chunks)
                if (ch.edited) return (int)i;
    return -1;
}

// Recomputes per-slot serialized lengths. Serialization is expensive — call
// per commit, NEVER per frame (that was a choppiness source).
static void app_refresh_slot_lens(App& a)
{
    a.slot_lens.clear();
    a.slot_lens.reserve(a.proj.files.size());
    for (size_t i = 0; i < a.proj.files.size(); i++)
        a.slot_lens.push_back(a.proj.file_serialized_len(i));
}

// Per-slot size budgets. The pristine slots NEVER change, so this is computed
// once per project load — serializing the originals every frame (35 x
// file_serialized_len) was the 33fps choppiness source.
static void app_refresh_budgets(App& a)
{
    a.slot_budgets.clear();
    a.slot_budgets.reserve(a.proj.files.size());
    for (size_t i = 0; i < a.proj.files.size(); i++)
        a.slot_budgets.push_back(a.proj.file_budget(i));
}

// translated/total counters for the status bar (entries() walks the whole
// model — too slow to run every frame; refreshed per commit).
static void app_refresh_counts(App& a)
{
    a.total_count = 0;
    a.translated_count = 0;
    for (const auto& e : a.proj.entries())
    {
        a.total_count++;
        if (a.proj.files[e.file].actions[e.action].chunks[e.chunk].edited)
            a.translated_count++;
    }
}

// After any successful model mutation: invalidate UI caches and refresh the
// per-slot length cache. lastFile = the slot the change touched (-1 unknown).
static void app_model_changed(App& a, int lastFile)
{
    a.model_version++;
    app_refresh_slot_lens(a);
    app_refresh_counts(a);
    if (lastFile >= 0) a.last_edit_file = lastFile;
}

// ---------- state file (project.txt) ----------

static std::string state_path(const std::string& root) { return path_join(root, "project.txt"); }

static bool save_state(App& a)
{
    std::string out;
    out += "# Dokuro translation project state - managed by the editor, do not edit\n";
    char hashbuf[32];
    snprintf(hashbuf, sizeof(hashbuf), "%016llX", (unsigned long long)a.pristine_hash);
    out += "pristine = " + std::string(hashbuf) + "\n";
    out += "pristine_path = " + a.paths.orig_uni + "\n";
    out += "iso = " + (a.paths.orig_iso.empty() ? std::string("-") : a.paths.orig_iso) + "\n";
    out += "last_edit_file = " + std::to_string(a.last_edit_file) + "\n";
    // custom actions keyed by (file, addr at save time)
    try
    {
        a.proj.build_container(); // fills new_addr
    }
    catch (const Stcm2Error&) {}
    for (size_t fi = 0; fi < a.proj.files.size(); fi++)
        for (const auto& act : a.proj.files[fi].actions)
            if (act.custom)
            {
                char line[64];
                snprintf(line, sizeof(line), "custom = %zu:0x%X\n", fi, act.new_addr);
                out += line;
            }
    try
    {
        write_file(state_path(a.paths.root), (const uint8_t*)out.data(), out.size());
        return true;
    }
    catch (const Stcm2Error&) { return false; }
}

static bool load_state(App& a, std::string* err,
                       std::vector<std::pair<int, uint32_t>>& customs)
{
    std::ifstream f(state_path(a.paths.root));
    if (!f)
    {
        if (err) *err = "project.txt missing";
        return false;
    }
    std::string line;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        std::string val = line.substr(eq + 1);
        while (!val.empty() && val.front() == ' ') val.erase(0, 1);
        if (key == "pristine_path") a.paths.orig_uni = val;
        else if (key == "iso") a.paths.orig_iso = (val == "-") ? "" : val;
        else if (key == "last_edit_file") a.last_edit_file = atoi(val.c_str());
        else if (key == "custom")
        {
            // "f:0xADDR"
            size_t colon = val.find(':');
            if (colon == std::string::npos) continue;
            int fi = atoi(val.substr(0, colon).c_str());
            unsigned addr = 0;
            if (sscanf(val.c_str() + colon + 1, "0x%X", &addr) == 1)
                customs.push_back({ fi, addr });
        }
    }
    return true;
}

// ---------- project lifecycle ----------

bool app_create_project(App& a, const std::string& uniPath, const std::string& isoPath,
                        std::string* err)
{
    // Project folders live in the standard projects dir, uniquely named —
    // NOT next to the source .UNI (multiple projects would collide).
    std::string proot = projects_root(a.settings);
    std::string base = unique_project_name(uniPath);
    std::string root;
    for (int n = 1; ; n++)
    {
        root = path_join(proot, n == 1 ? base : base + "-" + std::to_string(n));
        if (GetFileAttributesA(root.c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    std::string origDir = path_join(root, "original");
    std::string workDir = path_join(root, "working");
    if (!make_dirs(origDir) || !make_dirs(workDir))
    {
        if (err) *err = "cannot create project folders under " + root;
        return false;
    }

    App::Paths p;
    p.root = root;
    p.orig_uni = path_join(origDir, "SCRIPT.UNI");
    p.work_uni = path_join(workDir, "SCRIPT.UNI");
    if (!isoPath.empty())
    {
        p.iso_name = path_basename(isoPath) + ".iso";
        p.orig_iso = path_join(origDir, p.iso_name);
        p.work_iso = path_join(workDir, p.iso_name);
    }

    // copy the pristine UNI
    try
    {
        std::vector<uint8_t> uni = read_file(uniPath);
        write_file(p.orig_uni, uni);
        write_file(p.work_uni, uni); // working copy starts identical; app_open_project requires it
        a.pristine_bytes = uni;
        a.pristine_hash = fnv1a64(uni.data(), uni.size());
    }
    catch (const Stcm2Error& e)
    {
        if (err) *err = e.what();
        return false;
    }

    // parse the pristine project (throws on malformed file)
    try
    {
        a.proj = Project::load(p.orig_uni);
    }
    catch (const Stcm2Error& e)
    {
        if (err) *err = std::string("not a valid SCRIPT.UNI: ") + e.what();
        return false;
    }

    a.paths = p;
    a.have_project = true;

    // ISO copy is async (big); the UI pumps a.copier and finishes setup after.
    if (!isoPath.empty())
    {
        std::string cerr;
        if (!a.copier.begin(isoPath, p.orig_iso, &cerr))
        {
            if (err) *err = "cannot copy ISO: " + cerr;
            a.have_project = false;
            return false;
        }
        a.busy = true;
        a.busy_mode = 1;
        a.busy_title = "Copying original ISO into the project folder...";
        a.busy_frac = 0.0;
    }

    if (!a.undo.open(a.paths.root, a.pristine_hash, a.pristine_bytes, err))
    {
        if (err) *err = "undo: " + *err;
        a.have_project = false;
        return false;
    }
    if (!save_state(a))
    {
        if (err) *err = "cannot write project state";
        a.have_project = false;
        return false;
    }
    app_set_status(a, "Project created: " + root, 8);
    a.dirty = false;
    a.last_edit_file = -1;
    a.model_version++;
    app_refresh_slot_lens(a);
    app_refresh_budgets(a);
    app_refresh_counts(a);
    // land on the first slot with dialogue lines
    a.sel_file = first_dialogue_slot(a.proj);
    a.sel_action = -1;
    a.asm_file = a.sel_file;
    a.asm_text = dokuro::asm_disasm(a.proj.files[a.sel_file], a.sel_file);
    a.asm_dirty = false;
    a.asm_error = false;
    a.asm_model_version = a.model_version;
    return true;
}

// Called by the UI when the async ISO copy finishes (or fails).
void app_finish_create(App& a, bool ok, const std::string& err)
{
    a.busy = false;
    if (!ok)
    {
        app_set_status(a, "ISO copy failed: " + err, 10);
        return;
    }
    app_set_status(a, "Project ready — original and working copies are in the project folder", 8);
}

bool app_open_project(App& a, const std::string& projTxtPath, std::string* err)
{
    app_log("app_open_project: %s exists=%d", projTxtPath.c_str(),
            GetFileAttributesA(projTxtPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    std::string root = path_dirname(projTxtPath);
    App::Paths p;
    p.root = root;
    p.orig_uni = path_join(root, "original/SCRIPT.UNI");
    p.work_uni = path_join(root, "working/SCRIPT.UNI");
    p.orig_iso = "";
    p.work_iso = "";
    p.iso_name = "";

    std::string staterr;
    std::vector<std::pair<int, uint32_t>> customs;
    // parse state to learn iso path + custom flags (paths first)
    a.paths = p;
    if (!load_state(a, &staterr, customs))
    {
        if (err) *err = "cannot read project state: " + staterr;
        return false;
    }
    if (!file_exists(p.orig_uni) || !file_exists(p.work_uni))
    {
        if (err) *err = "original/SCRIPT.UNI or working/SCRIPT.UNI missing";
        return false;
    }
    // recompute iso paths from state
    if (!a.paths.orig_iso.empty())
    {
        std::string name = path_basename(a.paths.orig_iso) + ".iso";
        a.paths.iso_name = name;
        a.paths.work_iso = path_join(root, "working/" + name);
    }

    try
    {
        a.pristine_bytes = read_file(p.orig_uni);
        a.pristine_hash = fnv1a64(a.pristine_bytes.data(), a.pristine_bytes.size());
        // customs are applied inside load_pair (marks tool-added actions and
        // excludes them from edited-flag matching)
        a.proj = Project::load_pair(p.orig_uni, p.work_uni, &customs);
    }
    catch (const Stcm2Error& e)
    {
        if (err) *err = std::string("cannot load project: ") + e.what();
        return false;
    }

    std::string uerr;
    if (!a.undo.open(root, a.pristine_hash, read_file(p.work_uni), &uerr))
    {
        if (err) *err = uerr;
        return false;
    }
    if (!uerr.empty()) app_set_status(a, uerr, 8);

    a.have_project = true;
    app_refresh_budgets(a);
    app_refresh_counts(a);
    a.dirty = false;
    a.iso_dirty = false;
    a.model_version++;
    app_refresh_slot_lens(a);
    // init the direct-script buffer (also reached via auto-reopen) and land
    // on the last edited slot (persisted in state), else the last slot with
    // edits, else the first slot with dialogue lines
    if (!a.proj.files.empty())
    {
        int target = a.last_edit_file;
        if (target < 0 || target >= (int)a.proj.files.size())
            target = last_edited_slot(a.proj);
        if (target < 0) target = first_dialogue_slot(a.proj);
        a.sel_file = target;
        a.sel_action = -1;
        a.asm_file = target;
        a.asm_text = dokuro::asm_disasm(a.proj.files[target], target);
        a.asm_dirty = false;
        a.asm_error = false;
        a.asm_model_version = a.model_version;
    }
    app_set_status(a, "Project opened: " + root, 6);
    return true;
}

void app_close_project(App& a)
{
    // never lose a translation typed into the detail panel (debounced commit)
    app_flush_ru(a);
    a.undo.close();
    a.have_project = false;
    a.proj = Project();
    a.pristine_bytes.clear();
    a.dirty = false;
    a.iso_dirty = false;
}

// ---------- commit plumbing ----------

bool app_autosave(App& a)
{
    if (!a.have_project) return false;
    try
    {
        std::vector<uint8_t> bytes = a.proj.build_container();
        write_file(a.paths.work_uni, bytes);
        if (!save_state(a))
        {
            app_set_status(a, "WARNING: could not write project state", 10);
            return false;
        }
        return true;
    }
    catch (const Stcm2Error& e)
    {
        app_set_status(a, std::string("AUTOSAVE FAILED: ") + e.what(), 15);
        return false;
    }
}

bool app_commit(App& a, std::vector<UndoOp> ops, const std::string& label)
{
    if (ops.empty()) return true;
    std::vector<uint8_t> bytes;
    try
    {
        bytes = a.proj.build_container();
    }
    catch (const Stcm2Error& e)
    {
        app_set_status(a, std::string("change rejected: ") + e.what(), 10);
        return false;
    }
    int lastFile = ops.empty() ? -1 : ops.back().file;
    app_log("commit: container %zu bytes, undo.commit", bytes.size());
    a.undo.commit(std::move(ops), bytes, label);
    app_log("commit: undo committed");
    a.dirty = true;
    a.dirty_since = (double)app_now_ms();
    a.iso_dirty = true;
    app_model_changed(a, lastFile);
    app_log("commit: model changed, autosaving");
    app_autosave(a);
    app_log("commit: autosaved");
    return true;
}

// ---------- document ops ----------

// Commits the detail panel's debounced RU edit (set by editor_ui on every
// keystroke, flushed by app_tick after a quiet period or immediately on
// focus loss / project close). No-op when the text is unchanged.
void app_flush_ru(App& a)
{
    if (!a.ru_pending || a.ru_file < 0 || !a.have_project) return;
    int fi = a.ru_file, ai = a.ru_action, ci = a.ru_chunk;
    std::string text = a.ru_text;
    a.ru_pending = false;
    a.ru_file = -1;
    if (fi < 0 || fi >= (int)a.proj.files.size()) return;
    auto& f = a.proj.files[fi];
    if (ai < 0 || ai >= (int)f.actions.size()) return;
    if (ci < 0 || ci >= (int)f.actions[ai].chunks.size()) return;
    auto& ch = f.actions[ai].chunks[ci];
    if (!ch.is_text) return;
    if (text == (ch.edited ? ch.edited_text : "")) return; // no change
    EntryRef e{ fi, ai, ci,
                f.actions[ai].opcode == 0x11A ? EntryKind::Nameplate
                                              : EntryKind::Dialogue };
    app_set_translation(a, e, text.empty() ? nullptr : &text);
}

bool app_set_translation(App& a, const EntryRef& e, const std::string* text)
{
    if (!a.have_project) return false;
    auto& ch = a.proj.files[e.file].actions[e.action].chunks[e.chunk];
    UndoOp op;
    op.kind = UndoOpKind::TextEdit;
    op.file = e.file;
    op.a = e.action;
    op.c = e.chunk;
    op.old_edited = ch.edited;
    op.old_text = ch.edited_text;
    op.old_raw.assign(ch.raw.begin(), ch.raw.end());
    if (text)
    {
        op.new_edited = true;
        op.new_text = sanitize(*text);
        std::vector<uint8_t> enc;
        size_t bad;
        uint32_t badcp;
        if (!sjis::encode(op.new_text, enc, &bad, &badcp))
        {
            app_set_status(a, "character can't be encoded to the game charset (cp932) — fixed by editing the text", 8);
            return false;
        }
        op.new_raw.assign((const char*)enc.data(), enc.size());
    }
    a.proj.set_translation_raw(e, op.new_edited, op.new_text);
    std::vector<UndoOp> ops;
    ops.push_back(std::move(op));
    return app_commit(a, std::move(ops), "edit text");
}

bool app_revert_chunk(App& a, const EntryRef& e)
{
    if (!a.have_project) return false;
    auto& ch = a.proj.files[e.file].actions[e.action].chunks[e.chunk];
    UndoOp op;
    op.kind = UndoOpKind::TextEdit;
    op.file = e.file;
    op.a = e.action;
    op.c = e.chunk;
    op.old_edited = ch.edited;
    op.old_text = ch.edited_text;
    op.old_raw.assign(ch.raw.begin(), ch.raw.end());
    if (!a.proj.revert_chunk(e))
    {
        app_set_status(a, "revert unavailable: the pristine original has no matching line", 6);
        return false;
    }
    std::vector<UndoOp> ops;
    ops.push_back(std::move(op));
    return app_commit(a, std::move(ops), "revert line");
}

bool app_insert_action(App& a, int fi, int after_action, uint32_t opcode,
                       const std::string& text, std::string* warnOverBudget)
{
    app_log("app_insert_action: have_project=%d fi=%d after=%d opcode=%#x warn=%p text=%s",
            a.have_project, fi, after_action, opcode, (void*)warnOverBudget,
            text.c_str());
    if (!a.have_project) return false;
    if (fi < 0 || fi >= (int)a.proj.files.size()) return false;
    auto& f = a.proj.files[fi];

    // build the action
    Action act;
    act.opcode = opcode;
    act.custom = true;
    if (opcode == 0x118 || opcode == 0x11A)
    {
        DataChunk c;
        c.type = 0;
        c.is_text = true;
        c.edited = true;
        c.edited_text = text;
        act.chunks.push_back(c);
        act.params.push_back(Param{ ParamKind::DataPointer, 0 });
    }
    size_t at = (size_t)(after_action + 1);
    if (at > f.actions.size()) at = f.actions.size();

    std::vector<uint8_t> bytes = Stcm2File::serialize_action(act, 0);
    app_log("insert: serialized %zu bytes", bytes.size());
    // check budget impact before applying
    size_t lenBefore = 0, lenAfter = 0;
    uint32_t customAddr = 0; // synthetic annotation address (undo op)
    try
    {
        lenBefore = a.proj.file_serialized_len(fi);
        app_log("insert: lenBefore=%zu global=%zu", lenBefore, f.global_data.size());
        // Custom actions have no pristine address; parse_action with addr 0
        // gives every added line original_addr == 0, which the direct-script
        // view shows as "action @0x00000000" and — worse — makes the
        // assembler reject the slot ("duplicate action address") once two
        // custom lines exist. Allocate a unique synthetic annotation address
        // from a reserved high range (real script addresses stay below 4 MB,
        // so this can never collide with an original action or with the real
        // addresses customs get after a restart). The address is assigned
        // AFTER parsing: parse_action derives the param data window from the
        // addr argument, which must match the address used at encode time.
        static uint32_t s_customAddr = 0xF0000000u;
        customAddr = s_customAddr++;
        Action parsed = Stcm2File::parse_action(bytes.data(), bytes.size(), 0,
                                                (uint32_t)f.global_data.size());
        parsed.original_addr = customAddr;
        parsed.custom = true;
        app_log("insert: parsed ok, inserting at %zu of %zu", at, f.actions.size());
        f.actions.insert(f.actions.begin() + at, std::move(parsed));
        lenAfter = a.proj.file_serialized_len(fi);
        app_log("insert: inserted, lenAfter=%zu", lenAfter);
    }
    catch (const Stcm2Error& e)
    {
        app_set_status(a, std::string("cannot add line: ") + e.what(), 8);
        return false;
    }
    bool over = a.proj.file_over_budget(fi);
    if (over && warnOverBudget)
    {
        *warnOverBudget = "file slot " + std::to_string(fi) + " exceeds its sector padding by " +
                          std::to_string(lenAfter > a.proj.file_budget(fi) ? lenAfter - a.proj.file_budget(fi) : 0) +
                          " bytes — SCRIPT.UNI will grow past its original size "
                          "(in-game the rebuilt chunk table reads it fine; Update ISO rebuilds "
                          "the whole disc image automatically). Shorten other lines in this slot, or undo.";
    }

    UndoOp op;
    op.kind = UndoOpKind::ActionInsert;
    op.file = fi;
    op.index = (uint32_t)at;
    op.action_bytes.assign((const char*)bytes.data(), bytes.size());
    op.addr = customAddr; // synthetic annotation address
    op.custom = true;
    std::vector<UndoOp> ops;
    ops.push_back(std::move(op));
    (void)lenBefore;
    return app_commit(a, std::move(ops), "add line");
}

bool app_delete_action(App& a, int fi, int ai)
{
    if (!a.have_project) return false;
    if (fi < 0 || fi >= (int)a.proj.files.size()) return false;
    auto& f = a.proj.files[fi];
    if (ai < 0 || ai >= (int)f.actions.size()) return false;
    if (a.proj.action_is_referenced(fi, ai))
    {
        app_set_status(a, "cannot delete: other actions/exports reference this event", 8);
        return false;
    }
    const Action& act = f.actions[ai];
    std::vector<uint8_t> bytes = Stcm2File::serialize_action(act, 0);
    UndoOp op;
    op.kind = UndoOpKind::ActionDelete;
    op.file = fi;
    op.index = (uint32_t)ai;
    op.addr = act.original_addr; // restored on undo (display/asm/ref checks)
    op.action_bytes.assign((const char*)bytes.data(), bytes.size());
    op.custom = act.custom;
    f.actions.erase(f.actions.begin() + ai);
    std::vector<UndoOp> ops;
    ops.push_back(std::move(op));
    return app_commit(a, std::move(ops), act.custom ? "delete custom event" : "delete event");
}

bool app_move_action(App& a, int fi, int from, int to)
{
    if (!a.have_project) return false;
    auto& f = a.proj.files[fi];
    if (from >= (int)f.actions.size() || to > (int)f.actions.size() || from == to) return false;
    UndoOp op;
    op.kind = UndoOpKind::ActionMove;
    op.file = fi;
    op.from = (uint32_t)from;
    op.to = (uint32_t)to;
    Action act = std::move(f.actions[from]);
    f.actions.erase(f.actions.begin() + from);
    if (to > (int)f.actions.size()) to = (int)f.actions.size();
    f.actions.insert(f.actions.begin() + to, std::move(act));
    std::vector<UndoOp> ops;
    ops.push_back(std::move(op));
    return app_commit(a, std::move(ops), "reorder");
}

bool app_apply_asm(App& a, int fi, const Stcm2File& assembled)
{
    if (!a.have_project) return false;
    if (fi < 0 || fi >= (int)a.proj.files.size()) return false;
    UndoOp op;
    op.kind = UndoOpKind::AsmReplace;
    op.file = fi;
    std::vector<uint8_t> oldBytes = a.proj.files[fi].serialize();
    std::vector<uint8_t> newBytes = assembled.serialize();
    op.old_bytes.assign((const char*)oldBytes.data(), oldBytes.size());
    op.new_bytes.assign((const char*)newBytes.data(), newBytes.size());
    if (op.old_bytes == op.new_bytes)
    {
        // no change (e.g. comment-only edit) — still mark clean
        return true;
    }
    // The assembled file is a fresh parse: custom flags and edited flags are
    // lost. Re-mark tool-added actions (their annotation address survives the
    // round trip), then restore the edited flags by diffing against pristine.
    std::vector<uint32_t> customAddrs;
    for (const auto& act : a.proj.files[fi].actions)
        if (act.custom) customAddrs.push_back(act.original_addr);
    a.proj.files[fi] = assembled;
    for (auto& act : a.proj.files[fi].actions)
        for (uint32_t addr : customAddrs)
            if (act.original_addr == addr) { act.custom = true; break; }
    a.proj.restore_edited_flags(fi);
    std::vector<UndoOp> ops;
    ops.push_back(std::move(op));
    return app_commit(a, std::move(ops), "script edit slot " + std::to_string(fi));
}

bool app_undo(App& a)
{
    if (!a.have_project || !a.undo.can_undo()) return false;
    std::vector<UndoOp> ops;
    if (!a.undo.undo(ops)) return false;
    if (!apply_undo_step(a.proj, ops, false))
    {
        app_set_status(a, "undo could not be applied (model mismatch)", 8);
        return false;
    }
    a.dirty = true;
    a.dirty_since = (double)app_now_ms();
    a.iso_dirty = true;
    app_model_changed(a, ops.empty() ? -1 : ops.front().file);
    app_autosave(a);
    return true;
}

bool app_redo(App& a)
{
    if (!a.have_project || !a.undo.can_redo()) return false;
    std::vector<UndoOp> ops;
    if (!a.undo.redo(ops)) return false;
    if (!apply_undo_step(a.proj, ops, true))
    {
        app_set_status(a, "redo could not be applied (model mismatch)", 8);
        return false;
    }
    a.dirty = true;
    a.dirty_since = (double)app_now_ms();
    a.iso_dirty = true;
    app_model_changed(a, ops.empty() ? -1 : ops.front().file);
    app_autosave(a);
    return true;
}

bool app_begin_iso_patch(App& a, std::string* err)
{
    if (!a.have_project || a.paths.work_iso.empty()) return false;
    std::vector<uint8_t> bytes = a.proj.build_container();
    // In-place patch keeps the disc byte-for-byte and is fast; it only fails
    // when the rebuilt SCRIPT.UNI outgrows the original extent. In that case
    // (a slot grew past its sector padding) fall back to a full ISO rebuild —
    // a fresh ISO9660 image with the path tables at LBA 257/258, the position
    // SONY's cdvdman hardcodes for PS2 DVDs (proven in-game; see
    // docs/investigations/2026-08-14-chunk-budget-bypass/).
    IsoEntry e;
    std::string findErr;
    if (!iso_find(a.paths.orig_iso, "/UNION/SCRIPT.UNI", &e, &findErr))
    {
        if (err) *err = findErr;
        return false;
    }
    if (bytes.size() <= e.size)
    {
        if (!a.patcher.begin(a.paths.orig_iso, "/UNION/SCRIPT.UNI", bytes,
                             a.paths.work_iso, err))
            return false;
        a.iso_build_kind = 1;
        return true;
    }
    if (!a.rebuilder.begin(a.paths.orig_iso, "/UNION/SCRIPT.UNI", bytes,
                           a.paths.work_iso, err))
        return false;
    a.iso_build_kind = 2;
    return true;
}

void app_save_settings(App& a) { settings_save(a.settings_path, a.settings); }
