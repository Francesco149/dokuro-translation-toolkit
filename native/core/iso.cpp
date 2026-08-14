#include "iso.h"
#include "uni2.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace dokuro {

namespace {

bool read_sector(std::ifstream& f, uint32_t lba, std::vector<uint8_t>& out)
{
    out.resize(2048);
    f.seekg((std::streamoff)lba * 2048);
    if (!f) return false;
    f.read((char*)out.data(), 2048);
    return (size_t)f.gcount() == 2048;
}

bool walk_dir(std::ifstream& f, uint32_t extent, uint32_t size,
              std::vector<IsoEntry>& entries, std::string* err)
{
    entries.clear();
    size_t pos = 0;
    while (pos < size)
    {
        f.seekg((std::streamoff)extent * 2048 + (pos / 2048) * 2048);
        std::vector<uint8_t> data(2048);
        f.read((char*)data.data(), 2048);
        if ((size_t)f.gcount() != 2048)
        {
            if (err) *err = "ISO: directory read past end";
            return false;
        }
        const uint8_t* rec = data.data() + (pos % 2048);
        size_t remaining = 2048 - (pos % 2048);
        uint8_t ln = rec[0];
        if (ln == 0)
        {
            pos = (pos / 2048 + 1) * 2048;
            continue;
        }
        if (ln > remaining)
        {
            if (err) *err = "ISO: directory record crosses sector (unsupported)";
            return false;
        }
        std::string name((const char*)rec + 33, rec[32]);
        IsoEntry e;
        e.name = name;
        e.flags = rec[25];
        e.extent_lba = (uint32_t)rec[2] | ((uint32_t)rec[3] << 8) |
                       ((uint32_t)rec[4] << 16) | ((uint32_t)rec[5] << 24);
        e.size = (uint32_t)rec[10] | ((uint32_t)rec[11] << 8) |
                 ((uint32_t)rec[12] << 16) | ((uint32_t)rec[13] << 24);
        // strip version ";1"
        size_t semi = e.name.find(';');
        if (semi != std::string::npos) e.name = e.name.substr(0, semi);
        entries.push_back(e);
        pos += ln;
    }
    return true;
}

std::string upper(const std::string& s)
{
    std::string o = s;
    for (auto& c : o)
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    return o;
}

} // namespace

bool iso_find(const std::string& isoPath, const std::string& pathInImage,
              IsoEntry* out, std::string* err)
{
    // split path
    std::vector<std::string> parts;
    std::string cur;
    for (char c : pathInImage)
    {
        if (c == '/')
        {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        }
        else cur += c;
    }
    if (!cur.empty()) parts.push_back(cur);

    std::ifstream f(isoPath, std::ios::binary);
    if (!f)
    {
        if (err) *err = "cannot open ISO: " + isoPath;
        return false;
    }
    std::vector<uint8_t> pvd;
    if (!read_sector(f, 16, pvd) || memcmp(pvd.data() + 1, "CD001", 5) != 0)
    {
        if (err) *err = "not an ISO9660 image";
        return false;
    }
    const uint8_t* root = pvd.data() + 156;
    uint32_t extent = (uint32_t)root[2] | ((uint32_t)root[3] << 8) |
                      ((uint32_t)root[4] << 16) | ((uint32_t)root[5] << 24);
    uint32_t size = (uint32_t)root[10] | ((uint32_t)root[11] << 8) |
                    ((uint32_t)root[12] << 16) | ((uint32_t)root[13] << 24);

    IsoEntry match;
    for (auto& part : parts)
    {
        std::vector<IsoEntry> entries;
        if (!walk_dir(f, extent, size, entries, err)) return false;
        bool found = false;
        for (auto& e : entries)
        {
            if (upper(e.name) == upper(part))
            {
                match = e;
                found = true;
                break;
            }
        }
        if (!found)
        {
            if (err) *err = "'" + part + "' not found in ISO";
            return false;
        }
        extent = match.extent_lba;
        size = match.size;
    }
    *out = match;
    return true;
}

