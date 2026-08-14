// Undo/redo history for the document model.
//
// Storage is OPERATION-based (never full-state snapshots): each committed step
// records only what changed, and any payload bytes (edited text, serialized
// actions, whole-file replacements from the asm editor) are stored in a
// content-addressed blob store so consecutive steps that share content (the
// common case) don't duplicate it. Steps are appended to a single log file in
// the project dir, so history persists across sessions; the in-RAM window is
// capped (settings) and older steps are loaded from disk on demand.
//
// Cross-session safety: every step records the CRC32 of the working container
// AFTER that step. On open, if the newest step's CRC doesn't match the current
// working file (external modification / another tool), history is discarded.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace dokuro {

enum class UndoOpKind { TextEdit, ActionInsert, ActionDelete, ActionMove, AsmReplace };

struct UndoOp
{
    UndoOpKind kind;
    int file = 0;
    // TextEdit — chunk (file, a, c): edited state + display text + content bytes
    uint32_t a = 0, c = 0;
    bool old_edited = false, new_edited = false;
    std::string old_text, old_raw, new_text, new_raw;
    // ActionInsert / ActionDelete — serialized action (standalone round-trip)
    uint32_t index = 0;
    uint32_t addr = 0;             // action's original file address (restored on undo)
    std::string action_bytes;
    bool custom = false;
    // ActionMove — from -> to (before whatever is at `to`)
    uint32_t from = 0, to = 0;
    // AsmReplace — whole-slot serialized bytes (old = undo target, new = redo)
    std::string old_bytes, new_bytes;
};

struct UndoStep
{
    std::string label;
    uint32_t crc_after = 0;
    bool has_ops = false;              // ops resident in RAM
    std::vector<UndoOp> ops;
    uint64_t disk_offset = 0, disk_size = 0;
    bool on_disk = false;
};

class UndoLog
{
public:
    UndoLog();
    ~UndoLog();

    // Opens/creates <dir>/undo.log. pristine_hash ties the log to one project.
    // working = current container bytes; mismatch discards history.
    // Returns false on I/O errors (err filled). Note: history validity state
    // can be queried via steps() == 0 after a valid open.
    bool open(const std::string& dir, uint64_t pristine_hash,
              const std::vector<uint8_t>& working, std::string* err);
    void close();

    void set_ram_cap(size_t n) { ram_cap_ = n; }
    size_t ram_cap() const { return ram_cap_; }

    size_t steps() const { return steps_.size(); }
    uint64_t disk_bytes() const { return disk_bytes_; }
    uint64_t pristine_hash() const { return pristine_hash_; }
    bool is_open() const { return open_; }
    const UndoStep& step(size_t i) const { return steps_[i]; }

    // Commits a step; caller has already applied ops to the model and passes
    // the resulting working container bytes. CRC is computed here.
    void commit(std::vector<UndoOp> ops, const std::vector<uint8_t>& working,
                const std::string& label);

    bool can_undo() const { return !steps_.empty(); }
    bool can_redo() const { return !redo_.empty(); }

    // Pops the newest undo step (loading from disk if evicted from RAM) and
    // returns its ops. Caller applies the INVERSE to the model.
    bool undo(std::vector<UndoOp>& out);
    // Caller applied the FORWARD ops; step is pushed back to the undo stack.
    bool redo(std::vector<UndoOp>& out);

    // Discards everything (RAM + disk) — "clear undo history".
    void clear_history();
    // Rewrites the log keeping only the newest `keep` steps (0 = keep all).
    void prune(size_t keep);

private:
    bool load_step_from_disk(size_t idx, UndoStep& out);
    void append_step_to_disk(UndoStep& step);
    void evict_ram_if_needed();
    void rebuild_blob_index();
    bool read_records_from_disk(std::string* err);

    std::string dir_;
    std::string path_;
    bool open_ = false;
    uint64_t pristine_hash_ = 0;
    size_t ram_cap_ = 200;
    std::vector<UndoStep> steps_; // newest last
    std::vector<UndoStep> redo_;  // in-RAM only (not persisted)
    uint64_t disk_bytes_ = 0;
    std::set<uint64_t> blobs_seen_on_disk_;
};

// ---------- model application (needs Project; defined in project.cpp) ----------

struct Project;

// Applies a step's ops to the project. forward=true applies redo semantics,
// false applies undo (inverse) semantics. Returns false if an op can't be
// applied (index/parse error) — model state is then undefined-but-safe.
bool apply_undo_step(Project& p, const std::vector<UndoOp>& ops, bool forward);

} // namespace dokuro
