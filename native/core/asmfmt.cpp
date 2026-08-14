// STCM2 <-> human-editable text format for the direct script editor tab.
// See asmfmt.h for the grammar. Round-trip contract: disasm() of any
// parseable Stcm2File, fed back through assemble(), reproduces the same model
// bytes. Text chunks whose content cannot be losslessly re-encoded (e.g.
// 4-byte text with no trailing zero padding, or invalid-cp932 bytes) are
// emitted as `chunk text_raw` so nothing is ever silently corrupted.
#include "asmfmt.h"
#include "cp932.h"
#include "utf8.h"
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>

namespace dokuro {

namespace {

// ---------- shared helpers ----------

bool parse_hex_word(const std::string& tok, uint32_t* out)
{
    if (tok.empty()) return false;
    size_t i = 0;
    if (tok.size() >= 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) i = 2;
    if (i == tok.size()) return false;
    uint64_t v = 0;
    for (; i < tok.size(); i++)
    {
        char c = tok[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * 16 + (uint64_t)d;
        if (v > 0xFFFFFFFFull) return false;
    }
    if (out) *out = (uint32_t)v;
    return true;
}

std::string hex32(uint32_t v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

// One part of a parsed token: either a run of raw BYTES (from \xNN escapes /
// control escapes) or a run of UTF-8 text (to be sjis-encoded on use).
struct StrPart
{
    bool raw;
    std::string bytes;
};

// Parses the content of a quoted token (opening quote already consumed).
// Regular chars are UTF-8 text parts; escapes are raw-byte parts.
bool parse_quoted(const std::string& line, size_t* i, std::vector<StrPart>& parts)
{
    StrPart cur{ false, "" };
    auto flush = [&]() {
        if (!cur.bytes.empty()) { parts.push_back(cur); cur.bytes.clear(); }
        cur.raw = false;
    };
    while (*i < line.size())
    {
        char c = line[*i];
        if (c == '"') { flush(); (*i)++; return true; }
        if (c != '\\')
        {
            cur.raw = false;
            cur.bytes += c;
            (*i)++;
            continue;
        }
        if (*i + 1 >= line.size()) return false;
        char e = line[*i + 1];
        auto raw = [&](char b) {
            flush();
            cur.raw = true;
            cur.bytes += b;
        };
        switch (e)
        {
        case '"': raw('"'); *i += 2; break;
        case '\\': raw('\\'); *i += 2; break;
        case 'n': raw('\n'); *i += 2; break;
        case 'r': raw('\r'); *i += 2; break;
        case 't': raw('\t'); *i += 2; break;
        case 'x':
        {
            if (*i + 3 >= line.size()) return false;
            char h1 = line[*i + 2], h2 = line[*i + 3];
            int d1 = (h1 >= '0' && h1 <= '9') ? h1 - '0'
                    : (h1 >= 'a' && h1 <= 'f') ? h1 - 'a' + 10
                    : (h1 >= 'A' && h1 <= 'F') ? h1 - 'A' + 10 : -1;
            int d2 = (h2 >= '0' && h2 <= '9') ? h2 - '0'
                    : (h2 >= 'a' && h2 <= 'f') ? h2 - 'a' + 10
                    : (h2 >= 'A' && h2 <= 'F') ? h2 - 'A' + 10 : -1;
            if (d1 < 0 || d2 < 0) return false;
            raw((char)(d1 * 16 + d2));
            *i += 4;
            break;
        }
        default:
            return false;
        }
    }
    return false; // unterminated
}

// Splits one line into tokens: quoted strings stay whole (as StrPart vectors),
// ';' starts a comment outside quotes, everything else is whitespace-separated.
// Bare words come back as a single non-raw part.
bool lex_line(const std::string& line, std::vector<std::vector<StrPart>>& toks)
{
    toks.clear();
    size_t i = 0;
    while (i < line.size())
    {
        char c = line[i];
        if (c == ';') break;
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        std::vector<StrPart> tok;
        if (c == '"')
        {
            i++;
            if (!parse_quoted(line, &i, tok)) return false;
        }
        else
        {
            std::string word;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' &&
                   line[i] != '\r' && line[i] != ';')
                word += line[i++];
            tok.push_back({ false, word });
        }
        toks.push_back(std::move(tok));
    }
    return true;
}

// Concatenate token parts into a plain UTF-8 string (for keyword compare /
// hex words). Raw parts are kept byte-identical.
std::string tok_text(const std::vector<StrPart>& tok)
{
    std::string s;
    for (const auto& p : tok) s += p.bytes;
    return s;
}

// Escape a UTF-8 string for a quoted asm token (text chunk bodies).
std::string escape_text(const std::string& s)
{
    std::string out;
    for (unsigned char ch : s)
    {
        switch (ch)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20 || ch == 0x7F)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\x%02X", ch);
                out += buf;
            }
            else out += (char)ch;
        }
    }
    return out;
}

