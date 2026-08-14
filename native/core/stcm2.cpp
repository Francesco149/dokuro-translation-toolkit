#include "stcm2.h"
#include "cp932.h"
#include "utf8.h"
#include <cstring>
#include <functional>
#include <map>

namespace dokuro {

// ---------- low level ----------

uint32_t read_u32le(const uint8_t* b, size_t len, size_t off)
{
    if (off + 4 > len)
        throw Stcm2Error("read past end at " + std::to_string(off));
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

void write_u32le(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back((uint8_t)v);
    out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)(v >> 16));
    out.push_back((uint8_t)(v >> 24));
}

void write_bytes(std::vector<uint8_t>& out, const uint8_t* b, size_t n)
{
    out.insert(out.end(), b, b + n);
}

// ---------- parameters ----------

Param Param::parse(const uint32_t triple[3], uint32_t dataAddr,
                   uint32_t dataLen, uint32_t globalLen)
{
    const uint32_t GDO = Stcm2File::kGlobalDataOffset;
    uint32_t a = triple[0], b = triple[1], c = triple[2];
    bool bcTag = (b == 0x40000000 || b == 0xff000000) && (c == 0x40000000 || c == 0xff000000);

    if (a == 0xffffff41 && (c == 0x40000000 || c == 0xff000000))
        return Param{ ParamKind::ActionRef, b };

    if (bcTag && a >= dataAddr && a < dataAddr + dataLen)
        return Param{ ParamKind::DataPointer, a - dataAddr };

    if (bcTag && a >= GDO && a < GDO + globalLen)
        return Param{ ParamKind::GlobalDataPointer, a - GDO };

    if (bcTag)
        return Param{ ParamKind::Value, a };

    throw Stcm2Error("bad parameter: " + [&]() {
        char buf[48];
        snprintf(buf, sizeof(buf), "%08X %08X %08X", a, b, c);
        return std::string(buf);
    }());
}

void Param::encode(uint32_t dataAddr, uint32_t out[3],
                   const std::function<uint32_t(uint32_t)>& resolve) const
{
    const uint32_t GDO = Stcm2File::kGlobalDataOffset;
    switch (kind)
    {
    case ParamKind::ActionRef:
        out[0] = 0xffffff41;
        out[1] = resolve(value);
        out[2] = 0xff000000;
        break;
    case ParamKind::DataPointer:
        out[0] = dataAddr + value;
        out[1] = 0xff000000;
        out[2] = 0xff000000;
        break;
    case ParamKind::GlobalDataPointer:
        out[0] = GDO + value;
        out[1] = 0xff000000;
        out[2] = 0xff000000;
        break;
    case ParamKind::Value:
        out[0] = value;
        out[1] = 0xff000000;
        out[2] = 0xff000000;
        break;
    }
}

// ---------- data chunks ----------

int DataChunk::on_disk_length() const
{
    size_t payload;
    if (!is_text || !edited)
        payload = raw.size();
    else
    {
        std::vector<uint8_t> enc;
        size_t bad;
        uint32_t badcp;
        if (!sjis::encode(edited_text, enc, &bad, &badcp))
            throw Stcm2Error("cannot encode edited text (bad character)");
        int pad = 4 - (int)(enc.size() % 4);
        if (pad == 0) pad = 4; // always at least 1 null pad byte, matches original format
        payload = enc.size() + (size_t)pad;
    }
    return 16 + (int)payload;
}

void DataChunk::write(std::vector<uint8_t>& out) const
{
    if (!is_text || !edited)
    {
        write_u32le(out, type);
        write_u32le(out, (uint32_t)(raw.size() / 4));
        write_u32le(out, 1u); // magic
        write_u32le(out, (uint32_t)raw.size());
        write_bytes(out, raw.data(), raw.size());
        return;
    }

    std::vector<uint8_t> enc;
    size_t bad;
    uint32_t badcp;
    if (!sjis::encode(edited_text, enc, &bad, &badcp))
        throw Stcm2Error("cannot encode edited text (bad character)");
    int pad = 4 - (int)(enc.size() % 4);
    if (pad == 0) pad = 4;
    size_t len = enc.size() + (size_t)pad;

    write_u32le(out, 0u);              // type
    write_u32le(out, (uint32_t)(len / 4)); // qlen
    write_u32le(out, 1u);              // magic
    write_u32le(out, (uint32_t)len);   // len
    write_bytes(out, enc.data(), enc.size());
    for (int i = 0; i < pad; i++) out.push_back(0);
}

