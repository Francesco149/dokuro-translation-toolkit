#include "project.h"
#include "cp932.h"
#include "text.h"
#include "uni2.h"
#include "utf8.h"
#include <map>
#include <sstream>
#include <cstring>

namespace dokuro {

namespace {
constexpr uint32_t kOpDialogue = 0x118;
constexpr uint32_t kOpBoxAdvance = 0x119;
constexpr uint32_t kOpNameplate = 0x11A;

bool is_text_chunk(const Stcm2File& f, int ai, int ci)
{
    return ai >= 0 && ai < (int)f.actions.size() &&
           ci >= 0 && ci < (int)f.actions[ai].chunks.size() &&
           f.actions[ai].chunks[ci].is_text;
}

// Content signature used to match a working action to its pristine
// counterpart after structural edits (added/removed/moved actions shift the
// on-disk layout, so addresses and positions are unreliable on reopen).
//   exact    = structure + param values + ALL chunk bytes (identifies
//              UNEDITED actions: identical content round-trips)
//   !exact   = structure + param kinds + non-text chunk bytes only (text
//              content excluded — that is what edits change)
// ActionRef param VALUES are excluded either way (they retarget when the
// layout shifts). DataPointer values are kept in exact mode (relative to the
// owning action's data, so stable for unedited actions).
std::string action_sig(const Action& a, bool exact)
{
    std::string s;
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "%c%u%zu|", a.call ? 'C' : 'A', a.opcode, a.params.size());
    s += hdr;
    for (const auto& p : a.params)
    {
        s += (char)('0' + (int)p.kind);
        if (exact && p.kind != ParamKind::ActionRef)
        {
            s += (char)(p.value >> 24);
            s += (char)(p.value >> 16);
            s += (char)(p.value >> 8);
            s += (char)p.value;
        }
        s += '|';
    }
    for (const auto& c : a.chunks)
    {
        s += c.is_text ? 'T' : 'N';
        s += (c.type ? '1' : '0');
        s += ':';
        if (!c.is_text || exact)
            s.append((const char*)c.raw.data(), c.raw.size());
        s += '|';
    }
    return s;
}

// Positional diff of one matched action pair: any text chunk whose content
// differs from the pristine chunk is a translation.
void restore_edited_chunks(Action& wa, const Action& oa)
{
    if (oa.chunks.size() != wa.chunks.size()) return;
    for (size_t ci = 0; ci < wa.chunks.size(); ci++)
    {
        auto& wc = wa.chunks[ci];
        const auto& oc = oa.chunks[ci];
        if (wc.is_text && oc.is_text && wc.text != oc.text)
        {
            wc.edited = true;
            wc.edited_text = wc.text;
            wc.text = oc.text; // JP view stays pristine
            wc.raw = oc.raw;   // write() uses edited_text while edited
        }
    }
}

// Tool-added actions carry no original text: their content IS the translation.
// The working file doesn't store edited flags, so after a reopen/apply a custom
// chunk would otherwise parse back with its content in the JP view and
// edited=false (not editable, and the next restart keeps shifting the text to
// the read-only JP side). Keep custom chunks on the editable RU side.
void mark_custom_chunks(Stcm2File& wf)
{
    for (auto& a : wf.actions)
    {
        if (!a.custom) continue;
        for (auto& c : a.chunks)
            if (c.is_text)
            {
                c.edited = true;
                c.edited_text = c.text;
                c.text.clear(); // JP view: no original text for custom lines
            }
    }
}

// Restore edited flags for a slot whose structure changed: pair each working
// action with its pristine counterpart by signature, then diff positionally
// within the pair. Custom (tool-added) actions must already be marked.
void restore_edited_actions(Stcm2File& wf, const Stcm2File& of)
{
    std::vector<int> used(of.actions.size(), 0);
    std::vector<int> partner(wf.actions.size(), -1);

    // pass 1: exact matches (structure + full text content) — unedited lines
    std::map<std::string, std::vector<size_t>> byExact;
    for (size_t i = 0; i < of.actions.size(); i++)
        byExact[action_sig(of.actions[i], true)].push_back(i);
    for (size_t i = 0; i < wf.actions.size(); i++)
    {
        if (wf.actions[i].custom) continue;
        auto it = byExact.find(action_sig(wf.actions[i], true));
        if (it == byExact.end()) continue;
        for (size_t j : it->second)
            if (!used[j]) { partner[i] = (int)j; used[j] = 1; break; }
    }
    // pass 2: structural matches (text content excluded) — edited lines
    std::map<std::string, std::vector<size_t>> byStruct;
    for (size_t i = 0; i < of.actions.size(); i++)
        byStruct[action_sig(of.actions[i], false)].push_back(i);
    for (size_t i = 0; i < wf.actions.size(); i++)
    {
        if (wf.actions[i].custom || partner[i] >= 0) continue;
        auto it = byStruct.find(action_sig(wf.actions[i], false));
        if (it == byStruct.end()) continue;
        for (size_t j : it->second)
            if (!used[j]) { partner[i] = (int)j; used[j] = 1; break; }
    }
    for (size_t i = 0; i < wf.actions.size(); i++)
        if (partner[i] >= 0)
            restore_edited_chunks(wf.actions[i], of.actions[partner[i]]);
}
} // namespace