bool iso_patch(const std::string& srcIso, const std::string& pathInImage,
               const std::vector<uint8_t>& newData, const std::string& outIso,
               std::string* err, void (*progress_cb)(uint64_t, uint64_t, void*),
               void* progress_ud)
{
    IsoEntry e;
    if (!iso_find(srcIso, pathInImage, &e, err)) return false;
    if (newData.size() > e.size)
    {
        if (err)
            *err = "replacement " + std::to_string(newData.size()) +
                   " bytes > original " + std::to_string(e.size) +
                   " bytes — cannot patch in place (the slot outgrew its sector padding; "
                   "an ISO rebuild is required for the bigger file)";
        return false;
    }
    std::ifstream fin(srcIso, std::ios::binary);
    std::ofstream fout(outIso, std::ios::binary | std::ios::trunc);
    if (!fin || !fout)
    {
        if (err) *err = "cannot open ISO for patching";
        return false;
    }
    uint64_t start = (uint64_t)e.extent_lba * 2048;
    fin.seekg(0, std::ios::end);
    uint64_t total = (uint64_t)fin.tellg();
    fin.seekg(0);

    std::vector<char> buf(1 << 20);
    uint64_t done = 0;
    auto pump = [&](uint64_t amount) -> bool {
        while (amount > 0)
        {
            size_t chunk = (size_t)std::min<uint64_t>(amount, buf.size());
            fin.read(buf.data(), chunk);
            std::streamsize got = fin.gcount();
            if (got <= 0) return false;
            fout.write(buf.data(), got);
            done += (uint64_t)got;
            amount -= (uint64_t)got;
            if (progress_cb) progress_cb(done, total, progress_ud);
        }
        return true;
    };

    if (!pump(start)) { if (err) *err = "ISO: read error before target"; return false; }
    fout.write((const char*)newData.data(), (std::streamsize)newData.size());
    done += newData.size();
    // pad to the original extent
    uint64_t pad = e.size - newData.size();
    static const char zeros[4096] = {};
    while (pad > 0)
    {
        size_t n = (size_t)std::min<uint64_t>(pad, sizeof(zeros));
        fout.write(zeros, n);
        pad -= n;
    }
    done += e.size - newData.size();
    fin.seekg((std::streamoff)(start + e.size));
    if (!pump(total - (start + e.size)))
    {
        if (err) *err = "ISO: read error after target";
        return false;
    }
    if (progress_cb) progress_cb(total, total, progress_ud);
    return true;
}

// ---------- chunked copy ----------

bool FileCopier::begin(const std::string& src, const std::string& dst, std::string* err)
{
    src_ = src;
    dst_ = dst;
    aborted_ = false;
    finished_ = false;
    done_ = 0;
    total_ = file_size(src);
    in_.open(src, std::ios::binary);
    if (!in_)
    {
        if (err) *err = "cannot open " + src;
        return false;
    }
    out_.open(dst, std::ios::binary | std::ios::trunc);
    if (!out_)
    {
        if (err) *err = "cannot create " + dst;
        in_.close();
        return false;
    }
    return true;
}

bool FileCopier::pump(size_t chunkBytes, std::string* err)
{
    if (finished_) return true;
    std::vector<char> buf(chunkBytes);
    while (!aborted_ && done_ < total_)
    {
        in_.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize got = in_.gcount();
        if (got < 0)
        {
            if (err) *err = "copy read error";
            return false;
        }
        if (got == 0) break;
        out_.write(buf.data(), got);
        if (!out_)
        {
            if (err) *err = "copy write error";
            return false;
        }
        done_ += (uint64_t)got;
    }
    if (aborted_)
    {
        if (err) *err = "copy aborted";
        return false;
    }
    if (done_ == total_)
    {
        finished_ = true;
        in_.close();
        out_.close();
        return true;
    }
    return false; // not done yet — call pump again
}

// ---------- chunked in-place patch ----------

bool IsoPatcher::begin(const std::string& srcIso, const std::string& pathInImage,
                       const std::vector<uint8_t>& newData, const std::string& outIso,
                       std::string* err)
{
    IsoEntry e;
    if (!iso_find(srcIso, pathInImage, &e, err)) return false;
    if (newData.size() > e.size)
    {
        if (err)
            *err = "replacement " + std::to_string(newData.size()) +
                   " bytes > original " + std::to_string(e.size) +
                   " bytes — cannot patch in place";
        return false;
    }
    // copy the payload: the pump runs over later frames and the caller's
    // buffer dies when begin() returns (it was a local in the UI layer)
    new_data_owned_ = newData;
    in_.open(srcIso, std::ios::binary);
    out_.open(outIso, std::ios::binary | std::ios::trunc);
    if (!in_ || !out_)
    {
        if (err) *err = "cannot open ISO for patching";
        return false;
    }
    in_.seekg(0, std::ios::end);
    total_ = (uint64_t)in_.tellg();
    in_.seekg(0);
    start_ = (uint64_t)e.extent_lba * 2048;
    suffix_start_ = start_ + e.size;
    pad_left_ = e.size - newData.size();
    new_data_ = &new_data_owned_;
    done_ = 0;
    aborted_ = false;
    finished_ = false;
    phase_ = Phase::Prefix;
    return true;
}

