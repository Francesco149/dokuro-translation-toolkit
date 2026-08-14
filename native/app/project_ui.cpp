// Toolbar, project panel, welcome screen, settings, busy modal, confirm
// popups, and the per-frame tick (autosave / ISO rebuild / asm debounce).
#include "app.h"
#include "win32_util.h"
#include "core/cp932.h"
#include "core/uni2.h"
#include <imgui.h>
#include <imgui_stdlib.h>
#include <cstdio>

extern void rebuild_fonts(float size); // main.cpp

// ---------- per-frame housekeeping ----------

// Kicks the busy modal for an ISO rebuild/patch after app_begin_iso_patch
// succeeded (a.iso_build_kind says which worker to pump).
static void start_iso_busy(App& a)
{
    a.busy = true;
    a.busy_mode = a.iso_build_kind == 2 ? 3 : 2;
    a.busy_title = a.iso_build_kind == 2
                       ? "Rebuilding working ISO (full rebuild)..."
                       : "Rebuilding working ISO (in-place patch)...";
    a.busy_frac = 0.0;
}

void app_tick(App& a)
{
    double now = (double)app_now_ms();

    // pump async file work (ISO copy / ISO patch / ISO rebuild)
    if (a.busy)
    {
        std::string err;
        bool done = false;
        if (a.busy_mode == 1)
        {
            done = a.copier.pump(4 << 20, &err);
            a.busy_frac = a.copier.total() ? (double)a.copier.done() / (double)a.copier.total() : 0.0;
        }
        else if (a.busy_mode == 2)
        {
            done = a.patcher.pump(4 << 20, &err);
            a.busy_frac = a.patcher.total() ? (double)a.patcher.done() / (double)a.patcher.total() : 0.0;
        }
        else if (a.busy_mode == 3)
        {
            done = a.rebuilder.pump(4 << 20, &err);
            a.busy_frac = a.rebuilder.total() ? (double)a.rebuilder.done() / (double)a.rebuilder.total() : 0.0;
        }
        if (done || !err.empty())
        {
            a.busy = false;
            if (a.busy_mode == 1)
                app_finish_create(a, err.empty(), err);
            else
            {
                if (err.empty())
                {
                    app_set_status(a, "Working ISO updated: " + a.paths.work_iso, 8);
                    a.iso_dirty = false;
                }
                else
                {
                    app_set_status(a, "ISO update failed: " + err, 12);
                    a.iso_dirty = false; // avoid retry loop; manual button remains
                }
            }
        }
    }

    // autosave ISO after a quiet period
    if (a.have_project && a.iso_dirty && a.settings.autosave_iso &&
        !a.paths.work_iso.empty() && !a.busy && now - a.dirty_since > 8000.0)
    {
        std::string err;
        if (app_begin_iso_patch(a, &err))
            start_iso_busy(a);
        else
        {
            app_set_status(a, "ISO autosave skipped: " + err, 10);
            a.iso_dirty = false;
        }
    }

    // friendly-editor RU field: debounced commit (same pattern as the asm tab)
    if (a.ru_pending && now - a.ru_at > 400.0)
        app_flush_ru(a);

    // direct script editor: debounced live apply
    if (a.have_project && a.asm_dirty && now - a.asm_last_edit > 600.0)
    {
        dokuro::Stcm2File assembled;
        dokuro::AsmError err;
        if (dokuro::asm_assemble(a.asm_text, a.asm_file, assembled, err))
        {
            bool ok = app_apply_asm(a, a.asm_file, assembled);
            a.asm_dirty = false;
            a.asm_error = false;
            if (ok) a.asm_model_version = a.model_version;
            if (!ok) app_set_status(a, "script change could not be applied", 6);
            // keep the user's buffer as-is (it is the source of truth)
        }
        else
        {
            a.asm_error = true;
            a.asm_err = err;
            // model untouched — the friendly tab keeps the last good state
        }
    }
}

// ---------- toolbar ----------

static void toolbar_button(App& a, const char* label, const char* tip, bool enabled, void (*fn)(App&))
{
    if (!enabled) ImGui::BeginDisabled();
    if (ImGui::Button(label)) fn(a);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    if (!enabled) ImGui::EndDisabled();
}

