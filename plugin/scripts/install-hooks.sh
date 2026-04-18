#!/usr/bin/env bash
# Merge our hooks block into ~/.claude/settings.json.
#
# We only add entries that aren't already there — we never clobber the
# user's existing hooks. Each hook's command includes a marker string
# ("127.0.0.1:9876/hook") so we can detect "already installed" reliably.

set -euo pipefail
SELF_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck source=common.sh
source "$SELF_DIR/common.sh"

SETTINGS="$HOME/.claude/settings.json"
PLUGIN_HOOKS="$PLUGIN_DIR/settings/hooks.json"
MARKER="127.0.0.1:9876/hook"

mkdir -p "$(dirname "$SETTINGS")"
[ -f "$SETTINGS" ] || echo '{}' > "$SETTINGS"

if grep -q "$MARKER" "$SETTINGS" 2>/dev/null; then
  echo "    hooks already present in $SETTINGS — skipping"
  exit 0
fi

# Back up before mutating.
cp "$SETTINGS" "$SETTINGS.buddy-backup-$(date +%s)"

need_python
"$PY" - "$SETTINGS" "$PLUGIN_HOOKS" <<'PY'
import json, sys
settings_path, hooks_path = sys.argv[1], sys.argv[2]
with open(settings_path) as f:
    settings = json.load(f)
with open(hooks_path) as f:
    plugin = json.load(f)

hooks = settings.setdefault("hooks", {})
for event, entries in plugin.get("hooks", {}).items():
    bucket = hooks.setdefault(event, [])
    # Append our entry (bucket already dedup-checked via marker grep above).
    bucket.extend(entries)

with open(settings_path, "w") as f:
    json.dump(settings, f, indent=2)
print("    hooks merged into", settings_path)
PY
