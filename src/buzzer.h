#pragma once
#include <Arduino.h>
#include <driver/gpio.h>  // gpio_reset_pin() (buzzerToneOff: fully clear the LEDC pad function)

// buzzer.h — small signal buzzer (OK/error tones on NFC read/write/load).
//
// Supports TWO part types (switchable via g_buzMode):
//   ACTIVE  (built-in oscillator): on/off only -> GPIO HIGH produces the fixed tone.
//           No frequency/volume control; patterns only vary by timing.
//   PASSIVE (bare piezo): needs a PWM square wave -> LEDC generates the pitch
//           (g_buzFreq) and rough volume via duty cycle (g_buzVol).
//
// Fully NON-BLOCKING: buzzerPlay() queues a short tone sequence, buzzerTick() (in
// loop() + pn5180Task) advances it over time. Never blocks the NFC reader (core 0)
// or the web server (core 1).
//
// Control globals live in main.cpp (NVS-persistent):
//   g_buzEnabled (bool), g_buzMode (0=active,1=passive), g_buzVol (0..255), g_buzFreq (Hz).

extern const int BUZZER_PIN;      // buzzer + pin (-1 = no buzzer)
extern bool     g_buzEnabled;     // master on/off (TFT + web UI)
extern uint8_t  g_buzMode;        // 0 = active (on/off), 1 = passive (LEDC tone)
extern uint8_t  g_buzVol;         // volume 0..255 (passive only, duty cycle)
extern uint16_t g_buzFreq;        // base pitch in Hz (passive only)
extern Adafruit_NeoPixel pixel;   // onboard WS2812 status LED (main.cpp, declared before this include)

// Synced status LED: on (given color) exactly while a tone segment is sounding, off in
// the gaps -> the LED visibly "beeps" along with the buzzer instead of a separate,
// unrelated flash. 0 = no LED sync for this sequence (LED left as whatever it was).
static uint32_t g_buzLedColor = 0;
static bool     g_buzLedOn = false;

// LEDC channel for passive mode. NOT 6 (tried before): channel/timer mapping on this
// core is timer = (chan/2)%4, so channel 6 shares timer 3 with the backlight's channel
// 7 -> every buzzer tone re-configured that timer's frequency, which also retuned the
// backlight PWM and left a faint leftover hiss until power-cycle. Channel 0 -> timer 0,
// fully separate from the backlight (channel 7 -> timer 3).
static const int BUZ_PWM_CH = 0;

// --- Tone sequence (non-blocking) -------------------------------------------
// A sequence of up to 6 segments: (freqHz, onMs, gapMs). freq=0 -> silent.
struct BuzSeg { uint16_t freq; uint16_t onMs; uint16_t gapMs; };
static const int BUZ_MAX_SEG = 6;

static BuzSeg   g_buzSeq[BUZ_MAX_SEG];
static int      g_buzSegCount = 0;
static int      g_buzSegIdx  = -1;   // -1 = idle
static uint32_t g_buzSegUntil = 0;   // end of the current phase (millis)
static bool     g_buzInGap   = false;

// Drives the WS2812 status LED in sync with the current tone phase (on/off), if this
// sequence has a color set. Safe to call every phase change (setPixelColor+show is
// cheap, one LED).
static inline void buzzerLedSet(bool on) {
  if (!g_buzLedColor) return;
  g_buzLedOn = on;
  pixel.setPixelColor(0, on ? g_buzLedColor : 0);
  pixel.show();
}

// Turn the tone on (mode-dependent). f only matters in passive mode.
static inline void buzzerToneOn(uint16_t f) {
  buzzerLedSet(true);
  if (BUZZER_PIN < 0) return;
  if (g_buzMode == 1) {                       // passive: LEDC tone
    uint16_t freq = f ? f : (g_buzFreq ? g_buzFreq : 2700);
    ledcSetup(BUZ_PWM_CH, freq, 8);
    ledcAttachPin(BUZZER_PIN, BUZ_PWM_CH);
    // Duty controls volume: ~50% is loudest for a square wave, so g_buzVol (0..255)
    // maps onto 0..127 duty. A piezo's perceived loudness isn't linear in duty cycle
    // (it's already fairly loud at a low duty) -> square the fraction so low slider
    // values actually sound quieter instead of barely-below-max.
    float frac = (float)g_buzVol / 255.0f;
    uint32_t duty = (uint32_t)(frac * frac * 127.0f);
    if (g_buzVol > 0 && duty == 0) duty = 1;  // audible floor, not silent
    ledcWrite(BUZ_PWM_CH, duty);
  } else {                                    // active: just HIGH
    ledcDetachPin(BUZZER_PIN);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, HIGH);
  }
}