// Escape arbitrary bytes (export names, tag) — printable ASCII stays,
// everything else becomes \xNN. Byte-exact.
std::string escape_bytes(const uint8_t* b, size_t n)
{
    std::string out;
    for (size_t i = 0; i < n; i++)
    {
        unsigned char ch = b[i];
        if (ch >= 0x20 && ch <= 0x7E && ch != '"' && ch != '\\')
            out += (char)ch;
        else
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\x%02X", ch);
            out += buf;
        }
    }
    return out;
}

// Convert token parts to the on-disk content bytes of a TEXT chunk: UTF-8
// parts are sjis-encoded (error if unencodable), raw parts are copied.
bool parts_to_sjis(const std::vector<StrPart>& tok, std::string& out,
                   std::string* errMsg)
{
    out.clear();
    for (const auto& p : tok)
    {
        if (p.raw)
        {
            out += p.bytes;
            continue;
        }
        size_t i = 0;
        while (i < p.bytes.size())
        {
            size_t adv;
            uint32_t cp = utf8::decode(p.bytes.data() + i, p.bytes.size() - i, &adv);
            if (cp == (uint32_t)-1)
            {
                if (errMsg) *errMsg = "invalid UTF-8 in quoted string";
                return false;
            }
            std::vector<uint8_t> enc;
            size_t bad;
            uint32_t badcp;
            if (!sjis::encode(std::string(p.bytes, i, adv), enc, &bad, &badcp))
            {
                if (errMsg)
                {
                    char buf[64];
                    snprintf(buf, sizeof(buf),
                             "character U+%04X can't be encoded to the game charset (cp932)",
                             (unsigned)badcp);
                    *errMsg = buf;
                }
                return false;
            }
            out.append((const char*)enc.data(), enc.size());
            i += adv;
        }
    }
    return true;
}

std::string word(const std::vector<StrPart>& tok) { return tok_text(tok); }

void emit_dwords(std::ostringstream& os, const uint8_t* b, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i += 4)
    {
        uint32_t w = (uint32_t)b[i] | ((uint32_t)b[i + 1] << 8) |
                     ((uint32_t)b[i + 2] << 16) | ((uint32_t)b[i + 3] << 24);
        os << " " << hex32(w);
    }
}

// ---------- disasm ----------

