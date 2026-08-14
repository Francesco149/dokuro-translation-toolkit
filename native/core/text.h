// Text-level helper: Sanitize (port of Stcm2Text.Sanitize). See FORMAT.md §3a.
//
// NOTE: the old overflow width heuristic (OverflowChecker) was removed — the
// verified crash constraint is the per-chunk size budget (Thread B), not
// dialogue-box width. See docs/FONT_AND_CRASH_INVESTIGATION.md.
#pragma once
#include <cstdint>
#include <string>

namespace dokuro {

// Normalizes "smart punctuation" that looks right but isn't in cp932 so a
// translator never has to know the difference. Port of Stcm2Text.Sanitize;
// NOTE the U+FF5E case was FIXED vs the original C#: cp932 maps 0x8160 to
// U+FF5E, so U+FF5E is kept as-is (the old U+FF5E->U+301C mapping produced a
// character cp932 cannot encode). UTF-8 in/out.
std::string sanitize(const std::string& input);

} // namespace dokuro