static inline void buzzerToneOff() {
  buzzerLedSet(false);
  if (BUZZER_PIN < 0) return;
  if (g_buzMode == 1) {
    ledcWrite(BUZ_PWM_CH, 0);
    ledcDetachPin(BUZZER_PIN);
    // ledcDetachPin() alone can leave a faint residual hiss on some cores (the pin's
    // pad function doesn't always fully revert to plain GPIO) -> force it back to a
    // driven digital output explicitly, not just rely on the detach.
    gpio_reset_pin((gpio_num_t)BUZZER_PIN);
  }
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
}

inline void buzzerInit() {
  if (BUZZER_PIN < 0) return;
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  g_buzSegIdx = -1;
}

// Queues a sequence (replaces any running one). Starts immediately. ledColor=0 (default)
// -> no LED sync for this call; otherwise the status LED pulses that color exactly
// while each segment's tone sounds (see buzzerLedSet/buzzerToneOn/Off).
static void buzzerPlay(const BuzSeg *segs, int n, uint32_t ledColor = 0) {
  g_buzLedColor = ledColor;
  if (BUZZER_PIN < 0 || !g_buzEnabled) return;
  if (n > BUZ_MAX_SEG) n = BUZ_MAX_SEG;
  for (int i = 0; i < n; i++) g_buzSeq[i] = segs[i];
  g_buzSegCount = n;
  g_buzSegIdx = 0;
  g_buzInGap = false;
  g_buzSegUntil = millis() + (segs[0].onMs);
  buzzerToneOn(segs[0].freq);
}

// Call from loop()/task: advances the sequence over time.
inline void buzzerTick() {
  if (BUZZER_PIN < 0 || g_buzSegIdx < 0) return;
  if ((int32_t)(millis() - g_buzSegUntil) < 0) return;   // phase still running

  if (!g_buzInGap) {
    buzzerToneOff();  // tone phase done -> this segment's gap
    uint16_t gap = g_buzSeq[g_buzSegIdx].gapMs;
    if (gap > 0) { g_buzInGap = true; g_buzSegUntil = millis() + gap; return; }
  }
  g_buzInGap = false;
  g_buzSegIdx++;
  if (g_buzSegIdx >= g_buzSegCount) { g_buzSegIdx = -1; g_buzLedColor = 0; buzzerToneOff(); return; }
  const BuzSeg &s = g_buzSeq[g_buzSegIdx];
  g_buzSegUntil = millis() + s.onMs;
  buzzerToneOn(s.freq);
}

// --- preset patterns ---------------------------------------------------------
// (Frequencies only matter in passive mode; active mode is a fixed tone, timing only.)
// Each preset also pulses the status LED in sync with the tone (see buzzerPlay's
// ledColor param) -> same color family as the old ledFlash() calls at the call sites.

// (kept as inline calls below rather than static consts: pixel.Color() needs pixel,
// which is only declared as extern above -> fine as a function call, not at file scope)
static inline uint32_t buzLedGreen() { return pixel.Color(0, 100, 0); }
static inline uint32_t buzLedRed()   { return pixel.Color(100, 0, 0); }
static inline uint32_t buzLedCyan()  { return pixel.Color(0, 60, 60); }

// OK: a short bright beep.
inline void buzzerOk() {
  static const BuzSeg s[] = {{2700, 90, 0}};
  buzzerPlay(s, 1, buzLedGreen());
}
// Success (spool loaded / weight saved): two rising beeps.
inline void buzzerSuccess() {
  static const BuzSeg s[] = {{2300, 80, 60}, {3200, 120, 0}};
  buzzerPlay(s, 2, buzLedGreen());
}
// Tag read: a very short click.
inline void buzzerRead() {
  static const BuzSeg s[] = {{3000, 45, 0}};
  buzzerPlay(s, 1, buzLedCyan());
}
// Error: a low double tone.
inline void buzzerError() {
  static const BuzSeg s[] = {{1400, 160, 90}, {1400, 260, 0}};
  buzzerPlay(s, 2, buzLedRed());
}

// Short test tone (web UI/TFT "test").
inline void buzzerTest() { buzzerOk(); }
