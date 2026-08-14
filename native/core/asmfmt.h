// STCM2 <-> human-editable text format for the direct script editor tab.
//
// Round-trip contract: disasm() of any parseable Stcm2File, fed back through
// assemble(), reproduces the SAME model bytes (and therefore the same
// serialize() output). Text chunks whose content cannot be losslessly
// re-encoded (e.g. 4-byte text with no trailing zero padding) are emitted as
// `chunk text_raw` so nothing is ever silently corrupted.
//
// Format (one statement per line, ';' comments, case-sensitive keywords):
//   .stcm2 "<tag27>"                    required first
//   .unk1 <hex>  .collection <hex>  .unk32 <hex>x8     (header fields)
//   .global_data / .bytes <hex>... / .end              (opaque blob)
//   .code_start
//   action @<hex>                       address is display-only
//     call 0|1
//     opcode <hex>            (call=0)
//     target <hex>            (call=1: target action address)
//     param action_ref|data_ptr|global_ptr|value <hex>
//     data
//       chunk text "<escaped>"          type-0 text; content = sjis(text)+pad
//       chunk text_raw <hex>...         type-0 text kept as exact bytes
//       chunk num <hex>                 type-1 u32
//       chunk val <hex>                 type-0 u32 (numeric 4-byte slot)
//       chunk raw <hex>...              type-0 unclassified bytes
//       bytes <hex>...                  raw data bytes (leading junk / unparsed)
//     end_action
//   exports
//     export "<name32>" @<hex>
//   .end
// All <hex> accept "0x" prefix or bare hex. Strings escape \" \\ \n \r \t \xNN.
#pragma once
#include "stcm2.h"
#include <string>

namespace dokuro {

struct AsmError
{
    int line = 0; // 1-based; 0 = not line-specific
    std::string msg;
};

// Renders file_index'th slot as asm text. Never fails.
std::string asm_disasm(const Stcm2File& f, int file_index);

// Parses asm text into a fresh Stcm2File. On failure returns false and fills
// err (first error only). Never throws (except bad_alloc).
bool asm_assemble(const std::string& text, int file_index, Stcm2File& out, AsmError& err);

} // namespace dokuro