// ---------- actions ----------

std::vector<uint8_t> Action::build_data() const
{
    std::vector<uint8_t> out;
    if (unparsed)
    {
        write_bytes(out, unparsed_data.data(), unparsed_data.size());
        return out;
    }
    write_bytes(out, leading_junk.data(), leading_junk.size());
    for (const auto& c : chunks) c.write(out);
    return out;
}

// ---------- data chunk scanning (port of ScanDataChunks/TryDecodeChunk) ----------

static bool is_control_cp(uint32_t cp)
{
    return cp <= 0x1F || (cp >= 0x7F && cp <= 0x9F);
}

static bool try_decode_chunk(const uint8_t* data, size_t len, size_t addr, DataChunk* out)
{
    if (len - addr <= 16) return false;
    uint32_t type_, qlen, magic1, len_;
    try
    {
        type_ = read_u32le(data, len, addr);
        if (type_ != 0 && type_ != 1) return false;
        qlen = read_u32le(data, len, addr + 4);
        magic1 = read_u32le(data, len, addr + 8);
        if (magic1 != 1) return false;
        len_ = read_u32le(data, len, addr + 12);
        if (len_ != qlen * 4) return false;
        if (len - (addr + 16) < len_) return false;
    }
    catch (const Stcm2Error&) { return false; }

    const uint8_t* content = data + addr + 16;
    size_t ilen = len_;

    DataChunk c;
    c.offset = (int)addr;
    c.type = type_;
    c.raw.assign(content, content + ilen);

    if (type_ == 1)
    {
        if (ilen != 4) return false; // unsupported per original format
        c.is_text = false;
        c.numeric = read_u32le(content, ilen, 0);
        *out = c;
        return true;
    }

    // type_ == 0
    if (ilen == 4)
    {
        uint32_t n = read_u32le(content, ilen, 0);
        if (n > 0xFFFFFF)
        {
            c.is_text = false;
            c.numeric = n;
            *out = c;
            return true;
        }
        // four-byte heuristic: strip trailing zero bytes
        size_t nzero = 0;
        for (size_t i = ilen; i > 0 && content[i - 1] == 0; i--) nzero++;
        size_t tlen = ilen - nzero;
        const uint8_t* trimmed = content;

        // Any length is candidate text — including 1-2 byte content (single
        // kanji nameplates like 桜 = 2 SJIS bytes) and all-zero content (a
        // tool-added blank line serializes to 4 zero bytes; without this it
        // round-trips as numeric 0 and the line is invisible in the editor).
        // The 2-char "op" exception is kept (real numeric values in the
        // script), as is the strict decode + control-char gate.
        bool isText = false;
        if (!(tlen == 2 && trimmed[0] == 'o' && trimmed[1] == 'p'))
        {
            std::string s;
            if (sjis::decode_strict(trimmed, tlen, s))
            {
                bool hasControl = false;
                size_t k = 0;
                while (k < s.size())
                {
                    size_t adv;
                    uint32_t cp = utf8::decode(s.data() + k, s.size() - k, &adv);
                    k += adv > 0 ? adv : 1;
                    if (is_control_cp(cp)) { hasControl = true; break; }
                }
                if (!hasControl) { c.is_text = true; c.text = s; isText = true; }
            }
        }
        if (!isText)
        {
            c.is_text = false;
            c.numeric = n;
        }
        *out = c;
        return true;
    }

    // long-form: always text (per original format's own rules), 1..4 trailing zeros
    size_t nzero = 0;
    for (size_t i = ilen; i > 0 && content[i - 1] == 0; i--) nzero++;
    if (nzero < 1 || nzero > 4) return false;
    c.is_text = true;
    c.text = sjis::decode_lenient(content, ilen - nzero);
    *out = c;
    return true;
}

