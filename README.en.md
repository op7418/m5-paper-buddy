# m5-paper-buddy

<p align="center">
  <a href="README.md">中文</a> · <a href="README.en.md"><b>English</b></a>
</p>

<p align="center">
  <img src="https://github.com/user-attachments/assets/49a543f3-ad1a-4735-a5d1-ef98867cff1e" alt="m5-paper-buddy on an M5Paper V1.1" width="540">
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-blue.svg" alt="GPL-3.0"></a>
  <img src="https://img.shields.io/badge/hardware-M5Paper%20V1.1-orange" alt="M5Paper">
  <img src="https://img.shields.io/badge/firmware-ESP32%20%2B%20PlatformIO-brightgreen" alt="ESP32">
  <img src="https://img.shields.io/badge/daemon-Python%203-yellow" alt="Python">
  <img src="https://img.shields.io/badge/integration-Claude%20Code%20Plugin-7F52FF" alt="Claude Code">
  <img src="https://img.shields.io/badge/i18n-EN%20%2F%20中文-lightgrey" alt="i18n">
</p>

<p align="center"><b>A physical Claude Code companion running on M5Paper V1.1</b></p>

---

## ✨ Intro

A Claude Code sidekick running on an **M5Paper V1.1** (4.7" e-ink,
540×960, GT911 touch, ESP32). Sits on your desk and mirrors every
Claude Code session you have open: project, branch, model, context
usage, recent activity, Claude's latest reply. When Claude wants to
run a tool, the full content shows up as a full-screen approval card
with hardware buttons and touch options.

---

## 🎛️ Features

| | |
| --- | --- |
| 📊 **Multi-session dashboard** | Left column lists every active Claude Code window; tap a row to focus. Right column shows model + context-window progress bar. |
| 🔐 **Hardware approval** | `PreToolUse` shows a full-screen card with the complete command / diff / preview. **PUSH** approves, **DOWN** denies. DND mode (long-press **UP**) auto-approves for batch tasks. |
| 💬 **Touch-answer questions** | Claude's `AskUserQuestion` options render as up to 4 big tap targets; tapping sends the chosen label back as the answer. |
| 🔁 **FIFO queue** | Multiple windows asking for approval get queued; resolving the current one automatically pops the next. |
| 🀄 **Bilingual UI** | 3.4 MB CJK TTF shipped on LittleFS; toggle **English ↔ 中文** in the Settings page. All prompts, replies, activity lines render Chinese correctly. |
| 🔌 **Two transports** | USB serial (default, zero-setup) or BLE (Nordic UART, macOS passkey pairing), auto-selected. |
| ⚙️ **Settings page** | Tap **SETTINGS** (top-right) for transport / battery / sessions / DND / budget / uptime / last message / language toggle. |
| 🐱 **Cat buddy** | A small ASCII cat in the footer reacts to state — idle / busy / attention / celebrate / DND / sleep. |
| 🔌 **Claude Code plugin** | One `/buddy-install` handles PlatformIO + Python deps + mklittlefs arch patch on Apple Silicon + hooks merge + firmware + filesystem flashing + daemon launch. |

---

## 🛠️ Hardware

