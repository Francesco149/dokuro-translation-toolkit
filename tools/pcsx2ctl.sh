#!/usr/bin/env bash
# pcsx2ctl.sh — durable launcher/status for the PCSX2-MCP emulator stack.
#
# Topology (WSL2 NAT):
#   Windows: pcsx2-qt.exe (MCP build, DebugServer on 127.0.0.1:21512)
#            + win/debugserver-relay.ps1 (0.0.0.0:21512 -> 127.0.0.1:21512)
#   WSL:     tools/ps2dbg.py -> <windows host ip>:21512
#
# Usage:
#   tools/pcsx2ctl.sh status   # emulator up? relay up? debugger reachable?
#   tools/pcsx2ctl.sh start    # start relay + emulator with the ISO
#   tools/pcsx2ctl.sh stop     # quit PCSX2 (graceful)
#
# Override paths via env: PCSX2_EXE, PCSX2_ISO, RELAY_PS1.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PCSX2_EXE="${PCSX2_EXE:-/mnt/c/Users/headpats/Documents/dokuro/PCSX2-MCP-v1.0.0-win64/pcsx2-qt.exe}"
PCSX2_ISO="${PCSX2_ISO:-/mnt/c/Users/headpats/Documents/PCSX2/iso/Game ni Natta yo! Dokuro-chan - Kenkou Shindan Daisakusen (Japan).iso}"
RELAY_PS1="${RELAY_PS1:-/mnt/c/Users/headpats/Documents/dokuro/dokuro-translation-toolkit/win/debugserver-relay.ps1}"
RELAY_PORT="${RELAY_PORT:-21512}"

# Windows host IP from WSL's perspective (NAT default gateway)
WIN_HOST="$(ip route | awk '/^default via /{print $3; exit}')"
WIN_HOST="${WIN_HOST:-127.0.0.1}"

port_open() { # host port
  timeout 2 bash -c "echo > /dev/tcp/$1/$2" 2>/dev/null
}

emulator_running() {
  powershell.exe -NoProfile -Command "Get-Process pcsx2-qt -ErrorAction SilentlyContinue" 2>/dev/null | grep -q pcsx2-qt
}

status() {
  echo "win_host=$WIN_HOST"
  if emulator_running; then echo "emulator: running"; else echo "emulator: stopped"; fi
  if port_open "$WIN_HOST" "$RELAY_PORT"; then
    echo "debugserver: reachable at ${WIN_HOST}:${RELAY_PORT}"
  else
    echo "debugserver: NOT reachable"
  fi
  if port_open "$WIN_HOST" 28011; then
    echo "pine: reachable at ${WIN_HOST}:28011"
  else
    echo "pine: NOT reachable"
  fi
}

start_relay() {
  if port_open "$WIN_HOST" "$RELAY_PORT"; then
    echo "relay already up"
    return
  fi
  # wslpath -w on a /mnt or /opt path yields \\wsl.localhost\... UNC, which
  # Windows PowerShell can read (cmd.exe cannot). Keep the script in the repo
  # so both sides always run the same copy.
  win_relay="$(wslpath -w "$RELAY_PS1")"
  echo "starting relay ($win_relay -> ${WIN_HOST}:${RELAY_PORT})"
  powershell.exe -NoProfile -Command "Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','$win_relay'" >/dev/null
  for _ in $(seq 1 15); do
    port_open "$WIN_HOST" "$RELAY_PORT" && { echo "relay up"; return; }
    sleep 1
  done
  echo "relay failed to come up" >&2
  exit 1
}

start() {
  start_relay
  if emulator_running; then
    echo "emulator already running"
  else
    win_iso="$(wslpath -w "$PCSX2_ISO")"
    win_exe="$(wslpath -w "$PCSX2_EXE")"
    echo "starting pcsx2 with $win_iso"
    powershell.exe -NoProfile -Command "Start-Process '$win_exe' -ArgumentList '\"$win_iso\"'" >/dev/null
  fi
  for _ in $(seq 1 60); do
    if port_open "$WIN_HOST" "$RELAY_PORT"; then
      echo "PCSX2 DebugServer is up"
      status
      return
    fi
    sleep 2
  done
  echo "PCSX2 DebugServer never came up (still booting?)" >&2
  exit 1
}

stop() {
  if emulator_running; then
    powershell.exe -NoProfile -Command "Stop-Process -Name pcsx2-qt" >/dev/null
    echo "pcsx2-qt stopped"
  else
    echo "emulator not running"
  fi
}

case "${1:-status}" in
  status) status ;;
  start) start ;;
  stop) stop ;;
  *) echo "usage: $0 {status|start|stop}" >&2; exit 1 ;;
esac