static void scan_data_chunks(const std::vector<uint8_t>& data, Action& action)
{
    size_t windowStart = 0;
    bool atBeginning = true;
    std::vector<uint8_t> leadingJunk;

    while (windowStart < data.size())
    {
        size_t pos = 0;
        bool found = false;
        DataChunk chunk;

        while (windowStart + pos < data.size())
        {
            if (try_decode_chunk(data.data(), data.size(), windowStart + pos, &chunk))
            {
                found = true;
                break;
            }
            pos++;
        }

        if (!found) break; // remainder becomes trailing junk below

        if (pos != 0)
        {
            if (!atBeginning)
            {
                // Shouldn't happen in well-formed files; be lenient and fold the
                // whole blob into the UnparsedData fallback instead of crashing.
                action.unparsed = true;
                action.unparsed_data = data;
                return;
            }
            leadingJunk.assign(data.begin(), data.begin() + pos);
        }

        atBeginning = false;
        chunk.offset = (int)(windowStart + pos);
        action.chunks.push_back(chunk);
        windowStart = windowStart + pos + (size_t)chunk.on_disk_length();
    }

    if (windowStart < data.size())
    {
        if (action.chunks.empty())
        {
            // whole blob is junk (e.g. opcodes with no string args)
            action.leading_junk = data;
            return;
        }
        // trailing bytes after the last chunk that aren't a valid chunk —
        // preserve verbatim via fallback rather than risk corrupting them.
        action.unparsed = true;
        action.unparsed_data = data;
        return;
    }

    action.leading_junk = leadingJunk;
}

// ---------- file ----------

Stcm2File Stcm2File::parse(const uint8_t* file, size_t len)
{
    size_t pos = 0;
    if (len < kMagicLen || memcmp(file, "STCM2", 5) != 0)
        throw Stcm2Error("missing STCM2 magic");
    pos += kMagicLen;

    Stcm2File result;
    if (pos + kTagLen > len) throw Stcm2Error("truncated tag");
    memcpy(result.tag.data(), file + pos, kTagLen);
    pos += kTagLen;

    uint32_t exportAddr = read_u32le(file, len, pos); pos += 4;
    uint32_t exportLen = read_u32le(file, len, pos); pos += 4;
    result.unk1 = read_u32le(file, len, pos); pos += 4;
    result.collection_addr = read_u32le(file, len, pos); pos += 4;
    if (pos + 32 > len) throw Stcm2Error("truncated unk32");
    memcpy(result.unk32.data(), file + pos, 32);
    pos += 32;

    static const uint8_t gdm[] = { 'G','L','O','B','A','L','_','D','A','T','A', 0 };
    static const uint8_t csm[] = { 'C','O','D','E','_','S','T','A','R','T','_', 0 };
    static const uint8_t edm[] = { 'E','X','P','O','R','T','_','D','A','T','A', 0 };
    constexpr size_t kGdmLen = 12, kCsmLen = 12, kEdmLen = 12;

    if (pos + kGdmLen > len || memcmp(file + pos, gdm, kGdmLen) != 0)
        throw Stcm2Error("missing GLOBAL_DATA magic");
    pos += kGdmLen;
    if (pos != kGlobalDataOffset) throw Stcm2Error("global data offset mismatch");

    size_t globalLen = 0;
    while (pos + globalLen + kCsmLen <= len &&
           memcmp(file + pos + globalLen, csm, kCsmLen) != 0)
        globalLen += 4;
    if (pos + globalLen + kCsmLen > len)
        throw Stcm2Error("missing CODE_START magic");
    result.global_data.assign(file + pos, file + pos + globalLen);
    pos += globalLen;

    if (memcmp(file + pos, csm, kCsmLen) != 0)
        throw Stcm2Error("missing CODE_START magic");
    pos += kCsmLen;

    if (exportAddr < kEdmLen)
        throw Stcm2Error("bad export_addr");
    size_t exportEntriesStart = (size_t)exportAddr - kEdmLen;

    std::map<uint32_t, size_t> actionsByAddr; // addr -> index in result.actions

    while (pos < exportEntriesStart)
    {
        uint32_t addr = (uint32_t)pos;
        uint32_t globalCall = read_u32le(file, len, pos); pos += 4;
        uint32_t opcode = read_u32le(file, len, pos); pos += 4;
        uint32_t nparams = read_u32le(file, len, pos); pos += 4;
        uint32_t length = read_u32le(file, len, pos); pos += 4;

        bool call;
        switch (globalCall)
        {
        case 0: call = false; break;
        case 1: call = true; break;
        default:
            throw Stcm2Error("global_call = " + [&]() {
                char buf[16];
                snprintf(buf, sizeof(buf), "%08X", globalCall);
                return std::string(buf);
            }());
        }

        Action action;
        action.original_addr = addr;
        action.call = call;
        action.opcode = opcode;

        uint32_t dataAddr = addr + 16 + 12 * nparams;
        if (length < 16 + 12 * nparams)
            throw Stcm2Error("bad action length");
        uint32_t dataLen = length - 16 - 12 * nparams;

        for (uint32_t i = 0; i < nparams; i++)
        {
            uint32_t triple[3];
            triple[0] = read_u32le(file, len, pos);
            triple[1] = read_u32le(file, len, pos + 4);
            triple[2] = read_u32le(file, len, pos + 8);
            pos += 12;
            action.params.push_back(Param::parse(triple, dataAddr, dataLen, (uint32_t)globalLen));
        }

        if (pos + dataLen > len) throw Stcm2Error("action data past end");
        std::vector<uint8_t> data(file + pos, file + pos + dataLen);
        pos += dataLen;

        scan_data_chunks(data, action);

        actionsByAddr[addr] = result.actions.size();
        result.actions.push_back(std::move(action));
    }

    if (pos + sizeof(edm) > len || memcmp(file + pos, edm, sizeof(edm)) != 0)
        throw Stcm2Error("missing EXPORT_DATA magic");
    pos += sizeof(edm);

    for (uint32_t i = 0; i < exportLen; i++)
    {
        uint32_t zero = read_u32le(file, len, pos); pos += 4;
        if (zero != 0) throw Stcm2Error("expected zero in export entry");
        ExportEntry e;
        if (pos + 32 > len) throw Stcm2Error("truncated export name");
        memcpy(e.name.data(), file + pos, 32);
        pos += 32;
        e.target = read_u32le(file, len, pos); pos += 4;
        result.exports.push_back(e);
    }

    return result;
}