static void do_new_project(App& a)
{
    std::string uni;
    if (!win32util::dialog_open("Select the clean SCRIPT.UNI (from the game ISO)",
                                win32util::filter_string("Script file (*.UNI)\0*.UNI\0All files\0*.*\0"), uni))
        return;
    std::string iso;
    if (!win32util::dialog_open("Select the game ISO (optional — needed for working-ISO output)",
                                win32util::filter_string("ISO image (*.iso)\0*.iso\0All files\0*.*\0"), iso))
        iso = ""; // cancelled: no ISO
    if (a.have_project) app_close_project(a);
    std::string err;
    if (!app_create_project(a, uni, iso, &err))
    {
        app_set_status(a, "Could not create project: " + err, 12);
        return;
    }
    a.settings.last_project = win32util::join(a.paths.root, "project.txt");
    settings_push_recent(a.settings, a.settings.last_project);
    // slot selection + asm buffer were initialized inside app_create_project
}

static void do_open_project(App& a)
{
    std::string p;
    if (!win32util::dialog_open("Open a Dokuro project (pick its project.txt)",
                                win32util::filter_string("Project file (project.txt)\0project.txt\0All files\0*.*\0"), p))
        return;
    if (a.have_project) app_close_project(a);
    std::string err;
    if (!app_open_project(a, p, &err))
    {
        app_set_status(a, "Could not open project: " + err, 12);
        return;
    }
    a.settings.last_project = p;
    settings_push_recent(a.settings, p);
    // slot selection + asm buffer were initialized inside app_open_project
}

static void do_save_now(App& a)
{
    if (app_autosave(a))
        app_set_status(a, "Saved working SCRIPT.UNI", 4);
}

static void do_update_iso(App& a)
{
    if (a.busy) return;
    std::string err;
    if (!app_begin_iso_patch(a, &err))
    {
        app_set_status(a, "ISO update failed: " + err, 12);
        return;
    }
    start_iso_busy(a);
}

static void do_export_txt(App& a)
{
    std::string out;
    if (!win32util::dialog_save("Export text dump", win32util::filter_string("Text file (*.txt)\0*.txt\0All files\0*.*\0"),
                                "dokuro_translation_dump.txt", out))
        return;
    std::string dump = dokuro::dump_export(a.proj, a.paths.orig_uni);
    try
    {
        dokuro::write_file(out, (const uint8_t*)dump.data(), dump.size());
        app_set_status(a, "Exported — edit the RU: lines in any text editor, then import back into THIS project", 8);
    }
    catch (const dokuro::Stcm2Error& e)
    {
        app_set_status(a, std::string("Export failed: ") + e.what(), 10);
    }
}

static void do_import_txt(App& a)
{
    std::string in;
    if (!win32util::dialog_open("Import text dump", win32util::filter_string("Text file (*.txt)\0*.txt\0All files\0*.*\0"), in))
        return;
    std::string text;
    try
    {
        auto bytes = dokuro::read_file(in);
        text.assign((const char*)bytes.data(), bytes.size());
    }
    catch (const dokuro::Stcm2Error& e)
    {
        app_set_status(a, std::string("Import failed: ") + e.what(), 10);
        return;
    }

    // collect one undo step for the whole import (old states captured first)
    std::vector<dokuro::UndoOp> ops;
    auto entries = a.proj.entries();
    std::vector<std::string> newTexts(entries.size());
    int n = 0;
    for (const auto& e : entries)
    {
        auto& ch = a.proj.files[e.file].actions[e.action].chunks[e.chunk];
        dokuro::UndoOp op;
        op.kind = dokuro::UndoOpKind::TextEdit;
        op.file = e.file;
        op.a = e.action;
        op.c = e.chunk;
        op.old_edited = ch.edited;
        op.old_text = ch.edited_text;
        op.old_raw.assign(ch.raw.begin(), ch.raw.end());
        ops.push_back(std::move(op));
    }
    // parse the dump into a throwaway project so we can diff reliably
    // (dump_import applies into the real model; we snapshot the deltas first)
    {
        dokuro::Project tmp = a.proj; // copy (cheap enough: 35 files, strings shared)
        dokuro::dump_import(tmp, text);
        auto tmpEntries = tmp.entries();
        for (size_t i = 0; i < ops.size() && i < tmpEntries.size(); i++)
        {
            auto& ch = tmp.files[tmpEntries[i].file].actions[tmpEntries[i].action].chunks[tmpEntries[i].chunk];
            ops[i].new_edited = ch.edited;
            ops[i].new_text = ch.edited_text;
            if (ch.edited)
            {
                std::vector<uint8_t> enc;
                size_t bad;
                uint32_t badcp;
                if (!sjis::encode(ch.edited_text, enc, &bad, &badcp))
                {
                    app_set_status(a, "Import rejected: the dump contains a character the game charset can't encode (see status)", 10);
                    return;
                }
                ops[i].new_raw.assign((const char*)enc.data(), enc.size());
            }
        }
    }
    // filter no-op ops, apply the deltas to the real model, commit once
    std::vector<dokuro::UndoOp> realOps;
    auto entries2 = a.proj.entries();
    int applied = 0;
    for (size_t i = 0; i < ops.size(); i++)
    {
        bool changed = ops[i].old_edited != ops[i].new_edited ||
                       (ops[i].new_edited && ops[i].old_text != ops[i].new_text);
        if (!changed) continue;
        auto& ch = a.proj.files[entries2[i].file].actions[entries2[i].action].chunks[entries2[i].chunk];
        ch.edited = ops[i].new_edited;
        ch.edited_text = ops[i].new_text;
        realOps.push_back(std::move(ops[i]));
        applied++;
    }
    if (app_commit(a, std::move(realOps), "import text dump"))
        app_set_status(a, "Imported: " + std::to_string(applied) + " line(s) changed. Review, then test in an emulator.", 8);
    else
        app_set_status(a, "Import could not be saved", 8);
}

