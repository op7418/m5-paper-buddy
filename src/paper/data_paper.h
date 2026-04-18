#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5EPD.h>
#include "../ble_bridge.h"
#include "xfer_paper.h"

// Paper fork of data.h. Only change vs the Stick original: RTC API — M5EPD
// uses rtc_time_t/rtc_date_t with lowercase field names, whereas M5StickC
// Plus uses RTC_TimeTypeDef/RTC_DateTypeDef with capitalised ones. Wire
// protocol parsing, demo mode, and line buffering are identical.

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint32_t lastUpdated;
  char     msg[24];
  bool     connected;
  char     lines[8][92];
  uint8_t  nLines;
  uint16_t lineGen;
  char     promptId[40];
  char     promptTool[20];
  char     promptHint[44];
  char     promptKind[16];    // "permission" (default) or "question"
  char     promptOptions[4][48];
  uint8_t  promptOptionCount;
  char     promptProject[24]; // project whose session raised this prompt
  char     promptSid[10];     // first 8 chars of session_id, for disambiguation

  // Extended fields (all optional; empty/zero means "don't display").
  char     promptBody[512];   // full multi-line content for approval (diff, full command, etc.)
  char     project[40];       // basename of the session's git root / cwd
  char     branch[40];        // current git branch, or "" if not a repo
  uint16_t dirty;              // git dirty file count
  uint32_t budgetLimit;        // daily token budget; 0 = hide bar
  char     modelName[32];     // "Opus 4.6", "Sonnet 4.5", etc.
  char     assistantMsg[240]; // most recent Claude text, truncated
  uint16_t assistantGen;       // bumps on each new assistant_msg so UI can detect change

  // Per-session list. If sessionCount > 1, the dashboard shows a list
  // row per session instead of the single-project view.
  struct SessionRow {
    char     sid[10];         // short, for display
    char     full[40];        // full session id, echoed back on focus tap
    char     project[24];
    char     branch[18];
    uint16_t dirty;
    bool     running;
    bool     waiting;
    bool     focused;         // daemon's current dashboard focus
  };
  SessionRow sessions[5];
  uint8_t    sessionCount;

  // Tabs for the approval card when multiple sessions have pending
  // PreToolUse requests at the same time. Tapping a tab sends a focus
  // command back so the daemon swaps ACTIVE_PROMPT.
  struct PendingTab {
    char id[40];
    char tool[14];
    char project[20];
  };
  PendingTab pending[4];
  uint8_t    pendingCount;
};

static uint32_t _lastLiveMs = 0;
static uint32_t _lastBtByteMs = 0;
static bool     _demoMode   = false;
static uint8_t  _demoIdx    = 0;
static uint32_t _demoNext   = 0;

struct _Fake { const char* n; uint8_t t,r,w; bool c; uint32_t tok; };
static const _Fake _FAKES[] = {
  {"asleep",0,0,0,false,0}, {"one idle",1,0,0,false,12000},
  {"busy",4,3,0,false,89000}, {"attention",2,1,1,false,45000},
  {"completed",1,0,0,true,142000},
};

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline bool dataBtActive() {
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].n;
  if (dataConnected()) return dataBtActive() ? "bt" : "usb";
  return "none";
}

static bool _rtcValid = false;
inline bool dataRtcValid() { return _rtcValid; }

