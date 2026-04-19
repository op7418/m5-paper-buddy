// M5Paper V1.1 buddy firmware.
//
// 4.7" 540x960 e-ink portrait. Always-on dashboard for Claude Code:
//   - top band: project/sessions (L) + model/budget (R)
//   - middle: latest assistant message ("Claude says")
//   - lower: recent activity log
//   - footer: state-driven vector buddy face + link/button hints
// Full-screen approval card takes over when a permission decision is needed.
//
// Controls (side buttons, top→bottom in portrait):
//   UP (top)       short: force GC16 full refresh (clears ghosting)
//                  long (1.5s): toggle DND mode (auto-approve)
//   PUSH (middle)  approve when prompt is up, otherwise nudge a redraw
//   DOWN (bottom)  deny when prompt is up, otherwise toggle demo mode

#include <LittleFS.h>
#include <M5Unified.h>
#include <stdarg.h>
#include <rom/rtc.h>
#include "../ble_bridge.h"
#include "data_paper.h"
#include "buddy_frames.h"

M5Canvas canvas(&M5.Display);

static const int W = 540;
static const int H = 960;

// Text sizes — target pixel heights. M5GFX's setTextSize is a multiplier,
// so setTS() below picks the best built-in efontCN and applies a scale.
static const int TS_SM   = 18;   // small body / labels
static const int TS_MD   = 26;   // primary body text
static const int TS_LG   = 34;   // emphasis
static const int TS_XL   = 44;   // tool name, option labels
static const int TS_XXL  = 56;   // big headline
static const int TS_HUGE = 72;   // passkey digits / splash

// Map a target pixel height to the best available efontCN + scale factor.
// efontCN built-in sizes: 10, 12, 14, 16, 24 pixels.
static void setTS(int px) {
  if (px <= 12) {
    canvas.setFont(&fonts::efontCN_10);
    canvas.setTextSize((float)px / 10.0f);
  } else if (px <= 18) {
    canvas.setFont(&fonts::efontCN_16);
    canvas.setTextSize((float)px / 16.0f);
  } else {
    canvas.setFont(&fonts::efontCN_24);
    canvas.setTextSize((float)px / 24.0f);
  }
}

static const uint16_t INK      = TFT_BLACK;
static const uint16_t INK_DIM  = 0x2945;   // dark gray (~15% bright)
static const uint16_t PAPER    = TFT_WHITE;

// Section rules — use full INK + 2px thick so they're clearly visible
// under both GC16 (where grayscales differ) and DU (where everything
// under threshold collapses to white).
static void drawRule(int y) {
  canvas.drawFastHLine(0, y,     W, INK);
  canvas.drawFastHLine(0, y + 1, W, INK);
}
static void drawRuleInset(int y, int inset) {
  canvas.drawFastHLine(inset, y,     W - 2*inset, INK);
  canvas.drawFastHLine(inset, y + 1, W - 2*inset, INK);
}

static char btName[16] = "Claude";

static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
#ifndef BUDDY_DISABLE_BLE
  bleInit(btName);
#else
  Serial.println("[ble] DISABLED via BUDDY_DISABLE_BLE");
#endif
}

static TamaState tama;
static char     lastPromptId[40] = "";
static uint32_t promptArrivedMs = 0;
static bool     responseSent = false;
static uint32_t lastFullRefreshMs = 0;
static uint32_t lastPartialRefreshMs = 0;
static bool     redrawPending = true;
static bool     lastMode = false;

static bool     dndMode = false;
static const uint32_t DND_AUTO_DELAY_MS = 600;
static bool     dndAutoSent = false;

// UI language. 0 = English, 1 = 中文. Persisted in NVS via Preferences.
// Usage: canvas.drawString(LX("PROJECT", "项目"), x, y);
static uint8_t  uiLang = 0;
#define LX(en, zh) (uiLang == 1 ? (zh) : (en))

// Short-lived "celebrate" flash after any approve/deny so the buddy face
// briefly shows a reaction even when no data-driven state change follows.
static uint32_t celebrateUntil = 0;

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

static void fmtTokens(char* out, size_t n, uint32_t v) {
  if (v >= 1000000)      snprintf(out, n, "%lu.%luM", v/1000000, (v/100000)%10);
  else if (v >= 1000)    snprintf(out, n, "%lu.%luK", v/1000, (v/100)%10);
  else                    snprintf(out, n, "%lu", v);
}

// Manual codepoint-width estimator. M5EPD's canvas.textWidth() returns
// questionable values for CJK glyphs (often way smaller than they actually
// render), so we count codepoint bytes and scale by the current text
// size: ASCII ~0.55*size, anything multi-byte ~size (square glyph).
static int estWidth(const char* s, int textSize) {
  int w = 0;
  const unsigned char* p = (const unsigned char*)s;
  while (*p) {
    int cpLen = 1;
    if      ((*p & 0x80) == 0)     cpLen = 1;
    else if ((*p & 0xE0) == 0xC0)  cpLen = 2;
    else if ((*p & 0xF0) == 0xE0)  cpLen = 3;
    else if ((*p & 0xF8) == 0xF0)  cpLen = 4;
    w += (cpLen == 1) ? (textSize * 55 / 100) : textSize;
    p += cpLen;
  }
  return w;
}

// UTF-8 + pixel-width word wrap. Walks the input a codepoint at a time,
// using the manual estimator above. Caller passes textSize matching
// whatever setTextSize will be used at draw time.
static uint8_t wrapText(const char* in, char out[][256], uint8_t maxRows, int maxWidthPx, int textSize) {
  uint8_t row = 0;
  char cur[256]; cur[0] = 0; int curLen = 0;
  const char* p = in;

  auto flush = [&](bool force = false) {
    if (curLen > 0 || force) {
      if (row < maxRows) {
        strncpy(out[row], cur, 255);
        out[row][255] = 0;
      }
      row++; curLen = 0; cur[0] = 0;
    }
  };

  while (*p && row < maxRows) {
    if (*p == '\n') { flush(true); p++; continue; }

    uint8_t lead = (uint8_t)*p;
    int cpLen = 1;
    if      ((lead & 0x80) == 0)     cpLen = 1;
    else if ((lead & 0xE0) == 0xC0)  cpLen = 2;
    else if ((lead & 0xF0) == 0xE0)  cpLen = 3;
    else if ((lead & 0xF8) == 0xF0)  cpLen = 4;
    for (int i = 0; i < cpLen; i++) if (!p[i]) { cpLen = i; break; }
    if (cpLen == 0) break;

    if (cpLen == 1 && *p == ' ' && curLen == 0) { p++; continue; }

    // Build the probe line and measure.
    if (curLen + cpLen >= 255) { flush(); if (row >= maxRows) break; }
    char probe[256];
    memcpy(probe, cur, curLen);
    memcpy(probe + curLen, p, cpLen);
    probe[curLen + cpLen] = 0;
    int w = estWidth(probe, textSize);

    if (w > maxWidthPx && curLen > 0) {
      // Try to back up to the last ASCII space so we don't break a word.
      int lastSpace = -1;
      for (int i = curLen - 1; i >= 0; i--) if (cur[i] == ' ') { lastSpace = i; break; }
      if (lastSpace > 0) {
        char tail[256];
        int tailLen = curLen - (lastSpace + 1);
        memcpy(tail, cur + lastSpace + 1, tailLen);
        cur[lastSpace] = 0;
        curLen = lastSpace;
        flush();
        if (row >= maxRows) break;
        memcpy(cur, tail, tailLen);
        curLen = tailLen;
        cur[curLen] = 0;
      } else {
        flush();
        if (row >= maxRows) break;
      }
      continue;
    }

    memcpy(cur + curLen, p, cpLen);
    curLen += cpLen;
    cur[curLen] = 0;
    p += cpLen;
  }
  flush();
  return row;
}