static bool g_settingsWindowOpen = false;

void ui_project_toolbar(App& a)
{
    ImGui::BeginDisabled(!a.have_project);
    ImGui::TextUnformatted("Project:");
    ImGui::EndDisabled();
    ImGui::SameLine();
    toolbar_button(a, "New project", "Start a fresh project from a clean SCRIPT.UNI (+ optional ISO)", true, do_new_project);
    ImGui::SameLine();
    toolbar_button(a, "Open project", "Open an existing project (pick project.txt)", true, do_open_project);
    ImGui::SameLine();
    if (a.have_project)
    {
        ImGui::SameLine();
        ImGui::TextUnformatted(win32util::basename_full(a.paths.root).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Open working folder")) win32util::open_in_explorer(win32util::dirname(a.paths.work_uni));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.paths.work_uni.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Open original folder")) win32util::open_in_explorer(win32util::dirname(a.paths.orig_uni));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.paths.orig_uni.c_str());
        ImGui::SameLine();
        toolbar_button(a, "Save now", "Write the working SCRIPT.UNI immediately", !a.busy, do_save_now);
        ImGui::SameLine();
        toolbar_button(a, "Export txt", "Export the plain-text dump (RU: lines only)", !a.busy, do_export_txt);
        ImGui::SameLine();
        toolbar_button(a, "Import txt", "Import an edited dump (robust: unknown blocks ignored)", !a.busy, do_import_txt);
        if (!a.paths.work_iso.empty())
        {
            ImGui::SameLine();
            toolbar_button(a, "Update ISO", "Rebuild the working ISO with the current script", !a.busy, do_update_iso);
        }
        ImGui::SameLine();
        if (ImGui::Button("Undo") && a.undo.can_undo()) app_undo(a);
        ImGui::SameLine();
        if (ImGui::Button("Redo") && a.undo.can_redo()) app_redo(a);
        ImGui::SameLine();
        char ubuf[64];
        snprintf(ubuf, sizeof(ubuf), "(%zu)", a.undo.steps());
        ImGui::TextUnformatted(ubuf);
    }
    ImGui::SameLine();
    if (ImGui::Button("Settings")) g_settingsWindowOpen = true;
    ImGui::Separator();
}

// ---------- project panel (where things are) ----------
// Rendered INSIDE the main window as a collapsible section (the old floating
// window blocked the editor). The over-budget warning stays visible even
// when the section is collapsed.

