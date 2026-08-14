// Shift-JIS / Windows cp932 codec — byte-exact with .NET Encoding.GetEncoding(932)
// (see tools/gen_cp932 — the tables are generated FROM .NET, which is the
// reference behavior of DokuroScript.Core/Stcm2Text.cs).
//
// All string APIs are UTF-8 in/out. cp932 here = Windows-31J: byte 0x8160 maps
// to U+FF5E (FULLWIDTH TILDE), and U+301C (WAVE DASH) is NOT encodable — this
// matches .NET exactly and matters for Sanitize() (see text.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sjis {

// Strict encode: every codepoint of utf8 must be encodable. On success fills
// out and returns true. On failure sets *bad_byte (UTF-8 byte offset of the
// offending codepoint) and *bad_cp (the codepoint) and returns false.
bool encode(const std::string& utf8, std::vector<uint8_t>& out,
            size_t* bad_byte, uint32_t* bad_cp);

// True iff the single codepoint can be encoded to cp932.
bool can_encode_cp(uint32_t cp);

// Encoded byte length (or 0 when unencodable).
size_t sjis_encoded_len(const std::string& utf8);

// Strict decode: every byte must be a valid cp932 sequence. Returns false on
// any invalid byte (nothing is appended for the invalid part).
bool decode_strict(const uint8_t* b, size_t n, std::string& utf8out);

// Lenient decode (for display): invalid bytes become '?' — same replacement
// behavior as .NET's lenient cp932 decoder.
std::string decode_lenient(const uint8_t* b, size_t n);

// Full decode for round-trip-sensitive consumers (asm text emission): one
// entry per decoded unit. lossy=true means the source byte(s) could not be
// decoded as cp932 and cp holds U+FFFD; bytes are the original source bytes.
struct Decoded {
    uint32_t cp;
    bool lossy;
    uint8_t bytes[2];
    int nbytes;
};
void decode_full(const uint8_t* b, size_t n, std::vector<Decoded>& out);

} // namespace sjis
