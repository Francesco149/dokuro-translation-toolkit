#include "undo.h"
#include "cp932.h"
#include "project.h"
#include "uni2.h"
#include "vendor/miniz.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace dokuro {

// ---------- small primitives ----------

namespace {

uint64_t fnv1a64(const uint8_t* b, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++)
    {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

uint64_t fnv1a64(const std::string& s) { return fnv1a64((const uint8_t*)s.data(), s.size()); }

uint32_t crc32_of(const std::vector<uint8_t>& data)
{
    return (uint32_t)mz_crc32(MZ_CRC32_INIT, data.data(), data.size());
}

void put_u8(std::string& out, uint8_t v) { out += (char)v; }
void put_u16(std::string& out, uint16_t v)
{
    out += (char)v; out += (char)(v >> 8);
}
void put_u32(std::string& out, uint32_t v)
{
    out += (char)v; out += (char)(v >> 8); out += (char)(v >> 16); out += (char)(v >> 24);
}
void put_u64(std::string& out, uint64_t v)
{
    for (int i = 0; i < 8; i++) out += (char)(v >> (8 * i));
}

template <typename T>
bool get_u8(const T& b, size_t* p, uint8_t* v)
{
    if (*p + 1 > b.size()) return false;
    *v = (uint8_t)b[*p];
    *p += 1;
    return true;
}
template <typename T>
bool get_u32(const T& b, size_t* p, uint32_t* v)
{
    if (*p + 4 > b.size()) return false;
    *v = (uint32_t)(uint8_t)b[*p] | ((uint32_t)(uint8_t)b[*p + 1] << 8) |
         ((uint32_t)(uint8_t)b[*p + 2] << 16) | ((uint32_t)(uint8_t)b[*p + 3] << 24);
    *p += 4;
    return true;
}
template <typename T>
bool get_u64(const T& b, size_t* p, uint64_t* v)
{
    if (*p + 8 > b.size()) return false;
    *v = 0;
    for (int i = 0; i < 8; i++) *v |= (uint64_t)(uint8_t)b[*p + i] << (8 * i);
    *p += 8;
    return true;
}

struct BlobStore
{
    // hash -> (raw data, seen in current log)
    std::map<uint64_t, std::string> data;

    // resolves a hash to its bytes; empty string when hash==0 or unknown
    const std::string& get(uint64_t h) const
    {
        static const std::string empty;
        if (h == 0) return empty;
        auto it = data.find(h);
        return it == data.end() ? empty : it->second;
    }

    // Returns the hash for bytes, storing them if new.
    uint64_t put(const std::string& bytes)
    {
        if (bytes.empty()) return 0;
        uint64_t h = fnv1a64(bytes);
        if (data.find(h) == data.end()) data[h] = bytes;
        return h;
    }
};

// ---------- op serialization ----------

void encode_op(std::string& out, const UndoOp& op, BlobStore& blobs)
{
    put_u8(out, (uint8_t)op.kind);
    put_u8(out, (uint8_t)op.file);
    switch (op.kind)
    {
    case UndoOpKind::TextEdit:
        put_u32(out, op.a);
        put_u32(out, op.c);
        put_u8(out, op.old_edited ? 1 : 0);
        put_u8(out, op.new_edited ? 1 : 0);
        put_u64(out, blobs.put(op.old_text));
        put_u64(out, blobs.put(op.old_raw));
        put_u64(out, blobs.put(op.new_text));
        put_u64(out, blobs.put(op.new_raw));
        break;
    case UndoOpKind::ActionInsert:
    case UndoOpKind::ActionDelete:
        put_u32(out, op.index);
        put_u32(out, op.addr);
        put_u8(out, op.custom ? 1 : 0);
        put_u64(out, blobs.put(op.action_bytes));
        break;
    case UndoOpKind::ActionMove:
        put_u32(out, op.from);
        put_u32(out, op.to);
        break;
    case UndoOpKind::AsmReplace:
        put_u64(out, blobs.put(op.old_bytes));
        put_u64(out, blobs.put(op.new_bytes));
        break;
    }
}

template <typename T>
bool decode_op(const T& b, size_t* p, UndoOp& op, const BlobStore& blobs)
{
    uint8_t k, file;
    if (!get_u8(b, p, &k) || !get_u8(b, p, &file)) return false;
    op.kind = (UndoOpKind)k;
    op.file = file;
    switch (op.kind)
    {
    case UndoOpKind::TextEdit:
    {
        uint32_t a, c;
        uint8_t oe, ne;
        uint64_t h1, h2, h3, h4;
        if (!get_u32(b, p, &a) || !get_u32(b, p, &c) || !get_u8(b, p, &oe) ||
            !get_u8(b, p, &ne) || !get_u64(b, p, &h1) || !get_u64(b, p, &h2) ||
            !get_u64(b, p, &h3) || !get_u64(b, p, &h4))
            return false;
        op.a = a; op.c = c;
        op.old_edited = oe != 0; op.new_edited = ne != 0;
        op.old_text = blobs.get(h1); op.old_raw = blobs.get(h2);
        op.new_text = blobs.get(h3); op.new_raw = blobs.get(h4);
        return true;
    }
    case UndoOpKind::ActionInsert:
    case UndoOpKind::ActionDelete:
    {
        uint32_t index, addr;
        uint8_t custom;
        uint64_t h;
        if (!get_u32(b, p, &index) || !get_u32(b, p, &addr) || !get_u8(b, p, &custom) ||
            !get_u64(b, p, &h))
            return false;
        op.index = index;
        op.addr = addr;
        op.custom = custom != 0;
        op.action_bytes = blobs.get(h);
        return true;
    }
    case UndoOpKind::ActionMove:
    {
        uint32_t from, to;
        if (!get_u32(b, p, &from) || !get_u32(b, p, &to)) return false;
        op.from = from; op.to = to;
        return true;
    }
    case UndoOpKind::AsmReplace:
    {
        uint64_t h1, h2;
        if (!get_u64(b, p, &h1) || !get_u64(b, p, &h2)) return false;
        op.old_bytes = blobs.get(h1);
        op.new_bytes = blobs.get(h2);
        return true;
    }
    default:
        return false;
    }
}

} // namespace

// ---------- UndoLog ----------

UndoLog::UndoLog() = default;
UndoLog::~UndoLog() { close(); }

bool UndoLog::open(const std::string& dir, uint64_t pristine_hash,
                   const std::vector<uint8_t>& working, std::string* err)
{
    close();
    dir_ = dir;
    path_ = dir + "/undo.log";
    pristine_hash_ = pristine_hash;
    disk_bytes_ = 0;

    if (!file_exists(path_))
    {
        open_ = true;
        return true; // fresh log
    }

    if (!read_records_from_disk(err))
        return false;

    // validity: newest step's crc must match the current working container
    if (!steps_.empty() && crc32_of(working) != steps_.back().crc_after)
    {
        // history no longer matches the working file — discard it
        clear_history();
        if (err) *err = "undo history discarded (working file changed outside the editor)";
    }
    else
    {
        // keep only the last ram_cap_ steps in RAM
        if (steps_.size() > ram_cap_)
        {
            for (size_t i = 0; i < steps_.size() - ram_cap_; i++)
                steps_[i].has_ops = false; // evicted; loadable from disk
        }
    }
    open_ = true;
    return true;
}

void UndoLog::close()
{
    open_ = false;
    steps_.clear();
    redo_.clear();
    disk_bytes_ = 0;
}

bool UndoLog::read_records_from_disk(std::string* err)
{
    std::vector<uint8_t> raw = read_file(path_);
    if (raw.size() < 16 || memcmp(raw.data(), "DKUL", 4) != 0)
    {
        if (err) *err = "undo.log: bad magic";
        return false;
    }
    uint32_t ver = (uint32_t)raw[4] | ((uint32_t)raw[5] << 8) |
                   ((uint32_t)raw[6] << 16) | ((uint32_t)raw[7] << 24);
    if (ver != 1)
    {
        if (err) *err = "undo.log: unsupported version";
        return false;
    }
    uint64_t ph = 0;
    for (int i = 0; i < 8; i++) ph |= (uint64_t)raw[8 + i] << (8 * i);
    if (ph != pristine_hash_)
    {
        if (err) *err = "undo.log: belongs to a different project";
        return false;
    }

    BlobStore blobs;
    steps_.clear();
    size_t p = 16;
    while (p < raw.size())
    {
        uint8_t tag = raw[p++];
        if (tag == 'B')
        {
            // blob record: hash(8) rawlen(4) zlen(4) zdata
            if (p + 16 > raw.size()) { if (err) *err = "undo.log: truncated blob"; return false; }
            uint64_t h = 0;
            for (int i = 0; i < 8; i++) h |= (uint64_t)raw[p + i] << (8 * i);
            uint32_t rawlen = (uint32_t)raw[p + 8] | ((uint32_t)raw[p + 9] << 8) |
                              ((uint32_t)raw[p + 10] << 16) | ((uint32_t)raw[p + 11] << 24);
            uint32_t zlen = (uint32_t)raw[p + 12] | ((uint32_t)raw[p + 13] << 8) |
                            ((uint32_t)raw[p + 14] << 16) | ((uint32_t)raw[p + 15] << 24);
            p += 16;
            if (p + zlen > raw.size()) { if (err) *err = "undo.log: truncated blob data"; return false; }
            std::string out;
            out.resize(rawlen);
            mz_ulong outLen = rawlen;
            int st = mz_uncompress((unsigned char*)&out[0], &outLen,
                                   raw.data() + p, zlen);
            p += zlen;
            if (st != MZ_OK || outLen != rawlen)
            {
                if (err) *err = "undo.log: blob decompress failed";
                return false;
            }
            // trust the stored hash only if it matches (dedup safety)
            if (fnv1a64((const uint8_t*)out.data(), out.size()) == h || blobs.data.find(h) == blobs.data.end())
                blobs.data[h] = std::move(out);
        }
        else if (tag == 'S')
        {
            // step record: crc(4) label_len(2) label nops(4) ops...
            uint64_t recStart = p - 1; // offset of the 'S' tag
            uint32_t crc;
            uint16_t llen;
            uint32_t nops;
            if (!get_u32(raw, &p, &crc) || p + 2 > raw.size()) { if (err) *err = "undo.log: truncated step"; return false; }
            llen = (uint16_t)(uint8_t)raw[p] | ((uint16_t)(uint8_t)raw[p + 1] << 8);
            p += 2;
            if (p + llen > raw.size()) { if (err) *err = "undo.log: truncated label"; return false; }
            std::string label(raw.begin() + p, raw.begin() + p + llen);
            p += llen;
            if (!get_u32(raw, &p, &nops)) { if (err) *err = "undo.log: truncated step ops"; return false; }
            UndoStep st;
            st.label = label;
            st.crc_after = crc;
            st.disk_offset = recStart;
            st.on_disk = true;
            st.has_ops = false;
            steps_.push_back(std::move(st));
            // skip the ops (we decode lazily)
            for (uint32_t i = 0; i < nops; i++)
            {
                uint8_t k;
                uint8_t file;
                if (!get_u8(raw, &p, &k) || !get_u8(raw, &p, &file)) { if (err) *err = "undo.log: truncated op"; return false; }
                switch ((UndoOpKind)k)
                {
                case UndoOpKind::TextEdit: p += 4 + 4 + 1 + 1 + 32; break;
                case UndoOpKind::ActionInsert:
                case UndoOpKind::ActionDelete: p += 4 + 4 + 1 + 8; break;
                case UndoOpKind::ActionMove: p += 8; break;
                case UndoOpKind::AsmReplace: p += 16; break;
                default: if (err) *err = "undo.log: bad op kind"; return false;
                }
            }
        }
        else
        {
            if (err) *err = "undo.log: bad record tag";
            return false;
        }
    }
    disk_bytes_ = raw.size();
    return true;
}

bool UndoLog::load_step_from_disk(size_t idx, UndoStep& out)
{
    std::vector<uint8_t> raw = read_file(path_);
    // walk records again, decoding blobs lazily for this step
    BlobStore blobs;
    size_t p = 16;
    size_t stepNo = 0;
    while (p < raw.size())
    {
        uint8_t tag = raw[p++];
        if (tag == 'B')
        {
            uint64_t h = 0;
            for (int i = 0; i < 8; i++) h |= (uint64_t)raw[p + i] << (8 * i);
            uint32_t rawlen = (uint32_t)raw[p + 8] | ((uint32_t)raw[p + 9] << 8) |
                              ((uint32_t)raw[p + 10] << 16) | ((uint32_t)raw[p + 11] << 24);
            uint32_t zlen = (uint32_t)raw[p + 12] | ((uint32_t)raw[p + 13] << 8) |
                            ((uint32_t)raw[p + 14] << 16) | ((uint32_t)raw[p + 15] << 24);
            p += 16;
            std::string outb;
            outb.resize(rawlen);
            mz_ulong outLen = rawlen;
            if (mz_uncompress((unsigned char*)&outb[0], &outLen, raw.data() + p, zlen) != MZ_OK)
                return false;
            p += zlen;
            blobs.data[h] = std::move(outb);
        }
        else if (tag == 'S')
        {
            uint32_t crc;
            uint16_t llen;
            uint32_t nops;
            if (!get_u32(raw, &p, &crc) || p + 2 > raw.size()) return false;
            llen = (uint16_t)(uint8_t)raw[p] | ((uint16_t)(uint8_t)raw[p + 1] << 8);
            p += 2;
            std::string label(raw.begin() + p, raw.begin() + p + llen);
            p += llen;
            if (!get_u32(raw, &p, &nops)) return false;
            if (stepNo == idx)
            {
                out.label = label;
                out.crc_after = crc;
                for (uint32_t i = 0; i < nops; i++)
                {
                    UndoOp op;
                    if (!decode_op(raw, &p, op, blobs)) return false;
                    out.ops.push_back(std::move(op));
                }
                out.has_ops = true;
                return true;
            }
            for (uint32_t i = 0; i < nops; i++)
            {
                uint8_t k, file;
                if (!get_u8(raw, &p, &k) || !get_u8(raw, &p, &file)) return false;
                switch ((UndoOpKind)k)
                {
                case UndoOpKind::TextEdit: p += 4 + 4 + 1 + 1 + 32; break;
                case UndoOpKind::ActionInsert:
                case UndoOpKind::ActionDelete: p += 4 + 4 + 1 + 8; break;
                case UndoOpKind::ActionMove: p += 8; break;
                case UndoOpKind::AsmReplace: p += 16; break;
                default: return false;
                }
            }
            stepNo++;
        }
        else return false;
    }
    return false;
}

void UndoLog::append_step_to_disk(UndoStep& step)
{
    std::ofstream f(path_, std::ios::binary | std::ios::app);
    if (!f) return; // disk write failure: keep history in RAM only

    bool newFile = disk_bytes_ == 0;
    if (newFile)
    {
        std::string hdr;
        hdr += "DKUL";
        hdr += (char)1; hdr += (char)0; hdr += (char)0; hdr += (char)0;
        for (int i = 0; i < 8; i++) hdr += (char)(pristine_hash_ >> (8 * i));
        f.write(hdr.data(), hdr.size());
        disk_bytes_ = hdr.size();
    }

    BlobStore blobs;
    // register all payloads for hashing; the store dedups within this step
    std::string rec;
    rec += 'S';
    put_u32(rec, step.crc_after);
    put_u16(rec, (uint16_t)step.label.size());
    rec += step.label;
    put_u32(rec, (uint32_t)step.ops.size());
    // first pass: encode ops to learn blob hashes
    for (const auto& op : step.ops) encode_op(rec, op, blobs);

    // second pass: emit blob records for any hash not yet on disk
    std::set<uint64_t> already = blobs_seen_on_disk_;
    std::string blobRecs;
    for (const auto& kv : blobs.data)
    {
        if (already.count(kv.first)) continue;
        std::vector<uint8_t> comp(kv.second.size() + 256);
        mz_ulong compLen = (mz_ulong)comp.size();
        int st = mz_compress2(comp.data(), &compLen,
                              (const unsigned char*)kv.second.data(),
                              (mz_ulong)kv.second.size(), 6);
        if (st != MZ_OK) continue; // skip blob on failure (log stays valid, undo loses this payload)
        blobRecs += 'B';
        for (int i = 0; i < 8; i++) blobRecs += (char)(kv.first >> (8 * i));
        put_u32(blobRecs, (uint32_t)kv.second.size());
        put_u32(blobRecs, (uint32_t)compLen);
        blobRecs.append((const char*)comp.data(), compLen);
        blobs_seen_on_disk_.insert(kv.first);
    }

    f.write(blobRecs.data(), blobRecs.size());
    step.disk_offset = disk_bytes_ + blobRecs.size(); // offset of the 'S' record
    step.on_disk = true;
    f.write(rec.data(), rec.size());
    f.flush();
    disk_bytes_ += blobRecs.size() + rec.size();
}

// Rebuilds the on-disk blob hash set from the current log file contents.
// Needed after truncation (undo) so dedup never references a missing blob.
void UndoLog::rebuild_blob_index()
{
    blobs_seen_on_disk_.clear();
    if (!file_exists(path_)) return;
    std::vector<uint8_t> raw = read_file(path_);
    size_t p = 16;
    while (p + 1 <= raw.size())
    {
        uint8_t tag = raw[p++];
        if (tag == 'B')
        {
            if (p + 16 > raw.size()) break;
            uint64_t h = 0;
            for (int i = 0; i < 8; i++) h |= (uint64_t)raw[p + i] << (8 * i);
            uint32_t zlen = (uint32_t)raw[p + 12] | ((uint32_t)raw[p + 13] << 8) |
                            ((uint32_t)raw[p + 14] << 16) | ((uint32_t)raw[p + 15] << 24);
            p += 16 + zlen;
            blobs_seen_on_disk_.insert(h);
        }
        else if (tag == 'S')
        {
            if (p + 4 > raw.size()) break;
            p += 4; // crc
            if (p + 2 > raw.size()) break;
            uint16_t llen = (uint16_t)(uint8_t)raw[p] | ((uint16_t)(uint8_t)raw[p + 1] << 8);
            p += 2 + llen;
            if (p + 4 > raw.size()) break;
            uint32_t nops;
            memcpy(&nops, raw.data() + p, 4);
            p += 4;
            for (uint32_t i = 0; i < nops; i++)
            {
                if (p + 2 > raw.size()) return;
                uint8_t k = raw[p], file = raw[p + 1];
                p += 2;
                switch ((UndoOpKind)k)
                {
                case UndoOpKind::TextEdit: p += 4 + 4 + 1 + 1 + 32; break;
                case UndoOpKind::ActionInsert:
                case UndoOpKind::ActionDelete: p += 4 + 4 + 1 + 8; break;
                case UndoOpKind::ActionMove: p += 8; break;
                case UndoOpKind::AsmReplace: p += 16; break;
                default: return;
                }
                (void)file;
            }
        }
        else return;
    }
}

void UndoLog::evict_ram_if_needed()
{
    while (steps_.size() > ram_cap_)
    {
        // evict the OLDEST steps from RAM (they remain on disk)
        if (!steps_.front().on_disk)
        {
            // can't reload from disk — drop entirely
            steps_.erase(steps_.begin());
            continue;
        }
        steps_.front().has_ops = false;
        steps_.front().ops.clear();
        steps_.front().ops.shrink_to_fit();
        steps_.erase(steps_.begin());
    }
}

void UndoLog::commit(std::vector<UndoOp> ops, const std::vector<uint8_t>& working,
                     const std::string& label)
{
    if (!open_) return;
    if (ops.empty()) return;
    UndoStep st;
    st.label = label;
    st.crc_after = crc32_of(working);
    st.has_ops = true;
    st.ops = std::move(ops);
    append_step_to_disk(st);
    steps_.push_back(std::move(st));
    evict_ram_if_needed();
    redo_.clear(); // new commit invalidates redo
}

bool UndoLog::undo(std::vector<UndoOp>& out)
{
    while (true)
    {
        if (steps_.empty()) return false;
        UndoStep st = std::move(steps_.back());
        steps_.pop_back();
        if (!st.has_ops && st.on_disk)
        {
            UndoStep loaded;
            if (!load_step_from_disk(steps_.size(), loaded))
                continue; // unreadable — drop and try the next
            st = std::move(loaded);
        }
        // truncate the log so it mirrors the RAM stack exactly (cross-session
        // undo semantics: an undone step is gone even if the app crashes)
        if (st.on_disk && st.disk_offset > 0)
        {
#ifdef _WIN32
            HANDLE h = CreateFileA(path_.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                SetFilePointer(h, (LONG)st.disk_offset, nullptr, FILE_BEGIN);
                SetEndOfFile(h);
                CloseHandle(h);
                disk_bytes_ = st.disk_offset;
                rebuild_blob_index();
            }
#else
            if (truncate(path_.c_str(), (off_t)st.disk_offset) == 0)
            {
                disk_bytes_ = st.disk_offset;
                rebuild_blob_index();
            }
#endif
        }
        redo_.push_back(std::move(st));
        out = redo_.back().ops; // copy: the step stays whole in redo_
        return true;
    }
}
bool UndoLog::redo(std::vector<UndoOp>& out)
{
    if (redo_.empty()) return false;
    UndoStep st = std::move(redo_.back());
    redo_.pop_back();
    append_step_to_disk(st); // re-append the record (new offset)
    steps_.push_back(std::move(st));
    out = steps_.back().ops; // copy: the step stays whole in steps_
    return true;
}

void UndoLog::clear_history()
{
    steps_.clear();
    redo_.clear();
    blobs_seen_on_disk_.clear();
    disk_bytes_ = 0;
    if (!path_.empty())
    {
        std::remove(path_.c_str());
        // reset the file so the next commit starts fresh
        std::ofstream f(path_, std::ios::binary | std::ios::trunc);
        if (f)
        {
            std::string hdr;
            hdr += "DKUL";
            hdr += (char)1; hdr += (char)0; hdr += (char)0; hdr += (char)0;
            for (int i = 0; i < 8; i++) hdr += (char)(pristine_hash_ >> (8 * i));
            f.write(hdr.data(), hdr.size());
            disk_bytes_ = hdr.size();
        }
    }
}

void UndoLog::prune(size_t keep)
{
    if (keep != 0 && steps_.size() <= keep) return;
    // keep the newest `keep` steps; rewrite the log from scratch
    size_t start = keep == 0 ? 0 : (steps_.size() > keep ? steps_.size() - keep : 0);

    // ensure kept steps have ops resident (load from disk by OLD index)
    for (size_t i = start; i < steps_.size(); i++)
    {
        if (!steps_[i].has_ops && steps_[i].on_disk)
        {
            UndoStep loaded;
            if (load_step_from_disk(i, loaded))
            {
                steps_[i].ops = std::move(loaded.ops);
                steps_[i].has_ops = true;
            }
        }
    }

    std::vector<UndoStep> kept;
    for (size_t i = start; i < steps_.size(); i++)
        kept.push_back(std::move(steps_[i]));

    // rebuild file: header + blobs + steps
    std::string fileData;
    fileData += "DKUL";
    fileData += (char)1; fileData += (char)0; fileData += (char)0; fileData += (char)0;
    for (int i = 0; i < 8; i++) fileData += (char)(pristine_hash_ >> (8 * i));

    BlobStore blobs;
    std::string stepsRec;
    for (auto& st : kept)
    {
        std::string rec;
        rec += 'S';
        put_u32(rec, st.crc_after);
        put_u16(rec, (uint16_t)st.label.size());
        rec += st.label;
        put_u32(rec, (uint32_t)st.ops.size());
        for (const auto& op : st.ops) encode_op(rec, op, blobs);
        stepsRec += rec;
    }
    std::set<uint64_t> seen;
    for (const auto& kv : blobs.data)
    {
        if (seen.count(kv.first)) continue;
        seen.insert(kv.first);
        std::vector<uint8_t> comp(kv.second.size() + 256);
        mz_ulong compLen = (mz_ulong)comp.size();
        if (mz_compress2(comp.data(), &compLen, (const unsigned char*)kv.second.data(),
                         (mz_ulong)kv.second.size(), 6) != MZ_OK)
            continue;
        fileData += 'B';
        for (int i = 0; i < 8; i++) fileData += (char)(kv.first >> (8 * i));
        put_u32(fileData, (uint32_t)kv.second.size());
        put_u32(fileData, (uint32_t)compLen);
        fileData.append((const char*)comp.data(), compLen);
    }
    fileData += stepsRec;

    {
        std::ofstream f(path_, std::ios::binary | std::ios::trunc);
        if (f) f.write(fileData.data(), fileData.size());
    }
    steps_ = std::move(kept);
    redo_.clear();
    blobs_seen_on_disk_ = seen;
    disk_bytes_ = fileData.size();
    evict_ram_if_needed();
}

// ---------- model application ----------

bool apply_undo_step(Project& p, const std::vector<UndoOp>& ops, bool forward)
{
    // undo: apply ops in REVERSE with inverse semantics; redo: forward order.
    auto applyOne = [&](const UndoOp& op, bool fwd) -> bool {
        switch (op.kind)
        {
        case UndoOpKind::TextEdit:
        {
            if (op.file < 0 || op.file >= (int)p.files.size()) return false;
            auto& f = p.files[op.file];
            if (op.a >= f.actions.size() || op.c >= f.actions[op.a].chunks.size()) return false;
            auto& ch = f.actions[op.a].chunks[op.c];
            const bool edited = fwd ? op.new_edited : op.old_edited;
            const std::string& text = fwd ? op.new_text : op.old_text;
            const std::string& raw = fwd ? op.new_raw : op.old_raw;
            ch.edited = edited;
            ch.edited_text = text;
            if (!raw.empty())
            {
                ch.raw.assign(raw.begin(), raw.end());
                ch.text = sjis::decode_lenient((const uint8_t*)raw.data(), raw.size());
            }
            return true;
        }
        case UndoOpKind::ActionInsert:
        {
            // forward: insert; inverse: delete
            auto& f = p.files[op.file];
            if (fwd)
            {
                if (op.index > f.actions.size()) return false;
                try
                {
                    Action a = Stcm2File::parse_action(
                        (const uint8_t*)op.action_bytes.data(), op.action_bytes.size(),
                        0, (uint32_t)f.global_data.size());
                    a.original_addr = op.addr; // custom annotation address
                    a.custom = op.custom;
                    f.actions.insert(f.actions.begin() + op.index, std::move(a));
                }
                catch (const Stcm2Error&) { return false; }
            }
            else
            {
                if (op.index >= f.actions.size()) return false;
                f.actions.erase(f.actions.begin() + op.index);
            }
            return true;
        }
        case UndoOpKind::ActionDelete:
        {
            // forward: delete; inverse: insert back
            auto& f = p.files[op.file];
            if (!fwd)
            {
                if (op.index > f.actions.size()) return false;
                try
                {
                    Action a = Stcm2File::parse_action(
                        (const uint8_t*)op.action_bytes.data(), op.action_bytes.size(),
                        0, (uint32_t)f.global_data.size());
                    a.original_addr = op.addr;
                    a.custom = op.custom;
                    f.actions.insert(f.actions.begin() + op.index, std::move(a));
                }
                catch (const Stcm2Error&) { return false; }
            }
            else
            {
                if (op.index >= f.actions.size()) return false;
                f.actions.erase(f.actions.begin() + op.index);
            }
            return true;
        }
        case UndoOpKind::ActionMove:
        {
            auto& f = p.files[op.file];
            uint32_t from = fwd ? op.from : (op.to > op.from ? op.to - 1 : op.to);
            uint32_t to = fwd ? op.to : op.from;
            if (from >= f.actions.size()) return false;
            Action a = std::move(f.actions[from]);
            f.actions.erase(f.actions.begin() + from);
            if (to > f.actions.size()) return false;
            f.actions.insert(f.actions.begin() + to, std::move(a));
            return true;
        }
        case UndoOpKind::AsmReplace:
        {
            const std::string& bytes = fwd ? op.new_bytes : op.old_bytes;
            if (op.file < 0 || op.file >= (int)p.files.size()) return false;
            try
            {
                p.files[op.file] = Stcm2File::parse(
                    (const uint8_t*)bytes.data(), bytes.size());
            }
            catch (const Stcm2Error&) { return false; }
            return true;
        }
        }
        return false;
    };

    if (forward)
    {
        for (const auto& op : ops)
            if (!applyOne(op, true)) return false;
        return true;
    }
    for (auto it = ops.rbegin(); it != ops.rend(); ++it)
        if (!applyOne(*it, false)) return false;
    return true;
}

} // namespace dokuro