void ui_project_panel(App& a)
{
    if (!a.have_project) return;
    if (ImGui::CollapsingHeader("Project files"))
    {
        auto row = [&](const char* label, const std::string& path) {
            if (path.empty()) return;
            ImGui::BulletText("%s:", label);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", path.c_str());
            ImGui::SameLine();
            char id[64];
            snprintf(id, sizeof(id), "open##%s", label);
            if (ImGui::SmallButton(id)) win32util::open_in_explorer(path);
        };
        row("Original script", a.paths.orig_uni);
        row("Original ISO", a.paths.orig_iso);
        row("WORKING script (autosaved)", a.paths.work_uni);
        row("WORKING ISO (autosaved)", a.paths.work_iso);
        ImGui::TextDisabled("Project folder:");
        ImGui::SameLine();
        ImGui::TextUnformatted(a.paths.root.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("open##root")) win32util::open_in_explorer(a.paths.root);
    }
    // budget summary (cached lens + budgets; serializing the pristine slots
    // per frame — file_budget — was the 33fps choppiness source)
    int over = 0;
    for (size_t i = 0; i < a.slot_lens.size() && i < a.slot_budgets.size(); i++)
        if (a.slot_lens[i] > a.slot_budgets[i]) over++;
    if (over > 0)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("WARNING: %d script slot(s) exceed their sector padding — SCRIPT.UNI will "
                           "grow and the ISO cannot be patched in place; Update ISO rebuilds the whole "
                           "disc image to hold the bigger file (in-game it plays fine). Shorten lines "
                           "in those slots to stay with the fast in-place patch.", over);
        ImGui::PopStyleColor();
    }
}

// ---------- welcome ----------

void ui_welcome(App& a)
{
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(900, 520), ImGuiCond_Always);
    ImGui::Begin("Dokuro-chan Script Editor", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    ImGui::TextWrapped(
        "This tool edits the Japanese dialogue and nameplate text directly inside SCRIPT.UNI.\n"
        "\n"
        "Start a project: select the clean SCRIPT.UNI (e.g. extracted from UNION/ on the game ISO), "
        "and optionally the game ISO itself. The tool creates a project folder in your Documents "
        "under dokuro\\projects, named after the script and timestamped:\n"
        "\n"
        "  <My Documents>\\dokuro\\projects\\Dokuro_<script>_<date>-<time>/\n"
        "    original/   - untouched copies of SCRIPT.UNI and the ISO (never modified)\n"
        "    working/    - YOUR working modified SCRIPT.UNI and ISO, autosaved on every change\n"
        "    project.txt - project state (open the project later by picking this file)\n"
        "\n"
        "Safety rails (from in-game testing):\n"
        "  - Script slots may grow freely within their sector padding; the editor rebuilds the "
        "embedded chunk table on save so the game reads grown slots correctly. Slots that grow "
        "PAST their padding make SCRIPT.UNI bigger than the original, which the in-place ISO "
        "patch cannot hold — Update ISO rebuilds the whole disc image for those; they are "
        "flagged in red.\n"
        "  - Deleting original lines/events warns first. Added lines are tracked as 'custom'.\n"
        "\n"
        "Undo history is saved per project (undo.log) and survives restarts. Both the in-memory "
        "backlog size and on-disk pruning are adjustable in Settings.");
    ImGui::Dummy(ImVec2(0, 12));
    if (ImGui::Button("New project...", ImVec2(220, 0))) do_new_project(a);
    ImGui::SameLine();
    if (ImGui::Button("Open project...", ImVec2(220, 0))) do_open_project(a);
    if (!a.settings.recent_projects.empty())
    {
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::TextUnformatted("Recent projects:");
        for (const auto& r : a.settings.recent_projects)
        {
            if (ImGui::SmallButton(r.c_str()))
            {
                std::string err;
                if (a.have_project) app_close_project(a);
                if (!app_open_project(a, r, &err))
                    app_set_status(a, "Could not open project: " + err, 12);
            }
        }
    }
    ImGui::End();
}

// ---------- busy modal ----------

