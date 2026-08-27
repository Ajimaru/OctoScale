#pragma once
#include <Arduino.h>

// encoder.h — EC11 rotary encoder (A/B) + encoder push + separate start button (KO).
// Module S11-05. A=15, B=16, PUSH=17, KO=18 (all INPUT_PULLUP, other side to GND).
// Rotation is interrupt-based (no poll losses), buttons are debounced.
//
// Usage: encoderInit() in setup(), encoderTick() in loop(). State via
// encoderPosition()/encoderTakeDelta() + encoderPush*/encoderStart*.

extern const int ENC_A;
extern const int ENC_B;
extern const int ENC_SW;
extern const int KEY_START;

// --- Rotary encoder (quadrature, ISR with a state table) -------------------
// Robust against both bounce and missed edges: BOTH pins (A+B) trigger the interrupt
// (CHANGE). A lookup table maps old+new 2-bit state to a VALID quadrature transition
// (+1/-1); bounce/double states resolve to 0 and are dropped. An EC11 produces 4 valid
// transitions per detent, so we accumulate in g_encSub and only emit a detent
// (g_encPos +/-1) at +/-4 — no skipping, no missed step, exactly one menu step per detent.
static volatile long g_encPos = 0;      // absolute position (detents)
static long g_encLastReported = 0;      // for delta queries
static volatile int8_t g_encSub = 0;    // sub-steps within a detent (-4..+4)
static volatile uint8_t g_encPrev = 0;  // previous 2-bit state (A<<1|B)

// Quadrature transition table: index = (prev<<2)|cur, value = +1/-1 (valid) or 0.
static const int8_t ENC_QTAB[16] = {
   0, -1, +1,  0,
  +1,  0,  0, -1,
  -1,  0,  0, +1,
   0, +1, -1,  0
};

static void IRAM_ATTR encIsr() {
  uint8_t cur = (uint8_t)((digitalRead(ENC_A) << 1) | digitalRead(ENC_B));
  int8_t d = ENC_QTAB[(g_encPrev << 2) | cur];
  g_encPrev = cur;
  if (d == 0) return;                    // invalid (bounce) -> ignore
  g_encSub += d;
  if (g_encSub >= 4)      { g_encPos++; g_encSub = 0; }   // full detent forward
  else if (g_encSub <= -4){ g_encPos--; g_encSub = 0; }   // full detent backward
}

// --- Buttons (interrupt-latched, active LOW) --------------------------------
// Earlier polling-based debounce (btnUpdate in encoderTick) lost presses whenever the
// task was stuck in a blocking PN5180 read between two encoderTick() calls (up to
// ~500 ms): a short press+release fell entirely between two reads. Now a FALLING ISR
// latches every press IMMEDIATELY (with timestamp debounce), regardless of what the
// task is doing. Consumed via encoderPushPressed().
struct IsrButton {
  int pin;
  volatile bool pressed = false;     // latched one-shot event (ISR sets, read clears)
  volatile uint32_t lastEdgeMs = 0;  // debounce timestamp
  volatile uint32_t count = 0;       // cumulative (HTTP status / backlight activity)
};

static IsrButton g_btnPush;   // encoder click (PUSH)
static IsrButton g_btnStart;  // start button (KO)

// Debug-log analysis on real hardware showed double-triggers ~130-140ms apart (not the
// few-ms contact chatter 120ms was sized for -- this button mechanically "re-bounces"
// on release over a longer window). 250 ms covers that with margin; still short enough
// that back-to-back intentional presses (menu confirm, then another) don't feel laggy.
static void IRAM_ATTR btnIsr(IsrButton &b) {
  uint32_t now = millis();
  if (now - b.lastEdgeMs < 250) return;  // debounce
  b.lastEdgeMs = now;
  if (digitalRead(b.pin) == LOW) { b.pressed = true; b.count++; }  // real falling edge
}
static void IRAM_ATTR pushIsr()  { btnIsr(g_btnPush); }
static void IRAM_ATTR startIsr() { btnIsr(g_btnStart); }

inline void encoderInit() {
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  pinMode(KEY_START, INPUT_PULLUP);
  g_btnPush.pin = ENC_SW;
  g_btnStart.pin = KEY_START;
  g_encPrev = (uint8_t)((digitalRead(ENC_A) << 1) | digitalRead(ENC_B));
  attachInterrupt(digitalPinToInterrupt(ENC_A), encIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_SW), pushIsr, FALLING);
  attachInterrupt(digitalPinToInterrupt(KEY_START), startIsr, FALLING);
}

// Buttons run entirely via ISR now; kept as a no-op for API compatibility.
inline void encoderTick() {}

inline long encoderPosition() { return g_encPos; }

// Change since the last call (for "N steps turned"). Resets the counter.
inline long encoderTakeDelta() {
  long now = g_encPos;
  long d = now - g_encLastReported;
  g_encLastReported = now;
  return d;
}

// One-shot flag: was PUSH/KO pressed since the last check? (consumes the ISR latch)
inline bool encoderPushPressed()  { bool e = g_btnPush.pressed;  g_btnPush.pressed = false;  return e; }
inline bool encoderStartPressed() { bool e = g_btnStart.pressed; g_btnStart.pressed = false; return e; }

inline bool encoderPushDown()  { return digitalRead(g_btnPush.pin) == LOW; }
inline bool encoderStartDown() { return digitalRead(g_btnStart.pin) == LOW; }

inline uint32_t encoderPushCount()  { return g_btnPush.count; }
inline uint32_t encoderStartCount() { return g_btnStart.count; }
