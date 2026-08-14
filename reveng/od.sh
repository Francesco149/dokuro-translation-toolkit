#!/bin/bash
LO=$1
HI=$2
BIN="${SLPM_BIN:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../game-files" && pwd)/SLPM_661.85}"
mipsel-linux-gnu-objdump -d --start-address=$LO --stop-address=$HI -m mips:5900 -M no-aliases "$BIN" 2>&1 | sed -n '/Disassembly/,$p' | tail -n +2
