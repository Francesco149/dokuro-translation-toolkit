import struct

path = "/home/claude/work/game-files/SLPM_661.85"
data = open(path, "rb").read()

DEBUG_OFF = 0x265940
DEBUG_SIZE = 0x13af52
DEBUG_END = DEBUG_OFF + DEBUG_SIZE

def u16(off): return struct.unpack_from("<H", data, off)[0]
def u32(off): return struct.unpack_from("<I", data, off)[0]

TAG_NAMES = {
    0x0000:"padding",0x0001:"array_type",0x0002:"class_type",0x0003:"entry_point",
    0x0004:"enumeration_type",0x0005:"formal_parameter",0x0006:"global_subroutine",
    0x0007:"global_variable",0x000a:"label",0x000b:"lexical_block",0x000c:"local_variable",
    0x000d:"member",0x000f:"pointer_type",0x0010:"reference_type",0x0011:"compile_unit",
    0x0012:"string_type",0x0013:"structure_type",0x0014:"subroutine",0x0015:"subroutine_type",
    0x0016:"typedef",0x0017:"union_type",0x0018:"unspecified_parameters",0x0019:"variant",
    0x001a:"common_block",0x001b:"common_inclusion",0x001c:"inheritance",0x001d:"inlined_subroutine",
    0x001e:"module",0x001f:"ptr_to_member_type",0x0020:"set_type",0x0021:"subrange_type",
    0x0022:"with_stmt",0x8000:"format_label",0x8001:"namelist",0x8002:"function_template",
    0x8003:"class_template",
}
AT_NAMES = {
    0x0012:"sibling", 0x0023:"location", 0x0038:"name", 0x0052:"fund_type",
    0x0063:"mod_fund_type", 0x0072:"user_def_type", 0x0083:"mod_u_d_type",
    0x0095:"ordering", 0x00a3:"subscr_data", 0x00b6:"byte_size", 0x00c5:"bit_offset",
    0x00d6:"bit_size", 0x00f4:"element_list", 0x0106:"stmt_list", 0x0111:"low_pc",
    0x0121:"high_pc", 0x0136:"language", 0x0142:"member", 0x0152:"discr",
    0x0163:"discr_value", 0x0193:"string_length", 0x01a2:"common_reference",
    0x01b8:"comp_dir", 0x01c8:"const_value_string", 0x01c5:"const_value_data2",
    0x01c6:"const_value_data4", 0x01c7:"const_value_data8", 0x01c3:"const_value_block2",
    0x01c4:"const_value_block4", 0x01d2:"containing_type", 0x01e1:"default_value_addr",
    0x01f3:"friends", 0x0208:"inline", 0x0218:"is_optional", 0x0222:"lower_bound_ref",
    0x0248:"private", 0x0258:"producer", 0x0238:"program", 0x0268:"protected",
    0x0278:"prototyped", 0x0288:"public", 0x0298:"pure_virtual", 0x02a3:"return_addr",
    0x02b2:"abstract_origin", 0x02c6:"start_scope", 0x02e6:"stride_size",
    0x02f2:"upper_bound_ref", 0x0308:"virtual",
    0x8006:"sf_names", 0x8016:"src_info", 0x8026:"mac_info", 0x8036:"src_coords",
    0x8041:"body_begin", 0x8051:"body_end",
}

def parse_die(off):
    if off+4 > DEBUG_END:
        return None
    length = u32(off)
    if length == 0:
        return None
    if length < 4:
        return {"tag":None,"length":length,"attrs":{},"off":off,"raw_tag":None}
    if length == 4:
        return {"tag":None,"length":4,"attrs":{},"off":off,"raw_tag":None}
    tag = u16(off+4)
    attrs = {}
    p = off+6
    endp = off+length
    while p+2 <= endp:
        attr = u16(p); p += 2
        form = attr & 0xF
        try:
            if form==1:
                val = u32(p); p+=4
            elif form==2:
                val = u32(p); p+=4
            elif form==3:
                blen = u16(p); p+=2; val = data[p:p+blen]; p+=blen
            elif form==4:
                blen = u32(p); p+=4; val = data[p:p+blen]; p+=blen
            elif form==5:
                val = u16(p); p+=2
            elif form==6:
                val = u32(p); p+=4
            elif form==7:
                val = data[p:p+8]; p+=8
            elif form==8:
                zpos = data.index(b'\x00', p, endp+64)
                val = data[p:zpos].decode('latin1', errors='replace'); p = zpos+1
            else:
                break
        except Exception:
            break
        attrs[attr] = val
    return {"tag":tag,"length":length,"attrs":attrs,"off":off,"raw_tag":tag}

# Walk the whole .debug section linearly
entries = []
off = DEBUG_OFF
tag_counts = {}
while off < DEBUG_END - 4:
    d = parse_die(off)
    if d is None:
        break
    entries.append(d)
    tag_counts[d["tag"]] = tag_counts.get(d["tag"],0)+1
    off = d["off"] + max(d["length"],4)

print(f"Total DIEs parsed: {len(entries)}")
print("Tag histogram (top 20):")
for t,c in sorted(tag_counts.items(), key=lambda x:-x[1])[:20]:
    name = TAG_NAMES.get(t, f"unk_{t:#x}" if t is not None else "NULL/terminator")
    print(f"  {name:25s} {c}")

import pickle
with open("dwarf1_entries.pkl","wb") as f:
    pickle.dump(entries, f)
print("saved", len(entries), "entries")