// drawString, but truncates with a trailing "." if the text is too wide.
// Cap is in glyphs, not pixels — good enough given the default monospace-ish
// font.
static void drawTrunc(const char* s, int x, int y, int maxChars) {
  int n = (int)strlen(s);
  if (n <= maxChars) { canvas.drawString(s, x, y); return; }
  char buf[64];
  int take = maxChars - 1;
  if (take < 1) take = 1;
  if (take > (int)sizeof(buf) - 2) take = sizeof(buf) - 2;
  memcpy(buf, s, take);
  buf[take] = '.';
  buf[take + 1] = 0;
  canvas.drawString(buf, x, y);
}

// -----------------------------------------------------------------------------
// Buddy face (state-driven, ~80x80 vector drawing). Not animated per-frame
// — just repainted when state changes or on the 5-min full refresh. That's
// enough life on an e-ink panel without thrashing the display.
// -----------------------------------------------------------------------------

enum BuddyState { B_SLEEP, B_IDLE, B_BUSY, B_ATTENTION, B_CELEBRATE, B_DND };

static BuddyState currentBuddy() {
  if (dndMode)                     return B_DND;
  if (tama.promptId[0] && !responseSent) return B_ATTENTION;
  if (millis() < celebrateUntil)   return B_CELEBRATE;
  if (!tama.connected)             return B_SLEEP;
  if (tama.sessionsWaiting > 0)    return B_ATTENTION;
  if (tama.sessionsRunning > 0)    return B_BUSY;
  return B_IDLE;
}

// Pick the state's ASCII frame (lifted from src/buddies/cat.cpp — see
// buddy_frames.h).
static const BuddyFrame& currentFrame(BuddyState state) {
  switch (state) {
    case B_SLEEP:     return buddy_cat::SLEEP;
    case B_BUSY:      return buddy_cat::BUSY;
    case B_ATTENTION: return buddy_cat::ATTENTION;
    case B_CELEBRATE: return buddy_cat::CELEBRATE;
    case B_DND:       return buddy_cat::DND;
    case B_IDLE:
    default:          return buddy_cat::IDLE;
  }
}

// Render an ASCII buddy frame centered on (cx, cy). The ASCII art
// assumes a fixed-width font, but our TTF is proportional — so we draw
// each character into its own fixed cell (cellW × cellH) so columns
// line up. cellW tuned so 12 chars × cellW ≈ 180px, legible but compact.
static void drawBuddy(int cx, int cy, BuddyState state) {
  const BuddyFrame& f = currentFrame(state);
  const int cellW = 15;
  const int lineH = 24;
  const int totalW = 12 * cellW;
  const int totalH = 5 * lineH;
  int x0 = cx - totalW / 2;
  int y0 = cy - totalH / 2;
  setTS(TS_MD);
  canvas.setTextColor(INK);
  canvas.setTextDatum(TC_DATUM);
  for (int i = 0; i < 5; i++) {
    const char* line = f.lines[i];
    for (int c = 0; c < 12; c++) {
      char ch[2] = { line[c], 0 };
      if (ch[0] && ch[0] != ' ') {
        canvas.drawString(ch, x0 + c * cellW + cellW / 2, y0 + i * lineH);
      }
    }
  }
  canvas.setTextDatum(TL_DATUM);
}

// -----------------------------------------------------------------------------
// Dashboard sections
// -----------------------------------------------------------------------------

// Touch hit regions + settings page state — declared here (before any
// draw function that references them) so both drawHeader (sets
// settingsTrigger), drawSettings (sets settingsCloseHit), and loop()
// (hit-tests) can all see them.
struct HitRect { int x, y, w, h; };
static HitRect optionRects[4] = {};
static uint8_t optionRectCount = 0;
static int8_t  selectedOption  = -1;

static bool   settingsOpen = false;
static HitRect settingsTrigger  = {W - 140, 0, 140, 100};
static HitRect settingsCloseHit = {0, 0, 0, 0};
static HitRect settingsLangHit  = {0, 0, 0, 0};

// Legacy tab hit rects (approval tabs were dropped; kept for future use).
static HitRect tabRects[4] = {};
static uint8_t tabRectCount = 0;

// Sessions list tap targets — tapping a row sends a focus_session cmd
// back to the daemon, shifting the dashboard view to that session.
static HitRect sessionRects[5] = {};
static uint8_t sessionRectCount = 0;