std::vector<uint8_t> Stcm2File::serialize() const
{
    // Pass 1: compute new data bytes + lengths for every action, assign addresses.
    uint32_t pos = kGlobalDataOffset + (uint32_t)global_data.size() + 12; // CODE_START_\0
    std::map<uint32_t, uint32_t> addrMap;
    struct Built { const Action* action; std::vector<uint8_t> data; uint32_t newAddr; uint32_t length; };
    std::vector<Built> built;
    built.reserve(actions.size());

    for (const auto& action : actions)
    {
        std::vector<uint8_t> data = action.build_data();
        uint32_t nparams = (uint32_t)action.params.size();
        uint32_t length = 16 + 12 * nparams + (uint32_t)data.size();
        addrMap[action.original_addr] = pos;
        built.push_back({ &action, std::move(data), pos, length });
        pos += length;
    }

    std::function<uint32_t(uint32_t)> resolve = [&](uint32_t originalAddr) {
        auto it = addrMap.find(originalAddr);
        if (it == addrMap.end())
            throw Stcm2Error("action ref to unknown address " + [&]() {
                char buf[16];
                snprintf(buf, sizeof(buf), "%X", originalAddr);
                return std::string(buf);
            }());
        return it->second;
    };

    std::vector<uint8_t> out;
    write_bytes(out, (const uint8_t*)"STCM2", 5);
    write_bytes(out, tag.data(), tag.size());

    size_t headerFixupPos = out.size(); // export_addr, export_len
    write_u32le(out, 0);
    write_u32le(out, 0);
    write_u32le(out, unk1);
    write_u32le(out, collection_addr);
    write_bytes(out, unk32.data(), unk32.size());
    write_bytes(out, (const uint8_t*)"GLOBAL_DATA\0", 12);
    write_bytes(out, global_data.data(), global_data.size());
    write_bytes(out, (const uint8_t*)"CODE_START_\0", 12);

    for (auto& b : built)
    {
        uint32_t dataAddr = b.newAddr + 16 + 12 * (uint32_t)b.action->params.size();
        write_u32le(out, b.action->call ? 1u : 0u);
        write_u32le(out, b.action->call ? resolve(b.action->opcode) : b.action->opcode);
        write_u32le(out, (uint32_t)b.action->params.size());
        write_u32le(out, b.length);
        for (const auto& p : b.action->params)
        {
            uint32_t enc[3];
            p.encode(dataAddr, enc, resolve);
            write_u32le(out, enc[0]);
            write_u32le(out, enc[1]);
            write_u32le(out, enc[2]);
        }
        write_bytes(out, b.data.data(), b.data.size());
    }

    uint32_t exportEntriesStart = (uint32_t)out.size() + 12;
    write_bytes(out, (const uint8_t*)"EXPORT_DATA\0", 12);
    for (const auto& e : exports)
    {
        write_u32le(out, 0);
        write_bytes(out, e.name.data(), e.name.size());
        write_u32le(out, resolve(e.target));
    }

    // fix up export_addr/export_len
    out[headerFixupPos + 0] = (uint8_t)exportEntriesStart;
    out[headerFixupPos + 1] = (uint8_t)(exportEntriesStart >> 8);
    out[headerFixupPos + 2] = (uint8_t)(exportEntriesStart >> 16);
    out[headerFixupPos + 3] = (uint8_t)(exportEntriesStart >> 24);
    uint32_t n = (uint32_t)exports.size();
    out[headerFixupPos + 4] = (uint8_t)n;
    out[headerFixupPos + 5] = (uint8_t)(n >> 8);
    out[headerFixupPos + 6] = (uint8_t)(n >> 16);
    out[headerFixupPos + 7] = (uint8_t)(n >> 24);

    // record new addresses (needed by the caller to persist per-action state)
    for (auto& b : built) const_cast<Action*>(b.action)->new_addr = b.newAddr;

    return out;
}

