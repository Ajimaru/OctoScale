#pragma once
#include <Arduino.h>

// menu.h — TFT menu system, driven by the EC11 rotary encoder (rotate / PUSH / KO).
//
// Included in main.cpp AFTER the flow globals (needs g_flowState, g_flow*, g_octo[],
// g_weight, g_tareReq, g_flowMenuReq/Arg, g_nfcDebug) and AFTER display.h (g_tft) +
// encoder.h + buzzer.h.
//
// The menu drives the SAME g_flowState machine as the web UI (not a second system):
//  - Idle (FLOW_IDLE): large live weight + status lines. PUSH/KO -> device menu.
//  - Tag present: the menu mirrors the flow (choose action/printer/tool via the encoder).
// Blocking transitions (printer/tool/weigh -> octo* HTTP) are NOT run here (core 0) but
// delegated to loop() (core 1) via g_flowMenuReq. Non-blocking transitions (choose action,
// cancel) are set directly.
//
// Controls: rotate = move selection, PUSH = OK/confirm, KO = back/cancel.

#include <TFT_eSPI.h>
#include <WiFi.h>
#include "version.h"
#include "OctoFontBig.h"      // large antialiased font (Arial Bold 56pt) for weight/type
#include "OctoFontMid.h"      // mid antialiased font (Arial Bold 26pt) for titles/lists

// --- Palette (theme-switchable at runtime: dark / light) -------------------
// Neutrals swap between dark and light; the accent (cyan) + status colors stay put.
// menuApplyTheme() reassigns these from g_menuDark.
// The ACCENT (weight readout + selection bar) is amber in dark mode, cyan in light mode
// -> consistent per theme. Neutrals swap; status green/red/amber stay put.
static const uint16_t ACCENT_AMBER = 0xFD20;   // amber (dark theme accent)
static const uint16_t ACCENT_CYAN  = 0x05FF;   // cyan  (light theme accent)

static bool g_menuDark = true;         // display theme (toggle in device menu)
static uint16_t MENU_BG     = TFT_BLACK;
static uint16_t MENU_TITLE  = TFT_WHITE;
static uint16_t MENU_RULE   = 0x39C7;
static uint16_t MENU_DIM    = 0xAD75;
static uint16_t MENU_ROW_FG = 0xE71C;
static uint16_t MENU_SEL_BG = ACCENT_AMBER;    // selection bar = accent
static uint16_t MENU_SEL_FG = 0x0000;          // dark text on the accent bar
static uint16_t MENU_WEIGHT = ACCENT_AMBER;    // big readout = accent
static const uint16_t MENU_OK   = 0x2648;      // green
static const uint16_t MENU_ERR  = 0xE8E4;      // red
static const uint16_t MENU_WARN = 0xC300;      // amber
#define MENU_HINT MENU_DIM

// Apply the palette for the current theme (neutrals + accent).
static void menuApplyTheme() {
  if (g_menuDark) {
    MENU_BG = TFT_BLACK; MENU_TITLE = TFT_WHITE; MENU_RULE = 0x39C7;
    MENU_DIM = 0xAD75;   MENU_ROW_FG = 0xE71C;
    MENU_SEL_BG = ACCENT_AMBER; MENU_WEIGHT = ACCENT_AMBER; MENU_SEL_FG = 0x0000;
  } else {
    MENU_BG = TFT_WHITE; MENU_TITLE = 0x0000; MENU_RULE = 0xC618;
    MENU_DIM = 0x632C;   MENU_ROW_FG = 0x2124;
    MENU_SEL_BG = ACCENT_CYAN;  MENU_WEIGHT = ACCENT_CYAN;  MENU_SEL_FG = 0x0000;
  }
}

// --- Menu state ------------------------------------------------------------
enum MenuScreen {
  MENU_FLOW = 0,    // mirrors g_flowState (default)
  MENU_DEVICE,      // device menu (idle -> PUSH/KO): Tare/NFC-Debug/Buzzer/Theme/System
  MENU_SYSINFO,     // system info sub-screen
  MENU_TARED,       // brief "Tared" confirmation, then auto-back to MENU_FLOW
  MENU_SCREENSAVER, // logo shown after g_ssTimeoutSec idle (any input/state change exits)
};

static MenuScreen g_menuScreen = MENU_FLOW;
static int g_menuCursor = 0;          // selection index in the current flow state
static int g_deviceCursor = 0;        // selection index in the device menu
static long g_menuAccum = 0;          // EC11 delta accumulator (divisor 2)
static FlowState g_menuLastState = FLOW_IDLE;
static uint32_t g_menuLastHash = 0xFFFFFFFF;
static bool g_menuForceRedraw = true;
static uint32_t g_menuLastWeightDraw = 0;
static String g_menuLastWeightStr = "";   // last drawn weight string (skip redundant pushes)
static String g_menuLastNfcDebugStr = "";  // last drawn NFC-debug-screen content (skip redundant redraws)
static uint32_t g_menuTaredUntil = 0;     // millis() when MENU_TARED auto-returns to MENU_FLOW
static bool g_menuNfcWriteWasActive = false;  // tracks the NFC-write lock screen (see menuTick)
static bool g_menuNfcResultShown = false;     // result screen (Tag written/failed) drawn once already
static uint32_t g_menuNfcResultUntil = 0;     // millis() when the result screen auto-returns
static bool g_menuNfcResultDismissed = false; // result window elapsed; don't re-show it for THIS write

// Device-menu entries (order = cursor index).
static const int DEV_COUNT = 5;   // Tare / NFC debug / Buzzer / Theme / System info (KO = back)

// --- small drawing helpers -------------------------------------------------

// Top title (smooth mid font, centered). No rule line.
static void menuTitle(const char *t, uint16_t col = MENU_TITLE) {
  g_tft.loadFont(OctoFontMid);
  g_tft.setTextColor(col, MENU_BG);
  g_tft.setTextDatum(MC_DATUM);
  g_tft.drawString(t, g_tft.width() / 2, 20);
  g_tft.unloadFont();
  g_tft.setTextDatum(TL_DATUM);
}

// Bottom control hint (font 2, centered).
static void menuHint(const char *t, uint16_t col = MENU_HINT) {
  g_tft.setTextColor(col, MENU_BG);
  g_tft.setTextDatum(BC_DATUM);
  g_tft.drawString(t, g_tft.width() / 2, g_tft.height() - 6, 2);
  g_tft.setTextDatum(TL_DATUM);
}

// Selection list from y0, row height rowH. Selected row inverted (cyan / dark text).
// Keeps the cursor centered for long lists, clamped at the ends. Smooth mid font.
static void menuDrawList(const char *const *items, int n, int cursor,
                         int y0, int rowH, int maxRows) {
  int w = g_tft.width();
  int first = 0;
  if (n > maxRows) {
    first = cursor - maxRows / 2;
    if (first < 0) first = 0;
    if (first > n - maxRows) first = n - maxRows;
  }
  g_tft.loadFont(OctoFontMid);
  g_tft.setTextDatum(ML_DATUM);
  for (int r = 0; r < maxRows && (first + r) < n; r++) {
    int i = first + r;
    int y = y0 + r * rowH;
    bool sel = (i == cursor);
    g_tft.fillRect(8, y, w - 16, rowH - 5, sel ? MENU_SEL_BG : MENU_BG);
    g_tft.setTextColor(sel ? MENU_SEL_FG : MENU_ROW_FG, sel ? MENU_SEL_BG : MENU_BG);
    g_tft.drawString(items[i], 20, y + (rowH - 5) / 2);
  }
  g_tft.unloadFont();
  g_tft.setTextDatum(TL_DATUM);
}

// Centered status message (smooth mid font) + optional sub line.
// Word-wraps s to fit maxW at the given GLCD font, returns the wrapped lines (greedy
// fill). font must match whatever the caller actually draws the lines with -- wrapping
// against one font's metrics and drawing in a wider one overflows maxW.
static void menuWrapText(const String &s, int maxW, String lines[], int &lineCount, int maxLines, uint8_t font = 2) {
  lineCount = 0;
  int start = 0, n = s.length();
  while (start < n && lineCount < maxLines) {
    int lastSpace = -1, end = start;
    while (end < n) {
      if (s[end] == ' ') lastSpace = end;
      String cand = s.substring(start, end + 1);
      if (g_tft.textWidth(cand, font) > maxW) {
        int brk = (lastSpace >= start) ? lastSpace : end;  // no space -> hard break
        lines[lineCount++] = s.substring(start, brk);
        start = (lastSpace >= start) ? lastSpace + 1 : brk;
        end = start;
        lastSpace = -1;
        continue;
      }
      end++;
    }
    if (end >= n) { lines[lineCount++] = s.substring(start, n); break; }
  }
}

static void menuMessage(const char *title, uint16_t col, const char *sub = nullptr) {
  g_tft.fillScreen(MENU_BG);
  g_tft.loadFont(OctoFontMid);
  g_tft.setTextColor(col, MENU_BG);
  g_tft.setTextDatum(MC_DATUM);
  g_tft.drawString(title, g_tft.width() / 2, g_tft.height() / 2 - 14);
  g_tft.unloadFont();
  if (sub && sub[0]) {
    g_tft.setTextColor(MENU_DIM, MENU_BG);
    static const int MAX_LINES = 3;
    String lines[MAX_LINES];
    int n = 0;
    menuWrapText(String(sub), g_tft.width() - 24, lines, n, MAX_LINES, 4);
    // Font 4 (was 2) needs a taller line pitch than the old 18px, AND more clearance
    // from the title above it: at 3 lines the old H/2+22 anchor put the first line's
    // top edge inside the title's own OctoFontMid glyph box (title sits at H/2-14,
    // barely 14px above) -- found via the on-device screen preview. Anchor the BLOCK's
    // TOP (not center) a fixed 26px below the title's baseline instead, so it only
    // grows downward regardless of line count.
    int y0 = g_tft.height() / 2 + 12;
    for (int i = 0; i < n; i++) g_tft.drawString(lines[i], g_tft.width() / 2, y0 + i * 22, 4);
  }
  g_tft.setTextDatum(TL_DATUM);
}

// --- render per screen / state ---------------------------------------------

// Draws "Label:OK" / "Label:--" (label dim, marker green/red) at (x,y), font 2.
// Returns the x for the next chip (with a gap).
static int menuStatusChip(int x, int y, const char *label, bool ok) {
  g_tft.setTextDatum(TL_DATUM);
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  String lab = String(label) + ":";
  g_tft.drawString(lab, x, y, 2);
  x += g_tft.textWidth(lab, 2) + 3;
  g_tft.setTextColor(ok ? MENU_OK : MENU_ERR, MENU_BG);
  const char *mk = ok ? "OK" : "--";
  g_tft.drawString(mk, x, y, 2);
  x += g_tft.textWidth(mk, 2) + 14;   // gap to next chip
  return x;
}

