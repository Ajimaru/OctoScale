#pragma once
#include <Arduino.h>

// display.h — ST7789 TFT (module S11-05, 2.4" 320x240) via TFT_eSPI.
//
// HSPI bus, dedicated pins (SCK=42/MOSI=44, CS=38/DC=39/RST=40/BLK=41). Full TFT_eSPI
// config comes from build_flags (platformio.ini, [s3_base]) — no setup here.

#include <TFT_eSPI.h>
#include "OctoLogo.h"  // RGB565 bitmap, 200x200 (converted from assets/octoscale_logo.png)

static TFT_eSPI g_tft = TFT_eSPI();
static bool g_tftReady = false;

// Backlight via LEDC PWM (20 kHz, above flicker perception) instead of hard HIGH —
// smoother LED current, less sensitive to supply noise. 0..255 brightness.
static const int BL_PIN = 41;          // backlight pin (module "BLK")
static const int BL_PWM_CH = 7;        // LEDC channel (high, avoids conflicts)
static const int BL_PWM_FREQ = 20000;  // 20 kHz
static const int BL_PWM_RES = 8;       // 8-bit -> 0..255

inline void displaySetBacklight(uint8_t level) {
  ledcWrite(BL_PWM_CH, level);
}

inline bool displayInit() {
  g_tft.init();
  g_tft.setRotation(1);           // landscape 320x240
  // This panel needs inversion OFF (ON showed 0x0000/TFT_BLACK as white).
  g_tft.invertDisplay(false);
  g_tft.fillScreen(TFT_BLACK);

  // Set up backlight PWM AFTER g_tft.init(): TFT_eSPI would otherwise drive the pin
  // hard HIGH during init (if it knows TFT_BL) and override our LEDC. TFT_BL is
  // deliberately not given to the library, so we have full control here.
  ledcSetup(BL_PWM_CH, BL_PWM_FREQ, BL_PWM_RES);
  ledcAttachPin(BL_PIN, BL_PWM_CH);
  ledcWrite(BL_PWM_CH, 255);

  g_tftReady = true;
  return true;
}

// Panel power-down (third idle stage, after dimming and the logo screensaver).
// Backlight PWM to 0 AND the ST7789 into sleep: the backlight alone is what makes the
// panel look off, but the controller keeps driving the pixel matrix and burning ~15mA
// while doing it, so both go. Order matters in each direction -- kill the light before
// the controller sleeps, and let the controller wake (it needs ~120ms per datasheet
// before it accepts drawing again) before the light comes back, otherwise the wake
// shows a frame of garbage or a half-initialised image.
static bool g_tftAsleep = false;

inline void displaySleep() {
  if (!g_tftReady || g_tftAsleep) return;
  displaySetBacklight(0);
  g_tft.writecommand(0x28);   // DISPOFF
  g_tft.writecommand(0x10);   // SLPIN
  delay(20);                  // datasheet: no further commands for 5ms after SLPIN
  g_tftAsleep = true;
}

// Wakes the panel. Returns true if it actually woke something (caller then needs to
// repaint -- sleep loses nothing in RAM, but the menu redraws anyway on the state
// change that woke us, and a forced repaint avoids showing a stale pre-sleep frame).
inline bool displayWake(uint8_t level) {
  if (!g_tftReady || !g_tftAsleep) return false;
  g_tft.writecommand(0x11);   // SLPOUT
  delay(120);                 // datasheet: 120ms before the next command after SLPOUT
  g_tft.writecommand(0x29);   // DISPON
  g_tftAsleep = false;
  displaySetBacklight(level);
  return true;
}

// Full-screen red/green/blue/white cycle — confirms the panel lights up and SPI works.
inline void displayDiag() {
  if (!g_tftReady) return;
  uint16_t cols[4] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
  const char *names[4] = {"RED", "GREEN", "BLUE", "WHITE"};
  for (int r = 0; r < 2; r++) {
    for (int i = 0; i < 4; i++) {
      g_tft.fillScreen(cols[i]);
      Serial.printf("TFT diag: full screen %s\n", names[i]);
      delay(800);
    }
  }
}

// Draws the logo bitmap centered at (cx,cy). Uses pushImage (fast, single SPI burst)
// with PROGMEM data read via a small line buffer (pushImage wants a RAM pointer).
inline void displayLogo(int cx, int cy) {
  if (!g_tftReady) return;
  static uint16_t lineBuf[LOGO_W];
  int x0 = cx - LOGO_W / 2, y0 = cy - LOGO_H / 2;
  for (int row = 0; row < LOGO_H; row++) {
    memcpy_P(lineBuf, &OctoLogo[row * LOGO_W], LOGO_W * sizeof(uint16_t));
    g_tft.pushImage(x0, y0 + row, LOGO_W, 1, lineBuf);
  }
}

