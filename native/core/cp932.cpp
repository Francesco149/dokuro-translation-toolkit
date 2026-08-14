#include "cp932.h"
#include "cp932_tables.h"
#include "utf8.h"
#include <algorithm>

namespace sjis {

size_t sjis_encoded_len(const std::string& utf8)
{
    std::vector<uint8_t> enc;
    size_t bad;
    uint32_t badcp;
    return sjis::encode(utf8, enc, &bad, &badcp) ? enc.size() : 0;
}

bool can_encode_cp(uint32_t cp)
{
    if (cp > 0xFFFF) return false;
    const Cp932EncEntry key = { (uint16_t)cp, 0 };
    const Cp932EncEntry* begin = kCp932EncodeTable;
    const Cp932EncEntry* end = begin + (sizeof(kCp932EncodeTable) / sizeof(kCp932EncodeTable[0]));
    const Cp932EncEntry* it = std::lower_bound(begin, end, key,
        [](const Cp932EncEntry& a, const Cp932EncEntry& b) { return a.ch < b.ch; });
    return it != end && it->ch == cp;
}

bool encode(const std::string& utf8, std::vector<uint8_t>& out,
            size_t* bad_byte, uint32_t* bad_cp)
{
    out.clear();
    size_t i = 0;
    while (i < utf8.size())
    {
        size_t adv;
        uint32_t cp = utf8::decode(utf8.data() + i, utf8.size() - i, &adv);
        if (cp == (uint32_t)-1)
        {
            if (bad_byte) *bad_byte = i;
            if (bad_cp) *bad_cp = 0xFFFD;
            return false;
        }
        if (!can_encode_cp(cp))
        {
            if (bad_byte) *bad_byte = i;
            if (bad_cp) *bad_cp = cp;
            return false;
        }
        if (cp < 0x100)
        {
            out.push_back((uint8_t)cp);
        }
        else
        {
            const Cp932EncEntry key = { (uint16_t)cp, 0 };
            const Cp932EncEntry* begin = kCp932EncodeTable;
            const Cp932EncEntry* end = begin + (sizeof(kCp932EncodeTable) / sizeof(kCp932EncodeTable[0]));
            const Cp932EncEntry* it = std::lower_bound(begin, end, key,
                [](const Cp932EncEntry& a, const Cp932EncEntry& b) { return a.ch < b.ch; });
            uint16_t code = it->code;
            out.push_back((uint8_t)(code >> 8));
            out.push_back((uint8_t)(code & 0xFF));
        }
        i += adv;
    }
    return true;
}

static bool decode_pair(uint8_t lead, uint8_t trail, uint32_t* cp)
{
    if (!((lead >= 0x81 && lead <= 0x9F) || (lead >= 0xE0 && lead <= 0xFC))) return false;
    if (!((trail >= 0x40 && trail <= 0x7E) || (trail >= 0x80 && trail <= 0xFC))) return false;
    uint16_t v = kCp932PairDecode[((size_t)lead << 8) | trail];
    if (v == 0) return false;
    *cp = v - 1;
    return true;
}

bool decode_strict(const uint8_t* b, size_t n, std::string& utf8out)
{
    utf8out.clear();
    size_t i = 0;
    while (i < n)
    {
        uint8_t c = b[i];
        if (c < 0x80)
        {
            utf8out += (char)c;
            i++;
            continue;
        }
        uint16_t sv = kCp932SingleDecode[c];
        if (sv != 0)
        {
            utf8::encode(sv - 1, utf8out);
            i++;
            continue;
        }
        if (i + 1 < n)
        {
            uint32_t cp;
            if (decode_pair(c, b[i + 1], &cp))
            {
                utf8::encode(cp, utf8out);
                i += 2;
                continue;
            }
        }
        return false;
    }
    return true;
}

std::string decode_lenient(const uint8_t* b, size_t n)
{
    std::string out;
    size_t i = 0;
    while (i < n)
    {
        uint8_t c = b[i];
        if (c < 0x80)
        {
            out += (char)c;
            i++;
            continue;
        }
        uint16_t sv = kCp932SingleDecode[c];
        if (sv != 0)
        {
            utf8::encode(sv - 1, out);
            i++;
            continue;
        }
        if (i + 1 < n)
        {
            uint32_t cp;
            if (decode_pair(c, b[i + 1], &cp))
            {
                utf8::encode(cp, out);
                i += 2;
                continue;
            }
        }
        out += '?';
        i++;
    }
    return out;
}

void decode_full(const uint8_t* b, size_t n, std::vector<Decoded>& out)
{
    out.clear();
    size_t i = 0;
    while (i < n)
    {
        Decoded d;
        d.cp = 0xFFFD;
        d.lossy = true;
        d.bytes[0] = b[i];
        d.nbytes = 1;
        uint8_t c = b[i];
        if (c < 0x80)
        {
            d.cp = c;
            d.lossy = false;
            i++;
        }
        else
        {
            uint16_t sv = kCp932SingleDecode[c];
            if (sv != 0)
            {
                d.cp = sv - 1;
                d.lossy = false;
                i++;
            }
            else if (i + 1 < n)
            {
                uint32_t cp;
                if (decode_pair(c, b[i + 1], &cp))
                {
                    d.cp = cp;
                    d.lossy = false;
                    d.bytes[1] = b[i + 1];
                    d.nbytes = 2;
                    i += 2;
                }
                else
                {
                    i++;
                }
            }
            else
            {
                i++;
            }
        }
        out.push_back(d);
    }
}

} // namespace sjis
