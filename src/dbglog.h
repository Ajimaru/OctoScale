#pragma once
#include <Arduino.h>

// dbglog.h — small in-RAM ring buffer for a web-UI debug console. No serial access on
// this board (external power) -> this lets us see traces (esp. PN5180 write/read) live
// in the browser instead of guessing at crashes blind. Off by default (near-zero cost);
// only active while a WebUI checkbox has it enabled (see /debuglog in main.cpp).

// 400 lines. This was 1500 for a while and that turned out to be a real problem, not
// just a bigger buffer: a full buffer is ~1500 heap-allocated Strings of 40-90 B each
// (~100 KB), and /debuglog then has to serialize ALL of them into one JSON String --
// which needs roughly that much again, contiguously, in a single allocation. On a
// 320 KB-heap board with a Mifare tag sitting on the reader (every 500 ms poll writes
// TWO auth-trace lines, so the buffer fills in ~6 minutes), that allocation eventually
// fails and takes the web server down with it: the firmware keeps running -- scale,
// NFC, display all fine -- but WiFi/HTTP goes dead until a reset. Diagnosed live by
// watching the serial console while the network died.
// 400 lines still covers a full multi-tag debug session (a single Mifare Extended
// write/erase runs 30-40 lines) at a quarter of the peak memory. If more history is
// ever needed, stream /debuglog in chunks instead of raising this again.
// NOTE: this number is also quoted in the Debug console blurb in web_ui.h -- keep the
// two in sync when changing it.
static const int DBGLOG_LINES = 400;
// Refuse to serialize the buffer below this much free heap (see /debuglog in main.cpp).
// A full 400-line dump needs roughly 30-40 KB contiguous for the JSON String; 80 KB
// leaves comfortable margin for the WiFi stack and the response send itself.
static const uint32_t DBGLOG_MIN_HEAP_FOR_DUMP = 80000;
static String g_dbgLogBuf[DBGLOG_LINES];
static int g_dbgLogHead = 0;      // next slot to write
static uint32_t g_dbgLogSeq = 0;  // monotonic line counter (lets the client detect gaps)
volatile bool g_dbgLogEnabled = false;

// Fully inert while disabled (no Serial I/O, no formatting) -> safe to sprinkle into
// hot paths like the PN5180 write retry loop.
inline void dbgLog(const String &line) {
  if (!g_dbgLogEnabled) return;
  Serial.println(line);
  g_dbgLogSeq++;
  // ms, not s: debouncing/double-trigger bugs live in the tens-of-ms range and are
  // invisible at 1s resolution.
  char prefix[16];
  snprintf(prefix, sizeof(prefix), "[%lu] ", (unsigned long)millis());
  g_dbgLogBuf[g_dbgLogHead] = String(prefix) + line;
  g_dbgLogHead = (g_dbgLogHead + 1) % DBGLOG_LINES;
}

inline void dbgLogf(const char *fmt, ...) {
  if (!g_dbgLogEnabled) return;
  char buf[160];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  dbgLog(String(buf));
}