static void drawHeader() {
  canvas.setTextDatum(TL_DATUM);          // defensive reset every frame
  setTS(TS_MD);
  canvas.setTextColor(INK);
  canvas.drawString(LX("Paper Buddy", "Paper Buddy"), 24, 22);

  setTS(TS_SM);
  canvas.setTextColor(INK_DIM);
  char who[64];
  if (ownerName()[0]) snprintf(who, sizeof(who), "%s's %s", ownerName(), petName());
  else                snprintf(who, sizeof(who), "%s", petName());
  canvas.drawString(who, 24, 56);

  // Battery on line 1 (top-right, aligned with "Paper Buddy"), SETTINGS
  // as plain text on line 2 (aligned with owner). No chip, no box.
  int32_t vBat = M5.Power.getBatteryVoltage();
  int pct = ((int)vBat - 3200) * 100 / (4350 - 3200);
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  char bat[16]; snprintf(bat, sizeof(bat), "%d%%", pct);

  setTS(TS_SM);
  canvas.setTextColor(INK_DIM);
  canvas.setTextDatum(TR_DATUM);
  canvas.drawString(bat, W - 24, 26);

  setTS(TS_SM);
  canvas.setTextColor(INK);
  canvas.drawString(LX("SETTINGS", "设置"), W - 24, 60);

  // Tappable region covers both the "SETTINGS" label and some padding
  // around it so the user has a generous hit area.
  settingsTrigger = { W - 160, 52, 150, 40 };

  // DND badge sits to the left of SETTINGS on the same line.
  if (dndMode) {
    setTS(TS_SM);
    int dndW = 60, dndH = 30;
    int dx = W - 160 - 10 - dndW, dy = 50;
    canvas.fillRect(dx, dy, dndW, dndH, INK);
    canvas.setTextColor(PAPER, INK);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString("DND", dx + dndW/2, dy + dndH/2);
  }

  canvas.setTextDatum(TL_DATUM);
  drawRule(94);
}

static void drawTopBand() {
  canvas.setTextDatum(TL_DATUM);
  // Column rule — 2px for visual parity with horizontal rules.
  canvas.drawFastVLine(W/2,     100, 160, INK);
  canvas.drawFastVLine(W/2 + 1, 100, 160, INK);

  // --- LEFT: session list (2+) OR classic single-project view ---------
  int lx = 16, ly = 108;
  setTS(TS_SM);
  canvas.setTextColor(INK_DIM);
  canvas.drawString(tama.sessionCount > 1 ? LX("SESSIONS", "会话") : LX("PROJECT", "项目"), lx, ly);
  ly += 22;

  if (tama.sessionCount > 1) {
    // Multi-session: row per session. Tap a row to focus the dashboard
    // on that session. The focused row gets reverse-video; others show
    // a prefix marker: "!" = waiting on approval, "*" = running,
    // "." = idle. Project name on left, branch info on right.
    sessionRectCount = 0;
    setTS(TS_SM);
    int rowH = 26;
    for (uint8_t i = 0; i < tama.sessionCount && ly < 254; i++) {
      const auto& s = tama.sessions[i];
      int rowX = 4, rowY = ly - 4, rowW = (W/2) - 8;
      if (s.focused) {
        canvas.fillRect(rowX, rowY, rowW, rowH, INK);
        canvas.setTextColor(PAPER, INK);
      } else {
        canvas.setTextColor(INK, PAPER);
      }
      const char* mark = s.waiting ? "!" : (s.running ? "*" : ".");
      canvas.drawString(mark, lx, ly);
      char row[32];
      snprintf(row, sizeof(row), "%.18s",
               s.project[0] ? s.project : LX("(unknown)", "（未知）"));
      canvas.drawString(row, lx + 16, ly);
      if (s.branch[0]) {
        if (!s.focused) canvas.setTextColor(INK_DIM, PAPER);
        char bra[20];
        if (s.dirty > 0) snprintf(bra, sizeof(bra), "%.10s*%u", s.branch, s.dirty);
        else             snprintf(bra, sizeof(bra), "%.14s", s.branch);
        canvas.setTextDatum(TR_DATUM);
        canvas.drawString(bra, W/2 - 6, ly);
        canvas.setTextDatum(TL_DATUM);
      }
      sessionRects[sessionRectCount++] = { rowX, rowY, rowW, rowH };
      ly += rowH;
    }
  } else {
    sessionRectCount = 0;
    setTS(TS_MD);
    canvas.setTextColor(INK);
    drawTrunc(tama.project[0] ? tama.project : "-", lx, ly, 14);
    ly += 34;
    setTS(TS_SM);
    canvas.setTextColor(INK_DIM);
    if (tama.branch[0]) {
      char bra[48];
      if (tama.dirty > 0) snprintf(bra, sizeof(bra), "%s  *%u", tama.branch, tama.dirty);
      else                 snprintf(bra, sizeof(bra), "%s", tama.branch);
      drawTrunc(bra, lx, ly, 20);
    }
    ly = 200;
    setTS(TS_SM);
    canvas.setTextColor(INK_DIM);
    canvas.drawString(LX("SESSIONS", "会话"), lx, ly); ly += 22;
    setTS(TS_MD);
    canvas.setTextColor(INK);
    if (!tama.connected) {
      canvas.drawString("-", lx, ly);
    } else if (tama.sessionsTotal == 0) {
      canvas.drawString(LX("idle", "空闲"), lx, ly);
    } else {
      char s[48];
      snprintf(s, sizeof(s), LX("%u run  %u wait", "运行 %u  等待 %u"),
               tama.sessionsRunning, tama.sessionsWaiting);
      canvas.drawString(s, lx, ly);
    }
  }

  // --- RIGHT: model + budget --------------------------------------------
  int rx = W/2 + 16, ry = 108;
  setTS(TS_SM);
  canvas.setTextColor(INK_DIM);
  canvas.drawString(LX("MODEL", "模型"), rx, ry);  ry += 22;
  setTS(TS_MD);
  canvas.setTextColor(INK);
  // No em dash fallback — an empty model field just leaves the slot
  // blank rather than rendering a single-glyph that can look like a
  // stray line next to the column divider.
  if (tama.modelName[0]) drawTrunc(tama.modelName, rx, ry, 14);
  ry = 188;
  setTS(TS_SM);
  canvas.setTextColor(INK_DIM);
  canvas.drawString(LX("CONTEXT", "上下文"), rx, ry); ry += 22;
  setTS(TS_SM);
  canvas.setTextColor(INK);
  char tok[16]; fmtTokens(tok, sizeof(tok), tama.tokensToday);
  if (tama.budgetLimit > 0) {
    char lim[16]; fmtTokens(lim, sizeof(lim), tama.budgetLimit);
    char line[64]; snprintf(line, sizeof(line), "%s / %s", tok, lim);
    canvas.drawString(line, rx, ry);
    ry += 24;
    int bx = rx, by = ry, bw = (W/2) - 32, bh = 12;
    canvas.drawRect(bx, by, bw, bh, INK);
    int pct = (int)((uint64_t)tama.tokensToday * 100 / tama.budgetLimit);
    if (pct > 100) pct = 100;
    int fill = (int)((uint64_t)bw * pct / 100);
    if (fill > 2) canvas.fillRect(bx + 1, by + 1, fill - 2, bh - 2, INK);
  } else {
    canvas.drawString(tok, rx, ry);
  }

  drawRule(264);
}