// Same as displayLogo(), but recolors the (near-)white background to bgColor on the
// fly, so the logo sits on a themed (e.g. dark) background instead of always keeping
// its own baked-in white. OctoLogo.h is a flat-illustration JPEG converted to RGB565 --
// its background is white but not always the EXACT 0xFFFF (JPEG compression leaves
// faint off-white artifacts near the silhouette edges), so an exact-match transparency
// key (TFT_eSPI's pushImage(...,transparent) overload) would leave a visible light
// halo around the octopus on a dark background. Instead, every pixel whose R/G/B
// channels are all above a threshold (any faint near-white shade, not just pure white)
// is replaced with bgColor before the line is pushed -- catches the compression
// artifacts too. The actual logo colors (teal/dark navy, see assets/octoscale_logo.png)
// are all well below the threshold on at least one channel, so the artwork itself is
// never affected.
inline void displayLogoThemed(int cx, int cy, uint16_t bgColor) {
  if (!g_tftReady) return;
  static uint16_t lineBuf[LOGO_W];
  int x0 = cx - LOGO_W / 2, y0 = cy - LOGO_H / 2;
  // RGB565 channel thresholds (5/6/5 bits) -- "near-white" means all three channels
  // are within the top ~22% of their range. Verified against the actual bitmap data
  // (not guessed): JPEG compression leaves ~800 off-white edge pixels as light as
  // 0xEF9E, well caught by this threshold; the logo's own teal/navy colors are all far
  // below it on at least one channel (teal's green channel maxes out around 41/63,
  // navy is dark on all three) -- render-tested at three threshold levels to confirm
  // no visible halo around the silhouette and no bite taken out of the artwork itself.
  const uint8_t R_MIN = 24, G_MIN = 48, B_MIN = 24;  // out of 31/63/31
  for (int row = 0; row < LOGO_H; row++) {
    memcpy_P(lineBuf, &OctoLogo[row * LOGO_W], LOGO_W * sizeof(uint16_t));
    for (int i = 0; i < LOGO_W; i++) {
      uint16_t px = lineBuf[i];
      uint8_t r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
      if (r >= R_MIN && g >= G_MIN && b >= B_MIN) lineBuf[i] = bgColor;
    }
    g_tft.pushImage(x0, y0 + row, LOGO_W, 1, lineBuf);
  }
}

// Half-size logo (nearest-neighbour, every other pixel/row) for the bouncing
// screensaver -- at full 180x180 the logo covers most of a 240x320 panel, which leaves
// no room to actually bounce. Same near-white -> bgColor keying as displayLogoThemed().
static const int LOGO_SMALL_W = LOGO_W / 2;
static const int LOGO_SMALL_H = LOGO_H / 2;

// Pre-scaled, key-colour-applied copy of the logo, built once (see
// displayLogoSmallInit()) and re-blitted from RAM every screensaver frame. The
// original per-frame version re-walked and re-keyed all 8100 source pixels AND pushed
// them to the panel as 90 separate one-row SPI transfers -- SPI's per-call overhead on
// that many tiny transfers was the actual bottleneck (not the pixel math), and is what
// made the animation look choppy. One cached buffer + one pushImage() call per frame
// fixes both: no recompute, and a single wide transfer instead of ninety thin ones.
// bgColor is baked in at init time -- fine here because the screensaver always uses
// MENU_BG, and menuRenderScreensaver() re-inits on every entry, which also covers a
// theme change (dark/light) made just before the screensaver kicks in.
static uint16_t g_logoSmallCache[LOGO_SMALL_W * LOGO_SMALL_H];
static bool     g_logoSmallCacheValid = false;
static uint16_t g_logoSmallCacheBg = 0;

inline void displayLogoSmallInit(uint16_t bgColor) {
  static uint16_t src[LOGO_W];
  const uint8_t R_MIN = 24, G_MIN = 48, B_MIN = 24;  // see displayLogoThemed()
  for (int row = 0; row < LOGO_SMALL_H; row++) {
    memcpy_P(src, &OctoLogo[(row * 2) * LOGO_W], LOGO_W * sizeof(uint16_t));
    uint16_t *dstRow = &g_logoSmallCache[row * LOGO_SMALL_W];
    for (int i = 0; i < LOGO_SMALL_W; i++) {
      uint16_t px = src[i * 2];
      uint8_t r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
      dstRow[i] = (r >= R_MIN && g >= G_MIN && b >= B_MIN) ? bgColor : px;
    }
  }
  g_logoSmallCacheBg = bgColor;
  g_logoSmallCacheValid = true;
}

inline void displayLogoSmall(int x0, int y0) {
  if (!g_tftReady || !g_logoSmallCacheValid) return;
  g_tft.pushImage(x0, y0, LOGO_SMALL_W, LOGO_SMALL_H, g_logoSmallCache);
}


// Splash screen: white background (matches the logo's own background) + title + logo
// + version. Layout top-to-bottom so nothing overlaps: title (0-30), logo (centered
// in the remaining space), version pinned to the bottom.
inline void displaySplash(const char *version) {
  if (!g_tftReady) return;
  g_tft.fillScreen(TFT_WHITE);
  int w = g_tft.width(), h = g_tft.height();
  g_tft.setTextColor(TFT_BLACK, TFT_WHITE);
  g_tft.setTextDatum(MC_DATUM);
  g_tft.drawString("OctoScale", w / 2, 16, 4);
  // Logo (LOGO_H px) centered in the space between the title and the version line.
  int top = 32, bottom = h - 18;
  displayLogo(w / 2, top + (bottom - top) / 2);
  g_tft.drawString(String("v") + version, w / 2, h - 8, 4);
  g_tft.setTextDatum(TL_DATUM);
}

// Continuous diagnostic: cycles the full-screen color every second (for wiring checks).
inline void displayDiagTick() {
  if (!g_tftReady) return;
  static uint32_t last = 0;
  static uint8_t i = 0;
  if (millis() - last < 1000) return;
  last = millis();
  uint16_t cols[4] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
  g_tft.fillScreen(cols[i & 3]);
  i++;
}

inline void displayWeight(float grams) {
  if (!g_tftReady) return;
  g_tft.fillRect(0, g_tft.height() - 40, g_tft.width(), 40, TFT_BLACK);
  g_tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  g_tft.setTextDatum(BC_DATUM);
  g_tft.drawString(String(grams, 1) + " g", g_tft.width() / 2, g_tft.height() - 4, 4);
  g_tft.setTextDatum(TL_DATUM);
}
