#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <M5EPD.h>
#include "../ble_bridge.h"

// Paper fork of xfer.h. Dropped vs the Stick original:
//   - GIF character push (char_begin/file/chunk/file_end/char_end) — e-ink
//     can't meaningfully render a 30fps GIF, so we don't ack char_begin.
//     Per REFERENCE.md: "If your device doesn't want pushed files, don't
//     ack char_begin. The desktop times out after a few seconds."
//   - species cmd — no ASCII pets on the Paper build.
//
// Kept: name, owner, unpair, status. Status reports battery via
// M5.getBatteryVoltage() instead of the Stick's AXP192 reads.

static void _xAck(const char* what, bool ok, uint32_t n = 0) {
  char b[64];
  int len = snprintf(b, sizeof(b), "{\"ack\":\"%s\",\"ok\":%s,\"n\":%lu}\n", what, ok?"true":"false", (unsigned long)n);
  Serial.write(b, len);
  bleWrite((const uint8_t*)b, len);
}

void petNameSet(const char* name);
const char* petName();
void ownerSet(const char* name);
const char* ownerName();
#include "../stats.h"

inline bool xferCommand(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd) return false;

  if (strcmp(cmd, "name") == 0) {
    const char* n = doc["name"];
    if (n) petNameSet(n);
    _xAck("name", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "owner") == 0) {
    const char* n = doc["name"];
    if (n) ownerSet(n);
    _xAck("owner", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "unpair") == 0) {
    bleClearBonds();
    _xAck("unpair", true);
    return true;
  }

  if (strcmp(cmd, "status") == 0) {
    // M5Paper: battery is read straight off the ADC, no PMIC. 3.2V = empty,
    // 4.35V = full (per the M5EPD examples). No charge current exposed, so
    // we approximate usb/charging from the raw USB-in voltage.
    uint32_t vBat_mV = M5.getBatteryVoltage();
    int vBat = (int)vBat_mV;
    int pct = ((int)vBat - 3200) * 100 / (4350 - 3200);
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    // M5EPD doesn't expose VBUS directly; treat "bat voltage > 4.25V" as
    // a proxy for USB-connected. Good enough for the desktop's battery pill.
    bool usb = vBat_mV > 4250;
    char b[320];
    int len = snprintf(b, sizeof(b),
      "{\"ack\":\"status\",\"ok\":true,\"n\":0,\"data\":{"
      "\"name\":\"%s\",\"owner\":\"%s\",\"sec\":%s,"
      "\"bat\":{\"pct\":%d,\"mV\":%d,\"mA\":0,\"usb\":%s},"
      "\"sys\":{\"up\":%lu,\"heap\":%u,\"fsFree\":%lu,\"fsTotal\":%lu},"
      "\"stats\":{\"appr\":%u,\"deny\":%u,\"vel\":%u,\"nap\":%lu,\"lvl\":%u}"
      "}}\n",
      petName(), ownerName(), bleSecure() ? "true" : "false",
      pct, vBat, usb ? "true" : "false",
      millis() / 1000, ESP.getFreeHeap(),
      (unsigned long)(LittleFS.totalBytes() - LittleFS.usedBytes()),
      (unsigned long)LittleFS.totalBytes(),
      stats().approvals, stats().denials, statsMedianVelocity(),
      (unsigned long)stats().napSeconds, stats().level
    );
    Serial.write(b, len);
    bleWrite((const uint8_t*)b, len);
    return true;
  }

  // "permission" isn't ours — main.cpp sends it, desktop never sends it.
  // Everything else (char_begin/file/chunk/file_end/char_end/species) is
  // intentionally unhandled: no ack, desktop times out, user sees the
  // failure in the Hardware Buddy window. No partial state on our side.
  return false;
}

// Stubs so the character-related state hooks data.h (via stats) would
// otherwise call still link. Nothing to report: no xfer ever runs.
inline bool     xferActive()   { return false; }
inline uint32_t xferProgress() { return 0; }
inline uint32_t xferTotal()    { return 0; }
