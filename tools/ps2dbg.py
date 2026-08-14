#!/usr/bin/env python3
"""ps2dbg.py — direct TCP client for PCSX2's DebugServer (hkmodd/pcsx2-mcp build).

The DebugServer protocol is newline-delimited JSON over TCP port 21512
(see source/DebugServer.cpp in the PCSX2-MCP release). This is a stdlib-only
client so it runs anywhere (WSL2, plain Linux) without node/npm.

Network: PCSX2 runs on the Windows host and binds 127.0.0.1 only. Reach it
through win/debugserver-relay.ps1 (0.0.0.0 -> loopback) and point --host at
the Windows host IP (WSL default gateway). Host auto-detected from `ip route`
when --host is omitted.

Examples:
  ps2dbg.py status
  ps2dbg.py regs gpr                  # all GPRs
  ps2dbg.py read_mem 0x1c2f820 64
  ps2dbg.py write_mem 0x1653b0 3f10053c
  ps2dbg.py disasm 0x1573b8 12
  ps2dbg.py bp 0x1573b8 --cond 'a1 == 0x7424' --desc 'chunk5 load'
  ps2dbg.py resume && sleep 2 && ps2dbg.py bps
  ps2dbg.py memcheck 0x1c2f820 --type write --action break
  ps2dbg.py step_over
  ps2dbg.py bt 40
  ps2dbg.py shot frame.png          # GS framebuffer readback (patched build)
  ps2dbg.py set_pad --buttons start --hold-ms 300   # press Start 300ms
  ps2dbg.py set_pad --buttons circle --hold-ms 200
  ps2dbg.py get_pad
"""

import argparse
import base64
import binascii
import json
import socket
import struct
import subprocess
import sys
import time
import zlib

DEFAULT_PORT = 21512


def win_host_ip():
    """Windows host = default gateway in WSL2 NAT mode."""
    try:
        out = subprocess.run(
            ["ip", "route"], capture_output=True, text=True, timeout=5
        ).stdout
        for line in out.splitlines():
            if line.startswith("default via "):
                return line.split()[2]
    except Exception:
        pass
    return "127.0.0.1"


class Ps2Dbg:
    def __init__(self, host, port, timeout=10):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)

    def cmd(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode())
        resp = b""
        while not resp.endswith(b"\n"):
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("DebugServer closed connection")
            resp += chunk
        return json.loads(resp.decode())

    def close(self):
        self.sock.close()


def dump_regs(resp):
    data = resp.get("data", {})
    print(f"pc={data.get('pc')} hi={data.get('hi')} lo={data.get('lo')}")
    for cat, cdata in data.items():
        if not isinstance(cdata, dict) or "regs" not in cdata:
            continue
        print(f"[{cat}]")
        for r in cdata["regs"]:
            print(f"  {r['name']:<6} {r['value']:<40} {r['display']}")


def write_png(path, width, height, bgra_bytes):
    """Minimal stdlib PNG encoder (BGRA -> RGB, filter 0 rows)."""
    raw = b""
    stride = width * 3
    for y in range(height):
        row = bgra_bytes[y * width * 4:(y + 1) * width * 4]
        raw += b"\x00" + b"".join(
            bytes((row[i + 2], row[i + 1], row[i])) for i in range(0, len(row), 4)
        )
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", binascii.crc32(tag + data) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)
    return len(png)


