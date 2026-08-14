import pickle, struct

AT_NAME=0x0038
AT_LOW_PC=0x0111
AT_HIGH_PC=0x0121
TAG_GLOBAL_SUB=0x0006
TAG_SUBROUTINE=0x0014

entries = pickle.load(open("dwarf1_entries.pkl","rb"))

funcs = []
for e in entries:
    if e["tag"] in (TAG_GLOBAL_SUB, TAG_SUBROUTINE):
        a = e["attrs"]
        name = a.get(AT_NAME)
        low = a.get(AT_LOW_PC)
        high = a.get(AT_HIGH_PC)
        if name is not None:
            funcs.append((low, high, name, e["tag"]))

print(f"Functions with names: {len(funcs)}")
with_addr = [f for f in funcs if f[0] is not None]
print(f"Functions with low_pc: {len(with_addr)}")

# Sort by address
with_addr.sort(key=lambda f: f[0])
print("\nAddress range:", hex(with_addr[0][0]), "-", hex(with_addr[-1][0]))

import pickle as pk
pk.dump(funcs, open("funcs.pkl","wb"))

# Search font-related
print("\n=== Font-related functions ===")
for low, high, name, tag in with_addr:
    if 'font' in name.lower() or 'Font' in name:
        print(f"{hex(low) if low else '?':10s} - {hex(high) if high else '?':10s}  {name}")