std::string EncodingIssue::describe() const
{
    std::ostringstream os;
    os << "File " << file << ", "
       << (kind == EntryKind::Dialogue ? "dialogue" : "nameplate")
       << ": character U+" << std::hex << bad_cp << std::dec
       << " at position " << bad_index << " in \"" << text << "\"";
    return os.str();
}

Project Project::load(const std::string& uniPath)
{
    std::vector<uint8_t> data = read_file(uniPath);
    Project p;
    std::vector<std::vector<uint8_t>> slots;
    uni2_split(data.data(), data.size(), p.container_header, slots);
    p.source_path = uniPath;
    for (auto& slot : slots)
    {
        p.orig_slot_sizes.push_back((uint32_t)slot.size());
        p.files.push_back(Stcm2File::parse(slot.data(), slot.size()));
        p.originals.push_back(Stcm2File::parse(slot.data(), slot.size()));
    }
    return p;
}

Project Project::load_pair(const std::string& pristinePath, const std::string& workingPath,
                           const std::vector<std::pair<int, uint32_t>>* customs)
{
    Project p = load(pristinePath);
    std::vector<uint8_t> wdata = read_file(workingPath);
    std::vector<uint8_t> whdr;
    std::vector<std::vector<uint8_t>> wslots;
    uni2_split(wdata.data(), wdata.size(), whdr, wslots);
    if (wslots.size() != p.files.size())
        throw Stcm2Error("working file has " + std::to_string(wslots.size()) +
                         " slots, pristine has " + std::to_string(p.files.size()));
    // Same-lineage check: the embedded chunk TOC (0x800, rebuilt on every
    // save with the current slot sizes) legitimately differs from the
    // pristine header when slots grew, so only the immutable parts must
    // match: bytes before 0x800, the TOC entry ids, and everything after
    // the TOC. The mutable off/sector_len/size fields may differ.
    {
        const size_t kToc = 0x800, kStride = 16;
        auto le32at = [](const std::vector<uint8_t>& v, size_t o) {
            return (uint32_t)v[o] | ((uint32_t)v[o + 1] << 8) |
                   ((uint32_t)v[o + 2] << 16) | ((uint32_t)v[o + 3] << 24);
        };
        bool compat = whdr.size() == p.container_header.size() &&
                      memcmp(whdr.data(), p.container_header.data(), std::min(kToc, whdr.size())) == 0;
        if (compat)
        {
            uint32_t count = 0;
            if (whdr.size() >= 12) count = le32at(whdr, 8);
            if (count > 0 && count <= 128 &&
                whdr.size() >= kToc + count * kStride)
            {
                for (uint32_t i = 0; i < count; i++)
                {
                    if (le32at(whdr, kToc + i * kStride) !=
                        le32at(p.container_header, kToc + i * kStride))
                    { compat = false; break; }
                }
                if (compat && memcmp(whdr.data() + kToc + count * kStride,
                                     p.container_header.data() + kToc + count * kStride,
                                     whdr.size() - (kToc + count * kStride)) != 0)
                    compat = false;
            }
        }
        if (!compat)
            throw Stcm2Error("working file header differs from the pristine one "
                             "(different script lineage?)");
    }
    for (size_t i = 0; i < wslots.size(); i++)
    {
        Stcm2File wf = Stcm2File::parse(wslots[i].data(), wslots[i].size());
        const Stcm2File& of = p.originals[i];
        // Restore the "edited" flags the working file doesn't store: diff each
        // text chunk against the pristine slot.
        //
        // The working file's parse yields CURRENT layout addresses, which
        // shift when actions are added/removed/moved. Position-based matching
        // is therefore only safe when the slot structure is provably pristine
        // (equal action count AND no tool-added actions recorded in the state
        // file). Any other structural change falls back to content matching:
        // each working action is paired with its pristine counterpart by
        // signature (exact content first — unedited lines; then structure
        // only — edited lines). Tool-added actions are excluded via the
        // `customs` list (also marked here so the caller skips the old loop).
        bool structureChanged = of.actions.size() != wf.actions.size();
        if (!structureChanged && customs)
            for (const auto& c : *customs)
                if (c.first == (int)i) { structureChanged = true; break; }

        if (!structureChanged)
        {
            for (size_t ai = 0; ai < wf.actions.size(); ai++)
                restore_edited_chunks(wf.actions[ai], of.actions[ai]);
        }
        else
        {
            // mark tool-added actions from the state file (keyed by the
            // address they occupied at save time == their parse address now)
            if (customs)
                for (const auto& c : *customs)
                    if (c.first == (int)i)
                        for (auto& wa : wf.actions)
                            if (wa.original_addr == c.second) wa.custom = true;
            restore_edited_actions(wf, of);
            // custom chunks are translations, not original JP text
            mark_custom_chunks(wf);
        }
        p.files[i] = std::move(wf);
    }
    return p;
}

