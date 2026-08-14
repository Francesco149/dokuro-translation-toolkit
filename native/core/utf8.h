// Minimal UTF-8 helpers — no dependencies, platform-neutral.
#pragma once
#include <cstdint>
#include <string>

namespace utf8 {

// Decode one codepoint at s[0..n). On success returns codepoint and sets *adv
// to the byte length (1..4). On invalid input returns (uint32_t)-1, *adv = 1
// (so callers can skip a byte and continue).
uint32_t decode(const char* s, size_t n, size_t* adv);

// Append codepoint as UTF-8.
void encode(uint32_t cp, std::string& out);

bool valid(const std::string& s);

// Codepoint count (invalid bytes count as one).
size_t cp_count(const std::string& s);

} // namespace utf8