// Measured width of one chip ("Label:OK") for centering.
static int menuChipWidth(const char *label, bool ok) {
  String lab = String(label) + ":";
  return g_tft.textWidth(lab, 2) + 3 + g_tft.textWidth(ok ? "OK" : "--", 2);
}

// Centered status footer: NFC / Scale / DB / WiFi with colored OK/-- markers.
static void menuFooter() {
  int y = g_tft.height() - 20;
  const int gap = 14;
  bool nfc = pn5180IsReady(), sc = g_scaleReady, db = (g_dbReachable == 1), wf = WiFi.isConnected();
  int total = menuChipWidth("NFC", nfc) + menuChipWidth("Scale", sc)
            + menuChipWidth("DB", db) + menuChipWidth("WiFi", wf) + gap * 3;
  int x = (g_tft.width() - total) / 2;
  if (x < 2) x = 2;
  x = menuStatusChip(x, y, "NFC",   nfc); x += gap - 14;
  x = menuStatusChip(x, y, "Scale", sc);  x += gap - 14;
  x = menuStatusChip(x, y, "DB",    db);  x += gap - 14;
  x = menuStatusChip(x, y, "WiFi",  wf);
}

// Idle: title + status footer. Center area shows either weight (normal) or, when NFC
// debug is on, the detected tag type/UID (filled by menuCenterTick()).
static void menuRenderIdle() {
  g_tft.fillRect(0, 0, g_tft.width(), g_tft.height(), MENU_BG);
  menuTitle(g_nfcDebug ? "NFC Debug" : "OctoScale");
  // Still on the factory default factor -> nudge toward the web UI calibration wizard.
  if (g_calFactor == DEFAULT_CALIBRATION_FACTOR) {
    g_tft.setTextColor(MENU_WARN, MENU_BG);
    g_tft.setTextDatum(MC_DATUM);
    g_tft.drawString("Calibrate scale in web UI", g_tft.width() / 2, g_tft.height() - 38, 2);
    g_tft.setTextDatum(TL_DATUM);
  }
  menuFooter();
}

// Tag type short name for the small line under the weight.
static const char *menuTagShort(PN5180TagType t) {
  return (t == PN5180_TAG_NFCV) ? "NFC-V" : (t == PN5180_TAG_NFCA) ? "NFC-A" : "";
}

// Center of the idle screen (5 Hz), over the layout, no full redraw.
//  - normal: big weight; if a tag is present, its type shows small underneath.
//  - NFC-debug on: full tag info (type big + UID + ATQA/SAK) instead of weight.
static void menuCenterTick() {
  if (millis() - g_menuLastWeightDraw < 200) return;
  g_menuLastWeightDraw = millis();
  int w = g_tft.width();
  g_tft.setTextDatum(MC_DATUM);
  bool present = (g_nfcProbe.type != PN5180_TAG_NONE);

  if (g_nfcDebug && g_flowState == FLOW_IDLE) {
    // Full tag info (changes rarely). Redraw only when the DISPLAYED content actually
    // changes, same fix as the weight sprite above (g_menuLastWeightStr) -- this used
    // to fillRect+redraw unconditionally on every 200 ms tick, which read as a visible
    // flicker on both "no tag" and a present tag (neither string was changing, but the
    // full clear-then-redraw ran anyway).
    long effectiveId = (g_nfcExtCacheHasExtended && g_nfcExtCacheData.databaseId >= 0)
                      ? g_nfcExtCacheData.databaseId : g_nfcProbe.idParsed;
    String state = String(present) + "|" + String((int)g_nfcProbe.type) + "|" + g_nfcProbe.uid
                 + "|" + String(g_nfcExtCacheCapacityBytes) + "|" + String(effectiveId);
    if (state == g_menuLastNfcDebugStr) { g_tft.setTextDatum(TL_DATUM); return; }
    g_menuLastNfcDebugStr = state;

    g_tft.fillRect(0, 44, w, g_tft.height() - 44 - 30, MENU_BG);
    g_tft.loadFont(OctoFontBig);
    g_tft.setTextColor(present ? MENU_WEIGHT : MENU_DIM, MENU_BG);
    g_tft.drawString(present ? menuTagShort(g_nfcProbe.type) : "no tag", w / 2, 74);
    g_tft.unloadFont();
    if (present) {
      g_tft.setTextColor(MENU_TITLE, MENU_BG);
      g_tft.drawString(String("UID: ") + (g_nfcProbe.uid.length() ? g_nfcProbe.uid : String("--")),
                        w / 2, 120, 4);
      // Useable capacity -- what OctoScale can actually fit into this tag's own write
      // format, not the chip's raw storage size (a 1 KB Mifare Classic only has ~80 B
      // of that free for spool data once sector trailers/auth blocks are excluded; see
      // nfcClassifyTag's comment). Replaces the former ATQA/SAK line, which is a raw
      // protocol detail with no practical meaning for a user checking a tag's fitness
      // -- still in /nfcprobe's JSON for anyone who does need it.
      char buf[32];
      snprintf(buf, sizeof(buf), "%d B useable", g_nfcExtCacheCapacityBytes);
      g_tft.setTextColor(MENU_DIM, MENU_BG);
      g_tft.drawString(buf, w / 2, 154, 4);
      // What the real (debug-off) flow would use this tag for -- id (green, would use
      // the normal load flow) or no id -> UID lookup fallback (amber, needs a spool
      // taught with code=<this UID> in SpoolManager). Replaces the product-name line
      // (still in /nfcprobe's JSON) -- no more vertical room above the footer.
      // g_nfcProbe.idParsed alone is the LEGACY-area id -- OpenSpool/nfcvOpenSpool/OPT
      // overwrite that area with their own NDEF content, so idParsed reads back -1 on
      // every one of them even when the write carried a perfectly valid os_db_id in its
      // JSON. flowOnTagPresent already prefers the Extended-cache id over idParsed for
      // this exact reason (see its comment in main.cpp); this display was the one place
      // that still looked at idParsed alone, showing "No Spool ID found" for a tag that
      // in fact would have loaded fine. effectiveId (above) applies the same preference.
      // No OctoScale/OpenSpool/OpenPrintTag data found on this tag -> could be a blank
      // tag OR one written by something else entirely (foreign format/vendor tool).
      // Point at the web UI's dump button instead of "No Spool ID found" (which reads
      // as "normal, just-empty tag" and hides the foreign-format case). Raw block
      // dumping needs its own multi-sector auth pass ("create dump" button in the web
      // UI), too slow/heavy to run unconditionally on every 500 ms poll here.
      if (!g_nfcExtCacheHasExtended) {
        g_tft.setTextColor(ACCENT_AMBER, MENU_BG);
        g_tft.drawString("See dump in web UI", w / 2, 188, 4);
      } else {
        g_tft.setTextColor(effectiveId >= 0 ? MENU_OK : ACCENT_AMBER, MENU_BG);
        String idLine = effectiveId >= 0
          ? ("Spool ID " + String(effectiveId) + " found")
          : "No Spool ID found.";
        g_tft.drawString(idLine, w / 2, 188, 4);  // same 34 px gap as the capacity line above
      }
    }
  } else {
    // Big weight via an off-screen SPRITE -> flicker-free: the whole number area is
    // composed in RAM (incl. its background) and pushed in ONE transfer, so there is
    // never a visible "cleared then redrawn" flash. Only the (rarely changing) tag
    // line underneath is drawn directly.
    static TFT_eSprite spr = TFT_eSprite(&g_tft);
    static bool sprMade = false;
    const int SW = 300, SH = 72;                 // sprite size (fits "-9999.9 g" @56pt)
    int cy = present ? 88 : 100;                 // sprite center y
    if (!sprMade) { spr.setColorDepth(16); spr.createSprite(SW, SH); sprMade = true; }
    // Only rebuild + push when the DISPLAYED value actually changes -> no needless
    // 5 Hz refreshes from HX711 noise in the last digit (the main flicker source).
    String ws = String(g_weight, 1) + " g";
    if (ws != g_menuLastWeightStr) {
      g_menuLastWeightStr = ws;
      spr.fillSprite(MENU_BG);
      spr.loadFont(OctoFontBig);
      spr.setTextColor(MENU_WEIGHT, MENU_BG);
      spr.setTextDatum(MC_DATUM);
      spr.drawString(ws, SW / 2, SH / 2);
      spr.unloadFont();
      spr.pushSprite((w - SW) / 2, cy - SH / 2); // one atomic blit
    }

    if (present) {
      g_tft.setTextColor(MENU_DIM, MENU_BG);
      g_tft.setTextDatum(MC_DATUM);
      g_tft.drawString(menuTagShort(g_nfcProbe.type), w / 2, 156, 4);
    }
  }
  g_tft.setTextDatum(TL_DATUM);
}

// --- Color swatch (SpoolManager color code -> TFT tile) ---------------------
// Code formats: "#rrggbb", up to 3 ";"-separated, "rainbow", "transparent" or
// "transparent:#hex[;..]". Rendered as: solid -> fill, multi -> vertical stripes,
// rainbow -> 6 fixed stripes, transparent -> checkerboard (optionally tinted).

// "#rgb"/"#rrggbb" (alpha ignored if present) -> RGB565. false = invalid.
static bool menuParseHex565(const String &hx, uint16_t &out) {
  String s = hx; s.trim();
  if (!s.length() || s[0] != '#') return false;
  s = s.substring(1);
  uint8_t r, g, b;
  auto hexNyb = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  if (s.length() == 3) {                       // #rgb -> #rrggbb
    int R = hexNyb(s[0]), G = hexNyb(s[1]), B = hexNyb(s[2]);
    if (R < 0 || G < 0 || B < 0) return false;
    r = R * 17; g = G * 17; b = B * 17;
  } else if (s.length() >= 6) {                // #rrggbb[aa]
    int r1 = hexNyb(s[0]), r0 = hexNyb(s[1]), g1 = hexNyb(s[2]),
        g0 = hexNyb(s[3]), b1 = hexNyb(s[4]), b0 = hexNyb(s[5]);
    if (r1 < 0 || r0 < 0 || g1 < 0 || g0 < 0 || b1 < 0 || b0 < 0) return false;
    r = r1 * 16 + r0; g = g1 * 16 + g0; b = b1 * 16 + b0;
  } else return false;
  out = g_tft.color565(r, g, b);
  return true;
}

