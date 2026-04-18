---
description: Start the Claude Buddy bridge daemon (background).
---

Starts the `claude_code_bridge.py` daemon in the background. Writes its
PID to `~/.claude-buddy/daemon.pid` and logs to `~/.claude-buddy/daemon.log`.
Idempotent — re-running while already up is a no-op.

Transport defaults to `auto` (tries USB serial first, falls back to BLE).
Override via env: `BUDDY_TRANSPORT=ble BUDDY_BUDGET=1000000`.

!`bash "$CLAUDE_PLUGIN_ROOT/scripts/start.sh"`
