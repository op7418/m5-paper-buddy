#!/usr/bin/env bash
# Bootstrap the M5 Paper Buddy environment:
#   1. Verify PlatformIO + Python deps (pyserial, bleak).
#   2. Fix the mklittlefs CPU architecture mismatch on Apple Silicon.
#   3. Merge the hook config into ~/.claude/settings.json if not there.
#   4. Flash firmware + filesystem (CJK font) if a Paper is plugged in.
#   5. Start the daemon.
#
# Safe to re-run; every step is idempotent.

set -euo pipefail
SELF_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck source=common.sh
source "$SELF_DIR/common.sh"

echo "==> Checking PlatformIO..."
if ! have_pio; then
  echo "    PlatformIO not found. Install: brew install platformio" >&2
  exit 1
fi
echo "    ok ($(pio --version | head -1))"

need_python
echo "==> Python: $PY"

echo "==> Checking Python deps (pyserial, bleak)..."
if ! "$PY" -c "import serial" 2>/dev/null; then
  "$PY" -m pip install pyserial
fi
if ! "$PY" -c "import bleak" 2>/dev/null; then
  "$PY" -m pip install bleak
fi
echo "    ok"

# mklittlefs from PlatformIO ships as an x86_64 Mach-O. On Apple Silicon
# that fails with "Bad CPU type in executable" during `pio run -t uploadfs`.
# Homebrew has a native arm64 build; symlink it so PIO picks it up.
MKL="$HOME/.platformio/packages/tool-mklittlefs/mklittlefs"
if [ -e "$MKL" ] && file "$MKL" 2>/dev/null | grep -q "x86_64" && [ "$(uname -m)" = "arm64" ]; then
  echo "==> Patching mklittlefs (PIO ships x86_64, this Mac is arm64)..."
  if ! command -v mklittlefs >/dev/null 2>&1; then
    echo "    brew install mklittlefs ..."
    brew install mklittlefs
  fi
  mv "$MKL" "${MKL}.x86_64.bak" 2>/dev/null || true
  ln -sf "$(command -v mklittlefs)" "$MKL"
  echo "    ok"
fi

echo "==> Merging hooks into ~/.claude/settings.json..."
"$SELF_DIR/install-hooks.sh"

DEV="$(find_serial)"
if [ -n "$DEV" ]; then
  echo ""
  read -r -p "Flash firmware + font to $DEV now? [y/N] " yn
  if [[ "$yn" =~ ^[Yy]$ ]]; then
    "$SELF_DIR/flash.sh"
  fi
else
  echo ""
  echo "==> No M5Paper detected over USB."
  echo "    Plug it in and run: ${SELF_DIR}/flash.sh"
fi

echo ""
echo "==> Starting daemon..."
"$SELF_DIR/start.sh"

echo ""
echo "Done. Useful commands:"
echo "  $SELF_DIR/start.sh    — start daemon (idempotent)"
echo "  $SELF_DIR/stop.sh     — stop daemon"
echo "  $SELF_DIR/status.sh   — show daemon + device status"
echo "  $SELF_DIR/flash.sh    — (re)flash firmware + filesystem (font)"
echo "  tail -f $LOG_FILE"
