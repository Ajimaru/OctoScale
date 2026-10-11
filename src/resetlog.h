#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_system.h>

// resetlog.h — persistent reset history. esp_reset_reason() only ever reports the LAST
// reset, so a boot loop that has settled by the time anyone looks leaves nothing behind
// except "Power-On" or "Software". Every boot appends one entry to a small ring in NVS:
// the reset reason of this boot (= how the previous boot ended) and how long the
// previous boot had been running. A run of short uptimes with reason 9 is a brownout
// loop; one panic after three days is something else entirely.
//
// The previous uptime comes from RTC slow memory, which keeps its content across every
// reset that leaves the chip powered (software, panic, watchdogs, the brownout
// detector's own reset). resetLogTick() writes the uptime there once a second and the
// next boot reads it back. After a real power loss the memory holds garbage; the
// inverted check word rejects that, and the entry then carries no uptime. A power-on
// entry WITH an uptime means the supply dropped too briefly to clear RTC memory.

static const int RESETLOG_LEN = 20;
static const uint32_t RESETLOG_UNKNOWN = 0xFFFFFFFFu;

struct ResetLogEntry {
  uint32_t boot;        // running boot number, 1 = first boot after the log was cleared
  uint32_t prevUptime;  // seconds the previous boot ran, RESETLOG_UNKNOWN if not known
  uint8_t reason;       // esp_reset_reason_t of this boot
  uint8_t pad[3];
};

struct ResetLog {
  uint32_t boots;                 // boots recorded since the log was cleared
  ResetLogEntry e[RESETLOG_LEN];  // boot n sits in slot (n - 1) % RESETLOG_LEN
};

// Written in setup() and by /resetlog/clear, read by the web handlers -- all on the
// loop task, so no lock.
static ResetLog g_resetLog;

RTC_NOINIT_ATTR static uint32_t g_rtcUptime;
RTC_NOINIT_ATTR static uint32_t g_rtcUptimeCheck;  // ~g_rtcUptime while valid

// Once, early in setup(): before WiFi starts, whose start-up current is a classic
// brownout trigger, so a boot that dies there has already been counted.
inline void resetLogBoot() {
  uint32_t prev = g_rtcUptimeCheck == ~g_rtcUptime ? g_rtcUptime : RESETLOG_UNKNOWN;
  Preferences p;
  p.begin("resetlog", false);
  if (p.getBytes("log", &g_resetLog, sizeof(g_resetLog)) != sizeof(g_resetLog))
    memset(&g_resetLog, 0, sizeof(g_resetLog));
  g_resetLog.boots++;
  ResetLogEntry &e = g_resetLog.e[(g_resetLog.boots - 1) % RESETLOG_LEN];
  e.boot = g_resetLog.boots;
  e.prevUptime = prev;
  e.reason = (uint8_t)esp_reset_reason();
  p.putBytes("log", &g_resetLog, sizeof(g_resetLog));
  p.end();
  g_rtcUptime = 0;
  g_rtcUptimeCheck = ~0u;
}

// Once a second from loop(). Uptime first, check word second: a reset between the two
// writes leaves a mismatch, which the next boot reads as "unknown" rather than wrong.
inline void resetLogTick() {
  uint32_t up = millis() / 1000;
  g_rtcUptime = up;
  g_rtcUptimeCheck = ~up;
}

inline void resetLogClear() {
  memset(&g_resetLog, 0, sizeof(g_resetLog));
  Preferences p;
  p.begin("resetlog", false);
  p.remove("log");
  p.end();
}

// Newest first, at most max entries: {boot, reason, prevUptime (null if unknown)}.
inline void resetLogToJson(JsonArray arr, uint32_t max) {
  uint32_t n = g_resetLog.boots < (uint32_t)RESETLOG_LEN ? g_resetLog.boots : RESETLOG_LEN;
  if (n > max) n = max;
  for (uint32_t k = 0; k < n; k++) {
    const ResetLogEntry &e = g_resetLog.e[(g_resetLog.boots - 1 - k) % RESETLOG_LEN];
    JsonObject o = arr.add<JsonObject>();
    o["boot"] = e.boot;
    o["reason"] = e.reason;
    if (e.prevUptime == RESETLOG_UNKNOWN) o["prevUptime"] = nullptr;
    else o["prevUptime"] = e.prevUptime;
  }
}