def pine_cmd(host, port, payload):
    """Pine IPC: [4-byte LE size incl. itself][payload...]; resp [size][0x00=ok].
    Linux PINE listens on a unix socket at $XDG_RUNTIME_DIR/pcsx2.sock;
    Windows uses TCP 127.0.0.1:28011."""
    import os
    sock_path = os.environ.get("XDG_RUNTIME_DIR", "/run/user/1000") + "/pcsx2.sock"
    s = None
    if os.path.exists(sock_path):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect(sock_path)
    else:
        s = socket.create_connection((host, port), timeout=5)
    with s:
        s.sendall(struct.pack("<I", len(payload) + 4) + payload)
        hdr = b""
        while len(hdr) < 4:
            chunk = s.recv(4 - len(hdr))
            if not chunk:
                raise ConnectionError("pine closed")
            hdr += chunk
        total = struct.unpack("<I", hdr)[0]
        resp = b""
        while len(resp) < total - 4:
            chunk = s.recv(total - 4 - len(resp))
            if not chunk:
                raise ConnectionError("pine closed")
            resp += chunk
        if resp and resp[0] != 0:
            raise RuntimeError(f"pine error code 0x{resp[0]:02x}")
        return resp


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=None, help="Windows host IP (auto-detect)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--cpu", default="ee", choices=["ee", "iop"],
                    help="CPU target (default ee)")
    ap.add_argument("--json", action="store_true", help="print raw JSON response")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status")
    sub.add_parser("pause")
    sub.add_parser("resume")
    sub.add_parser("step")
    sub.add_parser("step_over")
    sub.add_parser("threads")
    sub.add_parser("modules")
    sub.add_parser("clrbps")

    p = sub.add_parser("regs"); p.add_argument("category", nargs="?", default="-1")
    p = sub.add_parser("wreg")
    p.add_argument("category", type=int); p.add_argument("index", type=int)
    p.add_argument("value", help="hex or number")
    p = sub.add_parser("setpc"); p.add_argument("value")
    p = sub.add_parser("read_mem"); p.add_argument("address"); p.add_argument("length", type=lambda s: int(s, 0), default=256)
    p = sub.add_parser("write_mem"); p.add_argument("address"); p.add_argument("data", help="hex string")
    p = sub.add_parser("read_str"); p.add_argument("address"); p.add_argument("max_length", type=int, default=256)
    p = sub.add_parser("disasm"); p.add_argument("address"); p.add_argument("count", type=int, default=20)
    p.add_argument("--no-simplify", action="store_true")
    p = sub.add_parser("eval"); p.add_argument("expression")
    p = sub.add_parser("bp")
    p.add_argument("address"); p.add_argument("--cond", default="")
    p.add_argument("--desc", default=""); p.add_argument("--temp", action="store_true")
    p = sub.add_parser("rmbp"); p.add_argument("address")
    sub.add_parser("bps")
    p = sub.add_parser("memcheck")
    p.add_argument("address"); p.add_argument("end", nargs="?", default=None)
    p.add_argument("--type", default="write", choices=["write", "read", "access", "onchange"])
    p.add_argument("--action", default="break", choices=["break", "log", "both"])
    p.add_argument("--desc", default="")
    p = sub.add_parser("rmmemcheck"); p.add_argument("address"); p.add_argument("end", nargs="?", default=None)
    sub.add_parser("mcs")
    p = sub.add_parser("bt"); p.add_argument("max_frames", type=int, default=32)
    p = sub.add_parser("isvalid"); p.add_argument("address")
    p = sub.add_parser("shot"); p.add_argument("out", nargs="?", default="shot.png")
    p = sub.add_parser("set_pad")
    p.add_argument("--pad", type=int, default=0)
    p.add_argument("--buttons", default="", help="comma list: start,circle,cross,...")
    p.add_argument("--mask", default=None, help="active-low hex mask")
    p.add_argument("--release", default="", help="comma list to release")
    p.add_argument("--hold-ms", type=int, default=0)
    p = sub.add_parser("clear_pad"); p.add_argument("--pad", type=int, default=0)
    sub.add_parser("get_pad")
    p = sub.add_parser("savestate"); p.add_argument("slot", type=int, nargs="?", default=0)
    p.add_argument("--pine-port", type=int, default=28011)
    p = sub.add_parser("loadstate"); p.add_argument("slot", type=int, nargs="?", default=0)
    p.add_argument("--pine-port", type=int, default=28011)

    a = ap.parse_args()

    host = a.host
    if host is None:
        # Prefer a local (WSL-side) patched emulator; fall back to the
        # Windows host (MCP build via relay) only if localhost refuses.
        host = "127.0.0.1"
        try:
            with socket.create_connection((host, a.port), timeout=2):
                pass
        except OSError:
            host = win_host_ip()
    dbg = Ps2Dbg(host, a.port)

    def C(obj):
        obj["cpu"] = a.cpu
        return dbg.cmd(obj)

    try:
        if a.cmd == "status":
            r = C({"cmd": "status"})
            d = r.get("data", {})
            print(f"ok={r.get('ok')} alive={d.get('alive')} paused={d.get('paused')} "
                  f"pc={d.get('pc')} cycles={d.get('cycles')}")
        elif a.cmd == "read_mem":
            r = C({"cmd": "read_memory", "address": int(a.address, 0), "length": a.length})
            print(r.get("hex", ""))
        elif a.cmd == "write_mem":
            r = C({"cmd": "write_memory", "address": int(a.address, 0), "data": a.data})
            print(f"written={r.get('written')} ok={r.get('ok')}")
        elif a.cmd == "read_str":
            r = C({"cmd": "read_string", "address": int(a.address, 0), "max_length": a.max_length})
            print(f"len={r.get('length')} string={r.get('string')!r}")
        elif a.cmd == "disasm":
            r = C({"cmd": "disassemble", "address": int(a.address, 0),
                   "count": a.count, "simplify": not a.no_simplify})
            for ins in r.get("instructions", []):
                print(f"{ins['address']}: {ins['disasm']}")
        elif a.cmd == "eval":
            r = C({"cmd": "evaluate", "expression": a.expression})
            print(f"{a.expression} = {r.get('result', r.get('error'))} ({r.get('hex', '')})")
        elif a.cmd == "bp":
            r = C({"cmd": "set_breakpoint", "address": int(a.address, 0),
                   "condition": a.cond, "description": a.desc, "temporary": a.temp})
            print(f"bp at {r.get('address')} ok={r.get('ok')}")
        elif a.cmd == "rmbp":
            r = C({"cmd": "remove_breakpoint", "address": int(a.address, 0)})
            print(f"removed ok={r.get('ok')}")
        elif a.cmd == "bps":
            r = C({"cmd": "list_breakpoints"})
            for b in r.get("breakpoints", []):
                print(f"{b['address']} enabled={b['enabled']} temp={b['temporary']} "
                      f"cond={b.get('condition', '')} desc={b.get('description', '')}")
        elif a.cmd == "memcheck":
            end = int(a.end, 0) if a.end else int(a.address, 0) + 4
            r = C({"cmd": "set_memcheck", "address": int(a.address, 0), "end": end,
                   "type": a.type, "action": a.action, "description": a.desc})
            print(f"memcheck {r.get('start')}-{r.get('end')} ok={r.get('ok')}")
        elif a.cmd == "rmmemcheck":
            end = int(a.end, 0) if a.end else int(a.address, 0) + 4
            r = C({"cmd": "remove_memcheck", "address": int(a.address, 0), "end": end})
            print(f"removed ok={r.get('ok')}")
        elif a.cmd == "mcs":
            r = C({"cmd": "list_memchecks"})
            for m in r.get("memchecks", []):
                print(f"{m['start']}-{m['end']} hits={m['hits']} last_pc={m['last_pc']} "
                      f"last_addr={m['last_addr']} desc={m.get('description', '')}")
        elif a.cmd == "pause":
            r = C({"cmd": "pause"}); print(f"paused at pc={r.get('pc')}")
        elif a.cmd == "resume":
            r = C({"cmd": "resume"}); print(f"resumed ok={r.get('ok')}")
        elif a.cmd == "step":
            r = C({"cmd": "step"})
            print(f"{r.get('old_pc')} -> {r.get('new_pc')}: {r.get('disasm')}")
        elif a.cmd == "step_over":
            r = C({"cmd": "step_over"})
            print(f"{r.get('old_pc')} -> {r.get('new_pc')}: {r.get('disasm')}")
        elif a.cmd == "threads":
            r = C({"cmd": "get_threads"})
            for t in r.get("threads", []):
                print(f"tid={t['id']} pc={t['pc']} status={t['status']} wait={t['wait_type']}")
        elif a.cmd == "modules":
            r = C({"cmd": "get_modules"})
            for m in r.get("modules", []):
                print(f"{m['name']} v{m['version']}")
        elif a.cmd == "bt":
            r = C({"cmd": "get_backtrace", "max_frames": a.max_frames})
            for f in r.get("frames", []):
                print(f"entry={f['entry']} pc={f['pc']} sp={f['sp']} "
                      f"stack={f['stack_size']}  {f['disasm']}")
        elif a.cmd == "regs":
            cat = -1 if a.category == "-1" else int(a.category, 0)
            r = C({"cmd": "read_registers", "category": cat})
            if a.json:
                print(json.dumps(r))
            else:
                dump_regs(r)
        elif a.cmd == "wreg":
            r = C({"cmd": "write_register", "category": a.category,
                   "index": a.index, "value": a.value})
            print(f"ok={r.get('ok')}")
        elif a.cmd == "setpc":
            r = C({"cmd": "set_pc", "value": int(a.value, 0)})
            print(f"pc={r.get('pc')}")
        elif a.cmd == "clrbps":
            r = C({"cmd": "clear_breakpoints"}); print(f"ok={r.get('ok')}")
        elif a.cmd == "isvalid":
            r = C({"cmd": "is_valid_address", "address": int(a.address, 0)})
            print(f"valid={r.get('valid')}")
        elif a.cmd == "shot":
            r = C({"cmd": "screenshot"})
            if not r.get("ok"):
                print(f"screenshot failed: {r.get('error')}", file=sys.stderr)
                sys.exit(1)
            w, h = r["width"], r["height"]
            raw = base64.b64decode(r["base64"])
            n = write_png(a.out, w, h, raw)
            print(f"saved {a.out} ({w}x{h}, {n} bytes)")
        elif a.cmd == "set_pad":
            obj = {"cmd": "set_pad", "pad": a.pad}
            if a.mask is not None:
                obj["mask"] = int(a.mask, 0)
            if a.buttons:
                obj["buttons"] = a.buttons
            if a.release:
                obj["release"] = a.release
            if a.hold_ms:
                obj["hold_ms"] = a.hold_ms
            r = C(obj)
            print(f"pad{a.pad} mask={r.get('mask')} ok={r.get('ok')}")
        elif a.cmd == "clear_pad":
            r = C({"cmd": "clear_pad", "pad": a.pad})
            print(f"pad{a.pad} cleared ok={r.get('ok')}")
        elif a.cmd == "get_pad":
            r = C({"cmd": "get_pad"})
            for m in r.get("masks", []):
                print(f"pad{m['pad']} mask={m['mask']}")
        elif a.cmd in ("savestate", "loadstate"):
            # Pine lives on the same host as the DebugServer.
            if a.host:
                h = a.host
            else:
                h = "127.0.0.1"
                try:
                    with socket.create_connection((h, a.port), timeout=2):
                        pass
                except OSError:
                    h = win_host_ip()
            op = 9 if a.cmd == "savestate" else 10
            pine_cmd(h, a.pine_port, bytes([op, a.slot & 0xFF]))
            print(f"slot {a.slot} {'saved' if a.cmd == 'savestate' else 'loaded'}")
        else:
            r = C({"cmd": a.cmd})
            print(json.dumps(r) if a.json else r)
    finally:
        dbg.close()


if __name__ == "__main__":
    main()