// Draws the color tile (x,y,w,h) for the current g_flowColor.
static void menuDrawColorSwatch(int x, int y, int w, int h) {
  String cv = g_flowColor; cv.trim();
  String lc = cv; lc.toLowerCase();

  auto frame = [&]() { g_tft.drawRoundRect(x, y, w, h, 3, MENU_DIM); };

  if (lc == "rainbow") {
    const uint16_t rb[6] = {0xF9A6, 0xFCC0, 0xFF20, 0x1DEC, 0x2C5F, 0xA25F};
    int seg = w / 6;
    for (int i = 0; i < 6; i++)
      g_tft.fillRect(x + i * seg, y, (i == 5 ? w - 5 * seg : seg), h, rb[i]);
    frame();
    return;
  }

  bool transp = (lc == "transparent") || lc.startsWith("transparent:");
  String codes = cv;
  if (lc.startsWith("transparent:")) codes = cv.substring(12);
  else if (lc == "transparent")      codes = "";

  if (transp) {  // checkerboard base
    const int cs = 5;
    for (int yy = 0; yy < h; yy += cs)
      for (int xx = 0; xx < w; xx += cs) {
        bool dark = ((xx / cs) + (yy / cs)) & 1;
        int cw = min(cs, w - xx), ch = min(cs, h - yy);
        g_tft.fillRect(x + xx, y + yy, cw, ch, dark ? 0xC618 : TFT_WHITE);
      }
  }

  uint16_t col[3]; int nc = 0;  // parse up to 3 colors
  if (codes.length()) {
    int start = 0;
    while (nc < 3 && start <= codes.length()) {
      int sep = codes.indexOf(';', start);
      String part = (sep < 0) ? codes.substring(start) : codes.substring(start, sep);
      uint16_t c;
      if (menuParseHex565(part, c)) col[nc++] = c;
      if (sep < 0) break;
      start = sep + 1;
    }
  }

  if (nc == 0) {  // pure transparent (checkerboard only) or invalid -> frame only
    frame();
    return;
  }

  if (!transp) {
    // opaque: solid for one color, vertical stripes for multiple
    int seg = w / nc;
    for (int i = 0; i < nc; i++)
      g_tft.fillRect(x + i * seg, y, (i == nc - 1 ? w - i * seg : seg), h, col[i]);
  } else {
    // tinted transparent: checkerboard stays visible below; a thin stripe on top
    // hints at the tint color (no true alpha blending on the TFT).
    int seg = w / nc;
    for (int i = 0; i < nc; i++)
      g_tft.fillRect(x + i * seg, y, (i == nc - 1 ? w - i * seg : seg), h / 3, col[i]);
  }
  frame();
}

static void menuRenderAskAction() {
  int w = g_tft.width();
  g_tft.fillRect(0, 0, w, g_tft.height(), MENU_BG);

  // Spool name as the title (saves a separate name line -> room for the info card).
  String nm = g_flowSpoolName.length() ? g_flowSpoolName : String("What to do?");
  if (nm.length() > 20) nm = nm.substring(0, 18) + "..";  // '…' isn't in the VLW set
  menuTitle(nm.c_str());

  // --- Spool info card (2x2): left vendor/material + 'remaining', right color tile
  //     + name + remaining weight. Color/weight in font 2. ---
  int cardY = 40, cardH = 78;  // compact -> more room for the menu items below
  int lx = 16;                 // left column
  int rx = w - 16;             // right column (right-aligned)
  g_tft.fillRoundRect(6, cardY, w - 12, cardH, 6, MENU_RULE);   // subtle card background
  g_tft.drawRoundRect(6, cardY, w - 12, cardH, 6, MENU_DIM);    // subtle border

  // ---- Row 1 ----
  int row1 = cardY + 14;
  // left: vendor (above) + material (below)
  g_tft.setTextDatum(TL_DATUM);
  g_tft.setTextColor(MENU_TITLE, MENU_RULE);
  g_tft.drawString(g_flowVendor.length() ? g_flowVendor : String("-"), lx, row1, 2);
  g_tft.setTextColor(MENU_DIM, MENU_RULE);
  g_tft.drawString(g_flowMaterial.length() ? g_flowMaterial : String("-"), lx, row1 + 20, 2);

  // right: color tile + color name (row-1 height), tile to the left of the name
  const int swW = 26, swH = 20;
  String cn = g_flowColorName;
  int cnW = cn.length() ? g_tft.textWidth(cn, 2) : 0;
  int swX = rx - cnW - (cn.length() ? 8 : 0) - swW;
  if (g_flowColor.length()) menuDrawColorSwatch(swX, row1, swW, swH);
  if (cn.length()) {
    g_tft.setTextDatum(TL_DATUM);
    g_tft.setTextColor(MENU_TITLE, MENU_RULE);
    g_tft.drawString(cn, swX + swW + 8, row1 + 2, 2);
  }

  // ---- Row 2 ----
  int row2c = cardY + 56;  // vertical center shared by label + weight
  String rem = (g_flowRemaining >= 0.0f) ? (String(g_flowRemaining, 0) + " g") : String("-");
  // right: remaining weight, LARGE (smooth mid font, accent), right-aligned-middle
  g_tft.loadFont(OctoFontMid);
  g_tft.setTextDatum(MR_DATUM);
  g_tft.setTextColor(MENU_WEIGHT, MENU_RULE);
  g_tft.drawString(rem, rx, row2c);
  g_tft.unloadFont();
  // left: "remaining weight" (font 2) at the same mid-height
  g_tft.setTextDatum(ML_DATUM);
  g_tft.setTextColor(MENU_DIM, MENU_RULE);
  g_tft.drawString("remaining weight", lx, row2c, 2);
  g_tft.setTextDatum(TL_DATUM);

  // --- action list below (more room -> larger rows) ---
  static const char *acts[] = {"Load into printer", "Save weight"};
  menuDrawList(acts, 2, g_menuCursor, cardY + cardH + 12, 46, 2);
  menuHint("Turn = select   PUSH = OK   KO = back");
}

static void menuRenderAskPrinter() {
  g_tft.fillRect(0, 0, g_tft.width(), g_tft.height(), MENU_BG);
  menuTitle("Choose printer");
  static const char *names[8];
  int n = g_octoCount > 8 ? 8 : g_octoCount;
  for (int i = 0; i < n; i++) names[i] = g_octo[i].name.c_str();
  if (n == 0) { menuMessage("No printer", MENU_WARN, "add one in the web UI"); return; }
  menuDrawList(names, n, g_menuCursor, 46, 40, 4);
  menuHint("Turn = select   PUSH = OK   KO = back");
}

static void menuRenderAskTool() {
  g_tft.fillRect(0, 0, g_tft.width(), g_tft.height(), MENU_BG);
  menuTitle("Choose tool");
  static String labels[16];
  static const char *lp[16];
  int n = g_flowToolCount > 16 ? 16 : g_flowToolCount;
  for (int i = 0; i < n; i++) { labels[i] = String("Tool ") + i; lp[i] = labels[i].c_str(); }
  menuDrawList(lp, n, g_menuCursor, 46, 40, 4);
  menuHint("Turn = select   PUSH = OK   KO = back");
}

static void menuRenderWeighConfirm() {
  g_tft.fillRect(0, 0, g_tft.width(), g_tft.height(), MENU_BG);
  menuTitle("Put on scale");
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  g_tft.setTextDatum(MC_DATUM);
  float rest = g_weight - g_flowSpoolWeight;
  if (rest < 0) rest = 0;
  if (rest > g_flowTotalWeight) rest = g_flowTotalWeight;
  g_tft.drawString(String("Remaining ~ ") + String(rest, 0) + " / " + String(g_flowTotalWeight, 0) + " g",
                   g_tft.width() / 2, 150, 2);
  g_tft.setTextDatum(TL_DATUM);
  menuHint("PUSH = save   KO = cancel", MENU_SEL_BG);
  // live gross weight drawn by menuCenterTick()
}

// --- redraw dispatcher -----------------------------------------------------

// Hash over everything that triggers a full redraw (NOT g_weight -> would redraw forever).
static uint32_t menuStateHash() {
  uint32_t h = (uint32_t)g_flowState * 131 + (uint32_t)g_menuScreen * 977
             + (uint32_t)(g_menuCursor + 1) * 31 + (uint32_t)(g_deviceCursor + 1) * 17
             + (uint32_t)(g_flowSpoolId + 2) * 7 + (uint32_t)(g_flowToolCount + 1) * 13
             + (uint32_t)g_octoCount * 101 + (uint32_t)(g_nfcDebug ? 1 : 0) * 3
             // status markers so the footer redraws when they change
             + (uint32_t)(pn5180IsReady() ? 1 : 0) * 5 + (uint32_t)(g_scaleReady ? 1 : 0) * 23
             + (uint32_t)(g_dbReachable + 2) * 37 + (uint32_t)(WiFi.isConnected() ? 1 : 0) * 41
             + (uint32_t)(g_menuDark ? 1 : 0) * 53 + (uint32_t)(g_pn5180Present ? 1 : 0) * 59;
  return h;
}

static void menuRenderDevice() {
  g_tft.fillRect(0, 0, g_tft.width(), g_tft.height(), MENU_BG);
  menuTitle("Device");
  static String items[DEV_COUNT];
  static const char *ip[DEV_COUNT];
  items[0] = "Tare";
  items[1] = String("NFC debug: ") + (g_nfcDebug ? "ON" : "OFF");
  items[2] = String("Buzzer: ") + (g_buzEnabled ? "ON" : "OFF");
  items[3] = String("Theme: ") + (g_menuDark ? "Dark" : "Light");
  items[4] = "System info";
  for (int i = 0; i < DEV_COUNT; i++) ip[i] = items[i].c_str();
  // 5 entries, smaller rows (34) so all fit without scrolling.
  menuDrawList(ip, DEV_COUNT, g_deviceCursor, 44, 34, DEV_COUNT);
  menuHint("Turn = select   PUSH = OK   KO = back");
}

// Draws "Label" (dim) + colored "value" on one system-info row, returns next y.
static int menuSysRow(int y, const char *label, const char *val, uint16_t valCol) {
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  g_tft.drawString(label, 10, y, 2);
  g_tft.setTextColor(valCol, MENU_BG);
  g_tft.drawString(val, 130, y, 2);
  return y + 21;
}

