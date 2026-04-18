# m5-paper-buddy

A physical Claude Code companion running on an **M5Paper V1.1** (4.7"
e-ink, 540×960, GT911 touch, ESP32). The Paper sits on your desk and
mirrors every Claude Code session you have open: project, branch,
current model, context window usage, recent activity, Claude's latest
reply. When Claude wants to run a tool, the full command / diff /
content shows up as a full-screen approval card — **PUSH** to approve,
**DOWN** to deny. `AskUserQuestion` renders its options as four big
tappable buttons.

Delivered as a Claude Code plugin — one `/buddy-install` and you're
wired up.

## What it does

| | |
| --- | --- |
| **Dashboard** | Per-session project + branch + dirty count; current Claude model; today's context-window usage with a progress bar; recent activity log; Claude's latest prose reply. |
| **Multi-session** | Left column lists every active Claude Code window. Tap a row to focus the dashboard on that session's project / activity / reply. |
| **Approval FIFO** | Permission prompts queue and pop one at a time. Deny the current, next auto-appears. DND mode (long-press **UP**) auto-approves — good for batch-y tasks. |
| **Touch questions** | Claude's `AskUserQuestion` shows as up to 4 big tap targets; tapping sends the chosen label back as the answer. |
| **Settings page** | Tap **SETTINGS** (top-right) for transport / battery / DND / context / uptime / language toggle (English ↔ 中文). |
| **CJK** | 3.4 MB TTF lives on LittleFS; all labels, prompts, Claude replies, user messages render via FreeType with UTF-8-aware wrapping. |
| **Two transports** | USB serial (default, zero setup) or BLE with macOS passkey pairing. Automatic fallback. |
| **Cat buddy** | A tiny ASCII cat in the footer changes expression with state: idle / busy / attention / celebrate / DND / sleep. |

## Repo layout

```
src/
  ble_bridge.cpp/h    — Nordic UART service, line-buffered TX/RX
  stats.h             — NVS-backed stats / settings / owner name
  paper/
    main.cpp          — UI, state machine, touch, settings, i18n
    data_paper.h      — TamaState + JSON parsing (UTF-8 safe)
    xfer_paper.h      — status responses, name/owner/unpair cmds
    buddy_frames.h    — ASCII cat frames (6 states)
data/cjk.ttf          — CJK font flashed via `pio run -t uploadfs`
partitions-m5paper.csv — 3 MB app + 13 MB LittleFS so the font fits
platformio.ini
plugin/               — Claude Code plugin (manifest + commands + scripts)
tools/claude_code_bridge.py — the bridge daemon (hook HTTP + serial/BLE)
```

## Install

Prereqs: [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
Homebrew (Apple Silicon only, for the native `mklittlefs`), and a USB-C
cable to the Paper.

**As a Claude Code plugin:**

```
/buddy-install
```

Runs `plugin/scripts/install.sh`, which:

1. Verifies PlatformIO is present
2. Installs Python deps (`pyserial`, `bleak` for BLE)
3. Patches PlatformIO's x86_64 `mklittlefs` on Apple Silicon
   (`brew install mklittlefs` + symlink) so `uploadfs` can run
4. Merges the hook block into `~/.claude/settings.json`
5. Offers to flash firmware **and** filesystem (the CJK font) if a
   Paper is on USB
6. Starts the daemon in the background

**Manual (no plugin):**

```bash
pio run -e m5paper -t uploadfs              # flash the font (~90s)
pio run -e m5paper -t upload                # flash the firmware (~30s)
python3 tools/claude_code_bridge.py --budget 200000
```

Then copy `plugin/settings/hooks.json` contents into
`~/.claude/settings.json` under the `hooks` key.

## Daily use

```
/buddy-start      # start daemon (idempotent)
/buddy-stop       # stop daemon
/buddy-status     # pid, serial device, hooks, last log lines
/buddy-flash      # rebuild + reflash firmware and filesystem
```

State directory: `~/.claude-buddy/` (pid, log).

## Controls

| Button / zone | Dashboard | Approval card |
| --- | --- | --- |
| **PUSH** (middle) | nudge a redraw | **approve** |
| **DOWN** (bottom) | toggle demo | **deny** |
| **UP** (top) | short: force GC16 refresh; long ≥1.5s: toggle DND | — |
| Tap session row | focus that session on dashboard | — |
| Tap `SETTINGS` | open settings page | — |
| Tap option card | — | answer `AskUserQuestion` |

## Transport

Default is `BUDDY_TRANSPORT=auto` — USB serial if plugged in, else BLE
(Nordic UART Service, encrypted with macOS system passkey pairing).

```
BUDDY_TRANSPORT=ble    /buddy-start
BUDDY_TRANSPORT=serial /buddy-start
```

## Context budget

The progress bar shows the **currently focused session's** context-window
usage vs a limit (default 200K = Claude 4.6 standard context). Set
`BUDDY_BUDGET=1000000` for Claude 4.7's 1M-context beta, or `0` to hide
the bar.

Values come from the session's transcript JSONL — specifically the last
assistant message's `usage.input_tokens + output_tokens`.

## Language

Default: English. Tap **SETTINGS → language / 语言** to cycle to 中文.
Choice persists to NVS.

## Credits

Inspired by Anthropic's
[`claude-desktop-buddy`](https://github.com/anthropics/claude-desktop-buddy) —
the Nordic UART + heartbeat-JSON wire protocol shape is compatible so
one Paper can in theory also be driven by a desktop-side bridge. The
cat ASCII art was lifted from that project's `src/buddies/cat.cpp`.
Everything else (Paper-specific firmware, CJK font + freetype
rendering, the multi-session dashboard, touch handling, Claude Code
hook bridge, the plugin packaging) is written fresh here.

Bundled font: GenSenRounded Regular, from the M5Stack `M5EPD` library's
examples.