// Restores per-chunk edited flags by diffing files[fi] against its pristine
// slot — used after a direct-script apply (the assembled file carries no
// edited flags). Custom actions must already be marked; they are excluded from
// matching and keep their content as-is.
void Project::restore_edited_flags(int fi)
{
    if (fi < 0 || fi >= (int)files.size() || fi >= (int)originals.size()) return;
    Stcm2File& wf = files[fi];
    const Stcm2File& of = originals[fi];
    bool hasCustom = false;
    for (const auto& a : wf.actions)
        if (a.custom) { hasCustom = true; break; }
    if (!hasCustom && wf.actions.size() == of.actions.size())
    {
        for (size_t ai = 0; ai < wf.actions.size(); ai++)
            restore_edited_chunks(wf.actions[ai], of.actions[ai]);
    }
    else
    {
        restore_edited_actions(wf, of);
        mark_custom_chunks(wf);
    }
}

std::vector<uint8_t> Project::build_container() const
{
    std::vector<std::vector<uint8_t>> rebuilt;
    rebuilt.reserve(files.size());
    for (const auto& f : files) rebuilt.push_back(f.serialize());
    return uni2_join(container_header, rebuilt);
}

uint32_t Project::file_budget(size_t fi) const
{
    // Sector-padding limit: the largest a slot can grow before SCRIPT.UNI's
    // file size grows (the container packs slots to 0x800 boundaries). With
    // the embedded-TOC rebuild (uni2_join) the game reads grown slots fine;
    // this rail exists because the in-place ISO patcher cannot hold a bigger
    // file. Growth within the padding needs no ISO change at all.
    uint32_t orig = 0;
    if (fi < originals.size())
    {
        try
        {
            orig = (uint32_t)originals[fi].serialize().size();
        }
        catch (const Stcm2Error&) { orig = 0; }
    }
    return (orig + 0x7FFu) / 0x800u * 0x800u;
}

size_t Project::file_serialized_len(size_t fi) const
{
    return files[fi].serialize().size();
}

bool Project::file_over_budget(size_t fi) const
{
    return file_serialized_len(fi) > file_budget(fi);
}