// Slim stats row — level (by tokens), approval/denial tallies. MOOD was
// dropped: its velocity-based tier isn't a useful signal in a coding
// workflow, and the stars read ambiguously.
static void drawStats() {
  canvas.setTextDatum(TL_DATUM);
  int y = 280;

  auto drawCell = [&](int x, const char* label, const char* value) {
    setTS(TS_SM);
    canvas.setTextColor(INK_DIM);
    canvas.drawString(label, x, y);
    setTS(TS_MD);
    canvas.setTextColor(INK);
    canvas.drawString(value, x, y + 22);
  };

  char lvl[12], appr[12], deny[12];
  snprintf(lvl,  sizeof(lvl),  "%u", stats().level);
  snprintf(appr, sizeof(appr), "%u", stats().approvals);
  snprintf(deny, sizeof(deny), "%u", stats().denials);

  drawCell( 20, LX("LEVEL",    "等级"),    lvl);
  drawCell(200, LX("APPROVED", "已批准"),  appr);
  drawCell(380, LX("DENIED",   "已拒绝"),  deny);

  drawRule(326);
}

// "Latest reply" — most recent assistant text pulled from the session's
// transcript_path. Shows whatever Claude last said in prose (not tool
// calls), so you can glance at the Paper and know what Claude is up to.
static void drawClaudeSays() {
  int y = 338;
  setTS(TS_SM);
  canvas.setTextColor(INK_DIM);
  canvas.drawString(LX("LATEST REPLY", "最新回复"), 16, y); y += 24;

  setTS(TS_MD);
  canvas.setTextColor(INK);
  if (!tama.assistantMsg[0]) {
    canvas.setTextColor(INK_DIM);
    canvas.drawString(LX("(nothing yet)", "（暂无）"), 16, y);
    drawRule(510);
    return;
  }
  static char wrapped[5][256];
  // Body width with 16px margins. Call setTextSize BEFORE wrapText so
  // the pixel measurement uses the right render.
  uint8_t rows = wrapText(tama.assistantMsg, wrapped, 5, W - 32, TS_MD);
  for (uint8_t i = 0; i < rows; i++) {
    canvas.drawString(wrapped[i], 16, y);
    y += 32;
    if (y > 500) break;
  }
  drawRule(510);
}

static void drawActivity() {
  canvas.setTextDatum(TL_DATUM);
  int y = 522;
  setTS(TS_SM);
  canvas.setTextColor(INK_DIM);
  canvas.drawString(LX("ACTIVITY", "活动"), 16, y); y += 32;

  setTS(TS_MD);
  if (tama.nLines == 0) {
    canvas.setTextColor(INK_DIM);
    canvas.drawString("-", 16, y);
    return;
  }
  uint8_t show = tama.nLines > 8 ? 8 : tama.nLines;
  for (uint8_t i = 0; i < show && y < H - 190; i++) {
    canvas.setTextColor(i == 0 ? INK : INK_DIM);
    // Wrap long Chinese entries so they don't clip off the right edge.
    // Most activity lines are short ("14:23 Bash done") and take 1 row.
    static char wrapped[3][256];
    uint8_t rows = wrapText(tama.lines[i], wrapped, 3, W - 32, TS_MD);
    for (uint8_t r = 0; r < rows && y < H - 190; r++) {
      canvas.drawString(wrapped[r], 16, y);
      y += 32;
    }
  }
}

static void drawFooter() {
  // ASCII buddy at text size 3 is ~216×120, so the footer has to be
  // roughly 160px tall to hold it plus a rule + some air.
  int top = H - 170;
  drawRule(top);

  // Buddy centered vertically in the footer, left side.
  drawBuddy(120, top + 80, currentBuddy());

  // Right column: link state + button legend.
  setTS(TS_SM);
  int rx = 254;
  int ry = top + 16;
  bool linked = bleConnected() && dataBtActive();
  canvas.setTextColor(linked ? INK : INK_DIM);
  const char* linkStr =
      linked           ? LX("LINKED",        "已连接") :
      bleConnected()   ? LX("BLE connected", "蓝牙已连") :
                         LX("USB / BLE adv", "USB / 蓝牙广播中");
  canvas.drawString(linkStr, rx, ry);
  ry += 30;
  canvas.setTextColor(INK);
  canvas.drawString(LX("UP hold = DND",    "长按 UP = 勿扰"),  rx, ry); ry += 26;
  canvas.drawString(LX("PUSH = approve",   "PUSH = 同意"),     rx, ry); ry += 26;
  canvas.drawString(LX("DOWN = deny/demo", "DOWN = 拒绝/演示"), rx, ry);
}

