#include "text.h"
#include "utf8.h"

namespace dokuro {

std::string sanitize(const std::string& input)
{
    std::string out;
    out.reserve(input.size());
    size_t i = 0;
    while (i < input.size())
    {
        size_t adv;
        uint32_t cp = utf8::decode(input.data() + i, input.size() - i, &adv);
        if (cp == (uint32_t)-1)
        {
            // invalid UTF-8: copy the byte through; the encode step reports it
            out += input[i];
            i += 1;
            continue;
        }
        i += adv;
        switch (cp)
        {
        case '\r':
        case '\n':
            // Script text is single-line dialogue — the game's text boxes
            // render one line and newlines are not representable. Pasting or
            // typing them becomes a space (never let them reach the script).
            out += ' ';
            break;
        case 0x00A0: // NO-BREAK SPACE
        case 0x202F: // NARROW NO-BREAK SPACE
            out += ' ';
            break;
        case 0x200B: // ZERO WIDTH SPACE
        case 0xFEFF: // BOM / ZERO WIDTH NO-BREAK SPACE
        case 0x00AD: // SOFT HYPHEN
            break; // drop entirely, invisible either way
        case 0x2013: // EN DASH
        case 0x2014: // EM DASH
        case 0x2212: // MINUS SIGN
            utf8::encode(0x2015, out); // JIS X0208 horizontal bar
            break;
        case 0xFF5E: // FULLWIDTH TILDE
            // Kept as-is: cp932 maps 0x8160 to U+FF5E. (The old C# mapping to
            // U+301C produced an unencodable character — see text.h.)
            utf8::encode(0xFF5E, out);
            break;
        case 0x201E: // DOUBLE LOW-9 QUOTATION MARK
        case 0x00AB: // LEFT GUILLEMET
        case 0x00BB: // RIGHT GUILLEMET
            out += '"';
            break;
        case 0x2022: // BULLET
            utf8::encode(0x30FB, out); // katakana middle dot
            break;
        default:
            utf8::encode(cp, out);
            break;
        }
    }
    return out;
}

} // namespace dokuro