static void menuRenderSysinfo() {
  g_tft.fillRect(0, 0, g_tft.width(), g_tft.height(), MENU_BG);
  menuTitle("System");
  g_tft.setTextDatum(TL_DATUM);
  int y = 46;
  // Order: connectivity -> subsystems -> identity.
  y = menuSysRow(y, "WiFi", WiFi.isConnected() ? (String(WiFi.RSSI()) + " dBm").c_str() : "--",
                 WiFi.isConnected() ? MENU_OK : MENU_ERR);
  y = menuSysRow(y, "IP", (WiFi.isConnected() ? WiFi.localIP().toString() : String("--")).c_str(), MENU_TITLE);
  y = menuSysRow(y, "NFC", pn5180IsReady() ? "ready" : "--", pn5180IsReady() ? MENU_OK : MENU_ERR);
  y = menuSysRow(y, "Scale", g_scaleReady ? "ready" : "--", g_scaleReady ? MENU_OK : MENU_ERR);
  y = menuSysRow(y, "DB", g_dbReachable == 1 ? "connected" : (g_dbReachable == 0 ? "offline" : "?"),
                 g_dbReachable == 1 ? MENU_OK : (g_dbReachable == 0 ? MENU_ERR : MENU_DIM));
  y = menuSysRow(y, "Printers", String(g_octoCount).c_str(), MENU_TITLE);
  y = menuSysRow(y, "Firmware", "v" FW_VERSION, MENU_TITLE);
  // Last reset reason: a normal power-on or a deliberate software/OTA restart is
  // fine (green); brownout, panic, or either watchdog kind means the PRIOR boot
  // ended unexpectedly -- worth surfacing here without needing the web UI.
  {
    esp_reset_reason_t rr = esp_reset_reason();
    const char *rrTxt;
    bool rrBad;
    switch (rr) {
      case ESP_RST_POWERON:  rrTxt = "Power-On";  rrBad = false; break;
      case ESP_RST_SW:       rrTxt = "Software";  rrBad = false; break;
      case ESP_RST_DEEPSLEEP: rrTxt = "Deep-Sleep"; rrBad = false; break;
      case ESP_RST_BROWNOUT: rrTxt = "Brownout";  rrBad = true;  break;
      case ESP_RST_PANIC:    rrTxt = "Panic";     rrBad = true;  break;
      case ESP_RST_INT_WDT:  rrTxt = "Int-WDT";   rrBad = true;  break;
      case ESP_RST_TASK_WDT: rrTxt = "Task-WDT";  rrBad = true;  break;
      case ESP_RST_WDT:      rrTxt = "Other-WDT"; rrBad = true;  break;
      default:                rrTxt = "Unknown";  rrBad = false; break;
    }
    y = menuSysRow(y, "Last reset", rrTxt, rrBad ? MENU_ERR : MENU_OK);
  }
  menuHint("PUSH / KO = back");
}

// Success screen after loading: "Loaded" + printer/tool, vendor/material, color.
// (When weighing, g_flowTool<0 -> a plain "Saved" screen via menuMessage instead.)
static void menuRenderDone() {
  int w = g_tft.width();
  g_tft.fillRect(0, 0, w, g_tft.height(), MENU_BG);
  menuTitle("Loaded", MENU_OK);

  int lx = 16, rx = w / 2 + 6;   // two columns (value left / value right)
  int y = 54;
  const int rh = 52;             // row height (label font 2 + value font 4)
  const int vy = 18;             // label -> value gap

  // Label in font 2 (dim), value large in font 4.
  // Row 1: printer | tool
  g_tft.setTextDatum(TL_DATUM);
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  g_tft.drawString("Printer", lx, y, 2);
  g_tft.drawString("Tool", rx, y, 2);
  g_tft.setTextColor(MENU_TITLE, MENU_BG);
  String pr = (g_flowPrinter >= 0 && g_flowPrinter < g_octoCount)
                ? g_octo[g_flowPrinter].name : String("-");
  if (pr.length() > 11) pr = pr.substring(0, 10) + ".";
  g_tft.drawString(pr, lx, y + vy, 4);
  g_tft.drawString(g_flowTool >= 0 ? String(g_flowTool) : String("-"), rx, y + vy, 4);
  y += rh;

  // Row 2: vendor | material
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  g_tft.drawString("Vendor", lx, y, 2);
  g_tft.drawString("Material", rx, y, 2);
  g_tft.setTextColor(MENU_TITLE, MENU_BG);
  String vd = g_flowVendor.length() ? g_flowVendor : String("-");
  if (vd.length() > 11) vd = vd.substring(0, 10) + ".";
  String ma = g_flowMaterial.length() ? g_flowMaterial : String("-");
  if (ma.length() > 11) ma = ma.substring(0, 10) + ".";
  g_tft.drawString(vd, lx, y + vy, 4);
  g_tft.drawString(ma, rx, y + vy, 4);
  y += rh;

  // Row 3: color (swatch + large name)
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  g_tft.drawString("Color", lx, y, 2);
  const int swW = 34, swH = 26;
  if (g_flowColor.length()) menuDrawColorSwatch(lx, y + vy, swW, swH);
  else { g_tft.setTextColor(MENU_DIM, MENU_BG); g_tft.drawString("-", lx, y + vy, 4); }
  if (g_flowColorName.length()) {
    g_tft.setTextColor(MENU_TITLE, MENU_BG);
    g_tft.setTextDatum(ML_DATUM);
    String cn = g_flowColorName;
    if (cn.length() > 14) cn = cn.substring(0, 13) + ".";
    g_tft.drawString(cn, lx + swW + 10, y + vy + swH / 2, 4);
    g_tft.setTextDatum(TL_DATUM);
  }

  menuHint("PUSH / KO = back");
}

// Fully redraw the current screen.
// Full-screen logo, shown after g_ssTimeoutSec idle at FLOW_IDLE. Themed like every
// other menu screen (MENU_BG, dark or light) instead of always forcing white --
// displayLogoThemed() recolors the logo's own (near-)white background to MENU_BG on
// the fly so the octopus sits cleanly on a dark background too, no mismatched white
// tile/border. Light theme: MENU_BG is already TFT_WHITE, so this is visually
// identical to the previous fixed-white behavior.
static void menuRenderScreensaver() {
  g_tft.fillScreen(MENU_BG);
  displayLogoThemed(g_tft.width() / 2, g_tft.height() / 2, MENU_BG);
}

static void menuRedraw() {
  if (g_menuScreen == MENU_DEVICE)      { menuRenderDevice();      return; }
  if (g_menuScreen == MENU_SYSINFO)     { menuRenderSysinfo();     return; }
  if (g_menuScreen == MENU_TARED)       { menuMessage("Tared", MENU_OK); return; }
  if (g_menuScreen == MENU_SCREENSAVER) { menuRenderScreensaver(); return; }
  switch (g_flowState) {
    case FLOW_IDLE:          menuRenderIdle(); break;
    case FLOW_DB_CHECK:      menuMessage("Checking DB...", MENU_TITLE); break;
    // A vendor tag is a normal thing to encounter, not a fault -- neutral title colour,
    // and it says what it is instead of "Unknown". Blank/unassigned tags keep the amber
    // warning, since those genuinely are "this should have been one of ours".
    case FLOW_UNKNOWN:
      if (g_flowForeignTag) menuMessage("Vendor tag", MENU_TITLE, g_flowMsg.c_str());
      else                  menuMessage("Unknown spool", MENU_WARN, g_flowMsg.c_str());
      break;
    case FLOW_ASK_ACTION:    menuRenderAskAction(); break;
    case FLOW_ASK_PRINTER:   menuRenderAskPrinter(); break;
    case FLOW_ASK_TOOL:      menuRenderAskTool(); break;
    case FLOW_LOADING:       menuMessage("Loading...", MENU_TITLE); break;
    case FLOW_WEIGH_CONFIRM: menuRenderWeighConfirm(); break;
    case FLOW_WEIGHING:      menuMessage("Saving...", MENU_TITLE); break;
    case FLOW_DONE:
      if (g_flowTool < 0) menuMessage("Saved", MENU_OK, g_flowMsg.c_str());  // weighed
      else                menuRenderDone();                                   // loaded
      break;
    case FLOW_ERROR:         menuMessage("Error", MENU_ERR, g_flowMsg.c_str()); break;
  }
}

// --- screen preview (web UI System tab: "Preview all screens") -------------
// Lets someone step through every TFT screen the firmware can show, without
// having to physically place tags / trigger flows / start an OTA / etc. to see
// them. Deliberately does NOT drive the real g_flowState/g_menuScreen machine
// (that would mean either forging real flow data, which risks a stale g_flow*
// value leaking into a real screen afterwards, or actually calling into the
// blocking octo*/NFC-write paths, which this must never do) -- instead each
// entry below draws with the SAME render helpers the real screens use, fed
// fixed demo values, exactly like the standalone SVG mockup this preview was
// modeled after (see the project's TFT screen catalogue artifact). Read-only:
// no NFC/octo/HTTP call is ever made while this is active.
static bool g_menuPreviewActive = false;
static int g_menuPreviewIdx = 0;

// Fires the SAME ledFlash()/buzzerXxx() calls the real trigger site for this screen
// uses (see main.cpp/buzzer.h), reproduced here rather than shared: the real call
// sites are threaded through blocking octo*/NFC calls this preview must never make.
// nullptr entries below (most idle/menu screens) mean "no event at this transition" --
// the LED keeps showing whatever the real idleColor()/g_ledState already is, same as
// on the real device between events.
typedef void (*MenuPreviewSignalFn)();

struct MenuPreviewEntry {
  const char *name;      // human-readable label -- shown in the web UI's "Screen" row
                          // (see /menupreview's "name" field), not drawn on the TFT itself
  void (*draw)();
  MenuPreviewSignalFn signal;  // nullptr = no LED/buzzer event when this screen is entered
};

static void mpDrawBoot() {
  g_tft.fillScreen(TFT_WHITE);
  int w = g_tft.width(), h = g_tft.height();
  g_tft.setTextColor(TFT_BLACK, TFT_WHITE);
  g_tft.setTextDatum(MC_DATUM);
  g_tft.drawString("OctoScale", w / 2, 16, 4);
  displayLogo(w / 2, 16 + (h - 18 - 32) / 2 + 32);
  g_tft.drawString(String("v") + FW_VERSION, w / 2, h - 8, 4);
  g_tft.setTextDatum(TL_DATUM);
}
static void mpDrawScreensaver() { menuRenderScreensaver(); }
static void mpDrawOta() {
  int w = g_tft.width(), h = g_tft.height();
  g_tft.fillRect(0, 0, w, h, MENU_BG);
  g_tft.setTextColor(MENU_TITLE, MENU_BG);
  g_tft.setTextDatum(MC_DATUM);
  g_tft.drawString("Software update", w / 2, h / 2 - 30, 4);
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  g_tft.drawString("Do not power off", w / 2, h / 2 - 4, 4);
  int barX = 30, barY = h / 2 + 20, barW = w - 60, barH = 18;
  g_tft.drawRect(barX, barY, barW, barH, MENU_DIM);
  g_tft.fillRect(barX + 2, barY + 2, (int)((barW - 4) * 0.62f), barH - 4, MENU_WEIGHT);
  g_tft.drawString("62%", w / 2, barY + barH + 16, 4);
  g_tft.setTextDatum(TL_DATUM);
}
static void mpNfcLock(bool erasing) {
  int w = g_tft.width(), h = g_tft.height();
  g_tft.fillRect(0, 0, w, h, MENU_BG);
  int cx = w / 2, cy = h / 2 - 46;
  g_tft.fillCircle(cx, cy, 7, MENU_WEIGHT);
  for (int i = 0; i < 2; i++) {
    int r = 20 + i * 18;
    g_tft.drawArc(cx, cy, r, r - 4, 300, 360, MENU_WEIGHT, MENU_BG, true);
  }
  g_tft.loadFont(OctoFontMid);
  g_tft.setTextColor(MENU_TITLE, MENU_BG);
  g_tft.setTextDatum(MC_DATUM);
  g_tft.drawString(erasing ? "Erasing NFC tag" : "Writing NFC tag", cx, h / 2 + 24);
  g_tft.unloadFont();
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  g_tft.drawString("Do not remove tag", cx, h / 2 + 58, 4);
  g_tft.setTextDatum(TL_DATUM);
}
static void mpDrawNfcWrite() { mpNfcLock(false); }
static void mpDrawNfcErase() { mpNfcLock(true); }
static void mpMessage(const char *title, uint16_t col, const char *sub) { menuMessage(title, col, sub); }
static void mpDrawNfcWritten()    { mpMessage("Tag written", MENU_OK, "octoscaleExtended"); }
static void mpDrawNfcIdOnly()     { mpMessage("ID only", MENU_WARN, "tag too small for this format - only the spool ID was stored"); }
static void mpDrawNfcWriteFail()  { mpMessage("Write failed", MENU_ERR, "Mifare auth failed on sector 2"); }
static void mpDrawNfcErased()     { mpMessage("Tag erased", MENU_OK, nullptr); }
static void mpDrawNfcEraseFail()  { mpMessage("Erase failed", MENU_ERR, "No tag present"); }
static void mpDrawDbCheck()       { mpMessage("Checking DB...", MENU_TITLE, nullptr); }
static void mpDrawUnknownSpool()  { mpMessage("Unknown spool", MENU_WARN, "Tag UID not assigned to a spool"); }
static void mpDrawLoading()       { mpMessage("Loading...", MENU_TITLE, nullptr); }
static void mpDrawSaving()        { mpMessage("Saving...", MENU_TITLE, nullptr); }
static void mpDrawSaved()         { mpMessage("Saved", MENU_OK, "Weight saved"); }
static void mpDrawTared()         { mpMessage("Tared", MENU_OK, nullptr); }
static void mpDrawFlowError()     { mpMessage("Error", MENU_ERR, "No OctoPrint instance configured"); }

