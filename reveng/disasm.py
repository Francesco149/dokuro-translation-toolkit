# DEPRECATED / DO NOT USE FOR ANYTHING TRUSTWORTHY.
#
# capstone's generic MIPS engine misdecodes this binary's PS2-EE-specific instructions
# (MMI ops, 128-bit sq/lq, daddu, etc.) as bogus MIPS-DSP-ASE opcodes — SILENTLY WRONG output,
# not just missing/(bad). See reveng/README.md's "Setup" section for the full explanation and
# the correct tool. This file is kept only because it was already checked in; a session in
# 2026-08 almost used it again before catching the warning in the README (the warning lives
# there, not here, which is exactly how that almost happened) — don't repeat that.
#
# Use `./od.sh <lo> <hi> [label]` instead (wraps `mipsel-linux-gnu-objdump -d -m mips:5900`),
# or `apt-get install -y binutils-mipsel-linux-gnu` if od.sh reports the binary missing — that
# package has been observed to NOT persist between tool calls in at least one sandbox session,
# so if it's ever missing again, just reinstall, don't assume something's more broken than it is.
#
# (Also note: the hardcoded path below is stale — this repo's binary lives at
# game-files/SLPM_661.85 relative to the repo root, not /home/claude/work/game-files/. Left
# as-is since this file shouldn't be run at all.)
import capstone

data = open("/home/claude/work/game-files/SLPM_661.85","rb").read()
VADDR_BASE=0x100000
FILE_OFF_BASE=0x80
def v2o(v): return v - VADDR_BASE + FILE_OFF_BASE

md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
md.skipdata = True
def _skip4(*args):
    return 4
md.skipdata_setup = (".word", _skip4, None)
md.detail = True

def disasm_range(lo, hi, label=""):
    print(f"\n=== {label}  [{hex(lo)} - {hex(hi)}]  size={hi-lo} ===")
    off = v2o(lo)
    code = data[off: off + (hi-lo)]
    for insn in md.disasm(code, lo):
        if insn.mnemonic == "(bad)" or insn.mnemonic == ".byte":
            print(f"  {insn.address:#010x}:  <data/unknown: {insn.bytes.hex()}>")
        else:
            print(f"  {insn.address:#010x}:  {insn.mnemonic:8s} {insn.op_str}")

if __name__ == "__main__":
    import sys
    lo = int(sys.argv[1], 16)
    hi = int(sys.argv[2], 16)
    label = sys.argv[3] if len(sys.argv)>3 else ""
    disasm_range(lo, hi, label)
