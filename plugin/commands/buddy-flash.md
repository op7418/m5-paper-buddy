---
description: Build + flash M5Paper firmware AND filesystem (CJK font). Stops + restarts the daemon around the flash.
---

Runs `pio run -t uploadfs` (filesystem, ~90s) then `pio run -t upload`
(firmware, ~30s). Both are needed: the firmware can't render non-ASCII
without the font on LittleFS.

!`bash -c 'bash "$CLAUDE_PLUGIN_ROOT/scripts/stop.sh" || true; bash "$CLAUDE_PLUGIN_ROOT/scripts/flash.sh"; bash "$CLAUDE_PLUGIN_ROOT/scripts/start.sh"'`
