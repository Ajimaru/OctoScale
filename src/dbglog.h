#pragma once
#include <Arduino.h>
#include <utility>

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
// NOTE: this number is also quoted in the Debug console blurb in web_ui.h, and the
// console keeps as many lines itself (dbgshow() there) -- keep all three in sync.
static const int DBGLOG_LINES = 400;
// Refuse to serialize the buffer below this much free heap (see /debuglog in main.cpp).
// A full 400-line dump needs roughly 30-40 KB contiguous for the JSON String; 80 KB
// leaves comfortable margin for the WiFi stack and the response send itself.
static const uint32_t DBGLOG_MIN_HEAP_FOR_DUMP = 80000;
// Line n (n = g_dbgLogSeq right after storing it, counting from 1) sits in slot
// n % DBGLOG_LINES until line n + DBGLOG_LINES replaces it.
static String g_dbgLogBuf[DBGLOG_LINES];
static uint32_t g_dbgLogSeq = 0;  // newest line's number (monotonic; lets the client detect gaps)
volatile bool g_dbgLogEnabled = false;

// Writers run on both cores at once (pn5180Task, scaleTask and dbPingTask on core 0,
// loop() and the web handlers on core 1) while /debuglog reads. Unguarded, two writers
// could assign the same slot concurrently and a reader could copy a String a writer is
// freeing -- heap corruption either way. A mutex, not a spinlock: storing and copying a
// line touch the heap, which does not belong in a critical section with interrupts off.
// It is held for one line at a time, never across a whole dump, so no writer waits on the
// web server serializing the buffer, and a task that does wait sleeps instead of spinning.
static StaticSemaphore_t g_dbgLogLockBuf;
static SemaphoreHandle_t g_dbgLogLock = xSemaphoreCreateMutexStatic(&g_dbgLogLockBuf);
static uint32_t g_dbgLogDropped = 0;  // lines lost to a lock timeout (see dbgLogStore())

// Stores one line, no serial output.
inline void dbgLogStore(const String &line) {
  // ms, not s: debouncing/double-trigger bugs live in the tens-of-ms range and are
  // invisible at 1s resolution.
  char prefix[16];
  snprintf(prefix, sizeof(prefix), "[%lu] ", (unsigned long)millis());
  String entry = String(prefix) + line;  // the new line's heap work, before the lock
  // Bounded wait: a writer never stalls its task for longer than this. The lock is free
  // again after one line's copy, so a drop means something holds it far too long.
  if (xSemaphoreTake(g_dbgLogLock, pdMS_TO_TICKS(10)) != pdTRUE) {
    __atomic_fetch_add(&g_dbgLogDropped, 1, __ATOMIC_RELAXED);
    return;
  }
  // Swap, not assign: the evicted line lands in `entry` and is freed after the unlock.
  std::swap(g_dbgLogBuf[(g_dbgLogSeq + 1) % DBGLOG_LINES], entry);
  g_dbgLogSeq++;
  xSemaphoreGive(g_dbgLogLock);
}

// Fully inert while disabled (no Serial I/O, no formatting) -> safe to sprinkle into
// hot paths like the PN5180 write retry loop.
inline void dbgLog(const String &line) {
  if (!g_dbgLogEnabled) return;
  Serial.println(line);
  dbgLogStore(line);
}

inline void dbgLogf(const char *fmt, ...) {
  if (!g_dbgLogEnabled) return;
  char buf[160];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  dbgLog(String(buf));
}

// Hands every stored line newer than `since` to fn, oldest first, and returns the number
// of the newest line covered; lines logged meanwhile are left for the next call. The lock
// is taken per line, so even a full dump makes a writer wait for one line copy at most.
// fn runs under the lock and must not log. Lines overwritten before they could be read
// (the caller fell more than DBGLOG_LINES behind) are counted in `lost`.
template <typename F>
inline uint32_t dbgLogRead(uint32_t since, uint32_t &lost, F fn) {
  lost = 0;
  const uint32_t newest = __atomic_load_n(&g_dbgLogSeq, __ATOMIC_RELAXED);
  uint32_t k = since + 1;
  if (newest >= (uint32_t)DBGLOG_LINES && k <= newest - DBGLOG_LINES) {
    lost = newest - DBGLOG_LINES + 1 - k;  // overwritten before this call
    k = newest - DBGLOG_LINES + 1;
  }
  for (; k <= newest; k++) {
    if (xSemaphoreTake(g_dbgLogLock, pdMS_TO_TICKS(50)) != pdTRUE) return k - 1;  // rest next call
    const bool kept = g_dbgLogSeq - k < (uint32_t)DBGLOG_LINES;  // not overwritten meanwhile
    if (kept) fn(g_dbgLogBuf[k % DBGLOG_LINES]);
    xSemaphoreGive(g_dbgLogLock);
    if (!kept) lost++;
  }
  return newest;
}