static void _applyJson(const char* line, TamaState* out) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  if (xferCommand(doc)) { _lastLiveMs = millis(); return; }

  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    time_t local = (time_t)t[0].as<uint32_t>() + (int32_t)t[1];
    struct tm lt; gmtime_r(&local, &lt);
    rtc_time_t tm;
    tm.hour = (int8_t)lt.tm_hour;
    tm.min  = (int8_t)lt.tm_min;
    tm.sec  = (int8_t)lt.tm_sec;
    rtc_date_t dt;
    dt.week = (int8_t)lt.tm_wday;
    dt.mon  = (int8_t)(lt.tm_mon + 1);
    dt.day  = (int8_t)lt.tm_mday;
    dt.year = (int16_t)(lt.tm_year + 1900);
    M5.RTC.setTime(&tm);
    M5.RTC.setDate(&dt);
    _rtcValid = true;
    _lastLiveMs = millis();
    return;
  }

  out->sessionsTotal     = doc["total"]     | out->sessionsTotal;
  out->sessionsRunning   = doc["running"]   | out->sessionsRunning;
  out->sessionsWaiting   = doc["waiting"]   | out->sessionsWaiting;
  out->recentlyCompleted = doc["completed"] | false;
  uint32_t bridgeTokens = doc["tokens"] | 0;
  if (doc["tokens"].is<uint32_t>()) statsOnBridgeTokens(bridgeTokens);
  out->tokensToday = doc["tokens_today"] | out->tokensToday;
  const char* m = doc["msg"];
  if (m) { strncpy(out->msg, m, sizeof(out->msg)-1); out->msg[sizeof(out->msg)-1]=0; }
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (n >= 8) break;
      const char* s = v.as<const char*>();
      strncpy(out->lines[n], s ? s : "", 91); out->lines[n][91]=0;
      n++;
    }
    if (n != out->nLines || (n > 0 && strcmp(out->lines[n-1], out->msg) != 0)) {
      out->lineGen++;
    }
    out->nLines = n;
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"];   const char* pt = pr["tool"];
    const char* ph = pr["hint"];  const char* pb = pr["body"];
    const char* pk = pr["kind"];
    strncpy(out->promptId,   pid ? pid : "", sizeof(out->promptId)-1);     out->promptId[sizeof(out->promptId)-1]=0;
    strncpy(out->promptTool, pt  ? pt  : "", sizeof(out->promptTool)-1);   out->promptTool[sizeof(out->promptTool)-1]=0;
    strncpy(out->promptHint, ph  ? ph  : "", sizeof(out->promptHint)-1);   out->promptHint[sizeof(out->promptHint)-1]=0;
    strncpy(out->promptBody, pb  ? pb  : "", sizeof(out->promptBody)-1);   out->promptBody[sizeof(out->promptBody)-1]=0;
    strncpy(out->promptKind, pk  ? pk  : "permission", sizeof(out->promptKind)-1); out->promptKind[sizeof(out->promptKind)-1]=0;

    JsonArray po = pr["options"];
    out->promptOptionCount = 0;
    if (!po.isNull()) {
      for (JsonVariant v : po) {
        if (out->promptOptionCount >= 4) break;
        const char* s = v.as<const char*>();
        strncpy(out->promptOptions[out->promptOptionCount], s ? s : "",
                sizeof(out->promptOptions[0]) - 1);
        out->promptOptions[out->promptOptionCount][sizeof(out->promptOptions[0]) - 1] = 0;
        out->promptOptionCount++;
      }
    }

    const char* ppj = pr["project"];
    const char* psi = pr["sid"];
    strncpy(out->promptProject, ppj ? ppj : "", sizeof(out->promptProject)-1);
    out->promptProject[sizeof(out->promptProject)-1]=0;
    strncpy(out->promptSid, psi ? psi : "", sizeof(out->promptSid)-1);
    out->promptSid[sizeof(out->promptSid)-1]=0;
  } else {
    out->promptId[0] = 0; out->promptTool[0] = 0;
    out->promptHint[0] = 0; out->promptBody[0] = 0;
    out->promptKind[0] = 0;
    out->promptOptionCount = 0;
    out->promptProject[0] = 0;
    out->promptSid[0] = 0;
  }

  // Pending prompts tab strip.
  JsonArray pa = doc["pending"];
  if (!pa.isNull()) {
    out->pendingCount = 0;
    for (JsonVariant v : pa) {
      if (out->pendingCount >= 4) break;
      JsonObject po = v.as<JsonObject>();
      if (po.isNull()) continue;
      TamaState::PendingTab& t = out->pending[out->pendingCount];
      const char* pid = po["id"]; const char* tl = po["tool"]; const char* pj = po["proj"];
      strncpy(t.id, pid ? pid : "", sizeof(t.id)-1);           t.id[sizeof(t.id)-1]=0;
      strncpy(t.tool, tl ? tl : "", sizeof(t.tool)-1);         t.tool[sizeof(t.tool)-1]=0;
      strncpy(t.project, pj ? pj : "", sizeof(t.project)-1);   t.project[sizeof(t.project)-1]=0;
      out->pendingCount++;
    }
  } else {
    out->pendingCount = 0;
  }

  // Parse sessions array — per-session rows for the dashboard.
  JsonArray sa = doc["sessions"];
  if (!sa.isNull()) {
    out->sessionCount = 0;
    for (JsonVariant v : sa) {
      if (out->sessionCount >= 5) break;
      JsonObject so = v.as<JsonObject>();
      if (so.isNull()) continue;
      TamaState::SessionRow& r = out->sessions[out->sessionCount];
      const char* sid  = so["sid"];
      const char* full = so["full"];
      const char* pj   = so["proj"];
      const char* br   = so["branch"];
      strncpy(r.sid, sid ? sid : "", sizeof(r.sid)-1);           r.sid[sizeof(r.sid)-1]=0;
      strncpy(r.full, full ? full : "", sizeof(r.full)-1);       r.full[sizeof(r.full)-1]=0;
      strncpy(r.project, pj ? pj : "", sizeof(r.project)-1);     r.project[sizeof(r.project)-1]=0;
      strncpy(r.branch, br ? br : "", sizeof(r.branch)-1);       r.branch[sizeof(r.branch)-1]=0;
      r.dirty   = so["dirty"]   | 0;
      r.running = so["running"] | false;
      r.waiting = so["waiting"] | false;
      r.focused = so["focused"] | false;
      out->sessionCount++;
    }
  }

  // Dashboard fields — flat top-level; empty/zero = "nothing to show".
  const char* pj = doc["project"];
  if (pj) { strncpy(out->project, pj, sizeof(out->project)-1); out->project[sizeof(out->project)-1]=0; }
  const char* br = doc["branch"];
  if (br) { strncpy(out->branch, br, sizeof(out->branch)-1); out->branch[sizeof(out->branch)-1]=0; }
  if (doc["dirty"].is<uint16_t>())       out->dirty       = doc["dirty"];
  if (doc["budget"].is<uint32_t>())      out->budgetLimit = doc["budget"];

  const char* md = doc["model"];
  if (md) { strncpy(out->modelName, md, sizeof(out->modelName)-1); out->modelName[sizeof(out->modelName)-1]=0; }
  const char* am = doc["assistant_msg"];
  if (am) {
    if (strncmp(am, out->assistantMsg, sizeof(out->assistantMsg)) != 0) {
      strncpy(out->assistantMsg, am, sizeof(out->assistantMsg)-1);
      out->assistantMsg[sizeof(out->assistantMsg)-1]=0;
      out->assistantGen++;
    }
  }
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