// Idle screens: real weight/footer chips (harmless to read live), demo tag info.
static void mpDrawIdleNormal() {
  g_tft.fillRect(0, 0, g_tft.width(), g_tft.height(), MENU_BG);
  menuTitle("OctoScale");
  menuFooter();
}
static void mpDrawIdleTag() {
  g_tft.fillRect(0, 0, g_tft.width(), g_tft.height(), MENU_BG);
  menuTitle("OctoScale");
  int w = g_tft.width();
  static TFT_eSprite spr = TFT_eSprite(&g_tft);
  static bool made = false;
  if (!made) { spr.setColorDepth(16); spr.createSprite(300, 72); made = true; }
  spr.fillSprite(MENU_BG);
  spr.loadFont(OctoFontBig);
  spr.setTextColor(MENU_WEIGHT, MENU_BG);
  spr.setTextDatum(MC_DATUM);
  spr.drawString("842.3 g", 150, 36);
  spr.unloadFont();
  spr.pushSprite((w - 300) / 2, 88 - 36);
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  g_tft.setTextDatum(MC_DATUM);
  g_tft.drawString("NFC-A", w / 2, 156, 4);
  g_tft.setTextDatum(TL_DATUM);
  menuFooter();
}
static void mpDrawIdleUncal() {
  g_tft.fillRect(0, 0, g_tft.width(), g_tft.height(), MENU_BG);
  menuTitle("OctoScale");
  g_tft.setTextColor(MENU_WARN, MENU_BG);
  g_tft.setTextDatum(MC_DATUM);
  g_tft.drawString("Calibrate scale in web UI", g_tft.width() / 2, g_tft.height() - 38, 2);
  g_tft.setTextDatum(TL_DATUM);
  menuFooter();
}
static void mpIdleDebugTag(const char *type, const char *uid, int capBytes, int spoolId) {
  int w = g_tft.width();
  g_tft.fillRect(0, 0, w, g_tft.height(), MENU_BG);
  menuTitle("NFC Debug");
  g_tft.setTextDatum(MC_DATUM);
  g_tft.loadFont(OctoFontBig);
  g_tft.setTextColor(MENU_WEIGHT, MENU_BG);
  g_tft.drawString(type, w / 2, 74);
  g_tft.unloadFont();
  g_tft.setTextColor(MENU_TITLE, MENU_BG);
  g_tft.drawString(String("UID: ") + uid, w / 2, 120, 4);
  char buf[32];
  snprintf(buf, sizeof(buf), "%d B useable", capBytes);
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  g_tft.drawString(buf, w / 2, 154, 4);
  if (spoolId >= 0) {
    g_tft.setTextColor(MENU_OK, MENU_BG);
    g_tft.drawString(String("Spool ID ") + spoolId + " found", w / 2, 188, 4);
  } else {
    g_tft.setTextColor(ACCENT_AMBER, MENU_BG);
    g_tft.drawString("See dump in web UI", w / 2, 188, 4);
  }
  g_tft.setTextDatum(TL_DATUM);
  menuFooter();
}
static void mpDrawIdleDebugEmpty() {
  g_tft.fillRect(0, 0, g_tft.width(), g_tft.height(), MENU_BG);
  menuTitle("NFC Debug");
  g_tft.setTextDatum(MC_DATUM);
  g_tft.loadFont(OctoFontBig);
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  g_tft.drawString("no tag", g_tft.width() / 2, 74);
  g_tft.unloadFont();
  g_tft.setTextDatum(TL_DATUM);
  menuFooter();
}
static void mpDrawIdleDebugId()   { mpIdleDebugTag("NFC-A", "40CA8A50", 80, 42); }
static void mpDrawIdleDebugNoId() { mpIdleDebugTag("NFC-V", "E004015...", 16, -1); }

static void mpDrawDeviceMenu() { menuRenderDevice(); }
static void mpDrawSysinfo()    { menuRenderSysinfo(); }

static void mpDrawAskAction() {
  int w = g_tft.width();
  g_tft.fillRect(0, 0, w, g_tft.height(), MENU_BG);
  menuTitle("Galaxy Black PLA");
  int cardY = 40, cardH = 78, lx = 16, rx = w - 16;
  g_tft.fillRoundRect(6, cardY, w - 12, cardH, 6, MENU_RULE);
  g_tft.drawRoundRect(6, cardY, w - 12, cardH, 6, MENU_DIM);
  int row1 = cardY + 14;
  g_tft.setTextDatum(TL_DATUM);
  g_tft.setTextColor(MENU_TITLE, MENU_RULE);
  g_tft.drawString("OctoTest", lx, row1, 2);
  g_tft.setTextColor(MENU_DIM, MENU_RULE);
  g_tft.drawString("PLA", lx, row1 + 20, 2);
  const int swW = 26, swH = 20;
  String cn = "Galaxy Black";
  int cnW = g_tft.textWidth(cn, 2);
  int swX = rx - cnW - 8 - swW;
  String savedColor = g_flowColor; g_flowColor = "0044FF";  // demo swatch, restored below
  menuDrawColorSwatch(swX, row1, swW, swH);
  g_flowColor = savedColor;
  g_tft.setTextColor(MENU_TITLE, MENU_RULE);
  g_tft.drawString(cn, swX + swW + 8, row1 + 2, 2);
  int row2c = cardY + 56;
  g_tft.loadFont(OctoFontMid);
  g_tft.setTextDatum(MR_DATUM);
  g_tft.setTextColor(MENU_WEIGHT, MENU_RULE);
  g_tft.drawString("842 g", rx, row2c);
  g_tft.unloadFont();
  g_tft.setTextDatum(ML_DATUM);
  g_tft.setTextColor(MENU_DIM, MENU_RULE);
  g_tft.drawString("remaining weight", lx, row2c, 2);
  g_tft.setTextDatum(TL_DATUM);
  static const char *acts[] = {"Load into printer", "Save weight"};
  menuDrawList(acts, 2, 0, cardY + cardH + 12, 46, 2);
  menuHint("Turn = select   PUSH = OK   KO = back");
}
static void mpDrawAskPrinter() {
  g_tft.fillRect(0, 0, g_tft.width(), g_tft.height(), MENU_BG);
  menuTitle("Choose printer");
  static const char *items[] = {"Voron 2.4", "Prusa MK4"};
  menuDrawList(items, 2, 0, 46, 40, 2);
  menuHint("Turn = select   PUSH = OK   KO = back");
}
static void mpDrawAskPrinterEmpty() { mpMessage("No printer", MENU_WARN, "add one in the web UI"); }
static void mpDrawAskTool() {
  g_tft.fillRect(0, 0, g_tft.width(), g_tft.height(), MENU_BG);
  menuTitle("Choose tool");
  static const char *items[] = {"Tool 0", "Tool 1", "Tool 2"};
  menuDrawList(items, 3, 1, 46, 40, 3);
  menuHint("Turn = select   PUSH = OK   KO = back");
}
static void mpDrawWeighConfirm() {
  int w = g_tft.width();
  g_tft.fillRect(0, 0, w, g_tft.height(), MENU_BG);
  menuTitle("Put on scale");
  static TFT_eSprite spr = TFT_eSprite(&g_tft);
  static bool made = false;
  if (!made) { spr.setColorDepth(16); spr.createSprite(300, 72); made = true; }
  spr.fillSprite(MENU_BG);
  spr.loadFont(OctoFontBig);
  spr.setTextColor(MENU_WEIGHT, MENU_BG);
  spr.setTextDatum(MC_DATUM);
  spr.drawString("0.0 g", 150, 36);
  spr.unloadFont();
  spr.pushSprite((w - 300) / 2, 88 - 36);
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  g_tft.setTextDatum(MC_DATUM);
  g_tft.drawString("Remaining ~ 620 / 1000 g", w / 2, 150, 4);
  g_tft.setTextDatum(TL_DATUM);
  menuHint("PUSH = save   KO = cancel", MENU_WEIGHT);
}
static void mpDrawDoneLoaded() {
  int w = g_tft.width();
  g_tft.fillRect(0, 0, w, g_tft.height(), MENU_BG);
  menuTitle("Loaded", MENU_OK);
  int lx = 16, rx = w / 2 + 6, y = 54; const int rh = 52, vy = 18;
  g_tft.setTextDatum(TL_DATUM);
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  g_tft.drawString("Printer", lx, y, 2); g_tft.drawString("Tool", rx, y, 2);
  g_tft.setTextColor(MENU_TITLE, MENU_BG);
  g_tft.drawString("Voron 2.4", lx, y + vy, 4); g_tft.drawString("0", rx, y + vy, 4);
  y += rh;
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  g_tft.drawString("Vendor", lx, y, 2); g_tft.drawString("Material", rx, y, 2);
  g_tft.setTextColor(MENU_TITLE, MENU_BG);
  g_tft.drawString("OctoTest", lx, y + vy, 4); g_tft.drawString("PLA", rx, y + vy, 4);
  y += rh;
  g_tft.setTextColor(MENU_DIM, MENU_BG);
  g_tft.drawString("Color", lx, y, 2);
  const int swW = 34, swH = 26;
  String savedColor = g_flowColor; g_flowColor = "0044FF";
  menuDrawColorSwatch(lx, y + vy, swW, swH);
  g_flowColor = savedColor;
  g_tft.setTextColor(MENU_TITLE, MENU_BG);
  g_tft.setTextDatum(ML_DATUM);
  g_tft.drawString("Galaxy Black", lx + swW + 10, y + vy + swH / 2, 4);
  g_tft.setTextDatum(TL_DATUM);
  menuHint("PUSH / KO = back");
}

