// Friendly dialogue editor tab: per-file event/line table with drag handles,
// filters, and a detail panel (JP read-only + RU editable, revert/delete/add).
#include "app.h"
#include "core/cp932.h"
#include "core/text.h"
#include <imgui.h>
#include <imgui_internal.h> // ImGuiInputTextState (Enter = next line)
#include <imgui_stdlib.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Row
{
    int action = -1;      // index into the current file's actions
    bool is_text = false; // carries a translatable text chunk
    int chunk = -1;
};

// Builds the visible row list for the current file with filters applied.
std::vector<Row> build_rows(App& a)
{
    std::vector<Row> rows;
    const auto& f = a.proj.files[a.sel_file];
    for (size_t i = 0; i < f.actions.size(); i++)
    {
        const auto& act = f.actions[i];
        if (act.call)
        {
            // calls are structural; only shown when no filter is active
            if (a.kind_filter == 0 && a.search.empty() && !a.only_untranslated && !a.only_issues)
                rows.push_back(Row{ (int)i, false, -1 });
            continue;
        }
        bool dialogue = act.opcode == 0x118;
        bool advance = act.opcode == 0x119;
        bool nameplate = act.opcode == 0x11A;
        if (!dialogue && !advance && !nameplate)
        {
            if (a.kind_filter == 0 && a.search.empty() && !a.only_untranslated && !a.only_issues)
                rows.push_back(Row{ (int)i, false, -1 });
            continue;
        }
        if (a.kind_filter == 1 && !dialogue && !advance && !nameplate) continue;
        // text chunk (every dialogue/nameplate action has exactly one)
        int ci = -1;
        for (size_t c = 0; c < act.chunks.size(); c++)
            if (act.chunks[c].is_text) { ci = (int)c; break; }
        if (ci < 0)
        {
            // no editable text (advance 0x119 rows): structural row, hidden by
            // the text-based filters
            if (!a.search.empty() || a.only_untranslated || a.only_issues) continue;
            rows.push_back(Row{ (int)i, false, -1 });
            continue;
        }
        const auto& ch = act.chunks[ci];
        bool untranslated = a.only_untranslated && ch.edited;
        // "issues only" = rows with unencodable characters (the one real
        // per-row problem the tool can detect; the old dialogue-width
        // heuristic was removed as outdated).
        bool issues = false;
        if (a.only_issues && ch.edited)
        {
            std::vector<uint8_t> enc;
            size_t bad;
            uint32_t badcp;
            issues = !sjis::encode(ch.edited_text, enc, &bad, &badcp);
        }
        if (untranslated || issues) continue;
        if (!a.search.empty())
        {
            bool hit = ch.text.find(a.search) != std::string::npos ||
                       ch.edited_text.find(a.search) != std::string::npos;
            if (!hit) continue;
        }
        rows.push_back(Row{ (int)i, true, ci });
    }
    return rows;
}

const char* kind_name(const dokuro::Stcm2File& f, int ai)
{
    const auto& act = f.actions[ai];
    if (act.call) return "call";
    switch (act.opcode)
    {
    case 0x118: return "dialogue";
    case 0x119: return "advance";
    case 0x11A: return "nameplate";
    default:
    {
        static char buf[32];
        snprintf(buf, sizeof(buf), "event 0x%X", act.opcode);
        return buf;
    }
    }
}

// Payload for drag & drop: action index within the current file.
struct DragRow
{
    int action;
};

// Detail-panel RU edit buffer (persists across frames; reloaded on selection
// or model change). File-scope so the uitest can assert its content.
char g_ruBuf[8192];
int g_editFile = -1, g_editAction = -1, g_editChunk = -1;
uint64_t g_editVersion = 0;

// Cached visible-row list (built under the current filters; rebuilt only when
// the model or filter state changed). File-scope so the uitest can assert the
// current row index (Enter = next line).
std::vector<Row> g_rows;

// RU field rect recorded during rendering (last frame it was visible) so the
// headless uitest can click it at the exact client coordinates.
int g_ruFieldX = -1, g_ruFieldY = -1;
uint64_t g_rows_version = ~0ull;
int g_rows_file = -1;
std::string g_rows_search;
int g_rows_kind = -1;
bool g_rows_ut = false, g_rows_issues = false;

