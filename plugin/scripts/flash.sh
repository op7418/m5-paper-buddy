#!/usr/bin/env bash
# Build + upload the Paper firmware AND filesystem (CJK font).
# Uploads the filesystem first (font), then firmware. Both are required
# for a fully working install — the firmware fails to render non-ASCII
# if the font isn't on LittleFS.
#
# Usage:
#   flash.sh                 # autodetect serial port
#   flash.sh /dev/cu.XYZ     # explicit port

set -euo pipefail
SELF_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck source=common.sh
source "$SELF_DIR/common.sh"

if ! have_pio; then
  echo "error: PlatformIO not found. brew install platformio" >&2
  exit 1
fi

PORT="${1:-}"
if [ -z "$PORT" ]; then PORT="$(find_serial)"; fi
if [ -z "$PORT" ]; then
  echo "error: no serial device found. plug in the Paper and retry." >&2
  exit 1
fi

cd "$REPO_ROOT"

echo "==> 1/2 Uploading filesystem (CJK font, ~3.4MB, takes ~90s)..."
pio run -e m5paper -t uploadfs --upload-port "$PORT"

echo ""
echo "==> 2/2 Uploading firmware to $PORT..."
pio run -e m5paper -t upload --upload-port "$PORT"

echo ""
echo "==> Done."