void emit_chunk(std::ostringstream& os, const DataChunk& c)
{
    if (c.is_text)
    {
        // Emit the ACTUAL script content: the translation for edited chunks,
        // the pristine text otherwise. (The old code emitted c.text — the
        // pristine JP — for edited chunks too, so a custom line that was
        // translated in the friendly editor disassembled to an empty string,
        // and applying an unchanged buffer silently reverted edits to JP.)
        if (c.edited)
        {
            std::vector<uint8_t> enc;
            size_t bad;
            uint32_t badcp;
            if (sjis::encode(c.edited_text, enc, &bad, &badcp))
            {
                os << "chunk text \"" << escape_text(c.edited_text) << "\"\n";
                return;
            }
            // unencodable edit: emit the pristine bytes so the round trip
            // stays valid (the translation can't reach the game anyway)
            os << "chunk text_raw";
            emit_dwords(os, c.raw.data(), c.raw.size());
            os << "\n";
            return;
        }
        // round-trip check: sjis(text) + pad must reproduce raw exactly
        std::vector<uint8_t> enc;
        size_t bad;
        uint32_t badcp;
        bool ok = sjis::encode(c.text, enc, &bad, &badcp);
        int pad = 4 - (int)(enc.size() % 4);
        if (pad == 0) pad = 4;
        enc.insert(enc.end(), (size_t)pad, 0);
        ok = ok && enc.size() == c.raw.size() &&
             (enc.empty() || memcmp(enc.data(), c.raw.data(), enc.size()) == 0);
        if (ok)
        {
            os << "chunk text \"" << escape_text(c.text) << "\"\n";
            return;
        }
        os << "chunk text_raw";
        emit_dwords(os, c.raw.data(), c.raw.size());
        os << "\n";
        return;
    }
    if (c.type == 1)
    {
        os << "chunk num " << hex32(c.numeric) << "\n";
        return;
    }
    if (c.raw.size() == 4)
    {
        os << "chunk val " << hex32(c.numeric) << "\n";
        return;
    }
    os << "chunk raw";
    emit_dwords(os, c.raw.data(), c.raw.size());
    os << "\n";
}

} // namespace

std::string asm_disasm(const Stcm2File& f, int file_index)
{
    std::ostringstream os;
    os << "; STCM2 slot " << file_index << " - direct script editor format\n";
    os << "; Round-trip safe: untouched statements reproduce byte-for-byte.\n";
    os << ".stcm2 \"" << escape_bytes(f.tag.data(), f.tag.size()) << "\"\n";
    os << ".unk1 " << hex32(f.unk1) << "\n";
    os << ".collection " << hex32(f.collection_addr) << "\n";
    os << ".unk32";
    emit_dwords(os, f.unk32.data(), f.unk32.size());
    os << "\n";
    os << ".global_data\n";
    for (size_t i = 0; i < f.global_data.size(); i += 16)
    {
        os << "  .bytes";
        size_t end = std::min(i + 16, f.global_data.size());
        emit_dwords(os, f.global_data.data() + i, end - i);
        os << "\n";
    }
    os << ".end\n";
    os << ".code_start\n";

    for (const auto& a : f.actions)
    {
        os << "action @" << hex32(a.original_addr) << "\n";
        os << "  call " << (a.call ? 1 : 0) << "\n";
        if (a.call)
            os << "  target " << hex32(a.opcode) << "\n";
        else
            os << "  opcode " << hex32(a.opcode) << "\n";
        for (const auto& p : a.params)
        {
            switch (p.kind)
            {
            case ParamKind::ActionRef: os << "  param action_ref " << hex32(p.value); break;
            case ParamKind::DataPointer: os << "  param data_ptr " << hex32(p.value); break;
            case ParamKind::GlobalDataPointer: os << "  param global_ptr " << hex32(p.value); break;
            case ParamKind::Value: os << "  param value " << hex32(p.value); break;
            }
            os << "\n";
        }
        os << "  data\n";
        if (a.unparsed)
        {
            os << "    ; data is opaque (unparsed)\n";
            for (size_t i = 0; i < a.unparsed_data.size(); i += 16)
            {
                os << "    .bytes";
                size_t end = std::min(i + 16, a.unparsed_data.size());
                emit_dwords(os, a.unparsed_data.data() + i, end - i);
                os << "\n";
            }
        }
        else
        {
            for (size_t i = 0; i < a.leading_junk.size(); i += 4)
            {
                os << "    .bytes";
                size_t end = std::min(i + 4, a.leading_junk.size());
                emit_dwords(os, a.leading_junk.data() + i, end - i);
                os << "\n";
            }
            for (const auto& c : a.chunks)
            {
                os << "    ";
                emit_chunk(os, c);
            }
        }
        os << "  end_action\n";
    }

    os << "exports\n";
    for (const auto& e : f.exports)
    {
        os << "  export \"" << escape_bytes(e.name.data(), e.name.size()) << "\" @"
           << hex32(e.target) << "\n";
    }
    os << ".end\n";
    return os.str();
}