static void drawSettings() {
  canvas.fillSprite(PAPER);
  canvas.setTextDatum(TC_DATUM);

  setTS(TS_LG);
  canvas.setTextColor(INK);
  canvas.drawString(LX("SETTINGS", "设置"), W / 2, 30);

  drawRule(80);

  canvas.setTextDatum(TL_DATUM);
  int y = 110;
  int lx = 30, vx = 240;

  auto row = [&](const char* label, const char* value) {
    setTS(TS_SM);
    canvas.setTextColor(INK_DIM);
    canvas.drawString(label, lx, y);
    setTS(TS_MD);
    canvas.setTextColor(INK);
    canvas.drawString(value, vx, y);
    y += 50;
  };

  // Language row — tappable. Label shows the current choice; tapping
  // the value area cycles to the other language and persists.
  {
    int langY = y;
    setTS(TS_SM);
    canvas.setTextColor(INK_DIM);
    canvas.drawString(LX("language", "语言"), lx, y);
    setTS(TS_MD);
    canvas.setTextColor(INK);
    canvas.drawString(uiLang == 1 ? "中文  >  English" : "English  >  中文", vx, y);
    // Hit region covers the whole row so tapping anywhere on the line works.
    settingsLangHit = { lx - 10, langY - 8, W - 60, 48 };
    y += 50;
  }

  const char* xport = !tama.connected ? LX("offline", "离线")
                    : (bleConnected() && bleSecure()) ? LX("BLE (paired)", "蓝牙（已配对）")
                    : (bleConnected()) ? "BLE"
                    : LX("USB serial", "USB 串口");
  row(LX("transport", "传输"), xport);

  char buf[80];
  snprintf(buf, sizeof(buf), LX("%u total / %u run / %u wait",
                                "共 %u / 运行 %u / 等待 %u"),
           tama.sessionsTotal, tama.sessionsRunning, tama.sessionsWaiting);
  row(LX("sessions", "会话"), buf);

  row(LX("device", "设备"), btName);

  int32_t vBat = M5.Power.getBatteryVoltage();
  int pct = ((int)vBat - 3200) * 100 / (4350 - 3200);
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  snprintf(buf, sizeof(buf), "%d%%  (%lu mV)", pct, (unsigned long)vBat);
  row(LX("battery", "电量"), buf);

  row(LX("DND", "勿扰"),
      dndMode ? LX("ON  (auto-approve)", "开启（自动同意）") : LX("OFF", "关闭"));

  if (tama.budgetLimit > 0) {
    char used[16], lim[16];
    fmtTokens(used, sizeof(used), tama.tokensToday);
    fmtTokens(lim, sizeof(lim), tama.budgetLimit);
    snprintf(buf, sizeof(buf), "%s / %s", used, lim);
    row(LX("budget", "预算"), buf);
  } else {
    row(LX("budget", "预算"), LX("(not set)", "（未设）"));
  }

  uint32_t up = millis() / 1000;
  snprintf(buf, sizeof(buf), LX("%luh %02lum", "%lu 小时 %02lu 分"),
           up / 3600, (up / 60) % 60);
  row(LX("uptime", "运行"), buf);

  uint32_t age = (millis() - tama.lastUpdated) / 1000;
  snprintf(buf, sizeof(buf), LX("%lus ago", "%lu 秒前"), (unsigned long)age);
  row(LX("last msg", "上次消息"), buf);

  // Tips
  drawRule(y + 10);
  y += 40;
  setTS(TS_SM);
  canvas.setTextColor(INK_DIM);
  canvas.drawString(LX("TIPS", "提示"), lx, y); y += 32;
  canvas.setTextColor(INK);
  canvas.drawString(LX("UP hold 1.5s = toggle DND",            "长按 UP 1.5 秒 = 切换勿扰"), lx, y); y += 28;
  canvas.drawString(LX("PUSH / DOWN = approve / deny",         "PUSH / DOWN = 同意 / 拒绝"),  lx, y); y += 28;
  canvas.drawString(LX("Tap top-right = open/close this page", "点右上角 = 打开/关闭本页"),    lx, y); y += 28;
  canvas.drawString(LX("Tap option buttons to answer",         "点选项按钮回答问题"),          lx, y);

  // Close button — big, tappable at bottom.
  int bh = 90;
  int by = H - bh - 24;
  int bx = 60, bw = W - 120;
  for (int d = 0; d < 3; d++)
    canvas.drawRect(bx + d, by + d, bw - 2*d, bh - 2*d, INK);
  canvas.setTextDatum(MC_DATUM);
  setTS(TS_LG);
  canvas.drawString(LX("CLOSE", "关闭"), W / 2, by + bh / 2);
  canvas.setTextDatum(TL_DATUM);

  settingsCloseHit = { bx, by, bw, bh };
}

static void drawIdle() {
  canvas.fillSprite(PAPER);
  drawHeader();
  drawTopBand();
  drawStats();
  drawClaudeSays();
  drawActivity();
  drawFooter();
}

// (HitRect + settings state declared earlier, before drawSettings().)

// Tab strip at the very top of the approval card when 2+ prompts are
// pending. Each tab is a tappable rect → firmware sends `{"cmd":"focus"}`
// on tap and the daemon swaps ACTIVE_PROMPT. Returns the y below which
// the card body should start (so callers can shift their own content).
static int drawPendingTabs() {
  tabRectCount = 0;
  if (tama.pendingCount <= 1) return 0;

  const int tabH = 48;
  const int margin = 6;
  int n = tama.pendingCount;
  int tabW = (W - margin * (n + 1)) / n;

  canvas.setTextDatum(MC_DATUM);
  setTS(TS_SM);
  for (int i = 0; i < n; i++) {
    int tx = margin + i * (tabW + margin);
    int ty = 4;
    bool active = strcmp(tama.promptId, tama.pending[i].id) == 0;
    if (active) {
      canvas.fillRect(tx, ty, tabW, tabH, INK);
      canvas.setTextColor(PAPER, INK);
    } else {
      for (int d = 0; d < 2; d++)
        canvas.drawRect(tx + d, ty + d, tabW - 2*d, tabH - 2*d, INK);
      canvas.setTextColor(INK);
    }
    // First line: tool name. Second line: project (dim).
    canvas.drawString(tama.pending[i].tool, tx + tabW/2, ty + 16);
    if (!active) canvas.setTextColor(INK_DIM);
    canvas.drawString(tama.pending[i].project, tx + tabW/2, ty + 36);
    tabRects[i] = { tx, ty, tabW, tabH };
    tabRectCount++;
  }
  canvas.setTextDatum(TL_DATUM);
  return tabH + 8;   // body should start this far below y=0
}

static void drawPermissionCard() {
  canvas.setTextDatum(TC_DATUM);

  setTS(TS_SM);
  canvas.setTextColor(INK_DIM);
  canvas.drawString(dndMode ? LX("AUTO-APPROVING (DND)", "自动同意（勿扰）")
                            : LX("PERMISSION REQUESTED", "请求权限"),
                    W / 2, 20);

  setTS(TS_LG);
  canvas.setTextColor(INK);
  canvas.drawString(tama.promptTool[0] ? tama.promptTool : "(tool)",
                    W / 2, 56);

  // Which project/window this came from — matters when multiple Claude
  // Code windows are open at once.
  if (tama.promptProject[0] || tama.promptSid[0]) {
    char who[48];
    if (tama.promptProject[0] && tama.promptSid[0])
      snprintf(who, sizeof(who), "%.24s  [%s]", tama.promptProject, tama.promptSid);
    else if (tama.promptProject[0])
      snprintf(who, sizeof(who), "%.24s", tama.promptProject);
    else
      snprintf(who, sizeof(who), "session %s", tama.promptSid);
    setTS(TS_SM);
    canvas.setTextColor(INK_DIM);
    canvas.drawString(who, W / 2, 102);
  }

  drawRule(124);

  // Body takes the lion's share of the card now. Size 3 (18x24 glyphs)
  // is significantly more readable than the previous size 2 and still
  // leaves room for ~16 lines.
  canvas.setTextDatum(TL_DATUM);
  setTS(TS_MD);
  canvas.setTextColor(INK);
  const char* src = tama.promptBody[0] ? tama.promptBody : tama.promptHint;
  if (src[0]) {
    static char wrapped[18][256];
    uint8_t rows = wrapText(src, wrapped, 18, W - 40, TS_MD);
    int ty = 140;
    for (uint8_t i = 0; i < rows; i++) {
      canvas.drawString(wrapped[i], 20, ty);
      ty += 32;
      if (ty > 740) break;
    }
  }

  drawRule(770);

  canvas.setTextDatum(TC_DATUM);
  setTS(TS_SM);
  canvas.setTextColor(INK_DIM);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  char wline[48]; snprintf(wline, sizeof(wline), LX("waiting %lus", "等待 %lu 秒"),
                           (unsigned long)waited);
  canvas.drawString(wline, W / 2, 790);

  if (responseSent) {
    setTS(TS_MD);
    canvas.setTextColor(INK_DIM);
    canvas.drawString(LX("sent - waiting for Claude...", "已发送 - 等 Claude 继续..."),
                      W / 2, 870);
    canvas.setTextDatum(TL_DATUM);
    return;
  }

  // Side-by-side action columns at the bottom — PUSH on left, DOWN on
  // right. More compact than the stacked layout, leaves more body room.
  int cy = 870;
  setTS(TS_LG);
  canvas.setTextColor(INK);
  canvas.drawString("PUSH", W / 4, cy);
  canvas.drawString("DOWN", 3 * W / 4, cy);
  setTS(TS_SM);
  canvas.setTextColor(INK_DIM);
  canvas.drawString(LX("approve", "同意"), W / 4,     cy + 50);
  canvas.drawString(LX("deny",    "拒绝"), 3 * W / 4, cy + 50);

  canvas.setTextDatum(TL_DATUM);
}

