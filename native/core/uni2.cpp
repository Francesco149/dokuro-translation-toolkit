#include "uni2.h"
#include "stcm2.h"
#include <cstring>
#include <fstream>
#include <sstream>

namespace dokuro {

namespace {
constexpr uint32_t kSector = 0x800;

bool starts_with(const uint8_t* b, size_t len, size_t off, const char* pat)
{
    size_t n = strlen(pat);
    if (off + n > len) return false;
    return memcmp(b + off, pat, n) == 0;
}

uint32_t le32(const std::vector<uint8_t>& v, size_t off)
{
    return (uint32_t)v[off] | ((uint32_t)v[off + 1] << 8) |
           ((uint32_t)v[off + 2] << 16) | ((uint32_t)v[off + 3] << 24);
}

void put_le32(std::vector<uint8_t>& v, size_t off, uint32_t x)
{
    v[off] = (uint8_t)(x & 0xFF);
    v[off + 1] = (uint8_t)((x >> 8) & 0xFF);
    v[off + 2] = (uint8_t)((x >> 16) & 0xFF);
    v[off + 3] = (uint8_t)((x >> 24) & 0xFF);
}
} // namespace

void uni2_split(const uint8_t* data, size_t len, std::vector<uint8_t>& header,
                std::vector<std::vector<uint8_t>>& slots)
{
    static const char magic[] = "STCM2";
    std::vector<size_t> offsets;
    for (size_t i = 0; i + sizeof(magic) - 1 < len; i++)
    {
        if (memcmp(data + i, magic, sizeof(magic) - 1) == 0)
        {
            offsets.push_back(i);
            i += 1; // continue scanning within the blob (mirrors C# idx += 1)
        }
    }
    if (offsets.empty()) throw Stcm2Error("no STCM2 entries found in container");

    header.assign(data, data + offsets[0]);
    slots.clear();
    for (size_t i = 0; i < offsets.size(); i++)
    {
        size_t start = offsets[i];
        size_t end = (i + 1 < offsets.size()) ? offsets[i + 1] : len;
        slots.emplace_back(data + start, data + end);
    }
}

// SCRIPT.UNI's "hidden" per-chunk TOC: 35 x 16-byte entries
// {id, off_sectors, sector_len, size_round16} at file offset 0x800, inside
// what FORMAT.md §1a wrongly called "zero padding". The game copies this
// table verbatim into its runtime container registry at boot and uses the
// size field as the disc-read length (Thread B); a grown chunk whose TOC
// entry is not rebuilt gets read truncated and crashes. Preserve the ids,
// recompute off (sector offset from data start), sector_len (padded size in
// sectors — grows when a chunk outgrows its padding) and size (round16 of
// the real length) from the actual slot layout. No-op when the header has
// no populated TOC (i.e. not a SCRIPT.UNI-style container).
static void rebuild_embedded_toc(std::vector<uint8_t>& out, size_t header_len,
                                 const std::vector<std::vector<uint8_t>>& slots)
{
    constexpr size_t kToc = 0x800;
    constexpr size_t kStride = 16;
    if (header_len < 12 || memcmp(out.data(), "UNI2", 4) != 0) return;
    uint32_t count = le32(out, 8);
    if (count == 0 || count > 128) return;
    if (header_len < kToc + count * kStride) return;
    bool populated = false;
    for (size_t i = 0; i < count && !populated; i++)
        for (size_t f = 0; f < 4; f++)
            if (le32(out, kToc + i * kStride + f * 4) != 0) { populated = true; break; }
    if (!populated) return;
    size_t pos = header_len;
    for (size_t i = 0; i < count && i < slots.size(); i++)
    {
        size_t len = slots[i].size();
        size_t padded = ((len + kSector - 1) / kSector) * kSector;
        uint32_t id = le32(out, kToc + i * kStride);
        put_le32(out, kToc + i * kStride + 4, (uint32_t)((pos - header_len) / kSector));
        put_le32(out, kToc + i * kStride + 8, (uint32_t)(padded / kSector));
        put_le32(out, kToc + i * kStride + 12, (uint32_t)(((len + 15) / 16) * 16));
        pos += padded;
    }
}

std::vector<uint8_t> uni2_join(const std::vector<uint8_t>& header,
                               const std::vector<std::vector<uint8_t>>& slots)
{
    std::vector<uint8_t> out = header;
    for (const auto& e : slots)
    {
        out.insert(out.end(), e.begin(), e.end());
        size_t pad = (kSector - (e.size() % kSector)) % kSector;
        out.insert(out.end(), pad, 0);
    }
    rebuild_embedded_toc(out, header.size(), slots);
    return out;
}

std::vector<uint8_t> read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw Stcm2Error("cannot open " + path);
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz < 0) throw Stcm2Error("cannot size " + path);
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data((size_t)sz);
    if (sz > 0 && !f.read((char*)data.data(), sz))
        throw Stcm2Error("cannot read " + path);
    return data;
}

void write_file(const std::string& path, const uint8_t* data, size_t len)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw Stcm2Error("cannot open for write: " + path);
    if (len > 0 && !f.write((const char*)data, (std::streamsize)len))
        throw Stcm2Error("cannot write " + path);
}

void write_file(const std::string& path, const std::vector<uint8_t>& data)
{
    write_file(path, data.data(), data.size());
}

bool file_exists(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

uint64_t file_size(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    f.seekg(0, std::ios::end);
    return (uint64_t)f.tellg();
}

} // namespace dokuro
