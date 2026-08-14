// Shared application state + document operations for the editor UI.
// All document mutations funnel through the app_* helpers so every change
// produces an undo op, an autosave, and a budget/encoding refresh.
#pragma once
#include "core/asmfmt.h"
#include "core/iso.h"
#include "core/project.h"
#include "core/undo.h"
#include "settings.h"
#include <string>
#include <vector>

using namespace dokuro;

struct App
{
    AppSettings settings;
    std::string settings_path;

    // ---------- project ----------
    bool have_project = false;
    struct Paths
    {
        std::string root;         // project dir
        std::string orig_uni;     // original/SCRIPT.UNI
        std::string orig_iso;     // original/<iso> (empty when no ISO)
        std::string work_uni;     // working/SCRIPT.UNI
        std::string work_iso;     // working/<iso> (empty when no ISO)
        std::string iso_name;     // ISO basename
    } paths;
    Project proj;
    std::vector<uint8_t> pristine_bytes; // pristine container (identity + hash)
    uint64_t pristine_hash = 0;
    UndoLog undo;
    bool dirty = false;
    double dirty_since = 0;      // clock time of last commit (autosave debounce)
    bool iso_dirty = false;

    // ---------- async file work (copies/patches pumped per frame) ----------
    FileCopier copier;
    IsoPatcher patcher;
    IsoRebuilder rebuilder;   // full ISO rebuild (grown SCRIPT.UNI)
    bool busy = false;
    int busy_mode = 0;           // 0 none, 1 iso copy (project create), 2 iso patch, 3 iso rebuild
    int iso_build_kind = 0;      // set by app_begin_iso_patch: 1 in-place, 2 full rebuild
    std::string busy_title;
    double busy_frac = 0.0;

    // ---------- friendly editor ----------
    int sel_file = 0;            // current file slot
    int sel_action = -1;         // selected action (row)
    std::string search;
    int kind_filter = 1;         // 0 all, 1 dialogue (dialogue+advance+nameplate, default)
    bool only_untranslated = false;
    bool only_issues = false;
    int last_edit_file = -1;     // slot of the most recent edit (persisted; -1 none)

    // ---------- perf ----------
    uint64_t model_version = 0;  // bumped on every commit — UI caches (rows,
                                 // slot lengths) keyed on this
    std::vector<size_t> slot_lens; // serialized length per slot, refreshed per
                                 // commit (file_serialized_len serializes the
                                 // whole slot — too slow to call per frame)
    std::vector<uint32_t> slot_budgets; // pristine slot budgets — computed once
                                 // per project load (originals never change;
                                 // file_budget serializes a pristine slot per
                                 // call — too slow to call per frame)
    int translated_count = 0;     // status bar counters, refreshed per commit
    int total_count = 0;          // (entries() walks the whole model)

    // ---------- friendly-editor RU field (debounced commit) ----------
    // Set by editor_ui on every keystroke; app_tick commits after a quiet
    // period, editor_ui flushes on focus loss, app_close_project flushes on
    // close — so a typed translation is never lost and the list refreshes
    // as you type.
    int ru_file = -1;
    int ru_action = -1;
    int ru_chunk = -1;
    std::string ru_text;
    bool ru_pending = false;
    double ru_at = 0;

    // ---------- direct script editor ----------
    int asm_file = 0;
    std::string asm_text;        // live buffer
    bool asm_dirty = false;
    double asm_last_edit = 0;
    bool asm_error = false;
    dokuro::AsmError asm_err;
    uint64_t asm_model_version = 0; // model_version when the buffer was last
                                 // synced (reload/apply) — differs while the
                                 // dialogue editor has newer edits

    // ---------- UI bits ----------
    std::string status;
    double status_until = 0;
    bool confirm_open = false;   // popup id states
    int confirm_kind = 0;        // 0 delete action, 2 add line over budget
    int confirm_action = -1;
    int confirm_file = -1;
    std::string confirm_msg;
};

// ---- project lifecycle ----
// Creates a fresh project folder next to the UNI (copies pristine files).
// ISO is optional ("" = none). Runs the copy synchronously (small files).
bool app_create_project(App& a, const std::string& uniPath,
                        const std::string& isoPath, std::string* err);
// Opens an existing project from its project.txt.
bool app_open_project(App& a, const std::string& projTxtPath, std::string* err);
void app_close_project(App& a);

// ---- document ops (each commits an undo step + autosaves) ----
// Low-level: applies nothing, just records ops + autosaves (used by bulk
// import). Callers must have already applied the ops to the model.
bool app_commit(App& a, std::vector<dokuro::UndoOp> ops, const std::string& label);
bool app_set_translation(App& a, const dokuro::EntryRef& e, const std::string* text);
// Commits the debounced RU-field edit if any is pending (no-op otherwise).
void app_flush_ru(App& a);
bool app_revert_chunk(App& a, const dokuro::EntryRef& e);
// inserts a new action (0x118/0x119/0x11A) after `after_action`; text used for 0x118
void app_log(const char* fmt, ...);

bool app_insert_action(App& a, int fi, int after_action, uint32_t opcode,
                       const std::string& text, std::string* warnOverBudget);
// deletes an action; was_custom controls the warning (checked by the UI)
bool app_delete_action(App& a, int fi, int ai);
// moves action from -> to (same file); from/to are indices at call time
bool app_move_action(App& a, int fi, int from, int to);
// direct-editor commit: replaces the whole slot with the assembled file
bool app_apply_asm(App& a, int fi, const dokuro::Stcm2File& assembled);
bool app_undo(App& a);
bool app_redo(App& a);

// ---- persistence ----
// Writes working/SCRIPT.UNI (+ state). Called by every commit; returns false
// on disk error (status message set).
bool app_autosave(App& a);
// Rebuilds working ISO from the current working SCRIPT.UNI (chunked; caller
// pumps a.patcher or a.rebuilder — check a.iso_build_kind: 1 in-place patch,
// 2 full rebuild for a SCRIPT.UNI grown past its original extent). Returns
// false only when the ISO cannot be read or written.
bool app_begin_iso_patch(App& a, std::string* err);

// ---- misc ----
// Called by the UI when the async ISO copy finishes (or fails).
void app_finish_create(App& a, bool ok, const std::string& err);
uint64_t app_now_ms();
void app_set_status(App& a, const std::string& msg, double seconds);
void app_save_settings(App& a);
