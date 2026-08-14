import struct
import os

# Path to the real game file, resolved relative to this script's location so it
# works regardless of where the repo is checked out.
SCRIPT_UNI_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "game-files", "SCRIPT.UNI"
)

def u32(b, off):
    return struct.unpack_from('<I', b, off)[0]

GDO = 92  # GlobalDataOffset

def parse_stcm2(blob):
    assert blob[:5] == b'STCM2'
    pos = 5
    tag = blob[pos:pos+27]; pos += 27
    export_addr = u32(blob, pos); pos += 4
    export_len = u32(blob, pos); pos += 4
    unk1 = u32(blob, pos); pos += 4
    coll = u32(blob, pos); pos += 4
    unk32 = blob[pos:pos+32]; pos += 32
    assert blob[pos:pos+12] == b'GLOBAL_DATA\x00'
    pos += 12
    assert pos == GDO
    gpos = pos
    while blob[gpos:gpos+12] != b'CODE_START_\x00':
        gpos += 4
    global_data_len = gpos - pos
    pos = gpos + 12

    actions = []
    export_entries_start = export_addr - 12

    while pos < export_entries_start:
        addr = pos
        global_call = u32(blob, pos); pos += 4
        opcode = u32(blob, pos); pos += 4
        nparams = u32(blob, pos); pos += 4
        length = u32(blob, pos); pos += 4
        params = []
        for i in range(nparams):
            a = u32(blob, pos); b = u32(blob, pos+4); c = u32(blob, pos+8)
            pos += 12
            params.append((a,b,c))
        data_start = pos
        data_len = length - 16 - 12*nparams
        pos += data_len
        actions.append(dict(addr=addr, opcode=opcode, call=bool(global_call), nparams=nparams,
                             length=length, params=params, data_start=data_start, data_len=data_len))

    assert blob[pos:pos+12] == b'EXPORT_DATA\x00'
    pos += 12
    exports = []
    for i in range(export_len):
        zero = u32(blob, pos); pos += 4
        assert zero == 0
        name = blob[pos:pos+32]; pos += 32
        target_addr = u32(blob, pos); pos += 4
        exports.append(dict(name=name, target_addr=target_addr))

    total_len = pos
    return dict(tag=tag, export_addr=export_addr, export_len=export_len,
                actions=actions, exports=exports, global_data_len=global_data_len,
                total_len=total_len)


def split_uni(path):
    data = open(path, 'rb').read()
    assert data[:4] == b'UNI2'
    count = u32(data, 8)
    blobs = []
    pos = 0x1000
    for i in range(count):
        assert data[pos:pos+5] == b'STCM2', (i, pos)
        info = parse_stcm2(data[pos:pos+(len(data)-pos)])
        blob_len = info['total_len']
        blobs.append(data[pos:pos+blob_len])
        padded = ((blob_len + 0x7ff)//0x800)*0x800
        pos += padded
    return blobs


def action_addr_set(info):
    return set(a['addr'] for a in info['actions'])


def classify_param(a,b,c, data_addr, data_len, gdlen):
    bcTag = b in (0x40000000, 0xff000000) and c in (0x40000000, 0xff000000)
    if a == 0xffffff41 and c in (0x40000000, 0xff000000):
        return ('ActionRef', b)
    if bcTag and data_addr <= a < data_addr + data_len:
        return ('DataPointer', a - data_addr)
    if bcTag and GDO <= a < GDO + gdlen:
        return ('GlobalDataPointer', a - GDO)
    if bcTag:
        return ('Value', a)
    return ('Unknown', (a,b,c))


def main():
    blobs = split_uni(SCRIPT_UNI_PATH)
    print(f'{len(blobs)} blobs parsed')
    total_actionrefs = 0
    total_exports = 0
    total_calls = 0
    bad_actionrefs = []
    bad_exports = []
    bad_calls = []
    for idx, blob in enumerate(blobs):
        info = parse_stcm2(blob)
        addrs = action_addr_set(info)
        gdlen = info['global_data_len']
        for a in info['actions']:
            data_addr = a['addr'] + 16 + 12*a['nparams']
            if a['call']:
                total_calls += 1
                if a['opcode'] not in addrs:
                    bad_calls.append((idx, a['addr'], a['opcode']))
            for (pa,pb,pc) in a['params']:
                kind, val = classify_param(pa,pb,pc, data_addr, a['data_len'], gdlen)
                if kind == 'ActionRef':
                    total_actionrefs += 1
                    if val not in addrs:
                        bad_actionrefs.append((idx, a['addr'], val))
        for e in info['exports']:
            total_exports += 1
            if e['target_addr'] not in addrs:
                bad_exports.append((idx, e['name'], e['target_addr']))

    print('total actionrefs:', total_actionrefs, 'bad:', len(bad_actionrefs))
    print('total exports:', total_exports, 'bad:', len(bad_exports))
    print('total call-actions:', total_calls, 'bad:', len(bad_calls))
    if bad_actionrefs:
        print('sample bad actionrefs:', bad_actionrefs[:10])
    if bad_exports:
        print('sample bad exports:', bad_exports[:10])
    if bad_calls:
        print('sample bad calls:', bad_calls[:10])

if __name__ == '__main__':
    main()
