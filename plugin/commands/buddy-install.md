---
description: First-time setup for the M5 Paper Buddy — checks PlatformIO, installs Python deps, patches mklittlefs, merges hooks, offers to flash firmware + font, starts the daemon.
---

Run the full install. Safe to re-run; every step is idempotent.

On Apple Silicon the install also patches PlatformIO's x86_64
`mklittlefs` binary (needed to upload the CJK font to LittleFS) by
`brew install mklittlefs` + symlink.

!`bash "$CLAUDE_PLUGIN_ROOT/scripts/install.sh"`