bool IsoPatcher::pump(size_t chunkBytes, std::string* err)
{
    if (finished_) return true;
    std::vector<char> buf(chunkBytes);
    while (!aborted_ && phase_ != Phase::Done)
    {
        if (phase_ == Phase::Prefix)
        {
            uint64_t left = start_ - done_;
            if (left == 0) { phase_ = Phase::Replace; continue; }
            size_t n = (size_t)std::min<uint64_t>(left, buf.size());
            in_.read(buf.data(), (std::streamsize)n);
            std::streamsize got = in_.gcount();
            if (got <= 0) { if (err) *err = "ISO: read error before target"; return false; }
            out_.write(buf.data(), got);
            done_ += (uint64_t)got;
        }
        else if (phase_ == Phase::Replace)
        {
            if (!new_data_->empty())
                out_.write((const char*)new_data_->data(), (std::streamsize)new_data_->size());
            done_ += new_data_->size();
            static const char zeros[4096] = {};
            size_t n = (size_t)std::min<uint64_t>(pad_left_, sizeof(zeros));
            if (n > 0)
            {
                out_.write(zeros, n);
                done_ += n;
                pad_left_ -= n;
            }
            if (pad_left_ == 0)
            {
                in_.seekg((std::streamoff)suffix_start_);
                phase_ = Phase::Suffix;
            }
        }
        else if (phase_ == Phase::Suffix)
        {
            uint64_t left = total_ - done_;
            if (left == 0) { phase_ = Phase::Done; break; }
            size_t n = (size_t)std::min<uint64_t>(left, buf.size());
            in_.read(buf.data(), (std::streamsize)n);
            std::streamsize got = in_.gcount();
            if (got <= 0) { if (err) *err = "ISO: read error after target"; return false; }
            out_.write(buf.data(), got);
            done_ += (uint64_t)got;
        }
    }
    if (aborted_)
    {
        if (err) *err = "ISO patch aborted";
        return false;
    }
    if (phase_ == Phase::Done)
    {
        finished_ = true;
        in_.close();
        out_.close();
        return true;
    }
    return false; // call pump again
}

// ---------- full ISO rebuild ----------

// One file on the disc. rec holds the ORIGINAL directory-record bytes (copied
// from the source ISO) so dates/flags/padding are preserved exactly; only the
// extent/size fields get patched for the new layout.
struct BuildFile
{
    std::string name;        // identifier, ";1" version suffix stripped
    uint32_t extent = 0;     // output LBA
    uint32_t size = 0;       // output size (== orig except for the replaced file)
    uint32_t orig_extent = 0;
    uint32_t orig_size = 0;
    bool replaced = false;   // carries the caller's new payload
    std::vector<uint8_t> rec;
};

struct ChildRef
{
    bool is_dir;
    int index;
};

struct BuildDir
{
    std::string name;        // "" for root
    int parent = -1;         // index into dirs_ (BFS order; -1 = root)
    uint32_t extent = 0;     // output LBA
    uint32_t size = 0;       // emitted record-set byte length
    std::vector<uint8_t> rec;    // this dir's record as listed in its parent
    std::vector<uint8_t> dot;    // "." record (raw, from source)
    std::vector<uint8_t> dotdot; // ".." record (raw, from source)
    std::vector<ChildRef> children; // original listing order
};

struct RebuildPlan
{
    std::vector<uint8_t> pvd;      // patched PVD sector
    std::vector<uint8_t> term;     // terminator sector
    std::vector<uint8_t> lba256;   // sector 256 (copied)
    std::vector<uint8_t> pathL, pathM;
    std::vector<BuildDir> dirs;    // index 0 = root (BFS order)
    std::vector<BuildFile> files;  // encounter order
    uint32_t total_sectors = 0;
    uint64_t header_bytes = 0;     // bytes before the file-data region
    uint64_t data_bytes = 0;       // file-data bytes to stream (incl. padding)
};

