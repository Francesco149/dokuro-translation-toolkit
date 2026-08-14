#include "utf8.h"

namespace utf8 {

uint32_t decode(const char* s, size_t n, size_t* adv)
{
    if (n == 0) { *adv = 0; return (uint32_t)-1; }
    const uint8_t* p = (const uint8_t*)s;
    uint8_t b0 = p[0];
    if (b0 < 0x80) { *adv = 1; return b0; }
    int len;
    uint32_t cp;
    uint32_t min;
    if ((b0 & 0xE0) == 0xC0) { len = 2; cp = b0 & 0x1F; min = 0x80; }
    else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = b0 & 0x0F; min = 0x800; }
    else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = b0 & 0x07; min = 0x10000; }
    else { *adv = 1; return (uint32_t)-1; }
    if (n < (size_t)len) { *adv = 1; return (uint32_t)-1; }
    for (int i = 1; i < len; i++)
    {
        uint8_t b = p[i];
        if ((b & 0xC0) != 0x80) { *adv = 1; return (uint32_t)-1; }
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
    {
        *adv = 1;
        return (uint32_t)-1;
    }
    *adv = (size_t)len;
    return cp;
}

void encode(uint32_t cp, std::string& out)
{
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800)
    {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

bool valid(const std::string& s)
{
    size_t i = 0;
    while (i < s.size())
    {
        size_t adv;
        uint32_t cp = decode(s.data() + i, s.size() - i, &adv);
        if (cp == (uint32_t)-1 || adv == 0) return false;
        i += adv;
    }
    return true;
}

size_t cp_count(const std::string& s)
{
    size_t count = 0, i = 0;
    while (i < s.size())
    {
        size_t adv;
        decode(s.data() + i, s.size() - i, &adv);
        i += adv > 0 ? adv : 1;
        count++;
    }
    return count;
}

} // namespace utf8