std::vector<EntryRef> Project::entries() const
{
    std::vector<EntryRef> out;
    for (int fi = 0; fi < (int)files.size(); fi++)
    {
        const auto& f = files[fi];
        for (int ai = 0; ai < (int)f.actions.size(); ai++)
        {
            const auto& a = f.actions[ai];
            if (a.call) continue;
            EntryKind kind;
            if (a.opcode == kOpDialogue) kind = EntryKind::Dialogue;
            else if (a.opcode == kOpNameplate) kind = EntryKind::Nameplate;
            else continue;
            for (int ci = 0; ci < (int)a.chunks.size(); ci++)
            {
                if (!a.chunks[ci].is_text) continue;
                out.push_back({ fi, ai, ci, kind });
            }
        }
    }
    return out;
}

void Project::set_translation(const EntryRef& e, const std::string* text)
{
    if (!is_text_chunk(files[e.file], e.action, e.chunk))
        throw Stcm2Error("not a text chunk");
    DataChunk& c = files[e.file].actions[e.action].chunks[e.chunk];
    if (text == nullptr)
    {
        c.edited = false;
        c.edited_text.clear();
        return;
    }
    c.edited = true;
    c.edited_text = sanitize(*text);
}

void Project::set_translation_raw(const EntryRef& e, bool edited, const std::string& edited_text)
{
    if (!is_text_chunk(files[e.file], e.action, e.chunk))
        throw Stcm2Error("not a text chunk");
    DataChunk& c = files[e.file].actions[e.action].chunks[e.chunk];
    c.edited = edited;
    c.edited_text = edited_text;
}

bool Project::revert_chunk(const EntryRef& e)
{
    if (e.file < 0 || e.file >= (int)originals.size()) return false;
    const auto& of = originals[e.file];
    const auto& wf = files[e.file];
    if (e.action < 0 || e.action >= (int)wf.actions.size()) return false;
    const auto& wa = wf.actions[e.action];

    // match the pristine action at the same original address
    size_t oa = SIZE_MAX;
    for (size_t i = 0; i < of.actions.size(); i++)
        if (of.actions[i].original_addr == wa.original_addr) { oa = i; break; }
    if (oa == SIZE_MAX && !wa.custom)
    {
        // Addresses shift after added/removed actions, so a reopened slot's
        // actions carry their CURRENT layout address. Fall back to structural
        // matching for original (non-custom) actions; custom actions have no
        // pristine counterpart by design.
        std::string sig = action_sig(wa, false);
        for (size_t i = 0; i < of.actions.size(); i++)
            if (action_sig(of.actions[i], false) == sig) { oa = i; break; }
    }
    if (oa == SIZE_MAX) return false;
    const auto& oact = of.actions[oa];
    if (e.chunk < 0 || e.chunk >= (int)oact.chunks.size()) return false;
    const auto& oc = oact.chunks[e.chunk];
    if (!oc.is_text) return false;

    DataChunk& c = files[e.file].actions[e.action].chunks[e.chunk];
    c.raw = oc.raw;
    c.text = oc.text;
    c.edited = false;
    c.edited_text.clear();
    return true;
}

std::vector<EncodingIssue> Project::validate() const
{
    std::vector<EncodingIssue> issues;
    for (const auto& e : entries())
    {
        const auto& c = files[e.file].actions[e.action].chunks[e.chunk];
        if (!c.edited) continue; // untouched original JP text is known-valid
        std::vector<uint8_t> enc;
        size_t badByte;
        uint32_t badCp;
        if (!sjis::encode(c.edited_text, enc, &badByte, &badCp))
        {
            EncodingIssue iss;
            iss.file = e.file;
            iss.action = e.action;
            iss.chunk = e.chunk;
            iss.kind = e.kind;
            iss.text = c.edited_text;
            iss.bad_cp = badCp;
            // convert UTF-8 byte offset to codepoint index
            size_t cps = 0, i = 0;
            while (i < badByte)
            {
                size_t adv;
                utf8::decode(c.edited_text.data() + i, badByte - i, &adv);
                i += adv > 0 ? adv : 1;
                cps++;
            }
            iss.bad_index = (int)cps;
            issues.push_back(iss);
        }
    }
    return issues;
}