namespace {

constexpr uint32_t kPathTableLba = 257; // SONY's fixed DVD path-table position (0x101)
constexpr uint32_t kFirstDirLba = 259;

inline uint32_t rd32le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
inline void put32le(std::vector<uint8_t>& v, size_t off, uint32_t x)
{
    v[off] = (uint8_t)x; v[off + 1] = (uint8_t)(x >> 8);
    v[off + 2] = (uint8_t)(x >> 16); v[off + 3] = (uint8_t)(x >> 24);
}
inline void put32be(std::vector<uint8_t>& v, size_t off, uint32_t x)
{
    v[off] = (uint8_t)(x >> 24); v[off + 1] = (uint8_t)(x >> 16);
    v[off + 2] = (uint8_t)(x >> 8); v[off + 3] = (uint8_t)x;
}

// Reads a sector; short/absent reads yield zero padding (never fails).
bool read_sector_pad(std::ifstream& f, uint32_t lba, std::vector<uint8_t>& out)
{
    out.assign(2048, 0);
    f.clear();
    f.seekg((std::streamoff)lba * 2048);
    if (!f) return true;
    f.read((char*)out.data(), 2048);
    return true;
}

// Walks the directory tree of the source ISO, capturing every record's raw
// bytes. dirs_ is built in BFS order (root first) — the path table's parent
// indices depend on that order.
bool collect_tree(std::ifstream& f, const std::vector<uint8_t>& pvd,
                  const std::string& pathInImage, RebuildPlan& plan, std::string* err)
{
    const uint8_t* root = pvd.data() + 156;
    BuildDir rootDir;
    rootDir.extent = rd32le(root + 2);
    rootDir.size = rd32le(root + 10);
    plan.dirs.push_back(std::move(rootDir));

    for (size_t qi = 0; qi < plan.dirs.size(); qi++)
    {
        // Work on locals: pushing child dirs into plan.dirs may reallocate,
        // invalidating any reference into the vector.
        const uint32_t dirExtent = plan.dirs[qi].extent;
        const uint32_t dirSize = plan.dirs[qi].size;
        std::vector<uint8_t> dot, dotdot;
        std::vector<ChildRef> children;
        size_t pos = 0;
        while (pos < dirSize)
        {
            std::vector<uint8_t> data;
            if (!read_sector_pad(f, dirExtent + (uint32_t)(pos / 2048), data))
            {
                if (err) *err = "ISO: directory read past end";
                return false;
            }
            const uint8_t* rec = data.data() + (pos % 2048);
            size_t remaining = 2048 - (pos % 2048);
            uint8_t ln = rec[0];
            if (ln == 0)
            {
                pos = (pos / 2048 + 1) * 2048;
                continue;
            }
            if (ln > remaining)
            {
                if (err) *err = "ISO: directory record crosses sector (unsupported)";
                return false;
            }
            if (ln < 34)
            {
                if (err) *err = "ISO: malformed directory record (too short)";
                return false;
            }
            std::vector<uint8_t> raw(rec, rec + ln);
            uint8_t nlen = rec[32];
            std::string name((const char*)rec + 33, nlen);
            uint8_t flags = rec[25];
            uint32_t extent = rd32le(rec + 2);
            uint32_t size = rd32le(rec + 10);
            size_t semi = name.find(';');
            if (semi != std::string::npos) name = name.substr(0, semi);
            if (name == ".")
            {
                dot = std::move(raw);
            }
            else if (name == "..")
            {
                dotdot = std::move(raw);
            }
            else
            {
                if (flags & 0x80)
                {
                    if (err) *err = "ISO: multi-extent file not supported (" + name + ")";
                    return false;
                }
                if (flags & 2)
                {
                    int idx = (int)plan.dirs.size();
                    BuildDir cd;
                    cd.name = name;
                    cd.parent = (int)qi;
                    cd.extent = extent;
                    cd.size = size;
                    cd.rec = std::move(raw);
                    plan.dirs.push_back(std::move(cd));
                    children.push_back(ChildRef{ true, idx });
                }
                else
                {
                    int idx = (int)plan.files.size();
                    BuildFile cf;
                    cf.name = name;
                    cf.extent = extent;
                    cf.size = size;
                    cf.orig_extent = extent;
                    cf.orig_size = size;
                    cf.rec = std::move(raw);
                    plan.files.push_back(std::move(cf));
                    children.push_back(ChildRef{ false, idx });
                }
            }
            pos += ln;
        }
        BuildDir& d = plan.dirs[qi];
        d.dot = std::move(dot);
        d.dotdot = std::move(dotdot);
        d.children = std::move(children);
    }
    // locate the file to replace (pathInImage like "/UNION/SCRIPT.UNI")
    std::string want = upper(pathInImage);
    size_t slash = want.rfind('/');
    if (slash != std::string::npos) want = want.substr(slash + 1);
    bool found = false;
    for (auto& fl : plan.files)
    {
        if (upper(fl.name) == want) { fl.replaced = true; found = true; break; }
    }
    if (!found)
    {
        if (err) *err = "'" + pathInImage + "' not found in ISO";
        return false;
    }
    return true;
}

// Byte length of a dir's emitted record set (., .., children), inserting
// 2048-alignment padding wherever a record would cross a sector boundary.
uint32_t dir_emitted_size(const RebuildPlan& plan, const BuildDir& d)
{
    std::vector<const std::vector<uint8_t>*> recs;
    if (!d.dot.empty()) recs.push_back(&d.dot);
    if (!d.dotdot.empty()) recs.push_back(&d.dotdot);
    for (auto& ch : d.children)
    {
        if (ch.is_dir) recs.push_back(&plan.dirs[ch.index].rec);
        else recs.push_back(&plan.files[ch.index].rec);
    }
    uint32_t total = 0;
    for (auto* r : recs)
    {
        if ((total % 2048) + (uint32_t)r->size() > 2048)
            total = (total / 2048 + 1) * 2048; // pad sector, then the record fits
        total += (uint32_t)r->size();
    }
    return total;
}

void patch_rec(std::vector<uint8_t>& rec, uint32_t extent, uint32_t size)
{
    if (rec.size() < 34) return;
    put32le(rec, 2, extent);
    put32be(rec, 6, extent);
    put32le(rec, 10, size);
    put32be(rec, 14, size);
}

std::vector<uint8_t> build_path_table(const RebuildPlan& plan, bool msb)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; i < plan.dirs.size(); i++)
    {
        const BuildDir& d = plan.dirs[i];
        bool root = (i == 0);
        uint8_t idlen = root ? 1 : (uint8_t)d.name.size();
        out.push_back(idlen);
        out.push_back(0); // extended attribute record length
        uint32_t ext = d.extent;
        uint16_t parent = root ? 1 : (uint16_t)(d.parent + 1);
        size_t off = out.size();
        out.resize(off + 4); // extent bytes (both-endian variant below)
        if (msb)
            put32be(out, off, ext);
        else
            put32le(out, off, ext);
        out.push_back((uint8_t)parent);
        out.push_back((uint8_t)(parent >> 8));
        if (msb) // big-endian variant stores parent MSB-first
        {
            out[out.size() - 2] = (uint8_t)(parent >> 8);
            out[out.size() - 1] = (uint8_t)parent;
        }
        if (root)
            out.push_back(0);
        else
            out.insert(out.end(), d.name.begin(), d.name.end());
        if (out.size() % 2) out.push_back(0); // entries are even-length
    }
    return out;
}