// Question card: big touch-target buttons, one per option. Tapping sends
// the answer back via the daemon; the daemon resolves option index → label
// and tells Claude "user selected X, proceed".
static void drawQuestionCard() {
  canvas.setTextDatum(TC_DATUM);

  setTS(TS_SM);
  canvas.setTextColor(INK_DIM);
  canvas.drawString(LX("QUESTION FROM CLAUDE", "Claude 提问"), W / 2, 20);

  // Project / session origin — helps disambiguate when multiple windows.
  if (tama.promptProject[0] || tama.promptSid[0]) {
    char who[48];
    if (tama.promptProject[0] && tama.promptSid[0])
      snprintf(who, sizeof(who), "%.24s  [%s]", tama.promptProject, tama.promptSid);
    else if (tama.promptProject[0])
      snprintf(who, sizeof(who), "%.24s", tama.promptProject);
    else
      snprintf(who, sizeof(who), "session %s", tama.promptSid);
    canvas.drawString(who, W / 2, 46);
  }

  // Question text (falls back to hint if no body). Up to ~3 lines at size 3
  // above the options area.
  const char* src = tama.promptBody[0] ? tama.promptBody : tama.promptHint;
  canvas.setTextDatum(TL_DATUM);
  setTS(TS_MD);
  canvas.setTextColor(INK);
  int qy = 90;
  if (src[0]) {
    static char wrapped[4][256];
    uint8_t rows = wrapText(src, wrapped, 4, W - 40, TS_MD);
    for (uint8_t i = 0; i < rows && qy < 220; i++) {
      canvas.drawString(wrapped[i], 24, qy);
      qy += 34;
    }
  }

  // Option buttons — stack up to 4, equal height, 10px gap.
  optionRectCount = 0;
  if (tama.promptOptionCount == 0) {
    canvas.setTextDatum(TC_DATUM);
    setTS(TS_SM);
    canvas.setTextColor(INK_DIM);
    canvas.drawString("(no options provided)", W / 2, 500);
    canvas.setTextDatum(TL_DATUM);
  } else {
    const int top = 250;
    const int bottom = 870;
    const int gap = 10;
    int n = tama.promptOptionCount;
    int btnH = (bottom - top - gap * (n - 1)) / n;
    int bx = 20, bw = W - 40;

    canvas.setTextDatum(MC_DATUM);
    for (int i = 0; i < n; i++) {
      int by = top + i * (btnH + gap);
      bool tapped = (i == selectedOption);
      if (tapped) {
        // Inverted fill — black background, white text — makes the tap
        // feedback unambiguous even at arm's length.
        canvas.fillRect(bx, by, bw, btnH, INK);
        canvas.setTextColor(PAPER, INK);
      } else {
        // 3px outline when not selected.
        for (int d = 0; d < 3; d++) {
          canvas.drawRect(bx + d, by + d, bw - 2*d, btnH - 2*d, INK);
        }
        canvas.setTextColor(INK);
      }
      setTS(TS_LG);
      char line[56];
      snprintf(line, sizeof(line), "%d  %s", i + 1, tama.promptOptions[i]);
      canvas.drawString(line, W / 2, by + btnH / 2);

      optionRects[i] = { bx, by, bw, btnH };
      optionRectCount++;
    }
    canvas.setTextDatum(TL_DATUM);
  }

  // Footer: waited counter + physical button hint (DOWN = cancel).
  canvas.setTextDatum(TC_DATUM);
  setTS(TS_SM);
  canvas.setTextColor(INK_DIM);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  char wline[64]; snprintf(wline, sizeof(wline),
                           LX("waiting %lus   ·   DOWN = cancel",
                              "等待 %lu 秒   ·   DOWN = 取消"),
                           (unsigned long)waited);
  canvas.drawString(wline, W / 2, 910);

  if (responseSent) {
    canvas.setTextColor(INK);
    canvas.drawString(LX("sent - waiting for Claude to resume...",
                         "已发送 - 等 Claude 继续..."),
                      W / 2, 940);
  }
  canvas.setTextDatum(TL_DATUM);
}

static void drawApproval() {
  canvas.fillSprite(PAPER);
  // No tabs on the approval card. Approvals FIFO out of the daemon's
  // queue; only one is shown at a time, resolving it pops the next.
  tabRectCount = 0;
  bool isQuestion = (strcmp(tama.promptKind, "question") == 0);
  if (isQuestion) drawQuestionCard();
  else            drawPermissionCard();
}

static void drawSplash() {
  canvas.fillSprite(PAPER);
  canvas.setTextDatum(MC_DATUM);
  setTS(TS_XXL);
  canvas.setTextColor(INK);
  canvas.drawString("Paper Buddy", W/2, H/2 - 160);
  canvas.setTextDatum(TL_DATUM);
  drawBuddy(W/2, H/2, B_IDLE);
  canvas.setTextDatum(MC_DATUM);
  setTS(TS_MD);
  canvas.setTextColor(INK_DIM);
  canvas.drawString("M5Paper V1.1", W/2, H/2 + 120);
  setTS(TS_SM);
  canvas.drawString(btName, W/2, H/2 + 170);
  canvas.setTextDatum(TL_DATUM);
}