// ---------- standalone action round-trip (undo log) ----------

Action Stcm2File::parse_action(const uint8_t* b, size_t len, uint32_t addr, uint32_t globalLen)
{
    if (len < 16) throw Stcm2Error("action too short");
    uint32_t globalCall = read_u32le(b, len, 0);
    uint32_t opcode = read_u32le(b, len, 4);
    uint32_t nparams = read_u32le(b, len, 8);
    uint32_t length = read_u32le(b, len, 12);
    if (length < 16 + 12 * nparams || length > len)
        throw Stcm2Error("bad action length");

    Action a;
    a.original_addr = addr;
    a.call = globalCall != 0;
    a.opcode = opcode;

    uint32_t dataAddr = addr + 16 + 12 * nparams;
    uint32_t dataLen = length - 16 - 12 * nparams;
    for (uint32_t i = 0; i < nparams; i++)
    {
        uint32_t triple[3];
        triple[0] = read_u32le(b, len, 16 + 12 * i);
        triple[1] = read_u32le(b, len, 16 + 12 * i + 4);
        triple[2] = read_u32le(b, len, 16 + 12 * i + 8);
        a.params.push_back(Param::parse(triple, dataAddr, dataLen, globalLen));
    }
    std::vector<uint8_t> data(b + 16 + 12 * nparams, b + length);
    scan_data_chunks(data, a);
    return a;
}

std::vector<uint8_t> Stcm2File::serialize_action(const Action& a, uint32_t addr)
{
    std::vector<uint8_t> data = a.build_data();
    std::vector<uint8_t> out;
    write_u32le(out, a.call ? 1u : 0u);
    write_u32le(out, a.opcode);
    write_u32le(out, (uint32_t)a.params.size());
    write_u32le(out, 16 + 12 * (uint32_t)a.params.size() + (uint32_t)data.size());
    std::function<uint32_t(uint32_t)> identity = [](uint32_t v) { return v; };
    for (const auto& p : a.params)
    {
        uint32_t enc[3];
        p.encode(addr + 16 + 12 * (uint32_t)a.params.size(), enc, identity);
        write_u32le(out, enc[0]);
        write_u32le(out, enc[1]);
        write_u32le(out, enc[2]);
    }
    write_bytes(out, data.data(), data.size());
    return out;
}

} // namespace dokuro
