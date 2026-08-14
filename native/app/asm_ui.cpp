// Direct script editor tab: full STCM2 slot as editable asm text. Changes are
// re-assembled on the fly (debounced in app_tick); on success the slot model
// is replaced (undoable, autosaved) and the friendly tab reflects it. On a
// syntax error the error is shown and the model keeps the last good state.
#include "app.h"
#include "core/asmfmt.h"
#include "win32_util.h"
#include <imgui.h>
#include <imgui_stdlib.h>
#include <cstdio>
#include <string>
#include <vector>

extern ImFont* app_font_mono(); // main.cpp

namespace {

// Regenerates the buffer from the current model (discarding unapplied edits).
void reload_asm(App& a)
{
    if (a.proj.files.empty()) return;
    if (a.asm_file >= (int)a.proj.files.size()) a.asm_file = 0;
    a.asm_text = dokuro::asm_disasm(a.proj.files[a.asm_file], a.asm_file);
    a.asm_dirty = false;
    a.asm_error = false;
    a.asm_model_version = a.model_version;
}

void apply_now(App& a)
{
    dokuro::Stcm2File assembled;
    dokuro::AsmError err;
    if (dokuro::asm_assemble(a.asm_text, a.asm_file, assembled, err))
    {
        if (app_apply_asm(a, a.asm_file, assembled))
        {
            a.asm_dirty = false;
            a.asm_error = false;
            a.asm_model_version = a.model_version;
            app_set_status(a, "Script slot " + std::to_string(a.asm_file) + " reassembled and applied", 5);
        }
    }
    else
    {
        a.asm_error = true;
        a.asm_err = err;
        app_set_status(a, "syntax error at line " + std::to_string(err.line), 8);
    }
}

} // namespace

void ui_asm_tab(App& a)
{
    if (!a.have_project || a.proj.files.empty()) return;

    // ---- file selector ----
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
        int want = a.asm_file;
        if (ImGui::Combo("##asmslot", &want, names.data(), (int)names.size()))
        {
            if (want != a.asm_file)
            {
                // applying a valid buffer first keeps the two tabs in sync
                if (a.asm_dirty)
                {
                    dokuro::Stcm2File assembled;
                    dokuro::AsmError err;
                    if (dokuro::asm_assemble(a.asm_text, a.asm_file, assembled, err))
                        app_apply_asm(a, a.asm_file, assembled);
                    else
                    {
                        // invalid: refuse to switch, surface the error
                        a.asm_error = true;
                        a.asm_err = err;
                        app_set_status(a, "Fix the syntax error before switching slots", 8);
                        want = a.asm_file;
                    }
                }
                if (want != a.asm_file)
                {
                    a.asm_file = want;
                    reload_asm(a);
                }
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload from model"))
        reload_asm(a);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Discard unapplied edits and regenerate the asm text from the current model");
    ImGui::SameLine();
    if (ImGui::SmallButton("Apply now"))
        apply_now(a);

    // ---- stale buffer warning ----
    // The dialogue editor may have committed edits since this buffer was
    // loaded; applying now would still be safe (the diff restore keeps edits),
    // but the user should know the buffer predates them.
    if (!a.asm_dirty && a.asm_model_version != a.model_version)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.25f, 1.0f));
        ImGui::TextWrapped("The script changed in the dialogue editor since this buffer was loaded — "
                           "Reload from model to see the latest edits.");
        ImGui::PopStyleColor();
    }

    // ---- status line ----
    if (a.asm_dirty && !a.asm_error)
    {
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f), "editing... applies automatically when valid");
    }
    else if (a.asm_error)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.3f, 1.0f));
        ImGui::TextWrapped("SYNTAX ERROR at line %d: %s", a.asm_err.line, a.asm_err.msg.c_str());
        ImGui::PopStyleColor();
        ImGui::TextDisabled("The friendly tab keeps the last valid state — nothing was changed.");
    }
    else
    {
        // count actions for feedback
        int n = (int)a.proj.files[a.asm_file].actions.size();
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1.0f),
                           "parsed OK — %d action(s); edits apply to the friendly tab automatically", n);
    }

    ImGui::Separator();

    // ---- editor ----
    ImGui::PushFont(app_font_mono());
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= 2;
    bool edited = ImGui::InputTextMultiline(
        "##asm", &a.asm_text, avail,
        ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_NoHorizontalScroll);
    ImGui::PopFont();
    if (edited)
    {
        a.asm_dirty = true;
        a.asm_last_edit = (double)app_now_ms();
        a.asm_error = false; // re-check after the debounce
    }
}