void do_add_line(App& a, int afterAction, uint32_t opcode)
{
    std::string warn;
    if (!app_insert_action(a, a.sel_file, afterAction, opcode, "", &warn))
        return;
    a.sel_action = afterAction + 1;
    if (!warn.empty())
    {
        // the insertion happened but pushed the slot over budget — informational
        a.confirm_kind = 3;
        a.confirm_action = a.sel_action;
        a.confirm_file = a.sel_file;
        a.confirm_msg = warn + "\n\nThe line was added. Use Undo to remove it, or shorten "
                               "other lines in this slot to get back within the padding.";
    }
}

} // namespace

// ---- test hooks (uitest): row visibility under the current filters ----
int ui_visible_row_count(App& a)
{
    return (int)build_rows(a).size();
}

bool ui_visible_rows_match_kind(App& a, int kindFilter)
{
    for (const Row& r : build_rows(a))
    {
        uint32_t op = a.proj.files[a.sel_file].actions[r.action].opcode;
        bool ok = kindFilter == 1 ? (op == 0x118 || op == 0x119 || op == 0x11A)
                                  : true;
        if (!ok) return false;
    }
    return true;
}

int ui_visible_advance_count(App& a)
{
    int n = 0;
    for (const Row& r : build_rows(a))
        if (a.proj.files[a.sel_file].actions[r.action].opcode == 0x119) n++;
    return n;
}

const char* ui_ru_buffer()
{
    return g_ruBuf;
}

// Index of the current selection in the visible-row list (-1 if not visible).
int ui_sel_row_index(App& a)
{
    for (size_t i = 0; i < g_rows.size(); i++)
        if (g_rows[i].action == a.sel_action) return (int)i;
    return -1;
}

bool ui_ru_field(int* x, int* y)
{
    if (g_ruFieldX < 0 || g_ruFieldY < 0) return false;
    *x = g_ruFieldX;
    *y = g_ruFieldY;
    return true;
}

