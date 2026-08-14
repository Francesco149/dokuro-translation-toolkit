// Whole-script project model — port of Stcm2Project.cs plus the new features
// of the native editor: per-file sector-padding budget (growing a slot past
// its 0x800 padding grows SCRIPT.UNI, which the in-place ISO patcher cannot
// hold — the embedded TOC rebuild in uni2_join makes the game itself read
// grown slots fine), custom-action tracking, and the hardened text-dump
// interchange (TextDump.cs + header reference).
#pragma once
#include "stcm2.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dokuro {

// The two opcodes that carry player-facing text (per in-game testing):
// 0x118 = one line of dialogue inside the current text box
// 0x119 = box-advance boundary; 0x11A = speaker nameplate
enum class EntryKind { Dialogue, Nameplate };

struct EntryRef
{
    int file = 0;
    int action = 0;
    int chunk = 0;
    EntryKind kind = EntryKind::Dialogue;
};

struct EncodingIssue
{
    int file = 0;
    int action = 0;
    int chunk = 0;
    EntryKind kind = EntryKind::Dialogue;
    std::string text;
    int bad_index = 0; // codepoint index into text
    uint32_t bad_cp = 0;
    std::string describe() const;
};

struct Project
{
    std::vector<uint8_t> container_header;
    std::vector<Stcm2File> files;          // working model
    std::vector<Stcm2File> originals;      // pristine parse (revert + budget source)
    std::vector<uint32_t> orig_slot_sizes; // pristine slot sizes (budget base)
    std::string source_path;               // pristine SCRIPT.UNI path

    static Project load(const std::string& uniPath); // throws Stcm2Error

    // Loads pristine state from pristinePath (originals + budgets) and the
    // working model from workingPath. Both must be valid UNI2 containers with
    // the same slot count. Throws Stcm2Error.
    // `customs` = tool-added actions persisted in the state file, keyed by
    // (file, address at save time). They are excluded from edited-flag
    // matching and marked custom; pass nullptr when unknown.
    static Project load_pair(const std::string& pristinePath,
                             const std::string& workingPath,
                             const std::vector<std::pair<int, uint32_t>>* customs = nullptr);

    // Serializes all working files; fills action.new_addr for every action.
    // Throws Stcm2Error on encode/ref problems.
    std::vector<uint8_t> build_container() const;

    // Per-file sector-padding budget: pristine slot size rounded up to 0x800
    // (the container's packing granularity). Growing a slot past this grows
    // SCRIPT.UNI itself — in-game it plays fine (the embedded TOC at 0x800 is
    // rebuilt on save; see FONT_AND_CRASH_INVESTIGATION.md Thread B), but the
    // in-place ISO patcher cannot hold a bigger file.
    uint32_t file_budget(size_t fi) const;
    size_t file_serialized_len(size_t fi) const; // serialize().size()
    bool file_over_budget(size_t fi) const;

    // Enumerates translatable entries (text chunks in 0x118/0x11A actions).
    std::vector<EntryRef> entries() const;

    // Sets/clears a chunk's translation; sanitizes like the C# tool.
    // text == null reverts to original. Throws Stcm2Error if not a text chunk.
    void set_translation(const EntryRef& e, const std::string* text);
    void set_translation_raw(const EntryRef& e, bool edited, const std::string& edited_text);

    // Reverts a chunk to the pristine original content. Returns false when the
    // pristine counterpart can't be found (e.g. action was added by the tool).
    bool revert_chunk(const EntryRef& e);

    // Validates every translated chunk for cp932-encodability (full report).
    std::vector<EncodingIssue> validate() const;

    // Re-derives per-chunk edited flags for one slot from the diff against
    // its pristine counterpart (used after a direct-script apply, which
    // produces a fresh parse without flags). Custom actions must be marked.
    void restore_edited_flags(int fi);

    // Custom-action tracking (actions added by the tool rather than present in
    // the pristine script). Persisted by the caller in project state, keyed by
    // (file, new_addr) at save time.
    bool action_custom(int fi, size_t ai) const { return files[fi].actions[ai].custom; }
    void set_action_custom(int fi, size_t ai, bool v) { files[fi].actions[ai].custom = v; }

    // True when some other action/export references this one (refuses delete).
    bool action_is_referenced(int fi, size_t ai) const;
};

// ---------- text dump interchange (TextDump.cs port) ----------

// Exports the whole project as plain text, one block per translatable string.
// scriptPath is baked into the header comment so the file is self-describing
// and tied to exactly one SCRIPT.UNI.
std::string dump_export(const Project& p, const std::string& scriptPath);

// Imports RU: lines back. Robust by design: unknown/malformed blocks are
// ignored, block order doesn't matter, extra lines are fine, blank RU reverts
// to the original. Never throws on malformed input.
void dump_import(Project& p, const std::string& text);

} // namespace dokuro
