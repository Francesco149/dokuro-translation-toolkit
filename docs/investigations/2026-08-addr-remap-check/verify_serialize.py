import struct
from verify_addrs import u32, GDO, parse_stcm2, split_uni, classify_param, action_addr_set, SCRIPT_UNI_PATH

def encode_sjis(s):
    return s.encode('shift_jis')

def build_action_data(action, edits):
    # edits: dict offset-in-original-data -> new text (str) or None
    # We rebuild the data blob by walking chunks in original order, using ORIGINAL bytes
    # for untouched chunks and re-encoding edited ones, exactly mirroring DataChunk.Write.
    out = bytearray()
    for ch in action['chunks']:
        if ch['offset'] in edits and edits[ch['offset']] is not None:
            enc = encode_sjis(edits[ch['offset']])
            pad = 4 - (len(enc) % 4)
            if pad == 0: pad = 4
            length = len(enc) + pad
            out += struct.pack('<III', 0, length//4, 1)
            out += struct.pack('<I', length)
            out += enc
            out += b'\x00' * pad
        else:
            raw = ch['raw']
            out += struct.pack('<III', ch['type_'], len(raw)//4, 1)
            out += struct.pack('<I', len(raw))
            out += raw
    return bytes(out)


def reparse_chunks(blob_data, data_start, data_len):
    """Re-scan chunks in an action's data region exactly like ScanDataChunks, capturing
    raw bytes + type + text, so build_action_data can reproduce them verbatim."""
    data = blob_data[data_start:data_start+data_len]
    chunks = []
    pos = 0
    while pos < len(data):
        if len(data) - pos <= 16:
            break
        type_ = u32(data, pos)
        if type_ not in (0, 1):
            break
        qlen = u32(data, pos+4)
        magic1 = u32(data, pos+8)
        if magic1 != 1:
            break
        length = u32(data, pos+12)
        if length != qlen*4:
            break
        if pos + 16 + length > len(data):
            break
        raw = data[pos+16:pos+16+length]
        is_text = False
        text = None
        if type_ == 1:
            if length != 4:
                break
        elif length == 4:
            n = u32(raw, 0)
            if n <= 0xFFFFFF:
                nzero = 0
                for b in reversed(raw):
                    if b == 0: nzero += 1
                    else: break
                trimmed = raw[:len(raw)-nzero]
                if len(trimmed) >= 3 and not (len(trimmed)==2 and trimmed==b'op'):
                    try:
                        s = trimmed.decode('shift_jis')
                        if not any(c.isprintable()==False for c in s):  # rough control check
                            is_text = True
                            text = s
                    except Exception:
                        pass
        else:
            nzero = 0
            for b in reversed(raw):
                if b == 0: nzero += 1
                else: break
            if 1 <= nzero <= 4:
                trimmed = raw[:len(raw)-nzero]
                try:
                    text = trimmed.decode('shift_jis')
                    is_text = True
                except Exception:
                    pass
        chunks.append(dict(offset=pos, type_=type_, raw=raw, is_text=is_text, text=text))
        pos += 16 + length
    return chunks


def serialize_with_edit(info, blob, target_action_addr, new_text, edit_all=False):
    """Mirror Stcm2File.Serialize(). If edit_all, rewrites EVERY text chunk in EVERY
    action to new_text (stress case); otherwise only the first text chunk of the
    action at target_action_addr. Returns new blob bytes."""
    actions = info['actions']
    for a in actions:
        a['chunks'] = reparse_chunks(blob, a['data_start'], a['data_len'])

    edits_by_addr = {}
    if edit_all:
        for a in actions:
            for ch in a['chunks']:
                if ch['is_text']:
                    a.setdefault('_edits', {})[ch['offset']] = new_text
    else:
        for a in actions:
            if a['addr'] == target_action_addr:
                for ch in a['chunks']:
                    if ch['is_text']:
                        edits_by_addr[ch['offset']] = new_text
                        break
                break

    # Pass 1: compute new data + lengths + addresses (mirrors Serialize pass 1)
    pos = GDO + info['global_data_len'] + 12  # CODE_START_ len
    addr_map = {}
    built = []
    for a in actions:
        if edit_all:
            edits = a.get('_edits', {})
        else:
            edits = edits_by_addr if a['addr'] == target_action_addr else {}
        data = build_action_data(a, edits)
        nparams = a['nparams']
        length = 16 + 12*nparams + len(data)
        addr_map[a['addr']] = pos
        built.append((a, data, pos, length))
        pos += length

    def resolve(orig_addr):
        return addr_map[orig_addr]

    out = bytearray()
    out += b'STCM2'
    out += blob[5:5+27]  # tag verbatim
    header_fixup_pos = len(out)
    out += struct.pack('<II', 0, 0)  # export_addr, export_len placeholder
    out += struct.pack('<I', 0)  # unk1 -- will patch below from original
    out += struct.pack('<I', 0)  # collection_addr placeholder
    out += bytes(32)  # unk32 placeholder
    # patch unk1/collection_addr/unk32 from original blob (offsets 36,40,44)
    orig_unk1 = blob[36:40]
    orig_coll = blob[40:44]
    orig_unk32 = blob[44:76]
    out[36:40] = orig_unk1
    out[40:44] = orig_coll
    out[44:76] = orig_unk32
    out += b'GLOBAL_DATA\x00'
    gd_start = GDO
    out += blob[gd_start:gd_start+info['global_data_len']]
    out += b'CODE_START_\x00'

    for (a, data, new_addr, length) in built:
        data_addr = new_addr + 16 + 12*a['nparams']
        out += struct.pack('<I', 1 if a['call'] else 0)
        out += struct.pack('<I', resolve(a['opcode']) if a['call'] else a['opcode'])
        out += struct.pack('<I', a['nparams'])
        out += struct.pack('<I', length)
        for (pa, pb, pc) in a['params']:
            kind, val = classify_param(pa, pb, pc, a['addr']+16+12*a['nparams'], a['data_len'], info['global_data_len'])
            if kind == 'ActionRef':
                out += struct.pack('<III', 0xffffff41, resolve(val), 0xff000000)
            elif kind == 'DataPointer':
                out += struct.pack('<III', data_addr + val, 0xff000000, 0xff000000)
            elif kind == 'GlobalDataPointer':
                out += struct.pack('<III', GDO + val, 0xff000000, 0xff000000)
            else:
                out += struct.pack('<III', val, 0xff000000, 0xff000000)
        out += data

    export_entries_start = len(out) + 12
    out += b'EXPORT_DATA\x00'
    for e in info['exports']:
        out += struct.pack('<I', 0)
        out += e['name']
        out += struct.pack('<I', resolve(e['target_addr']))

    final = bytearray(out)
    struct.pack_into('<I', final, header_fixup_pos, export_entries_start)
    struct.pack_into('<I', final, header_fixup_pos+4, len(info['exports']))
    return bytes(final)


def find_action_with_text(info):
    for a in info['actions']:
        chunks = reparse_chunks(bytes(), 0, 0)  # placeholder, unused
    return None


def check_one(info, newblob, label_prefix):
    info2 = parse_stcm2(newblob)
    addrs2 = action_addr_set(info2)
    orig_order = [x['addr'] for x in info['actions']]
    new_order = [x['addr'] for x in info2['actions']]
    orig_index = {addr: i for i, addr in enumerate(orig_order)}
    new_addr_by_index = {i: addr for i, addr in enumerate(new_order)}
    bad = []
    for orig_e, new_e in zip(info['exports'], info2['exports']):
        orig_target = orig_e['target_addr']
        if orig_target not in orig_index:
            continue
        oi = orig_index[orig_target]
        expected_new_addr = new_addr_by_index[oi]
        if new_e['target_addr'] != expected_new_addr:
            bad.append(('export', orig_e['name'][:16], expected_new_addr, new_e['target_addr']))
    for oi2, (orig_a, new_a) in enumerate(zip(info['actions'], info2['actions'])):
        if orig_a['call']:
            ot = orig_a['opcode']
            if ot in orig_index:
                expected = new_addr_by_index[orig_index[ot]]
                if new_a['opcode'] != expected:
                    bad.append(('call', oi2, expected, new_a['opcode']))
        orig_data_addr = orig_a['addr']+16+12*orig_a['nparams']
        for pi, ((pa,pb,pc), (na,nb,nc)) in enumerate(zip(orig_a['params'], new_a['params'])):
            kind, val = classify_param(pa,pb,pc, orig_data_addr, orig_a['data_len'], info['global_data_len'])
            if kind == 'ActionRef' and val in orig_index:
                expected = new_addr_by_index[orig_index[val]]
                if nb != expected:
                    bad.append(('actionref', oi2, pi, expected, nb))
    if bad:
        print(f"{label_prefix}: MISMATCHES: {bad[:5]}")
        return False
    return True


def main():
    blobs = split_uni(SCRIPT_UNI_PATH)
    total_tests = 0
    fail_count = 0
    for idx, blob in enumerate(blobs):
        info = parse_stcm2(blob)
        tested_this_blob = 0
        for a in info['actions']:
            if a['opcode'] != 0x118:
                continue
            if tested_this_blob >= 6:
                break
            chunks = reparse_chunks(blob, a['data_start'], a['data_len'])
            text_chunks = [c for c in chunks if c['is_text']]
            if not text_chunks:
                continue
            tested_this_blob += 1
            # try lengths around the 47/48 SJIS byte boundary
            for testlen, label in [(47, '47byte'), (48, '48byte')]:
                nchars = testlen // 2
                new_text = ('Ж' * nchars)
                actual_bytes = len(new_text.encode('shift_jis'))
                newblob = serialize_with_edit(info, blob, a['addr'], new_text)
                total_tests += 1
                ok = check_one(info, newblob, f"blob {idx} action {a['addr']:#x} len={label}({actual_bytes}B)")
                if not ok:
                    fail_count += 1
        # also stress-test: rewrite EVERY text chunk in the WHOLE blob to exactly 48 SJIS bytes
        new_text_48 = 'Ж' * 24
        newblob_all = serialize_with_edit(info, blob, None, new_text_48, edit_all=True)
        total_tests += 1
        ok = check_one(info, newblob_all, f"blob {idx} EDIT-ALL-CHUNKS len=48B")
        if not ok:
            fail_count += 1
    print(f"done. total_tests={total_tests} fail_count={fail_count}")


if __name__ == '__main__':
    main()