// --- preview LED/buzzer signals ---------------------------------------------
// Reproduces the exact ledFlash()/buzzerXxx() call that fires at the real transition
// into each screen (see the project's combined screen+signal reference artifact for
// the full trace). Two screens (OTA, DB check) show a LIVE/held LED state on the real
// device rather than a one-shot event -- those are special-cased in the overlay
// (mpSignalOverlay) instead of firing here, since g_otaInProgress/g_flowState must
// never be touched by this read-only preview.
// OTA and DB-check show a HELD LED state on the real device (the traffic-light stages /
// solid cyan from idleColor()), not a one-shot ledFlash() -- since this preview must
// never touch g_otaInProgress/g_flowState, the same visual is faked here by re-arming a
// short ledFlash() every time this screen redraws (every encoder tick, and once more on
// entry) so it never lapses back to the idle color while parked on these two screens.
static void mpHoldOta() {
  static uint8_t stage = 0;
  stage = (stage + 1) % 3;
  uint32_t col = stage == 0 ? pixel.Color(LED_BRIGHT * 2, 0, 0)
               : stage == 1 ? pixel.Color(LED_BRIGHT * 2, LED_BRIGHT * 2, 0)
                             : pixel.Color(0, LED_BRIGHT * 2, 0);
  ledFlash(col, 900);
}
static void mpHoldDbCheck() { ledFlash(pixel.Color(0, LED_BRIGHT * 2, LED_BRIGHT * 2), 900); }

static void mpSigWriting()   { ledFlash(pixel.Color(0, 0, LED_BRIGHT * 2), 3000); }             // blue
static void mpSigWritten()   { ledFlash(pixel.Color(0, LED_BRIGHT * 3, 0), 1500); buzzerSuccess(); }
static void mpSigIdOnly()    { ledFlash(pixel.Color(LED_BRIGHT * 3, LED_BRIGHT * 2, 0), 1500); buzzerSuccess(); }  // amber LED, still ok=true
static void mpSigWriteFail() { ledFlash(pixel.Color(LED_BRIGHT * 2, 0, 0), 1500); buzzerError(); }
static void mpSigUnknown()   { buzzerError(); }
static void mpSigSaved()     { buzzerSuccess(); }
static void mpSigError()     { buzzerError(); }  // flowDoTool()/flowDoWeighSave() path -- the printer/DB-lookup path uses a plain red ledFlash() with no buzzer instead
static void mpSigTagRead()   { buzzerRead(); }   // cyan blip, fires once when a tag first appears
static void mpSigSpoolFound(){ ledFlash(pixel.Color(0, LED_BRIGHT * 3, 0), 800); }
static void mpSigNoPrinter() { ledFlash(pixel.Color(LED_BRIGHT * 2, 0, 0), 1500); }
static void mpSigDoneLoaded(){ buzzerSuccess(); }

// Order matches the TFT screen catalogue artifact's section order (Startup, NFC write,
// Flow, Idle, Device menu, Load flow).
static const MenuPreviewEntry kMenuPreview[] = {
  {"Boot / Splash", mpDrawBoot, nullptr}, {"Screensaver", mpDrawScreensaver, nullptr}, {"Software Update", mpDrawOta, mpHoldOta},
  {"Writing NFC Tag", mpDrawNfcWrite, mpSigWriting}, {"Erasing NFC Tag", mpDrawNfcErase, mpSigWriting},
  {"Tag Written", mpDrawNfcWritten, mpSigWritten}, {"ID Only (warning)", mpDrawNfcIdOnly, mpSigIdOnly},
  {"Write Failed", mpDrawNfcWriteFail, mpSigWriteFail}, {"Tag Erased", mpDrawNfcErased, mpSigWritten},
  {"Erase Failed", mpDrawNfcEraseFail, mpSigWriteFail},
  {"Checking DB", mpDrawDbCheck, mpHoldDbCheck}, {"Unknown Spool", mpDrawUnknownSpool, mpSigUnknown},
  {"Loading", mpDrawLoading, nullptr}, {"Saving", mpDrawSaving, nullptr},
  {"Saved", mpDrawSaved, mpSigSaved}, {"Tared", mpDrawTared, nullptr}, {"Error", mpDrawFlowError, mpSigError},
  {"Idle - No Tag", mpDrawIdleNormal, nullptr}, {"Idle - Tag Present", mpDrawIdleTag, mpSigTagRead}, {"Idle - Uncalibrated", mpDrawIdleUncal, nullptr},
  {"NFC Debug - No Tag", mpDrawIdleDebugEmpty, nullptr}, {"NFC Debug - Spool Found", mpDrawIdleDebugId, nullptr},
  {"NFC Debug - No Spool ID", mpDrawIdleDebugNoId, nullptr},
  {"Device Menu", mpDrawDeviceMenu, nullptr}, {"System Info", mpDrawSysinfo, nullptr},
  {"Spool Found - Choose Action", mpDrawAskAction, mpSigSpoolFound}, {"Choose Printer", mpDrawAskPrinter, nullptr},
  {"No Printer Configured", mpDrawAskPrinterEmpty, mpSigNoPrinter}, {"Choose Tool", mpDrawAskTool, nullptr},
  {"Put on Scale", mpDrawWeighConfirm, nullptr}, {"Loaded", mpDrawDoneLoaded, mpSigDoneLoaded},
};
static const int kMenuPreviewCount = sizeof(kMenuPreview) / sizeof(kMenuPreview[0]);

// Draws the LED-color + buzzer-active indicator in the screen's top-left corner. Reads
// pixel.getPixelColor() rather than re-deriving the color -- that's exactly what the
// real hardware is showing right now (idle/flash/buzzer-sync/OTA, whichever ledTick()'s
// priority chain picked), so this can never drift out of sync with the actual LED.
static void mpDrawSignalOverlay() {
  uint32_t c = pixel.getPixelColor(0);
  uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  // Un-dim for on-screen visibility: the real LED_BRIGHT-scaled values (≤120) render as
  // near-black on a TFT/photo, same "true relative brightness, scaled up to be visible"
  // tradeoff as the standalone screen+signal reference artifact this mirrors.
  uint16_t maxc = max(r, max(g, b));
  float boost = maxc > 0 ? min(255.0f / maxc, 4.0f) : 1.0f;
  uint16_t ledCol = g_tft.color565((uint8_t)min(255.0f, r * boost), (uint8_t)min(255.0f, g * boost), (uint8_t)min(255.0f, b * boost));
  bool ledOn = (r || g || b);

  g_tft.fillCircle(14, 14, 8, ledOn ? ledCol : MENU_RULE);
  g_tft.drawCircle(14, 14, 8, MENU_DIM);

  bool buzzOn = g_buzLedColor != 0;  // a tone sequence is currently sounding (see buzzer.h)
  g_tft.setTextDatum(TL_DATUM);
  g_tft.setTextColor(buzzOn ? ACCENT_AMBER : MENU_DIM, MENU_BG);
  g_tft.drawString(buzzOn ? "\xF0\x9F\x94\x8A" : "-", 8, 26, 2);  // best-effort glyph; '-' is the safe fallback on VLW/GLCD fonts without emoji coverage
}

static void menuPreviewDraw() {
  static int lastIdx = -1;
  kMenuPreview[g_menuPreviewIdx].draw();

  // Fire this screen's LED/buzzer event exactly once per entry (not on every redraw
  // tick, which would otherwise restart a 3s "writing" flash or a buzzer tone every
  // time the encoder nudges) -- except the two held-state screens, which intentionally
  // re-arm every tick to stay lit while parked there (see mpHoldOta/mpHoldDbCheck).
  bool changed = (g_menuPreviewIdx != lastIdx);
  lastIdx = g_menuPreviewIdx;
  const MenuPreviewEntry &entry = kMenuPreview[g_menuPreviewIdx];
  if (entry.signal == mpHoldOta || entry.signal == mpHoldDbCheck) entry.signal();  // re-arm every tick
  else if (changed && entry.signal) entry.signal();

  mpDrawSignalOverlay();

  // Overlay: which screen + how to get out, so this never gets mistaken for a stuck
  // real screen. Small corner label, drawn last -> always on top.
  int w = g_tft.width();
  char idx[16];
  snprintf(idx, sizeof(idx), "%d/%d", g_menuPreviewIdx + 1, kMenuPreviewCount);
  g_tft.setTextDatum(TR_DATUM);
  g_tft.setTextColor(TFT_BLACK, ACCENT_AMBER);
  g_tft.fillRect(w - 54, 0, 54, 16, ACCENT_AMBER);
  g_tft.drawString(idx, w - 3, 2, 2);
  g_tft.setTextDatum(TL_DATUM);
}

// Starts/stops the preview from the web UI (System tab). Entering forces a redraw of
// the first screen; leaving forces a full redraw of whatever the real state is, so no
// preview frame lingers even for one tick.
static void menuPreviewSetActive(bool on) {
  if (on == g_menuPreviewActive) return;
  g_menuPreviewActive = on;
  if (on) { g_menuPreviewIdx = 0; menuPreviewDraw(); }
  else    { g_menuForceRedraw = true; menuRedraw(); }
}

// --- input -----------------------------------------------------------------

// Move cursor by steps within [0, n-1], WRAPPING around at both ends (endless scroll).
static void menuMoveCursor(int &cursor, int n, long steps) {
  if (n <= 0) { cursor = 0; return; }
  long c = (cursor + steps) % n;
  if (c < 0) c += n;   // keep positive after negative modulo
  cursor = (int)c;
}

// EC11 delta -> cursor steps. The ISR already reduces 4 quadrature edges to one detent
// (+/-1 per detent), so map 1:1 — no divisor (that would swallow every other step).
static long menuTakeSteps(long delta) {
  return delta;
}

inline void menuInit() {
  g_menuScreen = MENU_FLOW;
  g_menuCursor = 0;
  g_deviceCursor = 0;
  g_menuAccum = 0;
  g_menuForceRedraw = true;
  g_menuLastState = g_flowState;
  menuApplyTheme();
}

