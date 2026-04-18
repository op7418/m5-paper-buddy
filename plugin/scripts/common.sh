#!/usr/bin/env bash
# Shared helpers for the buddy plugin scripts.
#
# Path conventions:
#   REPO_ROOT  — the checked-out claude-desktop-buddy repo
#   STATE_DIR  — $HOME/.claude-buddy  (pid file, log)
#   DAEMON     — tools/claude_code_bridge.py
#   PY         — a Python that has pyserial (+ bleak for --transport ble)
# The plugin ships as part of the repo so REPO_ROOT is the repo containing
# this file's parent directories.

set -euo pipefail

PLUGIN_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
REPO_ROOT="$( cd "$PLUGIN_DIR/.." && pwd )"
STATE_DIR="$HOME/.claude-buddy"
PID_FILE="$STATE_DIR/daemon.pid"
LOG_FILE="$STATE_DIR/daemon.log"
DAEMON="$REPO_ROOT/tools/claude_code_bridge.py"

mkdir -p "$STATE_DIR"

# Pick a Python. Preference order:
#   1. $BUDDY_PYTHON (user override)
#   2. PlatformIO's embedded Python — always has pyserial, and after
#      `pip install bleak` has BLE too. Used while the repo is the only
#      install surface for the daemon.
#   3. plain python3 on PATH.
pick_python() {
  if [ -n "${BUDDY_PYTHON:-}" ] && [ -x "${BUDDY_PYTHON}" ]; then
    echo "$BUDDY_PYTHON"; return
  fi
  local pio_py="/opt/homebrew/Cellar/platformio/*/libexec/bin/python"
  for p in $pio_py; do
    if [ -x "$p" ]; then echo "$p"; return; fi
  done
  # Linux brew path
  local linux_pio_py="/home/linuxbrew/.linuxbrew/Cellar/platformio/*/libexec/bin/python"
  for p in $linux_pio_py; do
    if [ -x "$p" ]; then echo "$p"; return; fi
  done
  if command -v python3 >/dev/null 2>&1; then echo "$(command -v python3)"; return; fi
  echo ""; return 1
}

PY="$(pick_python || true)"

need_python() {
  if [ -z "$PY" ]; then
    echo "error: no suitable Python found." >&2
    echo "install PlatformIO (brew install platformio) or set BUDDY_PYTHON." >&2
    exit 1
  fi
}

have_pio() { command -v pio >/dev/null 2>&1; }

find_serial() {
  # Echo the first matching USB serial device, or empty if none.
  ls /dev/cu.usbserial-* /dev/ttyUSB* 2>/dev/null | head -n1 || true
}

is_running() {
  [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null
}