void ui_busy_modal(App& a)
{
    if (!a.busy) return;
    ImGui::OpenPopup("busy");
    ImGui::SetNextWindowSize(ImVec2(520, 0));
    if (ImGui::BeginPopupModal("busy", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
    {
        ImGui::TextUnformatted(a.busy_title.c_str());
        ImGui::ProgressBar((float)a.busy_frac, ImVec2(-1, 0));
        ImGui::EndPopup();
    }
}

// ---------- confirm popups (delete / add-over-budget) ----------

void ui_confirm_popups(App& a)
{
    if (a.confirm_kind == 0 && a.confirm_action >= 0)
    {
        ImGui::OpenPopup("confirm_delete");
        if (ImGui::BeginPopupModal("confirm_delete", nullptr,
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
        {
            bool custom = a.proj.files[a.confirm_file].actions[a.confirm_action].custom;
            ImGui::TextWrapped(
                custom
                    ? "Delete this line/event? It was added by you (not in the original script), so "
                      "the game does not depend on it."
                    : "Delete this line/event?\n\n"
                      "WARNING: this event exists in the ORIGINAL script. The game's scene logic "
                      "expects it, and removing it may break the scene (softlock, skipped text, or "
                      "crashes) even though the file itself stays valid. The size budget stays fine "
                      "since removing shrinks the slot.");
            ImGui::Dummy(ImVec2(0, 8));
            if (ImGui::Button("Delete", ImVec2(140, 0)))
            {
                int fi = a.confirm_file, ai = a.confirm_action;
                a.confirm_kind = 0;
                a.confirm_action = -1;
                ImGui::CloseCurrentPopup();
                app_delete_action(a, fi, ai);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(140, 0)))
            {
                a.confirm_kind = 0;
                a.confirm_action = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    if (a.confirm_kind == 3 && a.confirm_action >= 0)
    {
        ImGui::OpenPopup("add_over_budget");
        if (ImGui::BeginPopupModal("add_over_budget", nullptr,
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
        {
            ImGui::TextWrapped("%s", a.confirm_msg.c_str());
            ImGui::Dummy(ImVec2(0, 8));
            if (ImGui::Button("OK", ImVec2(140, 0)))
            {
                a.confirm_kind = 0;
                a.confirm_action = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    if (a.confirm_kind == 2 && a.confirm_action >= 0)
    {
        ImGui::OpenPopup("confirm_add");
        if (ImGui::BeginPopupModal("confirm_add", nullptr,
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
        {
            ImGui::TextWrapped("%s", a.confirm_msg.c_str());
            ImGui::TextWrapped(
                "Adding this line pushes the slot past its sector padding: SCRIPT.UNI will grow, "
                "so the ISO cannot be patched in place — Update ISO rebuilds the whole disc image "
                "to hold the bigger file (in-game it plays fine). Shorten other lines in the same "
                "slot to stay within the padding.");
            ImGui::Dummy(ImVec2(0, 8));
            if (ImGui::Button("Add anyway", ImVec2(140, 0)))
            {
                int fi = a.confirm_file, ai = a.confirm_action;
                a.confirm_kind = 0;
                a.confirm_action = -1;
                ImGui::CloseCurrentPopup();
                std::string warn;
                app_insert_action(a, fi, ai, 0x118, "", &warn);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(140, 0)))
            {
                a.confirm_kind = 0;
                a.confirm_action = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

// ---------- settings ----------

void ui_settings_window(App& a)
{
    if (!g_settingsWindowOpen) return;
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &g_settingsWindowOpen))
    {
        ImGui::End();
        return;
    }
    float fs = a.settings.font_size;
    ImGui::TextUnformatted("UI font size:");
    ImGui::SameLine();
    if (ImGui::SliderFloat("##fontsize", &fs, 13.0f, 30.0f, "%.0f px"))
    {
        a.settings.font_size = fs;
        rebuild_fonts(fs);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Default 19 px — the old WinForms tool used ~12 px");

    ImGui::Separator();
    ImGui::TextUnformatted("Undo history (stored as per-change operations, deduplicated):");
    int ram = a.settings.undo_ram_cap;
    if (ImGui::InputInt("Steps kept in RAM", &ram))
    {
        if (ram < 1) ram = 1;
        if (ram > 100000) ram = 100000;
        a.settings.undo_ram_cap = ram;
        a.undo.set_ram_cap((size_t)ram);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Steps beyond this are loaded from disk on demand. Lower for machines with little RAM.");
    int keep = a.settings.undo_disk_keep;
    if (ImGui::InputInt("Steps kept on disk (0 = unlimited)", &keep))
    {
        if (keep < 0) keep = 0;
        a.settings.undo_disk_keep = keep;
    }
    if (ImGui::Button("Prune undo history to this limit"))
    {
        a.undo.prune((size_t)keep);
        app_set_status(a, "Undo history pruned", 5);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear undo history"))
    {
        a.undo.clear_history();
        app_set_status(a, "Undo history cleared", 5);
    }

    ImGui::Separator();
    bool iso = a.settings.autosave_iso;
    if (ImGui::Checkbox("Autosave the working ISO (after edits settle)", &iso))
        a.settings.autosave_iso = iso;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The ISO is ~4 GB, so it is rebuilt only after a quiet period. "
                          "Use 'Update ISO' in the toolbar for an immediate rebuild.");
    ImGui::End();
}