- **M5Paper V1.1** (4.7" e-ink, 540×960, GT911 capacitive touch, ESP32, 16MB flash)
- USB-C cable (required for initial flash; BLE works afterwards)

---

## 🚀 Quick start

**Prereqs**: [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
Homebrew (Apple Silicon only, for native `mklittlefs`), and an M5Paper V1.1.

```bash
# Clone
git clone https://github.com/op7418/m5-paper-buddy.git
cd m5-paper-buddy

# Recommended: install as a Claude Code plugin.
# Register the plugin/ directory with Claude Code, then:
/buddy-install
```

`/buddy-install` automatically:

1. Verifies PlatformIO is installed
2. Installs Python deps (`pyserial`, `bleak` for BLE mode)
3. On Apple Silicon, **patches PlatformIO's x86_64 `mklittlefs`**
   (`brew install mklittlefs` + symlink) so `uploadfs` can run
4. Merges the hook block into `~/.claude/settings.json` (backs up first)
5. If a Paper is plugged in, **flashes firmware + filesystem (font)**
6. Launches the daemon in the background

**Manual install (no plugin)**:

```bash
pio run -e m5paper -t uploadfs          # flash the font to LittleFS (~90s)
pio run -e m5paper -t upload            # flash firmware (~30s)
python3 tools/claude_code_bridge.py --budget 200000

# Then manually copy plugin/settings/hooks.json's hooks block into
# ~/.claude/settings.json
```

---

## 📟 Daily use

Once installed, these slash commands are available in Claude Code:

| Command | Purpose |
| --- | --- |
| `/buddy-install` | First-time setup / re-verify environment |
| `/buddy-start` | Start the daemon (idempotent) |
| `/buddy-stop` | Stop the daemon |
| `/buddy-status` | Daemon pid, serial device, hooks state, tail of log |
| `/buddy-flash` | Rebuild + reflash firmware AND filesystem (stop → flash → start) |

State directory: `~/.claude-buddy/` (pid, log).

---

## ⌨️ Controls

| Button / zone | Dashboard | Approval card |
| --- | --- | --- |
| **PUSH** (middle) | nudge a redraw | **approve** |
| **DOWN** (bottom) | toggle demo | **deny** |
| **UP** (top) | short: force GC16 refresh (clears ghosting) · long ≥1.5s: toggle **DND** | — |
| Tap session row | focus that session on the dashboard | — |
| Tap `SETTINGS` | open settings page | — |
| Tap option card | — | answer `AskUserQuestion` |

---

## 🔌 Transport

Default is `BUDDY_TRANSPORT=auto` — USB serial if plugged in, else BLE.

```bash
BUDDY_TRANSPORT=ble    /buddy-start
BUDDY_TRANSPORT=serial /buddy-start
```

First BLE connect triggers macOS's system pairing dialog; the Paper
displays a 6-digit passkey, you type it on the Mac. Subsequent reconnects
are automatic.

---

## 💰 Context budget

The progress bar shows the **currently-focused session's**
context-window usage divided by a limit, computed from the last
assistant message's `usage.input_tokens + output_tokens` in the
session's transcript JSONL.

Default limit is 200K (Claude 4.6 standard context). For the 1M-context
4.7 beta:

```bash
BUDDY_BUDGET=1000000 /buddy-start
```

Set `0` to hide the bar.

---

## 🌐 Language toggle

Default: English. Tap **SETTINGS → language / 语言** to switch to 中文.
Choice persists in NVS across reboots.

---

## 📂 Layout

```
src/
  ble_bridge.cpp/h       # Nordic UART Service, line-buffered TX/RX
  stats.h                # NVS-backed state (approvals/denials/level/DND/language)
  paper/
    main.cpp             # UI, state machine, touch, settings, i18n
    data_paper.h         # TamaState + JSON parsing (UTF-8 safe)
    xfer_paper.h         # status responses, name/owner/unpair cmds
    buddy_frames.h       # ASCII cat frames (6 states)
data/cjk.ttf             # CJK font flashed via `pio run -t uploadfs`
partitions-m5paper.csv   # 3 MB app + 13 MB LittleFS (font needs room)
platformio.ini
plugin/                  # Claude Code plugin
  plugin.json            # manifest
  commands/              # /buddy-* slash commands
  scripts/               # install / start / stop / status / flash / common
  settings/hooks.json    # hooks to merge into ~/.claude/settings.json
  README.md              # plugin's own README
tools/claude_code_bridge.py   # daemon: HTTP → serial/BLE bridge
```

---

## 🧩 Development

Firmware iteration:

```bash
pio run -e m5paper              # build only
pio run -e m5paper -t upload    # flash firmware
pio run -e m5paper -t uploadfs  # refresh LittleFS (only when font changes)
```

Daemon iteration:

```bash
/buddy-stop && /buddy-start
# or:
plugin/scripts/stop.sh && plugin/scripts/start.sh
```

Tail the log: `tail -f ~/.claude-buddy/daemon.log`

---

## 🙏 Credits

This project was inspired by Anthropic's
[`claude-desktop-buddy`](https://github.com/anthropics/claude-desktop-buddy):

- The Nordic UART Service + heartbeat-JSON wire protocol shape is the
  same, so in principle a Paper running this firmware can also be
  driven by that project's desktop bridge
- The cat ASCII frames in `src/paper/buddy_frames.h` are adapted from
  its `src/buddies/cat.cpp`

Bundled font: GenSenRounded Regular, from the M5Stack `M5EPD` library's
examples.

---

## 📜 License

This project is licensed under **[GPL-3.0](LICENSE)** with an explicit
**attribution clause**:

> **Any fork, modification, or redistribution MUST:**
>
> 1. Preserve the `Copyright © 2026 op7418` notice
> 2. Visibly credit `op7418 / m5-paper-buddy` in the derivative work's
>    README or About page
> 3. Release the derivative itself under **GPL-3.0 or later** with
>    **full source code publicly available**

In short: you're free to fork / modify / use commercially, but any
modified version must be open-source and must credit this project.
Closed-source derivatives are not permitted.

See the "Attribution & derivative obligations" section at the top of
[LICENSE](LICENSE) for the full terms.

<details>
<summary>Third-party components</summary>

- `data/cjk.ttf`: GenSenRounded Regular, from the M5EPD library's
  examples. The font's own license terms apply to that file.
- `src/paper/buddy_frames.h`: ASCII frames adapted from
  anthropics/claude-desktop-buddy (MIT).
- Nordic UART Service UUIDs and heartbeat JSON schema reference
  anthropics/claude-desktop-buddy (MIT).

</details>
