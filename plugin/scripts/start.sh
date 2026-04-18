#!/usr/bin/env bash
# Start the bridge daemon in the background. Idempotent: if already
# running, do nothing. Uses $STATE_DIR/daemon.pid + daemon.log.
#
# Config via env vars:
#   BUDDY_TRANSPORT   auto|serial|ble (default: auto)
#   BUDDY_BUDGET      daily token budget for the Paper (default: 0 = hide)
#   BUDDY_OWNER       override $USER as the displayed owner name
#   BUDDY_HTTP_PORT   HTTP listener port (default: 9876)

set -euo pipefail
SELF_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck source=common.sh
source "$SELF_DIR/common.sh"

if is_running; then
  echo "daemon already running (pid $(cat "$PID_FILE"))"
  exit 0
fi

need_python

TRANSPORT="${BUDDY_TRANSPORT:-auto}"
# Context-window limit for the progress bar. Default 200K = Claude 4.6
# standard context; set BUDDY_BUDGET=1000000 for 1M beta, or 0 to hide.
BUDGET="${BUDDY_BUDGET:-200000}"
OWNER="${BUDDY_OWNER:-$USER}"
HTTP_PORT="${BUDDY_HTTP_PORT:-9876}"

nohup "$PY" "$DAEMON" \
  --transport "$TRANSPORT" \
  --budget "$BUDGET" \
  --owner "$OWNER" \
  --http-port "$HTTP_PORT" \
  >> "$LOG_FILE" 2>&1 &
echo $! > "$PID_FILE"

sleep 0.5
if is_running; then
  echo "daemon started (pid $(cat "$PID_FILE"))  transport=$TRANSPORT"
  echo "log: $LOG_FILE"
else
  echo "daemon failed to start — check $LOG_FILE"
  tail -n 20 "$LOG_FILE" || true
  exit 1
fi
