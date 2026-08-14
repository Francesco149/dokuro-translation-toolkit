// ISO9660 in-place file replacement — port of tools/patch_iso.py.
//
// Replacing a file in a PS2 ISO by REBUILDING the image breaks the disc layout
// (LBAs shift; Dokuro-chan reads fixed LBAs and dies with a TLB-miss storm at
// IOP pc=0x4). Patching the file's bytes at its original extent keeps the
// layout identical. Replacement must be <= the original file size (padded with
// zeros; PS2 games rarely care about trailing padding in script files).
#pragma once
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace dokuro {

struct IsoEntry
{
    std::string name; // version suffix stripped
    uint8_t flags = 0;
    uint32_t extent_lba = 0;
    uint32_t size = 0;
};

// Locates a path like "/UNION/SCRIPT.UNI" inside the ISO. Returns false with
// err filled when missing.
bool iso_find(const std::string& isoPath, const std::string& pathInImage,
              IsoEntry* out, std::string* err);

// Streams src_iso to out_iso replacing pathInImage's bytes in place. Fails
// when the new data is larger than the original extent. progress_cb (optional)
// receives (bytesDone, bytesTotal) periodically.
bool iso_patch(const std::string& srcIso, const std::string& pathInImage,
               const std::vector<uint8_t>& newData, const std::string& outIso,
               std::string* err,
               void (*progress_cb)(uint64_t done, uint64_t total, void* ud) = nullptr,
               void* progress_ud = nullptr);

// Chunked full-file copy for the UI (pump per frame to keep the window alive).
class FileCopier
{
public:
    bool begin(const std::string& src, const std::string& dst, std::string* err);
    // Copies up to chunkBytes; returns true when done. false = error or aborted.
    bool pump(size_t chunkBytes, std::string* err);
    void abort() { aborted_ = true; }
    uint64_t done() const { return done_; }
    uint64_t total() const { return total_; }
    bool finished() const { return finished_; }

private:
    std::string src_, dst_;
    uint64_t total_ = 0, done_ = 0;
    bool aborted_ = false, finished_ = false;
    std::ifstream in_;
    std::ofstream out_;
};

// Full ISO9660 rebuild — for growing a file past its original extent, which
// the in-place patcher cannot hold. Reads every file from srcIso and writes a
// fresh image to outIso: PVD patched (volume size, path-table fields, root
// record), path tables at LBA 257/258 (the position SONY's cdvdman hardcodes
// for PS2 DVDs — plain rebuilds without it die with an IOP TLB-miss storm;
// see tools/iso_path_table_fix.py and
// docs/investigations/2026-08-14-chunk-budget-bypass/), all other files
// byte-identical (their LBAs change; the game resolves every file through the
// ISO9660 directory records at boot). pathInImage's data is replaced with
// newData (any size). Fails on multi-extent files (none on this disc).
bool iso_rebuild(const std::string& srcIso, const std::string& pathInImage,
                 const std::vector<uint8_t>& newData, const std::string& outIso,
                 std::string* err,
                 void (*progress_cb)(uint64_t done, uint64_t total, void* ud) = nullptr,
                 void* progress_ud = nullptr);

// Chunked full ISO rebuild for the UI — same semantics as iso_rebuild, but
// pumped per frame so a 4 GB ISO doesn't freeze the window.
struct RebuildPlan; // defined in iso.cpp
class IsoRebuilder
{
public:
    bool begin(const std::string& srcIso, const std::string& pathInImage,
               const std::vector<uint8_t>& newData, const std::string& outIso,
               std::string* err);
    bool pump(size_t chunkBytes, std::string* err); // true when done
    void abort() { aborted_ = true; }
    uint64_t done() const { return done_; }
    uint64_t total() const { return total_; }
    bool finished() const { return finished_; }

private:
    RebuildPlan* plan_ = nullptr;
    std::ifstream in_;
    std::ofstream out_;
    std::vector<uint8_t> new_data_owned_; // owns the replacement bytes
    uint64_t total_ = 0, done_ = 0;
    size_t file_idx_ = 0;
    bool aborted_ = false, finished_ = false;
};

// Chunked in-place ISO patch for the UI — same semantics as iso_patch, but
// pumped per frame so a 4 GB ISO doesn't freeze the window.
class IsoPatcher
{
public:
    bool begin(const std::string& srcIso, const std::string& pathInImage,
               const std::vector<uint8_t>& newData, const std::string& outIso,
               std::string* err);
    bool pump(size_t chunkBytes, std::string* err); // true when done
    void abort() { aborted_ = true; }
    uint64_t done() const { return done_; }
    uint64_t total() const { return total_; }
    bool finished() const { return finished_; }

private:
    enum class Phase { Prefix, Replace, Suffix, Done };
    std::ifstream in_;
    std::ofstream out_;
    std::vector<uint8_t> new_data_owned_; // owns the replacement bytes
    const std::vector<uint8_t>* new_data_ = nullptr;
    uint64_t total_ = 0, done_ = 0, start_ = 0, suffix_start_ = 0;
    size_t pad_left_ = 0;
    bool aborted_ = false, finished_ = false;
    Phase phase_ = Phase::Prefix;
};

} // namespace dokuro