void ui_editor_tab(App& a)
{
    if (!a.have_project || a.proj.files.empty()) return;

    // ---- file selector + budget ----
    ImGui::TextUnformatted("Script slot:");
    ImGui::SameLine();
    {
        std::vector<const char*> names;
        std::vector<std::string> storage;
        for (size_t i = 0; i < a.proj.files.size(); i++)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "slot %zu", i);
            storage.push_back(buf);
            names.push_back(storage.back().c_str());
        }
        ImGui::SetNextItemWidth(160);
        if (ImGui::Combo("##slot", &a.sel_file, names.data(), (int)names.size()))
            a.sel_action = -1;
    }
    ImGui::SameLine();
    {
        size_t fi = (size_t)a.sel_file;
        // cached per-slot lengths + budgets (serializing slots per frame was
        // a choppiness source)
        size_t used = (size_t)fi < a.slot_lens.size() ? a.slot_lens[fi]
                                                      : a.proj.file_serialized_len(fi);
        uint32_t budget = (size_t)fi < a.slot_budgets.size() ? a.slot_budgets[fi]
                                                             : a.proj.file_budget(fi);
        char buf[128];
        snprintf(buf, sizeof(buf), "padding: 0x%X used: 0x%zX",
                 budget, used);
        bool over = used > budget;
        if (over) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.3f, 1.0f));
        else ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.85f, 0.5f, 1.0f));
        ImGui::TextUnformatted(buf);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Sector padding budget: the most this slot can grow before SCRIPT.UNI's "
                              "file size grows. In-game, grown slots read fine (the editor rebuilds the "
                              "embedded chunk table on save); 'padding' only matters because a bigger "
                              "SCRIPT.UNI needs a full ISO rebuild (Update ISO does it automatically).");
        ImGui::SameLine();
        if (over && ImGui::SmallButton("over padding!"))
            ImGui::OpenPopup("budget_help");
        if (ImGui::BeginPopup("budget_help"))
        {
            ImGui::TextWrapped("This slot exceeds its sector padding: SCRIPT.UNI will grow, so the "
                               "in-place ISO patch is impossible — Update ISO rebuilds the whole disc "
                               "image instead. In-game it plays fine — this is a file-size constraint, "
                               "not a crash risk.");
            ImGui::EndPopup();
        }
    }

    // ---- filters ----
    ImGui::TextUnformatted("Search:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    std::string searchCopy = a.search;
    if (ImGui::InputText("##search", &searchCopy)) a.search = searchCopy;
    ImGui::SameLine();
    ImGui::TextUnformatted("Show:");
    ImGui::SameLine();
    {
        static const char* kinds[] = { "All", "Dialogue" };
        ImGui::SetNextItemWidth(140);
        ImGui::Combo("##kind", &a.kind_filter, kinds, 2);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Only untranslated", &a.only_untranslated);
    ImGui::SameLine();
    ImGui::Checkbox("Only issues", &a.only_issues);
    ImGui::SameLine();
    if (ImGui::SmallButton("Revert all in this slot"))
    {
        int reverted = 0;
        for (const auto& e : a.proj.entries())
        {
            if (e.file != a.sel_file) continue;
            if (a.proj.revert_chunk(e)) reverted++;
        }
        if (reverted > 0)
        {
            std::vector<dokuro::UndoOp> ops;
            for (const auto& e : a.proj.entries())
            {
                if (e.file != a.sel_file) continue;
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
            app_commit(a, std::move(ops), "revert slot");
            app_set_status(a, "Reverted " + std::to_string(reverted) + " line(s) in this slot", 6);
        }
    }

    ImGui::Separator();

    if (a.sel_file >= (int)a.proj.files.size()) a.sel_file = 0;
    // build_rows walks every action in the slot (tens of thousands); cache
    // the result and rebuild only when the model or the filter state changed.
    if (g_rows_version != a.model_version || g_rows_file != a.sel_file ||
        g_rows_search != a.search || g_rows_kind != a.kind_filter ||
        g_rows_ut != a.only_untranslated || g_rows_issues != a.only_issues)
    {
        g_rows = build_rows(a);
        g_rows_version = a.model_version;
        g_rows_file = a.sel_file;
        g_rows_search = a.search;
        g_rows_kind = a.kind_filter;
        g_rows_ut = a.only_untranslated;
        g_rows_issues = a.only_issues;
    }
    const auto& rows = g_rows;
    if (a.sel_action >= (int)a.proj.files[a.sel_file].actions.size()) a.sel_action = -1;

    // ---- table ----
    float tableH = ImGui::GetContentRegionAvail().y * 0.55f;
    if (tableH < 120) tableH = 120;
    if (ImGui::BeginTable("lines", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_NoHostExtendY | ImGuiTableFlags_BordersV,
                          ImVec2(0, tableH)))
    {
        ImGui::TableSetupColumn("##drag", ImGuiTableColumnFlags_WidthFixed, 26);
        ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("addr", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("JP", ImGuiTableColumnFlags_WidthStretch, 340);
        ImGui::TableSetupColumn("RU", ImGuiTableColumnFlags_WidthStretch, 340);
        ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin((int)rows.size());
        while (clipper.Step())
        {
            for (int ri = clipper.DisplayStart; ri < clipper.DisplayEnd; ri++)
            {
                const Row& row = rows[ri];
                const auto& f = a.proj.files[a.sel_file];
                const auto& act = f.actions[row.action];
                bool selected = a.sel_action == row.action;
                ImGui::TableNextRow();

                // drag handle
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(row.action);
                if (ImGui::Button("::", ImVec2(22, 0)))
                {
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Drag to reorder. Lines can be moved between events; "
                                      "events move within this slot.");
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                {
                    DragRow payload{ row.action };
                    ImGui::SetDragDropPayload("DOKUROW", &payload, sizeof(payload));
                    ImGui::Text("move %s @0x%X", kind_name(f, row.action), act.original_addr);
                    ImGui::EndDragDropSource();
                }
                // drop target: insert before/after depending on cursor position
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("DOKUROW"))
                    {
                        DragRow src = *(const DragRow*)pl->Data;
                        if (src.action != row.action)
                        {
                            float cy = ImGui::GetCursorScreenPos().y - ImGui::GetTextLineHeight();
                            float my = ImGui::GetMousePos().y;
                            int to = row.action;
                            if (my > cy + ImGui::GetTextLineHeight() / 2) to = row.action + 1;
                            int from = src.action;
                            if (to > from) to--;
                            if (from != to) app_move_action(a, a.sel_file, from, to);
                            a.sel_action = to;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                // kind
                ImGui::TableSetColumnIndex(1);
                const char* kn = kind_name(f, row.action);
                ImGui::PushStyleColor(ImGuiCol_Text,
                    act.custom ? ImVec4(0.55f, 0.9f, 0.6f, 1.0f)
                               : ImVec4(0.85f, 0.85f, 0.9f, 1.0f));
                ImGui::TextUnformatted(kn);
                ImGui::PopStyleColor();
                if (act.custom && ImGui::IsItemHovered())
                    ImGui::SetTooltip("added by you (custom) — not in the original script");

                // addr
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("0x%X", act.original_addr);

                // JP + RU text
                if (row.is_text)
                {
                    const auto& ch = act.chunks[row.chunk];
                    ImGui::TableSetColumnIndex(3);
                    if (ImGui::Selectable(ch.text.c_str(), selected,
                                          ImGuiSelectableFlags_SpanAllColumns))
                    {
                        a.sel_action = row.action;
                    }
                    ImGui::TableSetColumnIndex(4);
                    // live preview while the detail panel's edit is pending
                    // (committed after a quiet period / focus loss)
                    const char* ruShown = ch.edited ? ch.edited_text.c_str() : "";
                    if (a.ru_pending && row.action == a.ru_action &&
                        a.sel_file == a.ru_file && row.chunk == a.ru_chunk)
                        ruShown = a.ru_text.c_str();
                    ImGui::TextUnformatted(ruShown);
                    ImGui::TableSetColumnIndex(5);
                    if (ch.edited)
                    {
                        std::vector<uint8_t> enc2;
                        size_t bad2;
                        uint32_t badcp2;
                        bool hasBad = !sjis::encode(ch.edited_text, enc2, &bad2, &badcp2);
                        if (hasBad)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.3f, 1.0f));
                            ImGui::TextUnformatted("BAD CHAR");
                            ImGui::PopStyleColor();
                        }
                    }
                }
                else
                {
                    ImGui::TableSetColumnIndex(3);
                    if (ImGui::Selectable("", selected, ImGuiSelectableFlags_SpanAllColumns))
                        a.sel_action = row.action;
                }
                ImGui::PopID();
            }
        }
        clipper.End();
        ImGui::EndTable();
    }

    // ---- detail panel ----
    if (a.sel_action >= 0 && a.sel_action < (int)a.proj.files[a.sel_file].actions.size())
    {
        auto& f = a.proj.files[a.sel_file];
        auto& act = f.actions[a.sel_action];
        bool nameplate = act.opcode == 0x11A;
        ImGui::Separator();
        ImGui::TextUnformatted(kind_name(f, a.sel_action));
        ImGui::SameLine();
        ImGui::TextDisabled("action @0x%X%s", act.original_addr, act.custom ? " (custom)" : "");
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete"))
        {
            a.confirm_kind = 0;
            a.confirm_action = a.sel_action;
            a.confirm_file = a.sel_file;
        }
        ImGui::SameLine();
        int clickAction = a.sel_action;
        bool didAdd = false;
        if (ImGui::SmallButton("+ dialogue line"))
        {
            do_add_line(a, clickAction, 0x118);
            didAdd = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+ box advance"))
        {
            do_add_line(a, clickAction, 0x119);
            didAdd = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+ nameplate"))
        {
            do_add_line(a, clickAction, 0x11A);
            didAdd = true;
        }
        // `act` is a reference into f.actions; the insert above may have
        // reallocated the vector, and the selection moved to the new line.
        // Render the new line next frame instead of reading a stale/dangling
        // reference (which also leaked the previous line's text into ruBuf).
        if (didAdd) return;

        int ci = -1;
        for (size_t c = 0; c < act.chunks.size(); c++)
            if (act.chunks[c].is_text) { ci = (int)c; break; }

        if (ci >= 0)
        {
            auto& ch = act.chunks[ci];
            // JP (read-only, selectable/copyable)
            ImGui::TextUnformatted("Japanese (original, read-only):");
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.12f, 0.15f, 1.0f));
            ImGui::InputTextMultiline("##jp", (char*)ch.text.c_str(), ch.text.size() + 1,
                                      ImVec2(-1, 3 * ImGui::GetTextLineHeight()),
                                      ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();

            // RU (editable, commit on focus loss)
            ImGui::TextUnformatted("Russian (translation):");
            if (g_editFile != a.sel_file || g_editAction != a.sel_action || g_editChunk != ci)
            {
                // selection changed: reload from the model (any pending edit for
                // the previous row is flushed by the focus-loss handler below)
                std::string cur = ch.edited ? ch.edited_text : "";
                strncpy(g_ruBuf, cur.c_str(), sizeof(g_ruBuf) - 1);
                g_ruBuf[sizeof(g_ruBuf) - 1] = 0;
                g_editFile = a.sel_file;
                g_editAction = a.sel_action;
                g_editChunk = ci;
                g_editVersion = a.model_version;
            }
            else if (g_editVersion != a.model_version && !a.ru_pending)
            {
                // the model changed under this row (add/undo/redo/import) —
                // refresh, but never clobber in-progress typing
                std::string cur = ch.edited ? ch.edited_text : "";
                strncpy(g_ruBuf, cur.c_str(), sizeof(g_ruBuf) - 1);
                g_ruBuf[sizeof(g_ruBuf) - 1] = 0;
                g_editVersion = a.model_version;
            }
            // Single-line: the script text cannot contain newlines (game
            // dialogue boxes render one line). Enter commits and moves to the
            // next visible line; the debounced commit covers typing pauses.
            //
            // NOTE: with EnterReturnsTrue the InputText return value is
            // Enter-only (it returns `validated`, not `value_changed`), so
            // typing edits must be detected via IsItemEdited — otherwise the
            // live table preview and the debounced autosave never fire while
            // typing.
            bool ruEnter = ImGui::InputText("##ru", g_ruBuf, sizeof(g_ruBuf),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemEdited())
            {
                a.ru_file = a.sel_file;
                a.ru_action = a.sel_action;
                a.ru_chunk = ci;
                a.ru_text = g_ruBuf;
                a.ru_pending = true;
                a.ru_at = (double)app_now_ms();
            }
            if (ruEnter)
            {
                // Enter = save + next line (return value is Enter-only with
                // EnterReturnsTrue)
                app_flush_ru(a);
                for (size_t i = 0; i < g_rows.size(); i++)
                    if (g_rows[i].action == a.sel_action)
                    {
                        if (i + 1 < g_rows.size())
                        {
                            a.sel_action = g_rows[i + 1].action;
                            // the field stays active; force it to re-read
                            // the next row's text next frame
                            if (ImGuiInputTextState* st =
                                    ImGui::GetInputTextState(ImGui::GetItemID()))
                                st->ReloadUserBufAndMoveToEnd();
                        }
                        break;
                    }
            }
            if (ImGui::IsItemDeactivated())
                app_flush_ru(a); // focus loss (click elsewhere) commits now
            // record the field rect (headless uitest clicks it at these coords)
            {
                ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                g_ruFieldX = (int)((mn.x + mx.x) * 0.5f);
                g_ruFieldY = (int)((mn.y + mx.y) * 0.5f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Revert to original"))
            {
                // drop any pending edit so the debounce can't resurrect it
                a.ru_pending = false;
                a.ru_file = -1;
                dokuro::EntryRef e{ a.sel_file, a.sel_action, ci,
                                    nameplate ? dokuro::EntryKind::Nameplate
                                              : dokuro::EntryKind::Dialogue };
                app_revert_chunk(a, e);
                g_editFile = -1; // force reload of the buffer next frame
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Restores the pristine Japanese text for this line");

            // status info
            std::string info;
            if (ch.edited)
            {
                std::vector<uint8_t> enc;
                size_t bad;
                uint32_t badcp;
                if (!sjis::encode(ch.edited_text, enc, &bad, &badcp))
                {
                    char buf2[128];
                    snprintf(buf2, sizeof(buf2), "Bad character: U+%04X at position %zu — "
                             "the game charset (cp932) can't display it.",
                             (unsigned)badcp, bad);
                    info = buf2;
                }
            }
            if (ch.edited) ImGui::TextWrapped("%s", info.c_str());
            char blen[128];
            snprintf(blen, sizeof(blen), "SJIS bytes: %zu",
                     (ch.edited ? sjis::sjis_encoded_len(ch.edited_text)
                                : ch.raw.size()));
            ImGui::TextDisabled("%s", blen);
        }
        else
        {
            ImGui::TextWrapped("This event carries no editable text (structural opcode).");
        }
    }
    else if (!rows.empty())
    {
        ImGui::Separator();
        ImGui::TextDisabled("Select a row to edit its translation.");
    }
}