static void drawPasskey() {
  canvas.fillSprite(PAPER);
  canvas.setTextDatum(TC_DATUM);
  setTS(TS_MD);
  canvas.setTextColor(INK_DIM);
  canvas.drawString(LX("BLUETOOTH PAIRING", "蓝牙配对"), W/2, 200);
  setTS(TS_HUGE);
  canvas.setTextColor(INK);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  canvas.drawString(b, W/2, 340);
  setTS(TS_SM);
  canvas.setTextColor(INK_DIM);
  canvas.drawString(LX("enter this on the desktop", "在电脑上输入这个数字"), W/2, 560);
  canvas.setTextDatum(TL_DATUM);
}

// -----------------------------------------------------------------------------

// GC16 = 16-gray with flash, crispest but blinks. Use it for mode
// changes (approval card ↔ idle) where the flash is acceptable.
// GL16 = 16-gray without flash — preserves TTF anti-aliasing so text
// doesn't look muddy after many partial updates. Slightly slower than
// DU (~450ms vs 260ms) but much cleaner for small-font content.
static void pushFull()    { M5.Display.setEpdMode(epd_mode_t::epd_quality); canvas.pushSprite(0, 0); lastFullRefreshMs = lastPartialRefreshMs = millis(); }
static void pushPartial() { M5.Display.setEpdMode(epd_mode_t::epd_text); canvas.pushSprite(0, 0); lastPartialRefreshMs = millis(); }

static void repaint(bool wantFull) {
  uint32_t pk = blePasskey();
  bool promptMode = tama.promptId[0] != 0;

  if (pk)                  drawPasskey();
  else if (settingsOpen)   drawSettings();
  else if (promptMode)     drawApproval();
  else                     drawIdle();

  bool modeChanged = promptMode != lastMode;
  lastMode = promptMode;

  if (wantFull || modeChanged) pushFull();
  else                          pushPartial();
}

// -----------------------------------------------------------------------------

static void dndLoad() {
  Preferences p; p.begin("buddy", true);
  dndMode = p.getBool("dnd", false);
  uiLang  = p.getUChar("lang", 0);
  if (uiLang > 1) uiLang = 0;
  p.end();
}
static void dndSave() {
  Preferences p; p.begin("buddy", false);
  p.putBool("dnd", dndMode);
  p.end();
}
static void langSave() {
  Preferences p; p.begin("buddy", false);
  p.putUChar("lang", uiLang);
  p.end();
}

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  M5.begin(cfg);

  // Print the cause of the previous reset so crash loops can be debugged
  // over serial. rtc_get_reset_reason() codes:
  //   1=POWERON, 3=SW, 5=DEEPSLEEP, 6=SDIO, 7=TG0WDT_SYS, 8=TG1WDT_SYS,
  //   9=RTCWDT_SYS, 11=INTRUSION, 12=TGWDT_CPU, 13=SW_CPU,
  //   14=RTCWDT_CPU, 15=EXT_CPU, 16=RTCWDT_BROWN_OUT, 17=RTCWDT_RTC
  Serial.printf("[boot] reset reason cpu0=%d cpu1=%d free_heap=%u\n",
                (int)rtc_get_reset_reason(0), (int)rtc_get_reset_reason(1),
                ESP.getFreeHeap());

  // M5Unified: rotation 0 = portrait (540x960) for IT8951 panel
  M5.Display.setRotation(0);
  M5.Display.clear(TFT_WHITE);

  if (!LittleFS.begin(true)) {
    Serial.println("[fs] LittleFS mount failed — continuing without it");
  }
  // Debug: dump LittleFS root so we can verify the font file is present.
  {
    File root = LittleFS.open("/");
    if (root && root.isDirectory()) {
      Serial.printf("[fs] LittleFS total=%u used=%u\n",
                    LittleFS.totalBytes(), LittleFS.usedBytes());
      File f = root.openNextFile();
      while (f) {
        Serial.printf("[fs]  %s  %u bytes\n", f.name(), f.size());
        f = root.openNextFile();
      }
    }
  }

  canvas.setColorDepth(8);        // 8-bit grayscale for e-ink
  canvas.createSprite(W, H);
  canvas.setFont(&fonts::efontCN_24);  // default CJK font
  Serial.println("[font] using built-in efontCN");

  statsLoad();
  settingsLoad();
  petNameLoad();
  dndLoad();
  startBt();

  drawSplash();
  pushFull();
  delay(1500);

  redrawPending = true;
  lastFullRefreshMs = millis();
}

