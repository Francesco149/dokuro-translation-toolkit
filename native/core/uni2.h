// UNI2 container (no-TOC flavor) — port of Uni2.cs. See docs/FORMAT.md §1a.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dokuro {

// Splits a SCRIPT.UNI into the leading header + one blob per STCM2 slot.
// Throws Stcm2Error on malformed input.
void uni2_split(const uint8_t* data, size_t len, std::vector<uint8_t>& header,
                std::vector<std::vector<uint8_t>>& slots);

// Rejoins header + slots, padding each slot to the 2048-byte sector boundary,
// and REBUILDS SCRIPT.UNI's embedded per-chunk TOC at file offset 0x800
// ({id, off_sectors, sector_len, size_round16}) from the actual slot layout —
// the game copies this table into its runtime registry at boot and uses the
// size field as the disc-read length, so grown/edited chunks MUST get fresh
// entries or they are read truncated (Thread B crash). See FORMAT.md §1a.
std::vector<uint8_t> uni2_join(const std::vector<uint8_t>& header,
                               const std::vector<std::vector<uint8_t>>& slots);

std::vector<uint8_t> read_file(const std::string& path);       // throws Stcm2Error
void write_file(const std::string& path, const uint8_t* data, size_t len);
void write_file(const std::string& path, const std::vector<uint8_t>& data);
bool file_exists(const std::string& path);
uint64_t file_size(const std::string& path);

} // namespace dokuro