void patch_pvd(std::vector<uint8_t>& pvd, const RebuildPlan& plan)
{
    put32le(pvd, 80, plan.total_sectors);
    put32be(pvd, 84, plan.total_sectors);
    put32le(pvd, 132, (uint32_t)plan.pathL.size());
    put32be(pvd, 136, (uint32_t)plan.pathL.size());
    put32le(pvd, 140, kPathTableLba);
    put32be(pvd, 144, kPathTableLba);
    put32le(pvd, 148, 0);
    put32be(pvd, 152, 0);
    put32le(pvd, 158, plan.dirs[0].extent);
    put32be(pvd, 162, plan.dirs[0].extent);
    put32le(pvd, 166, plan.dirs[0].size);
    put32be(pvd, 170, plan.dirs[0].size);
}

} // namespace

bool IsoRebuilder::begin(const std::string& srcIso, const std::string& pathInImage,
                         const std::vector<uint8_t>& newData, const std::string& outIso,
                         std::string* err)
{
    if (srcIso == outIso)
    {
        if (err) *err = "ISO rebuild: input and output must differ";
        return false;
    }
    plan_ = new RebuildPlan;
    new_data_owned_ = newData;
    in_.open(srcIso, std::ios::binary);
    out_.open(outIso, std::ios::binary | std::ios::trunc);
    if (!in_ || !out_)
    {
        if (err) *err = "cannot open ISO for rebuild";
        delete plan_;
        plan_ = nullptr;
        return false;
    }
    std::vector<uint8_t> pvd;
    if (!read_sector_pad(in_, 16, pvd) || memcmp(pvd.data() + 1, "CD001", 5) != 0)
    {
        if (err) *err = "not an ISO9660 image";
        delete plan_;
        plan_ = nullptr;
        return false;
    }
    plan_->pvd = pvd;
    if (!collect_tree(in_, pvd, pathInImage, *plan_, err))
    {
        delete plan_;
        plan_ = nullptr;
        return false;
    }
    // mark the replaced file's new size
    for (auto& fl : plan_->files)
        if (fl.replaced) fl.size = (uint32_t)newData.size();

    // assign output LBAs: dirs first (path-table order), then files
    uint32_t cursor = kFirstDirLba;
    for (auto& d : plan_->dirs)
    {
        d.extent = cursor;
        d.size = dir_emitted_size(*plan_, d);
        cursor += (d.size + 2047) / 2048;
    }
    for (auto& fl : plan_->files)
    {
        fl.extent = cursor;
        cursor += (fl.size + 2047) / 2048;
    }
    plan_->total_sectors = (cursor + 15) & ~15u; // SONY pads to 16-sector units
    plan_->pathL = build_path_table(*plan_, false);
    plan_->pathM = build_path_table(*plan_, true);
    patch_pvd(plan_->pvd, *plan_); // uses pathL.size() (pre-pad)
    plan_->pathL.resize(2048, 0);  // the sector writes below need full buffers
    plan_->pathM.resize(2048, 0);

    // ---- write the header region (sectors 0..first file) ----
    std::vector<uint8_t> buf;
    for (uint32_t i = 0; i < 16; i++) // system area
    {
        read_sector_pad(in_, i, buf);
        out_.write((const char*)buf.data(), 2048);
    }
    out_.write((const char*)plan_->pvd.data(), 2048); // 16: PVD
    read_sector_pad(in_, 17, plan_->term);
    out_.write((const char*)plan_->term.data(), 2048); // 17: terminator
    std::vector<uint8_t> zeros(2048, 0);
    for (uint32_t i = 18; i < 256; i++) out_.write((const char*)zeros.data(), 2048);
    read_sector_pad(in_, 256, plan_->lba256);
    out_.write((const char*)plan_->lba256.data(), 2048); // 256: SONY descriptor (faithful)
    out_.write((const char*)plan_->pathL.data(), 2048);  // 257: L path table
    out_.write((const char*)plan_->pathM.data(), 2048);  // 258: M path table
    for (auto& d : plan_->dirs) // dir records at their assigned extents
    {
        out_.seekp((std::streamoff)d.extent * 2048);
        std::vector<const std::vector<uint8_t>*> recs;
        if (!d.dot.empty()) recs.push_back(&d.dot);
        if (!d.dotdot.empty()) recs.push_back(&d.dotdot);
        for (auto& ch : d.children)
        {
            if (ch.is_dir) recs.push_back(&plan_->dirs[ch.index].rec);
            else recs.push_back(&plan_->files[ch.index].rec);
        }
        size_t pos = 0;
        size_t ri = 0;
        for (auto* r : recs)
        {
            std::vector<uint8_t> rec = *r;
            if (ri == 0 && !d.dot.empty())
            {
                patch_rec(rec, d.extent, d.size);
            }
            else if (ri == (d.dot.empty() ? 0u : 1u) && !d.dotdot.empty())
            {
                int p = d.parent < 0 ? 0 : d.parent;
                patch_rec(rec, plan_->dirs[p].extent, plan_->dirs[p].size);
            }
            else
            {
                size_t ci = ri - (d.dot.empty() ? 0u : 1u) - (d.dotdot.empty() ? 0u : 1u);
                const ChildRef& ch = d.children[ci];
                if (ch.is_dir)
                    patch_rec(rec, plan_->dirs[ch.index].extent, plan_->dirs[ch.index].size);
                else
                    patch_rec(rec, plan_->files[ch.index].extent, plan_->files[ch.index].size);
            }
            if ((pos % 2048) + rec.size() > 2048)
            {
                std::vector<uint8_t> z(2048 - (pos % 2048), 0);
                out_.write((const char*)z.data(), (std::streamsize)z.size());
                pos += z.size();
            }
            out_.write((const char*)rec.data(), (std::streamsize)rec.size());
            pos += rec.size();
            ri++;
        }
    }
    uint64_t firstFileLba = 0;
    for (auto& fl : plan_->files) { firstFileLba = fl.extent; break; }
    plan_->header_bytes = (uint64_t)firstFileLba * 2048;
    plan_->data_bytes = 0;
    for (auto& fl : plan_->files)
        plan_->data_bytes += (uint64_t)((fl.size + 2047) / 2048) * 2048;
    total_ = plan_->header_bytes + plan_->data_bytes;
    done_ = plan_->header_bytes;
    file_idx_ = 0;
    aborted_ = false;
    finished_ = false;
    return true;
}