template<size_t N>
struct _LineBuf {
  char buf[N];
  uint16_t len = 0;
  void feed(Stream& s, TamaState* out) {
    while (s.available()) {
      char c = s.read();
      if (c == '\n' || c == '\r') {
        if (len > 0) { buf[len]=0; if (buf[0]=='{') _applyJson(buf, out); len=0; }
      } else if (len < N-1) {
        buf[len++] = c;
      }
    }
  }
};

static _LineBuf<2560> _usbLine, _btLine;

inline void dataPoll(TamaState* out) {
  uint32_t now = millis();

  if (_demoMode) {
    if (now >= _demoNext) { _demoIdx = (_demoIdx + 1) % 5; _demoNext = now + 8000; }
    const _Fake& s = _FAKES[_demoIdx];
    out->sessionsTotal=s.t; out->sessionsRunning=s.r; out->sessionsWaiting=s.w;
    out->recentlyCompleted=s.c; out->tokensToday=s.tok; out->lastUpdated=now;
    out->connected = true;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
    return;
  }

  _usbLine.feed(Serial, out);
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    _lastBtByteMs = millis();
    if (c == '\n' || c == '\r') {
      if (_btLine.len > 0) {
        _btLine.buf[_btLine.len] = 0;
        if (_btLine.buf[0] == '{') _applyJson(_btLine.buf, out);
        _btLine.len = 0;
      }
    } else if (_btLine.len < sizeof(_btLine.buf) - 1) {
      _btLine.buf[_btLine.len++] = (char)c;
    }
  }

  out->connected = dataConnected();
  if (!out->connected) {
    out->sessionsTotal=0; out->sessionsRunning=0; out->sessionsWaiting=0;
    out->recentlyCompleted=false; out->lastUpdated=now;
    strncpy(out->msg, "No Claude connected", sizeof(out->msg)-1);
    out->msg[sizeof(out->msg)-1]=0;
  }
}
