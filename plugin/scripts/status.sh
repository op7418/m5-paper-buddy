#!/usr/bin/env bash
# At-a-glance view of daemon + device + settings state.

set -euo pipefail
SELF_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck source=common.sh
source "$SELF_DIR/common.sh"

echo "--- Claude Buddy status ---"

if is_running; then
  echo "daemon: running (pid $(cat "$PID_FILE"))"
else
  echo "daemon: stopped"
fi

DEV="$(find_serial)"
if [ -n "$DEV" ]; then
  echo "serial: $DEV"
else
  echo "serial: (none plugged in — daemon may be using BLE)"
fi

# Hook config presence
SETTINGS="$HOME/.claude/settings.json"
if [ -f "$SETTINGS" ] && grep -q "127.0.0.1:9876/hook" "$SETTINGS" 2>/dev/null; then
  echo "hooks:  installed in $SETTINGS"
else
  echo "hooks:  NOT installed — run $SELF_DIR/install-hooks.sh"
fi

# Last few log lines for quick debugging
if [ -f "$LOG_FILE" ]; then
  echo ""
  echo "--- last 5 log lines ---"
  tail -n 5 "$LOG_FILE"
fi