bool IsoRebuilder::pump(size_t chunkBytes, std::string* err)
{
    if (finished_) return true;
    std::vector<char> buf(chunkBytes ? chunkBytes : 1);
    while (!aborted_ && file_idx_ < plan_->files.size())
    {
        BuildFile& fl = plan_->files[file_idx_];
        out_.seekp((std::streamoff)fl.extent * 2048);
        uint64_t left = (uint64_t)((fl.size + 2047) / 2048) * 2048;
        if (fl.replaced)
        {
            const uint8_t* p = new_data_owned_.data();
            size_t n = new_data_owned_.size();
            while (left > 0 && !aborted_)
            {
                size_t take = (size_t)std::min<uint64_t>(left, (uint64_t)buf.size());
                if (n > 0)
                {
                    out_.write((const char*)p, (std::streamsize)std::min<size_t>(take, n));
                    size_t wrote = (size_t)std::min<size_t>(take, n);
                    p += wrote; n -= wrote;
                    done_ += wrote; left -= wrote;
                }
                else
                {
                    memset(buf.data(), 0, take);
                    out_.write(buf.data(), (std::streamsize)take);
                    done_ += take; left -= take;
                }
                if (out_.fail()) { if (err) *err = "ISO rebuild: write error"; return false; }
            }
        }
        else
        {
            in_.clear(); // begin() may have read past EOF on a small image
            in_.seekg((std::streamoff)fl.orig_extent * 2048);
            uint64_t dataLeft = fl.orig_size;
            while (left > 0 && !aborted_)
            {
                size_t take = (size_t)std::min<uint64_t>(left, (uint64_t)buf.size());
                if (dataLeft > 0)
                {
                    size_t want = (size_t)std::min<uint64_t>(take, dataLeft);
                    in_.read(buf.data(), (std::streamsize)want);
                    std::streamsize got = in_.gcount();
                    if (got <= 0) { if (err) *err = "ISO rebuild: read error"; return false; }
                    out_.write(buf.data(), got);
                    done_ += (uint64_t)got;
                    left -= (uint64_t)got;
                    dataLeft -= (uint64_t)got;
                }
                else
                {
                    memset(buf.data(), 0, take);
                    out_.write(buf.data(), (std::streamsize)take);
                    done_ += take; left -= take;
                }
                if (out_.fail()) { if (err) *err = "ISO rebuild: write error"; return false; }
            }
        }
        file_idx_++;
    }
    if (aborted_)
    {
        if (err) *err = "ISO rebuild aborted";
        return false;
    }
    if (file_idx_ >= plan_->files.size())
    {
        // pad the image to the declared volume size (SONY rounds to 16 sectors)
        std::vector<uint8_t> z(2048, 0);
        while ((uint64_t)out_.tellp() < (uint64_t)plan_->total_sectors * 2048)
            out_.write((const char*)z.data(), 2048);
        finished_ = true;
        in_.close();
        out_.close();
        delete plan_;
        plan_ = nullptr;
        return true;
    }
    return false; // call pump again
}

bool iso_rebuild(const std::string& srcIso, const std::string& pathInImage,
                 const std::vector<uint8_t>& newData, const std::string& outIso,
                 std::string* err, void (*progress_cb)(uint64_t, uint64_t, void*),
                 void* progress_ud)
{
    IsoRebuilder r;
    if (!r.begin(srcIso, pathInImage, newData, outIso, err)) return false;
    std::string dummy;
    std::string* pe = err ? err : &dummy;
    while (!r.finished())
    {
        if (!r.pump(4 << 20, pe)) return false;
        if (progress_cb) progress_cb(r.done(), r.total(), progress_ud);
    }
    return true;
}

} // namespace dokuro
