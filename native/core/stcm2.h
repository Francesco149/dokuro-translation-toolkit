// STCM2 script bytecode model — exact port of dotnet/DokuroScript.Core/Stcm2.cs.
// See docs/FORMAT.md §2 for the byte-level spec. All strings are UTF-8.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dokuro {

struct Stcm2Error : std::runtime_error
{
    explicit Stcm2Error(const std::string& msg) : std::runtime_error(msg) {}
};

// ---------- low level ----------

uint32_t read_u32le(const uint8_t* b, size_t len, size_t off); // throws Stcm2Error
void write_u32le(std::vector<uint8_t>& out, uint32_t v);
void write_bytes(std::vector<uint8_t>& out, const uint8_t* b, size_t n);

// ---------- data model ----------

enum class ParamKind { ActionRef, DataPointer, Value, GlobalDataPointer };

// One action parameter (12 bytes on disk: 3x u32 LE). Value holds the ORIGINAL
// address/offset at parse time; DataPointer's Value is already relative to the
// owning action's data blob. ActionRef's Value is the target action's ORIGINAL
// address and is remapped through the file's address table when re-serializing.
struct Param
{
    ParamKind kind = ParamKind::Value;
    uint32_t value = 0;

    static Param parse(const uint32_t triple[3], uint32_t dataAddr,
                       uint32_t dataLen, uint32_t globalLen); // throws Stcm2Error
    // Encodes back to the 3x u32 on-disk form. resolve maps an ORIGINAL action
    // address to its (possibly new) address.
    void encode(uint32_t dataAddr, uint32_t out[3],
                const std::function<uint32_t(uint32_t)>& resolve) const;
};

// One self-describing "boxed value" inside an action's data blob. is_text =
// real editable string content; everything else is an opaque number we must
// NOT touch. When !edited, write() reproduces the ORIGINAL bytes verbatim so
// an unmodified file round-trips byte-for-byte.
struct DataChunk
{
    int offset = 0;               // offset within the owning action's Data
    uint32_t type = 0;            // 0 or 1 (on-disk "type" field)
    bool is_text = false;
    std::vector<uint8_t> raw;     // content bytes (post 16-byte header)
    uint32_t numeric = 0;         // valid when !is_text
    std::string text;             // decoded text when is_text (UTF-8)
    bool edited = false;          // EditedText != null
    std::string edited_text;      // UTF-8

    int on_disk_length() const;
    void write(std::vector<uint8_t>& out) const;
};

// One action. When unparsed is set, unparsed_data is written back verbatim and
// chunks/leading_junk are ignored (mirrors Stcm2.cs's UnparsedData fallback).
struct Action
{
    uint32_t original_addr = 0;
    bool call = false;
    uint32_t opcode = 0;          // if call: ORIGINAL address of target action
    std::vector<Param> params;
    std::vector<uint8_t> leading_junk;
    std::vector<DataChunk> chunks;
    bool unparsed = false;
    std::vector<uint8_t> unparsed_data;
    uint32_t new_addr = 0;        // filled in during serialize()
    bool custom = false;          // added by the tool (not in the pristine script)

    std::vector<uint8_t> build_data() const;
};

struct ExportEntry
{
    std::array<uint8_t, 32> name{};
    uint32_t target = 0;          // ORIGINAL action address
};

// One STCM2 file (= one slot in SCRIPT.UNI).
struct Stcm2File
{
    static constexpr uint32_t kMagicLen = 5;
    static constexpr uint32_t kTagLen = 27;
    static constexpr uint32_t kGlobalDataOffset = 92; // 5+27+12*4+12

    std::array<uint8_t, kTagLen> tag{};
    uint32_t unk1 = 0;
    uint32_t collection_addr = 0;
    std::array<uint8_t, 32> unk32{};
    std::vector<uint8_t> global_data;
    std::vector<Action> actions;      // in on-disk order
    std::vector<ExportEntry> exports; // in on-disk order

    static Stcm2File parse(const uint8_t* data, size_t len); // throws Stcm2Error
    std::vector<uint8_t> serialize() const;                  // throws Stcm2Error

    // Standalone action round-trip (used by the undo log for insert/delete).
    static Action parse_action(const uint8_t* b, size_t len, uint32_t addr,
                               uint32_t globalLen); // throws Stcm2Error
    static std::vector<uint8_t> serialize_action(const Action& a, uint32_t addr);
};

} // namespace dokuro