bool Project::action_is_referenced(int fi, size_t ai) const
{
    const auto& f = files[fi];
    if (ai >= f.actions.size()) return false;
    uint32_t addr = f.actions[ai].original_addr;
    for (const auto& a : f.actions)
    {
        if (a.call && a.opcode == addr) return true;
        for (const auto& p : a.params)
            if (p.kind == ParamKind::ActionRef && p.value == addr) return true;
    }
    for (const auto& e : f.exports)
        if (e.target == addr) return true;
    return false;
}

// ---------- text dump ----------

namespace {

std::string dump_escape(const std::string& s)
{
    std::string out;
    for (char ch : s)
    {
        if (ch == '\r') out += "\\r";
        else if (ch == '\n') out += "\\n";
        else out += ch;
    }
    return out;
}

std::string dump_unescape(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            if (s[i + 1] == 'r') { out += '\r'; i++; continue; }
            if (s[i + 1] == 'n') { out += '\n'; i++; continue; }
        }
        out += s[i];
    }
    return out;
}

} // namespace

std::string dump_export(const Project& p, const std::string& scriptPath)
{
    std::ostringstream os;
    os << "# Dokuro-chan script translation dump\n";
    os << "# SCRIPT.UNI: " << scriptPath << "\n";
    os << "# This dump is tied to exactly that reference SCRIPT.UNI (it never changes).\n";
    os << "# Only edit the RU: lines. Do not add or remove @blocks; unknown blocks are ignored\n";
    os << "# on import, so extra lines are safe. Blank RU: keeps the Japanese text as-is.\n";
    os << "# Overflowing the dialogue box CRASHES the game - the tool will warn you,\n";
    os << "# but always test anything close to the limit in an emulator.\n";
    os << "\n";

    for (const auto& e : p.entries())
    {
        const auto& c = p.files[e.file].actions[e.action].chunks[e.chunk];
        os << "@" << e.file << "." << e.action << "." << e.chunk << " "
           << (e.kind == EntryKind::Dialogue ? "dialogue" : "nameplate") << "\n";
        os << "JP: " << dump_escape(c.text) << "\n";
        os << "RU: " << dump_escape(c.edited ? c.edited_text : "") << "\n";
        os << "\n";
    }
    return os.str();
}

void dump_import(Project& p, const std::string& text)
{
    // index entries by id for quick lookup
    std::map<std::tuple<int, int, int>, EntryRef> byId;
    for (const auto& e : p.entries())
        byId[std::make_tuple(e.file, e.action, e.chunk)] = e;

    std::tuple<int, int, int>* current = nullptr;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        // strip \r (CRLF tolerance)
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (!line.empty() && line[0] == '@')
        {
            size_t spaceIdx = line.find(' ');
            std::string idPart = (spaceIdx != std::string::npos)
                                     ? line.substr(1, spaceIdx - 1)
                                     : line.substr(1);
            // split on '.'
            size_t d1 = idPart.find('.');
            size_t d2 = (d1 != std::string::npos) ? idPart.find('.', d1 + 1) : std::string::npos;
            if (d1 != std::string::npos && d2 != std::string::npos)
            {
                try
                {
                    int fi = std::stoi(idPart.substr(0, d1));
                    int ai = std::stoi(idPart.substr(d1 + 1, d2 - d1 - 1));
                    int ci = std::stoi(idPart.substr(d2 + 1));
                    static std::tuple<int, int, int> cur;
                    cur = std::make_tuple(fi, ai, ci);
                    current = &cur;
                }
                catch (...)
                {
                    current = nullptr;
                }
            }
            else
            {
                current = nullptr;
            }
        }
        else if (line.rfind("RU:", 0) == 0 && current != nullptr)
        {
            std::string value = line.substr(3);
            // trim leading spaces only (mirrors TrimStart(' '))
            size_t n = 0;
            while (n < value.size() && value[n] == ' ') n++;
            value = value.substr(n);
            value = dump_unescape(value);
            auto it = byId.find(*current);
            if (it != byId.end())
            {
                std::string* vp = (value.empty()) ? nullptr : &value;
                p.set_translation(it->second, vp);
            }
        }
    }
}

} // namespace dokuro