// ---------- assemble ----------

namespace {

struct AsmErr
{
    int line = 0;
    std::string msg;
    bool failed = false;
};

void fail(AsmErr& e, int line, const std::string& msg)
{
    if (!e.failed)
    {
        e.failed = true;
        e.line = line;
        e.msg = msg;
    }
}

struct PendingParam
{
    int line;
    ParamKind kind;
    uint32_t value;
};

struct PendingExport
{
    int line;
    std::string name; // raw bytes
    uint32_t target;
};

struct DisElem
{
    bool is_chunk = false;
    DataChunk chunk;
    std::vector<uint8_t> bytes; // for raw byte runs
};

struct PendingAction
{
    int line = 0;
    uint32_t anno_addr = 0;   // address from the 'action @0x...' annotation
    bool anno_seen = false;
    bool call = false;
    uint32_t opcode = 0;
    std::vector<PendingParam> params;
    std::vector<DisElem> elems; // chunks + raw byte runs, in order
};

} // namespace

bool asm_assemble(const std::string& text, int file_index, Stcm2File& out, AsmError& err)
{
    (void)file_index;
    AsmErr e;
    Stcm2File f;

    std::istringstream in(text);
    std::string line;
    int lineNo = 0;

    enum class Phase { Header, Global, Code, Exports };
    Phase phase = Phase::Header;

    bool tagSeen = false;
    bool globalSeen = false;
    std::vector<PendingAction> actions;
    std::vector<uint8_t> globalBytes;
    std::vector<PendingExport> pendingExports;
    PendingAction* cur = nullptr;

    while (std::getline(in, line) && !e.failed)
    {
        lineNo++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::vector<StrPart>> toks;
        if (!lex_line(line, toks))
        {
            fail(e, lineNo, "unterminated or malformed quoted string");
            break;
        }
        if (toks.empty()) continue;

        const std::string& t = word(toks[0]);

        if (t == ".stcm2")
        {
            if (phase != Phase::Header || tagSeen)
            {
                fail(e, lineNo, ".stcm2 must be the first statement");
                break;
            }
            if (toks.size() != 2)
            {
                fail(e, lineNo, ".stcm2 expects one quoted tag");
                break;
            }
            std::string tag;
            if (!parts_to_sjis(toks[1], tag, &e.msg)) { e.failed = true; e.line = lineNo; break; }
            memset(f.tag.data(), 0, f.tag.size());
            memcpy(f.tag.data(), tag.data(), std::min<size_t>(tag.size(), f.tag.size()));
            tagSeen = true;
        }
        else if (t == ".unk1" || t == ".collection")
        {
            if (phase != Phase::Header)
            {
                fail(e, lineNo, t + " only valid in the header");
                break;
            }
            if (toks.size() != 2)
            {
                fail(e, lineNo, t + " expects one hex value");
                break;
            }
            uint32_t v;
            if (!parse_hex_word(word(toks[1]), &v))
            {
                fail(e, lineNo, t + ": bad hex value");
                break;
            }
            if (t == ".unk1") f.unk1 = v;
            else f.collection_addr = v;
        }
        else if (t == ".unk32")
        {
            if (phase != Phase::Header)
            {
                fail(e, lineNo, ".unk32 only valid in the header");
                break;
            }
            if (toks.size() != 9)
            {
                fail(e, lineNo, ".unk32 expects 8 hex values");
                break;
            }
            for (int i = 0; i < 8; i++)
            {
                uint32_t v;
                if (!parse_hex_word(word(toks[i + 1]), &v))
                {
                    fail(e, lineNo, ".unk32: bad hex value");
                    break;
                }
                f.unk32[i * 4] = (uint8_t)v;
                f.unk32[i * 4 + 1] = (uint8_t)(v >> 8);
                f.unk32[i * 4 + 2] = (uint8_t)(v >> 16);
                f.unk32[i * 4 + 3] = (uint8_t)(v >> 24);
            }
        }
        else if (t == ".global_data")
        {
            if (phase != Phase::Header)
            {
                fail(e, lineNo, ".global_data only valid before .code_start");
                break;
            }
            phase = Phase::Global;
            globalSeen = true;
        }
        else if (t == ".bytes")
        {
            if (phase != Phase::Global && phase != Phase::Code)
            {
                fail(e, lineNo, ".bytes only valid inside .global_data or an action's data");
                break;
            }
            if (phase == Phase::Global)
            {
                for (size_t i = 1; i < toks.size(); i++)
                {
                    uint32_t v;
                    if (!parse_hex_word(word(toks[i]), &v))
                    {
                        fail(e, lineNo, ".bytes: bad hex value");
                        break;
                    }
                    globalBytes.push_back((uint8_t)v);
                    globalBytes.push_back((uint8_t)(v >> 8));
                    globalBytes.push_back((uint8_t)(v >> 16));
                    globalBytes.push_back((uint8_t)(v >> 24));
                }
            }
            else
            {
                if (!cur)
                {
                    fail(e, lineNo, ".bytes outside an action");
                    break;
                }
                DisElem el;
                for (size_t i = 1; i < toks.size(); i++)
                {
                    uint32_t v;
                    if (!parse_hex_word(word(toks[i]), &v))
                    {
                        fail(e, lineNo, ".bytes: bad hex value");
                        break;
                    }
                    el.bytes.push_back((uint8_t)v);
                    el.bytes.push_back((uint8_t)(v >> 8));
                    el.bytes.push_back((uint8_t)(v >> 16));
                    el.bytes.push_back((uint8_t)(v >> 24));
                }
                cur->elems.push_back(std::move(el));
            }
        }
        else if (t == ".end")
        {
            if (phase == Phase::Global)
            {
                f.global_data = globalBytes;
                phase = Phase::Header;
            }
            else if (phase == Phase::Exports)
            {
                phase = Phase::Header; // done marker; stop accepting more
                globalSeen = false;    // any further statement is an error
            }
            else
            {
                fail(e, lineNo, ".end not expected here");
                break;
            }
        }
        else if (t == ".code_start")
        {
            if (phase != Phase::Header || !globalSeen)
            {
                fail(e, lineNo, ".code_start must follow .global_data");
                break;
            }
            phase = Phase::Code;
        }
        else if (t == "action")
        {
            if (phase != Phase::Code)
            {
                fail(e, lineNo, "action only valid after .code_start");
                break;
            }
            uint32_t anno;
            if (toks.size() < 2 || word(toks[1]).empty() || word(toks[1])[0] != '@' ||
                !parse_hex_word(word(toks[1]).substr(1), &anno))
            {
                fail(e, lineNo, "action expects '@<address>' (address must match the disassembly)");
                break;
            }
            actions.emplace_back();
            actions.back().line = lineNo;
            actions.back().anno_addr = anno;
            actions.back().anno_seen = true;
            cur = &actions.back();
        }
        else if (t == "call")
        {
            if (!cur)
            {
                fail(e, lineNo, "call outside an action");
                break;
            }
            if (toks.size() != 2 || (word(toks[1]) != "0" && word(toks[1]) != "1"))
            {
                fail(e, lineNo, "call expects 0 or 1");
                break;
            }
            cur->call = word(toks[1]) == "1";
        }
        else if (t == "opcode" || t == "target")
        {
            if (!cur)
            {
                fail(e, lineNo, t + " outside an action");
                break;
            }
            if (toks.size() != 2)
            {
                fail(e, lineNo, t + " expects one hex value");
                break;
            }
            uint32_t v;
            if (!parse_hex_word(word(toks[1]), &v))
            {
                fail(e, lineNo, t + ": bad hex value");
                break;
            }
            cur->call = (t == "target");
            cur->opcode = v;
        }
        else if (t == "param")
        {
            if (!cur)
            {
                fail(e, lineNo, "param outside an action");
                break;
            }
            if (toks.size() != 3)
            {
                fail(e, lineNo, "param expects <kind> <hex>");
                break;
            }
            ParamKind kind;
            const std::string& k = word(toks[1]);
            if (k == "action_ref") kind = ParamKind::ActionRef;
            else if (k == "data_ptr") kind = ParamKind::DataPointer;
            else if (k == "global_ptr") kind = ParamKind::GlobalDataPointer;
            else if (k == "value") kind = ParamKind::Value;
            else
            {
                fail(e, lineNo, "param: unknown kind '" + k + "'");
                break;
            }
            uint32_t v;
            if (!parse_hex_word(word(toks[2]), &v))
            {
                fail(e, lineNo, "param: bad hex value");
                break;
            }
            cur->params.push_back({ lineNo, kind, v });
        }
        else if (t == "data")
        {
            if (!cur)
            {
                fail(e, lineNo, "data outside an action");
                break;
            }
            // marker only
        }
        else if (t == "chunk")
        {
            if (!cur)
            {
                fail(e, lineNo, "chunk outside an action");
                break;
            }
            if (toks.size() < 2)
            {
                fail(e, lineNo, "chunk expects a kind");
                break;
            }
            DisElem el;
            el.is_chunk = true;
            const std::string& kind = word(toks[1]);
            if (kind == "text")
            {
                if (toks.size() != 3)
                {
                    fail(e, lineNo, "chunk text expects one quoted string");
                    break;
                }
                std::string content;
                std::string emsg;
                if (!parts_to_sjis(toks[2], content, &emsg))
                {
                    fail(e, lineNo, "chunk text: " + emsg);
                    break;
                }
                size_t pad = 4 - (content.size() % 4);
                if (pad == 0) pad = 4; // 1..4 trailing zero bytes, same rule as the C# writer
                el.chunk.type = 0;
                el.chunk.is_text = true;
                el.chunk.raw.assign(content.begin(), content.end());
                el.chunk.raw.insert(el.chunk.raw.end(), pad, 0);
                el.chunk.text = sjis::decode_lenient((const uint8_t*)content.data(), content.size());
                cur->elems.push_back(std::move(el));
            }
            else if (kind == "text_raw" || kind == "raw")
            {
                if (toks.size() < 3)
                {
                    fail(e, lineNo, "chunk " + kind + " expects hex dwords");
                    break;
                }
                el.chunk.type = 0;
                el.chunk.is_text = (kind == "text_raw");
                for (size_t i = 2; i < toks.size(); i++)
                {
                    uint32_t v;
                    if (!parse_hex_word(word(toks[i]), &v))
                    {
                        fail(e, lineNo, "chunk " + kind + ": bad hex value");
                        break;
                    }
                    el.chunk.raw.push_back((uint8_t)v);
                    el.chunk.raw.push_back((uint8_t)(v >> 8));
                    el.chunk.raw.push_back((uint8_t)(v >> 16));
                    el.chunk.raw.push_back((uint8_t)(v >> 24));
                }
                if (el.chunk.is_text)
                    el.chunk.text = sjis::decode_lenient(el.chunk.raw.data(), el.chunk.raw.size());
                cur->elems.push_back(std::move(el));
            }
            else if (kind == "num" || kind == "val")
            {
                if (toks.size() != 3)
                {
                    fail(e, lineNo, "chunk " + kind + " expects one hex value");
                    break;
                }
                uint32_t v;
                if (!parse_hex_word(word(toks[2]), &v))
                {
                    fail(e, lineNo, "chunk " + kind + ": bad hex value");
                    break;
                }
                el.chunk.type = (kind == "num") ? 1 : 0;
                el.chunk.is_text = false;
                el.chunk.numeric = v;
                el.chunk.raw.push_back((uint8_t)v);
                el.chunk.raw.push_back((uint8_t)(v >> 8));
                el.chunk.raw.push_back((uint8_t)(v >> 16));
                el.chunk.raw.push_back((uint8_t)(v >> 24));
                cur->elems.push_back(std::move(el));
            }
            else
            {
                fail(e, lineNo, "chunk: unknown kind '" + kind + "'");
                break;
            }
        }
        else if (t == "end_action")
        {
            if (!cur)
            {
                fail(e, lineNo, "end_action outside an action");
                break;
            }
            cur = nullptr;
        }
        else if (t == "exports")
        {
            if (phase != Phase::Code && phase != Phase::Header)
            {
                fail(e, lineNo, "exports not expected here");
                break;
            }
            if (phase == Phase::Code && !globalSeen)
            {
                fail(e, lineNo, "exports before .code_start");
                break;
            }
            phase = Phase::Exports;
            cur = nullptr;
        }
        else if (t == "export")
        {
            if (phase != Phase::Exports)
            {
                fail(e, lineNo, "export only valid after the exports marker");
                break;
            }
            if (toks.size() != 3 || word(toks[2]).empty() || word(toks[2])[0] != '@')
            {
                fail(e, lineNo, "export expects \"<name>\" @<address>");
                break;
            }
            uint32_t v;
            if (!parse_hex_word(word(toks[2]).substr(1), &v))
            {
                fail(e, lineNo, "export: bad target address");
                break;
            }
            PendingExport pe;
            pe.line = lineNo;
            std::string name;
            if (!parts_to_sjis(toks[1], name, &e.msg)) { e.failed = true; e.line = lineNo; break; }
            pe.name = name;
            pe.target = v;
            pendingExports.push_back(std::move(pe));
        }
        else
        {
            fail(e, lineNo, "unknown statement '" + t + "'");
            break;
        }
    }

    if (!e.failed && phase == Phase::Global)
    {
        f.global_data = globalBytes; // omitted .end — be lenient
        phase = Phase::Header;
    }
    if (!e.failed && !tagSeen)
    {
        e.failed = true;
        e.line = 1;
        e.msg = "missing .stcm2 tag";
    }

    // ---------- second pass: layout, classify params, build the model ----------
    if (!e.failed)
    {
        uint32_t addr = Stcm2File::kGlobalDataOffset + (uint32_t)f.global_data.size() + 12;
        // annotation addr -> new addr (each action carries its old address)
        std::map<uint32_t, uint32_t> remap;
        for (auto& pa : actions)
        {
            if (remap.count(pa.anno_addr))
            {
                fail(e, pa.line, "duplicate action address @" + hex32(pa.anno_addr) +
                     " (addresses must be unique)");
                break;
            }
            remap[pa.anno_addr] = addr;
            addr += 16 + 12 * (uint32_t)pa.params.size();
            for (auto& el : pa.elems)
                addr += el.is_chunk ? (uint32_t)(16 + el.chunk.raw.size()) : (uint32_t)el.bytes.size();
        }
        if (e.failed) goto refcheck_done; // (unreachable; keeps flow linear below)

        addr = Stcm2File::kGlobalDataOffset + (uint32_t)f.global_data.size() + 12;
        for (auto& pa : actions)
        {
            Action a;
            a.original_addr = pa.anno_addr;
            a.call = pa.call;
            a.opcode = pa.opcode;
            uint32_t nparams = (uint32_t)pa.params.size();
            uint32_t dataAddr = addr + 16 + 12 * nparams;

            // data bytes + chunk/bytes bookkeeping (mirror ScanDataChunks rules)
            std::vector<uint8_t> dataBytes;
            size_t junkLen = 0;      // bytes before the first chunk
            bool sawChunk = false;
            bool stray = false;      // bytes after the first chunk
            for (auto& el : pa.elems)
            {
                if (el.is_chunk)
                {
                    // serialize header + content
                    write_u32le(dataBytes, el.chunk.type);
                    write_u32le(dataBytes, (uint32_t)(el.chunk.raw.size() / 4));
                    write_u32le(dataBytes, 1u); // magic
                    write_u32le(dataBytes, (uint32_t)el.chunk.raw.size());
                    dataBytes.insert(dataBytes.end(), el.chunk.raw.begin(), el.chunk.raw.end());
                    sawChunk = true;
                }
                else
                {
                    if (!sawChunk) junkLen += el.bytes.size();
                    else stray = true;
                    dataBytes.insert(dataBytes.end(), el.bytes.begin(), el.bytes.end());
                }
            }

            uint32_t dataLen = (uint32_t)dataBytes.size();

            // classify params against the real ranges; declared kind must match
            std::function<uint32_t(uint32_t)> id = [](uint32_t v) { return v; };
            for (auto& pp : pa.params)
            {
                Param p;
                p.kind = pp.kind;
                p.value = pp.value;
                uint32_t triple[3];
                p.encode(dataAddr, triple, id);
                try
                {
                    Param re = Param::parse(triple, dataAddr, dataLen, (uint32_t)f.global_data.size());
                    if (re.kind != p.kind)
                    {
                        const char* names[] = { "action_ref", "data_ptr", "value", "global_ptr" };
                        fail(e, pp.line, std::string("param ") + names[(int)p.kind] +
                             " value falls outside that kind's range for this layout "
                             "(re-classified as " + names[(int)re.kind] + ")");
                        break;
                    }
                }
                catch (const Stcm2Error&)
                {
                    const char* names[] = { "action_ref", "data_ptr", "value", "global_ptr" };
                    fail(e, pp.line, std::string("param ") + names[(int)p.kind] +
                         " does not classify for this data layout");
                    break;
                }
                a.params.push_back(p);
            }
            if (e.failed) break;

            if (pa.elems.empty())
            {
                // no data at all
            }
            else if (!stray)
            {
                if (!sawChunk)
                {
                    a.leading_junk = dataBytes; // whole blob is junk bytes
                }
                else
                {
                    a.leading_junk.assign(dataBytes.begin(), dataBytes.begin() + junkLen);
                    // build chunks with real offsets
                    size_t off = junkLen;
                    for (auto& el : pa.elems)
                    {
                        if (!el.is_chunk) continue;
                        DataChunk c = el.chunk;
                        c.offset = (int)off;
                        off += (size_t)c.on_disk_length();
                        a.chunks.push_back(c);
                    }
                }
            }
            else
            {
                a.unparsed = true;
                a.unparsed_data = dataBytes;
            }

            addr += 16 + 12 * nparams + dataLen;
            f.actions.push_back(std::move(a));
        }

        // ---------- reference validation (through the annotation remap) ----------
        if (!e.failed)
        {
            auto checkTarget = [&](int line, uint32_t target, const char* what) {
                if (remap.find(target) == remap.end())
                    fail(e, line, std::string(what) + " target " + hex32(target) +
                         " is not an action address in this file");
            };
            for (auto& pa : actions)
            {
                if (pa.call) checkTarget(pa.line, pa.opcode, "call");
                for (auto& pp : pa.params)
                    if (pp.kind == ParamKind::ActionRef)
                        checkTarget(pp.line, pp.value, "action_ref");
            }
            for (auto& pe : pendingExports)
                checkTarget(pe.line, pe.target, "export");
        }
    }
refcheck_done:

    if (!e.failed)
    {
        for (auto& pe : pendingExports)
        {
            ExportEntry ex;
            memset(ex.name.data(), 0, ex.name.size());
            memcpy(ex.name.data(), pe.name.data(),
                   std::min<size_t>(pe.name.size(), ex.name.size()));
            ex.target = pe.target;
            f.exports.push_back(ex);
        }
        out = std::move(f);
        return true;
    }

    err.line = e.line;
    err.msg = e.msg;
    return false;
}

} // namespace dokuro