void loop() {
  M5.update();
  uint32_t now = millis();

  dataPoll(&tama);

  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId) - 1);
    lastPromptId[sizeof(lastPromptId) - 1] = 0;
    responseSent = false;
    dndAutoSent = false;
    selectedOption = -1;
    if (tama.promptId[0]) promptArrivedMs = now;
    redrawPending = true;
    // Mode transition (prompt arrives or leaves) — bypass the 2s DU
    // rate limit so the screen flips over immediately. Otherwise a tap
    // feedback or approval card entrance can lag up to 2s.
    lastPartialRefreshMs = 0;
  }

  bool inPrompt = tama.promptId[0] && !responseSent;
  bool isQuestion = inPrompt && strcmp(tama.promptKind, "question") == 0;

  // Touch input. M5Unified handles GT911 via M5.Touch — coordinates are
  // auto-rotated to match M5.Display.setRotation(). We track the latest
  // coords while finger is pressed, then hit-test on release.
  {
    static int  lastX = 0, lastY = 0;
    static bool hadTouch = false;

    auto t = M5.Touch.getDetail();
    if (t.isPressed()) {
      if (!hadTouch) {
        Serial.printf("[tp] down @ %d,%d\n", t.x, t.y);
      }
      lastX = t.x; lastY = t.y;
      hadTouch = true;
    }
    if (hadTouch && t.wasReleased()) {
      hadTouch = false;
      Serial.printf("[tp] up   @ %d,%d  (inPrompt=%d isQ=%d opts=%u settings=%d)\n",
                    lastX, lastY, (int)inPrompt, (int)isQuestion,
                    (unsigned)optionRectCount, (int)settingsOpen);

      auto hitTest = [&](const HitRect& r) {
        return lastX >= r.x && lastX < r.x + r.w &&
               lastY >= r.y && lastY < r.y + r.h;
      };

      if (settingsOpen) {
        // Language row first — tap toggles EN/ZH in place.
        if (hitTest(settingsLangHit)) {
          uiLang = uiLang == 0 ? 1 : 0;
          langSave();
          lastFullRefreshMs = 0;     // full GC16 to cleanly redraw all glyphs
          redrawPending = true;
        } else {
          // Anywhere else closes the page.
          settingsOpen = false;
          lastFullRefreshMs = 0;
          redrawPending = true;
        }
      } else if (!inPrompt && hitTest(settingsTrigger)) {
        settingsOpen = true;
        lastFullRefreshMs = 0;
        redrawPending = true;
      } else if (!inPrompt && sessionRectCount > 1) {
        // Tap a session row on the dashboard → tell daemon to focus
        // that session. Dashboard view flips on next heartbeat.
        for (uint8_t i = 0; i < sessionRectCount; i++) {
          if (hitTest(sessionRects[i])) {
            char cmd[80];
            snprintf(cmd, sizeof(cmd), "{\"cmd\":\"focus_session\",\"sid\":\"%s\"}",
                     tama.sessions[i].full[0] ? tama.sessions[i].full : tama.sessions[i].sid);
            sendCmd(cmd);
            break;
          }
        }
      } else if (inPrompt && tabRectCount > 1 && lastY < 60) {
        // Tab strip hit-test (top 60px of screen only) — switch focus
        // among pending prompts. Send focus cmd; daemon swaps ACTIVE_PROMPT
        // and the next heartbeat flips our view.
        for (uint8_t i = 0; i < tabRectCount; i++) {
          if (hitTest(tabRects[i])) {
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "{\"cmd\":\"focus\",\"id\":\"%s\"}",
                     tama.pending[i].id);
            sendCmd(cmd);
            break;
          }
        }
      } else if (isQuestion && optionRectCount > 0) {
        for (uint8_t i = 0; i < optionRectCount; i++) {
          const HitRect& r = optionRects[i];
          if (lastX >= r.x && lastX < r.x + r.w &&
              lastY >= r.y && lastY < r.y + r.h) {
            Serial.printf("[tp] HIT option %u\n", (unsigned)i);
            // Visual feedback first — invert the tapped button and paint
            // before anything clears the card.
            selectedOption = i;
            responseSent = true;
            lastPartialRefreshMs = 0;
            repaint(false);

            char cmd[96];
            snprintf(cmd, sizeof(cmd),
                     "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"option:%u\"}",
                     tama.promptId, (unsigned)i);
            sendCmd(cmd);
            uint32_t tookS = (now - promptArrivedMs) / 1000;
            statsOnApproval(tookS);
            celebrateUntil = now + 4000;
            redrawPending = true;
            break;
          }
        }
      }
    }
  }


  if (inPrompt && dndMode && !dndAutoSent && now - promptArrivedMs >= DND_AUTO_DELAY_MS) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
    sendCmd(cmd);
    responseSent = true;
    dndAutoSent = true;
    uint32_t tookS = (now - promptArrivedMs) / 1000;
    statsOnApproval(tookS);
    celebrateUntil = now + 4000;
    redrawPending = true;
  }

  if (M5.BtnB.wasPressed()) {
    Serial.println("[btn] BtnB pressed");
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      uint32_t tookS = (now - promptArrivedMs) / 1000;
      statsOnApproval(tookS);
      celebrateUntil = now + 4000;
    }
    lastPartialRefreshMs = 0;
    redrawPending = true;
  }

  if (M5.BtnC.wasPressed()) {
    Serial.println("[btn] BtnC pressed");
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
    } else {
      dataSetDemo(!dataDemo());
    }
    lastPartialRefreshMs = 0;
    redrawPending = true;
  }

  static bool upLongFired = false;
  if (M5.BtnA.pressedFor(1500) && !upLongFired && !inPrompt) {
    Serial.println("[btn] BtnA long press");
    upLongFired = true;
    dndMode = !dndMode;
    dndSave();
    lastFullRefreshMs = 0;
    redrawPending = true;
  }
  if (M5.BtnA.wasReleased()) {
    Serial.println("[btn] BtnA released");
    if (!upLongFired && !inPrompt) {
      lastFullRefreshMs = 0;
      redrawPending = true;
    }
    upLongFired = false;
  }

  static uint16_t   lastLineGen = 0, lastAsstGen = 0;
  static uint8_t    lastT = 255, lastR = 255, lastW = 255;
  static bool       lastConn = false;
  static uint32_t   lastTokDay = 0xFFFFFFFF;
  static uint16_t   lastDirty = 0xFFFF;
  static uint32_t   lastBudget = 0xFFFFFFFF;
  static char       lastBranch[40] = "\x01";
  static char       lastModel[32]  = "\x01";
  bool dataChanged = (tama.lineGen != lastLineGen)
                  || (tama.assistantGen != lastAsstGen)
                  || (tama.sessionsTotal != lastT) || (tama.sessionsRunning != lastR)
                  || (tama.sessionsWaiting != lastW) || (tama.connected != lastConn)
                  || (tama.tokensToday != lastTokDay) || (tama.dirty != lastDirty)
                  || (tama.budgetLimit != lastBudget)
                  || strcmp(tama.branch, lastBranch) != 0
                  || strcmp(tama.modelName, lastModel) != 0;
  if (dataChanged) {
    lastLineGen = tama.lineGen;  lastAsstGen = tama.assistantGen;
    lastT = tama.sessionsTotal;
    lastR = tama.sessionsRunning; lastW = tama.sessionsWaiting;
    lastConn = tama.connected; lastTokDay = tama.tokensToday;
    lastDirty = tama.dirty; lastBudget = tama.budgetLimit;
    strncpy(lastBranch, tama.branch, sizeof(lastBranch) - 1); lastBranch[sizeof(lastBranch) - 1] = 0;
    strncpy(lastModel, tama.modelName, sizeof(lastModel) - 1); lastModel[sizeof(lastModel) - 1] = 0;
    redrawPending = true;
  }

  static uint32_t lastPromptTick = 0;
  if (inPrompt && now - lastPromptTick >= 1000) {
    lastPromptTick = now;
    redrawPending = true;
  }

  // Idle dashboard: slow refresh (every 30s) so the e-ink isn't constantly
  // re-inking on heartbeat noise. Approval card / question card /
  // settings: fast refresh (2s) because they're interactive.
  // Mode transitions (prompt arrives/leaves) already bypass this by
  // setting lastPartialRefreshMs = 0.
  bool interactive = inPrompt || settingsOpen;
  uint32_t partialGap = interactive ? 2000UL : 30000UL;
  bool canPartial = (now - lastPartialRefreshMs) >= partialGap;
  bool shouldFull = (now - lastFullRefreshMs) >= 120000UL;   // GC16 sweep every 2 min

  if (redrawPending && (canPartial || shouldFull)) {
    repaint(shouldFull);
    redrawPending = false;
  }

  delay(20);
}