// Software-update lock screen: white background (readable regardless of theme),
// "Software update" + a progress bar (0 if the total size isn't known, e.g. the web
// /update file upload -> shown as an indeterminate-looking empty bar, still correct
// once it reaches 100 on completion).
//
// menuTick calls this every task iteration (many Hz) for the whole OTA duration --
// a full fillScreen() on every call was the visible flicker (the entire 320x240 panel
// repainted many times a second even though only the bar/percentage actually change).
// Fixed by splitting into a one-time layout draw (title/hint/bar outline, on entry only)
// and a per-tick update that touches just the bar fill + percentage text, and only
// when the percentage actually changed.
//
// Theme-colored (MENU_BG/MENU_TITLE/MENU_DIM/MENU_WEIGHT), not a fixed white/black --
// matches every other menu screen (dark/light, whichever the user picked in the device
// menu) instead of standing out with its own neutral look.
static uint8_t g_menuOtaLastPct = 0xFF;   // 0xFF = "not drawn yet" -> forces the first paint
static void menuRenderOta() {
  int w = g_tft.width(), h = g_tft.height();
  int barW = w - 60, barH = 18, barX = 30, barY = h / 2 + 20;

  if (g_menuOtaLastPct == 0xFF) {
    g_tft.fillRect(0, 0, w, h, MENU_BG);
    g_tft.setTextColor(MENU_TITLE, MENU_BG);
    g_tft.setTextDatum(MC_DATUM);
    g_tft.drawString("Software update", w / 2, h / 2 - 30, 4);
    g_tft.setTextColor(MENU_DIM, MENU_BG);
    g_tft.drawString("Do not power off", w / 2, h / 2 - 4, 4);
    g_tft.drawRect(barX, barY, barW, barH, MENU_DIM);
    g_tft.setTextDatum(TL_DATUM);
  }

  if (g_otaProgressPct == g_menuOtaLastPct) return;  // nothing changed -> no repaint
  g_menuOtaLastPct = g_otaProgressPct;

  int fillW = (barW - 4) * g_otaProgressPct / 100;
  // Redraw the bar interior each time (covers a shrinking fill too, e.g. a fresh
  // upload restarting the percentage) -- still just the bar rect, not the screen.
  g_tft.fillRect(barX + 2, barY + 2, barW - 4, barH - 4, MENU_BG);
  if (fillW > 0) g_tft.fillRect(barX + 2, barY + 2, fillW, barH - 4, MENU_WEIGHT);

  g_tft.setTextColor(MENU_TITLE, MENU_BG);
  g_tft.setTextDatum(MC_DATUM);
  char pct[8]; snprintf(pct, sizeof(pct), "%u%%", g_otaProgressPct);
  g_tft.drawString(pct, w / 2, barY + barH + 16, 4);
  g_tft.setTextDatum(TL_DATUM);
}

// NFC write lock screen: shown while an /nfcwriteid, /nfcwritespool, or /nfcerase
// request is in flight (g_nfcWriteReq or g_nfcWritePending, set by pn5180Task itself
// -> same core, safe to read directly, no volatile-cross-core dance needed). Same
// "take over the whole screen, ignore all input" pattern as menuRenderOta(). Briefly
// shows the result (format + OK/error) after the write lands, using g_nfcWriteDone as
// the still-fresh signal -- /nfcwritestatus (HTTP) clears Done/Pending once the caller
// (web UI or /nfcwritespool/nfcerase client) has consumed it, at which point the menu
// falls back to the normal flow screen on its own.
static void menuRenderNfcWrite() {
  int w = g_tft.width(), h = g_tft.height();
  bool erasing = (g_nfcWriteKind == NFCWRITE_ERASE);
  // g_nfcMenuResultPending, not g_nfcWriteDone: the latter is cleared by the HTTP
  // status handler on the other core and may already be gone here, which would draw
  // the "writing..." screen again instead of the result (see menuTick's comment).
  if (!g_nfcMenuResultPending) {
    // Theme-colored (MENU_BG/MENU_WEIGHT/MENU_DIM), NOT the fixed white/black of
    // menuRenderOta() -- an OTA lock is a one-off, rare event where a fixed neutral
    // look is fine, but a tag write happens often during normal use and should match
    // whichever theme (dark/light) the user has picked, same as every other screen.
    g_tft.fillRect(0, 0, w, h, MENU_BG);

    // Large NFC glyph: a dot + two concentric "signal" arcs, accent-colored. Static,
    // not animated -- this screen is drawn exactly ONCE per write (menuTick only gets
    // to run again after the several-second BLOCKING write call returns, so a
    // millis()-driven animation would just freeze on whatever frame happened to be
    // current at draw time -- worse than no animation, since it implies liveness that
    // isn't there). cx/cy above center, leaves room for the two text lines below.
    int cx = w / 2, cy = h / 2 - 46;
    g_tft.fillCircle(cx, cy, 7, MENU_WEIGHT);
    for (int i = 0; i < 2; i++) {
      int r = 20 + i * 18;
      g_tft.drawArc(cx, cy, r, r - 4, 300, 360, MENU_WEIGHT, MENU_BG, true);
    }

    g_tft.loadFont(OctoFontMid);
    g_tft.setTextColor(MENU_TITLE, MENU_BG);
    g_tft.setTextDatum(MC_DATUM);
    g_tft.drawString(erasing ? "Erasing NFC tag" : "Writing NFC tag", cx, h / 2 + 24);
    g_tft.unloadFont();
    g_tft.setTextColor(MENU_DIM, MENU_BG);
    g_tft.drawString("Do not remove tag", cx, h / 2 + 58, 4);
    g_tft.setTextDatum(TL_DATUM);
  } else {
    String fmt = (g_nfcWriteKind == NFCWRITE_SPOOL) ? g_nfcWriteFormat : String("legacy ID");
    if (erasing) {
      if (g_nfcWriteOk) menuMessage("Tag erased", MENU_OK);
      else              menuMessage("Erase failed", MENU_ERR, g_nfcWriteErr.c_str());
    } else if (g_nfcWriteOk) {
      // A write that succeeded but lost material/color (small tag + OpenSpool) gets the
      // warning colour and says so -- a plain green "Tag written" would hide that the
      // tag now holds little more than the spool ID. menuMessage word-wraps the sub line.
      String warn = (g_nfcWriteKind == NFCWRITE_SPOOL)
                      ? nfcWriteDropWarning(g_nfcWriteDroppedFields, g_nfcWriteFormat) : String("");
      if (warn.length()) menuMessage("ID only", MENU_WARN, warn.c_str());
      else               menuMessage("Tag written", MENU_OK, fmt.c_str());
    }
    else                     menuMessage("Write failed", MENU_ERR, g_nfcWriteErr.c_str());
  }
}

// Called each pn5180Task iteration. delta/push/start from the encoder.
// Names for the debug log (menu.h has no dependency on main.cpp's flowStateName()).
static const char *menuScreenName(MenuScreen s) {
  switch (s) {
    case MENU_FLOW:        return "FLOW";
    case MENU_DEVICE:      return "DEVICE";
    case MENU_SYSINFO:     return "SYSINFO";
    case MENU_TARED:       return "TARED";
    case MENU_SCREENSAVER: return "SCREENSAVER";
    default:                return "?";
  }
}
// Fixed labels for the device menu's 5 entries (independent of their live ON/OFF
// suffix in menuRenderDevice) -> the debug log can name a cursor position, not just
// its index.
static const char *menuDeviceItemName(int i) {
  switch (i) {
    case 0: return "Tare";
    case 1: return "NFC debug";
    case 2: return "Buzzer";
    case 3: return "Theme";
    case 4: return "System info";
    default: return "?";
  }
}

