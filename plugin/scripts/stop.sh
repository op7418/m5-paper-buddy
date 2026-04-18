#!/usr/bin/env bash
set -euo pipefail
SELF_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck source=common.sh
source "$SELF_DIR/common.sh"

if ! is_running; then
  echo "daemon not running"
  rm -f "$PID_FILE"
  exit 0
fi

PID="$(cat "$PID_FILE")"
echo "stopping daemon (pid $PID)..."
kill "$PID" 2>/dev/null || true
# Give it a moment; SIGKILL as last resort.
for _ in 1 2 3 4 5; do
  if ! kill -0 "$PID" 2>/dev/null; then break; fi
  sleep 0.3
done
if kill -0 "$PID" 2>/dev/null; then
  kill -9 "$PID" 2>/dev/null || true
fi
rm -f "$PID_FILE"
echo "stopped."