inline void menuTick(long delta, bool push, bool start) {
  if (!g_tftReady) return;

  // OTA in progress: lock screen + ignore all input, takes priority over everything
  // else (screensaver, menus, flow). menuRenderOta() only repaints the parts that
  // changed (see there); on success this branch never returns (ESP.restart()), but a
  // failed OTA falls back here without rebooting, so reset the "last drawn" tracker
  // for a clean full paint on the NEXT OTA attempt.
  if (g_otaInProgress) {
    menuRenderOta();
    return;
  }
  if (g_menuOtaLastPct != 0xFF) g_menuOtaLastPct = 0xFF;

  // NFC write actually in flight: lock screen + ignore all input, same priority tier
  // as OTA. Cancels/overrides whatever screen was up (device menu, flow, screensaver)
  // -- a write in flight must not be interrupted or obscured. Drawn once per write
  // (see menuRenderNfcWrite's own comment -- menuTick can't run again until the
  // blocking write call returns anyway, so this only ever fires once per write).
  // Gated on g_nfcMenuResultPending (not g_nfcWriteDone) so this cannot re-trigger
  // after the result has been latched: Pending may still be set while the result is
  // already waiting to be shown, and re-entering here would re-arm the flags below
  // and swallow the result screen.
  if ((g_nfcWriteReq || g_nfcWritePending) && !g_nfcMenuResultPending &&
      !g_menuNfcResultDismissed) {
    menuRenderNfcWrite();
    g_menuNfcWriteWasActive = true;
    // Arm the result screen for the write that is starting now.
    g_menuNfcResultShown = false;
    g_menuNfcResultDismissed = false;
    return;
  }
  // Result available (Done): show it for a fixed window, drawn ONCE, independent of
  // whether/when an HTTP caller polls /nfcwritestatus. Writes vary wildly in duration
  // (Mifare/NFC-V ~1-3s vs. NTAG's NDEF write ~20s) and SpoolManager polls every
  // 500ms, so tying the screen to Pending meant fast writes could clear before the
  // result was even visible, while flickering the whole time it stayed up (this
  // branch used to call menuRenderNfcWrite()/menuMessage() every tick unconditionally
  // -- menuMessage() does a fillScreen() each time, hence the flicker).
  // Result screen: driven by g_nfcMenuResultPending, which pn5180Task latches when the
  // write finishes and ONLY this code clears. Using g_nfcWriteDone here does not work --
  // the /nfcwritestatus handler (core 1) clears that as soon as the caller polls, which
  // for a fast Mifare/NFC-V write typically happens before the menu gets its next tick,
  // so the result screen never appeared at all. Shown for a fixed 5s window regardless
  // of whether anyone polls, then dismissed for good (g_menuNfcResultShown alone was
  // not enough: it got reset further down, re-arming the same result over and over).
  if (g_nfcMenuResultPending && !g_menuNfcResultDismissed) {
    if (!g_menuNfcResultShown) {
      menuRenderNfcWrite();
      g_menuNfcResultShown = true;
      g_menuNfcResultUntil = millis() + 5000;
    }
    if ((long)(millis() - g_menuNfcResultUntil) < 0) {
      g_menuNfcWriteWasActive = true;
      return;
    }
    g_menuNfcResultDismissed = true;   // window over -> back to the normal menu, for good
    g_nfcMenuResultPending = false;    // consumed
  }
  if (g_menuNfcWriteWasActive) {
    // Just released -> force a full redraw so the lock/result screen doesn't linger
    // (the state hash may not have changed underneath it, e.g. still FLOW_IDLE before
    // and after).
    g_menuNfcWriteWasActive = false;
    g_menuNfcResultShown = false;
    g_menuForceRedraw = true;
  }

  // Screen preview mode (web UI System tab): below OTA/NFC-write-lock in priority (a
  // real write/update in flight still wins -- see menuPreviewSetActive's comment on
  // why the preview itself never touches g_flowState/g_menuScreen), above everything
  // else (screensaver, normal menu/flow input). Rotate = step through screens, PUSH or
  // KO = stop preview and return to whatever the real state actually is.
  if (g_menuPreviewActive) {
    long steps = menuTakeSteps(delta);
    if (push || start) {
      menuPreviewSetActive(false);
      dbgLog("Screen preview: stopped");
    } else if (steps != 0) {
      menuMoveCursor(g_menuPreviewIdx, kMenuPreviewCount, steps);
      menuPreviewDraw();
    }
    return;
  }

  MenuScreen screenBefore = g_menuScreen;
  int cursorBefore = (g_menuScreen == MENU_DEVICE) ? g_deviceCursor : g_menuCursor;
  // What caused this tick's change, for the debug log (best-effort: multiple things can
  // be true, e.g. a state change AND a push in the same tick -> the log line just
  // reflects what menuTick was given, not a strict single cause).
  const char *cause = push ? "push" : start ? "ko" : (delta != 0) ? "turn" : "state";

  long steps = menuTakeSteps(delta);

  // Screensaver: enter from idle (MENU_FLOW + FLOW_IDLE) after g_ssTimeoutSec with no
  // activity (same g_blActivity clock as the backlight dim timeout). Any input, a new
  // tag, or a web UI action exits it immediately via displayTouch()/state change below.
  if (g_menuScreen == MENU_SCREENSAVER) {
    bool exit = steps || push || start || g_flowState != FLOW_IDLE ||
                millis() - g_blActivity < (unsigned long)g_ssTimeoutSec * 1000UL;
    if (exit) {
      g_menuScreen = MENU_FLOW;
      g_menuLastState = g_flowState;
      g_menuForceRedraw = true;
      if (steps || push || start) displayTouch();
      menuRedraw();
    }
    return;  // no further input handling while the screensaver is up (entering or staying)
  }
  if (g_ssEnabled && g_menuScreen == MENU_FLOW && g_flowState == FLOW_IDLE &&
      g_ssTimeoutSec > 0 && millis() - g_blActivity >= (unsigned long)g_ssTimeoutSec * 1000UL) {
    g_menuScreen = MENU_SCREENSAVER;
    g_menuForceRedraw = true;
    menuRedraw();
    return;
  }

  // Flow state changed (possibly via web UI) -> reset screen.
  if (g_flowState != g_menuLastState) {
    g_menuLastState = g_flowState;
    if (g_menuScreen == MENU_FLOW) { g_menuCursor = 0; g_menuForceRedraw = true; }
  }

  // ===== input per screen =====
  if (g_menuScreen == MENU_DEVICE) {
    if (steps) menuMoveCursor(g_deviceCursor, DEV_COUNT, steps);
    if (push) {
      switch (g_deviceCursor) {
        case 0:  // Tare -> brief "Tared" screen, then auto-back to idle
          g_tareReq = true;
          g_menuScreen = MENU_TARED;
          g_menuTaredUntil = millis() + 800;
          break;
        case 1:
          g_nfcDebug = !g_nfcDebug;
          g_menuLastNfcDebugStr = "";  // force a redraw on re-entry, don't reuse a stale match
          dbgLogf("Menu: NFC debug -> %s", g_nfcDebug ? "ON" : "OFF");
          break;
        case 2:                                          // toggle buzzer + NVS + test tone
          g_buzEnabled = !g_buzEnabled;
          { Preferences p; p.begin("octoscale", false); p.putBool("buzEn", g_buzEnabled); p.end(); }
          if (g_buzEnabled) buzzerOk();
          dbgLogf("Menu: Buzzer -> %s", g_buzEnabled ? "ON" : "OFF");
          break;
        case 3: g_menuDark = !g_menuDark; menuApplyTheme(); dbgLogf("Menu: Theme -> %s", g_menuDark ? "Dark" : "Light"); break;
        case 4: g_menuScreen = MENU_SYSINFO; break;      // System info
      }
      g_menuForceRedraw = true;
    }
    if (start) { g_menuScreen = MENU_FLOW; g_menuForceRedraw = true; }  // KO = back
  }
  else if (g_menuScreen == MENU_SYSINFO) {
    if (push || start) { g_menuScreen = MENU_DEVICE; g_menuForceRedraw = true; }
  }
  else if (g_menuScreen == MENU_TARED) {
    if (push || start || (long)(millis() - g_menuTaredUntil) >= 0) {
      g_menuScreen = MENU_FLOW; g_menuForceRedraw = true;
    }
  }
  else {  // MENU_FLOW — per g_flowState
    switch (g_flowState) {
      case FLOW_IDLE:
        if (push || start) { g_menuScreen = MENU_DEVICE; g_deviceCursor = 0; g_menuForceRedraw = true; }
        break;
      case FLOW_ASK_ACTION:
        if (steps) menuMoveCursor(g_menuCursor, 2, steps);
        if (push) {
          if (g_menuCursor == 0) { g_flowState = FLOW_ASK_PRINTER; g_menuCursor = 0; }
          else {  // weigh: check reference values (like /flow/action)
            if (g_flowSpoolWeight < 0.0f || g_flowTotalWeight < 0.0f) {
              g_flowState = FLOW_ERROR;
              g_flowMsg = "No empty/total weight in DB";
            } else g_flowState = FLOW_WEIGH_CONFIRM;
          }
          g_menuForceRedraw = true;
        }
        if (start) { flowReset(); g_menuForceRedraw = true; }
        break;
      case FLOW_ASK_PRINTER:
        if (steps) menuMoveCursor(g_menuCursor, g_octoCount, steps);
        if (push && g_octoCount > 0) { g_flowMenuReq = FMA_PRINTER; g_flowMenuArg = g_menuCursor; }
        if (start) { g_flowState = FLOW_ASK_ACTION; g_menuCursor = 0; g_menuForceRedraw = true; }
        break;
      case FLOW_ASK_TOOL:
        if (steps) menuMoveCursor(g_menuCursor, g_flowToolCount, steps);
        if (push) { g_flowMenuReq = FMA_TOOL; g_flowMenuArg = g_menuCursor; }
        if (start) { g_flowState = FLOW_ASK_PRINTER; g_menuCursor = 0; g_menuForceRedraw = true; }
        break;
      case FLOW_WEIGH_CONFIRM:
        if (push) g_flowMenuReq = FMA_WEIGH_SAVE;
        if (start) { flowReset(); g_menuForceRedraw = true; }
        break;
      case FLOW_UNKNOWN:
      case FLOW_DONE:
      case FLOW_ERROR:
        if (push || start) { flowReset(); g_menuForceRedraw = true; }
        break;
      default: break;  // DB_CHECK/LOADING/WEIGHING: no input (active states)
    }
  }

  // Log screen/selection changes for the web UI debug console (menu navigation trace).
  // Screen changes name the screen; a plain cursor move within MENU_DEVICE also names
  // the newly-selected entry (e.g. "System info") instead of a bare index.
  if (g_menuScreen != screenBefore) {
    dbgLogf("Menu: %s -> %s (%s)", menuScreenName(screenBefore), menuScreenName(g_menuScreen), cause);
  } else {
    int cursorAfter = (g_menuScreen == MENU_DEVICE) ? g_deviceCursor : g_menuCursor;
    if (cursorAfter != cursorBefore) {
      if (g_menuScreen == MENU_DEVICE)
        dbgLogf("Menu: DEVICE cursor -> %s", menuDeviceItemName(cursorAfter));
      else
        dbgLogf("Menu: %s cursor %d -> %d", menuScreenName(g_menuScreen), cursorBefore, cursorAfter);
    }
  }

  // ===== redraw only on change =====
  uint32_t hash = menuStateHash();
  if (g_menuForceRedraw || hash != g_menuLastHash) {
    // Cursor-only move within a list screen (same screen, same flow state, not a
    // forced redraw) -> update just the changed rows via menuDrawList instead of the
    // full menuRedraw() path, which starts with a full-screen fillRect and repaints
    // the title/card/hint too. That full clear-then-repaint on every single detent
    // was the visible flicker while turning the encoder through a list.
    bool cursorOnly = !g_menuForceRedraw && g_menuScreen == screenBefore;
    if (cursorOnly && g_menuScreen == MENU_DEVICE && g_deviceCursor != cursorBefore) {
      static String items[DEV_COUNT];
      static const char *ip[DEV_COUNT];
      items[0] = "Tare";
      items[1] = String("NFC debug: ") + (g_nfcDebug ? "ON" : "OFF");
      items[2] = String("Buzzer: ") + (g_buzEnabled ? "ON" : "OFF");
      items[3] = String("Theme: ") + (g_menuDark ? "Dark" : "Light");
      items[4] = "System info";
      for (int i = 0; i < DEV_COUNT; i++) ip[i] = items[i].c_str();
      menuDrawList(ip, DEV_COUNT, g_deviceCursor, 44, 34, DEV_COUNT);
    } else if (cursorOnly && g_menuScreen == MENU_FLOW && g_flowState == FLOW_ASK_PRINTER
               && g_menuCursor != cursorBefore) {
      static const char *names[8];
      int n = g_octoCount > 8 ? 8 : g_octoCount;
      for (int i = 0; i < n; i++) names[i] = g_octo[i].name.c_str();
      menuDrawList(names, n, g_menuCursor, 46, 40, 4);
    } else if (cursorOnly && g_menuScreen == MENU_FLOW && g_flowState == FLOW_ASK_TOOL
               && g_menuCursor != cursorBefore) {
      static String labels[16];
      static const char *lp[16];
      int n = g_flowToolCount > 16 ? 16 : g_flowToolCount;
      for (int i = 0; i < n; i++) { labels[i] = String("Tool ") + i; lp[i] = labels[i].c_str(); }
      menuDrawList(lp, n, g_menuCursor, 46, 40, 4);
    } else if (cursorOnly && g_menuScreen == MENU_FLOW && g_flowState == FLOW_ASK_ACTION
               && g_menuCursor != cursorBefore) {
      static const char *acts[] = {"Load into printer", "Save weight"};
      int cardY = 40, cardH = 78;
      menuDrawList(acts, 2, g_menuCursor, cardY + cardH + 12, 46, 2);
    } else {
      menuRedraw();
      g_menuLastWeightDraw = 0;       // redraw center immediately
      g_menuLastWeightStr = "";       // force weight sprite to repaint after a full redraw
    }
    g_menuForceRedraw = false;
    g_menuLastHash = hash;
  }
  // live center area (weight, or tag type in NFC-debug) over the static layout
  if (g_menuScreen == MENU_FLOW && (g_flowState == FLOW_IDLE || g_flowState == FLOW_WEIGH_CONFIRM))
    menuCenterTick();
}
