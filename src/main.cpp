// OctoScale — NFC filament scale for OctoPrint + SpoolManagerExtended
//
// Copyright (c) 2026 Ajimaru
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License as published by the Free
// Software Foundation; either version 2.1 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
// details. You should have received a copy of the license along with this
// program; if not, see <https://www.gnu.org/licenses/>.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>       // tzapu/WiFiManager
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <HTTPUpdate.h>       // URL-based OTA (/updateurl)
#include <HX711.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <esp_chip_info.h>     // chip model/revision/cores for /system
#include <esp_idf_version.h>   // IDF version string for /system
#include <esp_ota_ops.h>       // OTA partition size for /system
#include <hal/brownout_hal.h>  // raise BOD threshold, see raiseBrownoutThreshold()

// OctoScale (ESP32-S3) — WiFi provisioning (AP fallback) + OTA + HX711 scale +
// PN5180 NFC (SPI) + ST7789 TFT menu + EC11 encoder + buzzer.

#include "dbglog.h"     // in-RAM debug log ring buffer (web UI console, see /debuglog)
#include "version.h"    // FW_VERSION, FW_BUILD
#include "web_ui.h"     // INDEX_HTML (web UI HTML/CSS/JS)
#include "octoprint.h"  // OctoPrint instances + tool count/load URL/spool info
#include "spooldb.h"    // spoolExists() via the SpoolManagerExtended HTTP bridge (DB source)
#include "backup.h"     // config backup/restore (AES-encrypted API keys)
#include "display.h"    // ST7789 TFT
#include "pn5180nfc.h"  // PN5180 NFC reader (SPI)
#include "openprinttag.h"  // OpenPrintTag (OPT) read/write, NFC-V only
#include "encoder.h"    // EC11 encoder + buttons

const char *AP_NAME = "OctoScale-Setup";  // SoftAP when no WiFi is configured
const char *HOSTNAME = "octoscale";        // mDNS: octoscale.local

// --- ESP32-S3-N16R8 (dual-core), full build. Only target of this project. ---
// AVOIDED: strapping 0/3/45/46, USB 19/20, octal PSRAM+flash 26-37 (R8/N16!).
// Peripherals: HX711 (scale), PN5180 (NFC over SPI), ST7789 TFT (SPI), EC11 encoder +
// start button, buzzer, onboard RGB LED (GPIO48).
const int LED_PIN = 2;         // free GPIO used as a status LED
const int HX711_DOUT = 5;
const int HX711_SCK = 6;
const int WS2812_PIN = 48;     // onboard RGB LED (WS2812)
// External WS2812 on the enclosure front. The onboard LED at GPIO48 sits under the
// housing where its color isn't readable, so a second pixel mirrors it 1:1 to the
// outside. Own GPIO rather than chained into GPIO48's data-out: that pad isn't
// broken out on the DevKitC. GPIO 9 is free, not a strapping pin and has no boot
// function; 8 stays reserved for a possible I2C, 43 is U0TXD (the CH343 console).
// Powered from the 3.3V rail (Mini-360), NOT 5V: a WS2812 wants ~0.7 x VDD on DIN, so
// at 5V it expects 3.5V while the S3 drives 3.3V -- works on the bench but is out of
// spec. On 3.3V the level is exact. Module carries its own series resistor, no cap.
const int WS2812_EXT_PIN = 9;

// PN5180 and TFT have SEPARATE SPI buses. Sharing one bus caused the PN5180 to stop
// reading tags after the TFT init (the GPIO matrix only lets one controller cleanly
// drive shared pins). Hence:
//   PN5180 (FSPI, global SPI instance): SCK=12, MOSI=11, MISO=13
//   TFT    (HSPI, TFT_eSPI):            SCK=42, MOSI=44 (in platformio.ini), MISO unused
const int SPI_SCK  = 12;   // PN5180 bus (FSPI)
const int SPI_MOSI = 11;
const int SPI_MISO = 13;
// PN5180 (NFC, SPI) — NFC-A + NFC-V. Module labels: #5V/+3.3V/GND/SCK/MOSI/MISO/NSS/
// BUSY/RST/IRQ (GPIO/AUX/REQ unused -> left open).
const int PN5180_NSS  = 10;    // module "NSS" — chip select (active low)
const int PN5180_BUSY = 14;    // module "BUSY" — status (input)
const int PN5180_RST  = 21;    // module "RST" — reset (active low)
const int PN5180_IRQ  = 47;    // module "IRQ"
// Module S11-05: ST7789 TFT (2.4" 320x240, 4-wire SPI) + EC11 encoder + buttons on ONE
// connector: GND VCC SCL SDA RES DC CS BLK A B PUSH KO. SCL=SPI clock, SDA=MOSI
// (write-only, no MISO). The TFT pins themselves come as TFT_eSPI macros from
// platformio.ini (-DTFT_CS/DC/RST/BL) — these are documentation-only constants
// (prefixed TFTPIN_ to avoid a macro collision).
const int TFTPIN_CS  = 38;     // module "CS"  (== -DTFT_CS)
const int TFTPIN_DC  = 39;     // module "DC"  (== -DTFT_DC)
const int TFTPIN_RST = 40;     // module "RES" (== -DTFT_RST)
const int TFTPIN_BLK = 41;     // module "BLK" — backlight (== -DTFT_BL)
// Encoder + buttons (internal pull-ups, other side to GND).
const int ENC_A   = 15;        // module "A" — encoder rotation
const int ENC_B   = 16;        // module "B" — encoder rotation
const int ENC_SW  = 17;        // module "PUSH" — encoder click
const int KEY_START = 18;      // module "KO" — separate start button
const int BUZZER_PIN = 7;      // signal buzzer (+ on GPIO 7, - to GND); GPIO 2 = LED_PIN

// Neutral placeholder, NOT a real load-cell factor (that was specific to a previous
// scale) -> just needs to be a safe non-zero divisor until the user calibrates.
const float DEFAULT_CALIBRATION_FACTOR = 1.0;
// Tag layout of the databaseId (NFC-A/NTAG pages or NFC-V blocks) is encapsulated in
// pn5180nfc.h (PN5180_ID_START_PAGE / PN5180_ID_START_BLOCK / PN5180_ID_BYTES).

WebServer server(80);
HX711 scale;
Preferences prefs;  // NVS storage for the calibration factor
Adafruit_NeoPixel pixel(1, WS2812_PIN, NEO_GRB + NEO_KHZ800);  // 1x WS2812 status LED
// Mirrors `pixel` exactly -- never driven independently. Every write goes through
// ledShow() so both strands stay in lockstep and share the RMT lock (see ledShow()).
Adafruit_NeoPixel pixelExt(1, WS2812_EXT_PIN, NEO_GRB + NEO_KHZ800);

volatile float g_weight = 0.0;  // latest weight (g), updated by loop()
// Tare request: the web UI (/tare) and the TFT menu only set this flag. scale.tare()
// blocks ~1s and shares the HX711 with scaleTask -> the task consumes the flag and
// tares itself (no concurrent HX711 access from two tasks). Cross-task -> volatile.
volatile bool g_tareReq = false;

// CPU load approximation (loop task), averaged over a 1s window.
unsigned long g_cpuBusyAccum = 0;   // accumulated busy-us in the current window
unsigned long g_cpuWinStart = 0;    // window start time (millis)
volatile uint8_t g_cpuLoad = 0;     // 0-100%, last window's load (loop approximation)

// ---- Dual-core load (idle-counter method) --------------------------------
// FreeRTOS runtime stats aren't enabled in the Arduino core (no menuconfig).
// Alternative: one lowest-priority task per core that counts up and yields in a loop
// -> it only gets CPU when nothing else wants to run (= idle). If the counting rate
// falls below a calibrated idle baseline, the core is busy.
// load% = 100 - (rate / baseline * 100). Baseline = the highest rate ever seen/core.
volatile uint32_t g_idleCtr[2] = {0, 0};       // raw idle counter per core
uint32_t g_idleBaseline[2] = {1, 1};           // calibrated idle rate/s per core
volatile uint8_t g_coreLoad[2] = {0, 0};       // 0-100% per core
static void idleCounterTask(void *arg) {
  int core = (int)(intptr_t)arg;
  for (;;) { g_idleCtr[core]++; taskYIELD(); }
}
float g_calFactor = DEFAULT_CALIBRATION_FACTOR;  // active factor (loaded from NVS)
bool g_scaleReady = false;

// ---- HX711 diagnostics (/scaleinfo) -----------------------------------------------
// The HX711 itself has no status/health registers (unlike the PN5180) -- it's a bare
// 24-bit ADC. What's useful to surface instead: the raw (unscaled, untared) reading, so
// a stuck-near-full-scale value is visible as "ADC saturating" rather than just a wrong
// weight; whether the chip is currently answering ready() at all; and a noise estimate
// (std-dev over a short raw-reading window) as a proxy for wiring/load-cell health that
// the chip can't report directly but we can compute from what we already sample.
volatile long g_scaleRawLast = 0;        // most recent raw ADC reading (get_value(1), no smoothing)
volatile bool g_scaleHxReady = false;    // scale.is_ready() as of the last scaleTask iteration
volatile uint32_t g_scaleLastReadMs = 0; // millis() of the last successful HX711 read
#define SCALE_NOISE_WINDOW 10
volatile long g_scaleNoiseBuf[SCALE_NOISE_WINDOW] = {0};
volatile uint8_t g_scaleNoiseCount = 0;  // how many of the buffer's slots are filled (ramps up to WINDOW once)
// Stuck-HX711 watchdog. Symptom seen in the field: the scale stopped measuring and only
// came back after a full power cycle -- /scaleinfo showed raw == offset frozen and
// noiseStdDev exactly 0. That last part is the reliable signal: a real load cell always
// dithers by a few ADC counts (thermal + mains + quantisation), so ZERO variation over
// a whole window means no fresh conversion is arriving, not "a very steady weight".
//
// Explicitly NOT triggered by is_ready()==false: at 10 SPS polled every 20ms that is
// false most of the time by design (roughly 1 in 8 polls sees a sample), so resetting
// on it would re-init the chip several times a second.
volatile uint32_t g_scaleStuckSince = 0;    // millis() when raw last MOVED (0 = unknown yet)
// "Moved" means: away from the value the current stuck-window started at, by more than
// this many ADC counts. NOT plain inequality against the previous sample: at rest the
// dither is only a few counts wide, so it repeats a value now and then and sits there
// for seconds -- which a != check reads as "frozen" and power-cycles a perfectly healthy
// chip. Measured on the assembled unit: noiseStdDev 22-50 counts, raw wandering across a
// ~100-count band. 8 counts is comfortably inside that band (so live dither always
// clears it) and far below any real weight change (~0.02 g at the current calibration
// factor), while a truly wedged HX711 repeats one exact value forever and never clears
// it. This is what the comment above always described; the old check just didn't
// implement it.
volatile uint32_t g_scaleRecoveries = 0;    // how many times the watchdog fired since boot
volatile uint32_t g_scaleLastRecoveryMs = 0;
static const long SCALE_STUCK_TOLERANCE = 8;  // ADC counts, see above
static const uint32_t SCALE_STUCK_MS = 5000;  // frozen this long -> power-cycle the HX711
volatile uint8_t g_scaleNoiseIdx = 0;

String g_lastUid = "";       // UID of the last detected tag (hex), from the PN5180 poll
String g_lastTagData = "";   // text read back from the tag (ID pages)
long g_lastSpoolId = -1;     // databaseId parsed from the tag (-1 = none)

// PN5180 (ISO15693/NFC-V) test state — set by the loop poll, readable via /nfc5180.
String g_pn5180Uid = "";
bool g_pn5180Present = false;

// NFC debug mode: a web UI toggle. ON = the reader only reads and reports the tag
// TYPE (NFC-V/NFC-A) + UID/ATQA/SAK, no normal flow. OFF = normal operation
// (500ms ISO15693 poll, sets g_pn5180Uid/g_pn5180Present for /nfc5180 + the flow).
bool g_nfcDebug = false;
PN5180ProbeResult g_nfcProbe;   // latest debug result, readable via /nfcprobe
// Debug mode toggled via HTTP (core 1), but pn5180Recover() touches the PN5180 SPI
// object shared with pn5180Task (core 0) -> delegate via flag, same as g_tareReq.
volatile bool g_nfcDebugChangeReq = false;

// TFT screen preview (web UI System tab) toggled via HTTP (core 1), but the actual TFT
// draw calls (g_tft.*) all happen on core 0 inside pn5180Task/menuTick -> same
// delegate-via-flag pattern as g_nfcDebugChangeReq, never touch the TFT from here.
volatile bool g_menuPreviewReq = false;
volatile bool g_menuPreviewReqOn = false;
volatile bool g_menuPreviewStepReq = false;  // ?next=1 -- advance one screen, no encoder

// Test menu (hidden diagnostics, see menu.h MENU_TEST*) LED override: true while
// either the physical LED test screen or the web UI's "Diagnostics" card owns the
// pixels directly. ledTick() (below, defined BEFORE menu.h is included, so it cannot
// reference the MenuScreen enum directly -- hence this decoupled bool) checks this
// FIRST and backs off entirely, else it would overwrite a test color with the normal
// idle/flash color on its very next tick.
volatile bool g_ledTestActive = false;

// Web UI "Diagnostics" card (Debug tab) LED-test-color request. Cross-core: the HTTP
// handler runs on core 1, ledShow() must run where pixel/pixelExt actually live --
// consumed in the poll task, same delegate-via-flag pattern as g_menuPreviewReq.
volatile bool g_testLedReq = false;
volatile int  g_testLedReqColorIdx = -1;  // index into kTestLedColors, -1 = release override

// Same pattern for the web UI's TFT test pattern. pattern: 0-4 = full-screen color
// index (see kTestTftColors in menu.h), 10 = font sample, 20 = grayscale ramp, -1 =
// release (back to whatever the real menu state is).
volatile bool g_testTftReq = false;
volatile int  g_testTftReqPattern = -1;
volatile bool g_tftTestWebActive = false;  // true while the web-triggered pattern owns the TFT

// /nfcdump: raw sector/block dump of an unrecognized tag (debug view fallback when
// hasExtendedData is false). Same cross-core delegation as the writes below -- the
// actual Mifare auth+read only ever happens inside pn5180Task, HTTP handlers just
// set a request flag and poll for the result.
volatile bool g_nfcDumpReq = false;
volatile bool g_nfcDumpPending = false;
volatile bool g_nfcDumpDone = false;
volatile bool g_nfcDumpOk = false;
String g_nfcDumpErr = "";
// Sized by the LARGEST carrier, not by Mifare: NTAG216 has 226 pages (888 user bytes +
// 4 header pages), which packed 4-per-16-byte-row needs 57 rows. Mifare Classic 1K uses
// only the first 47 (its data blocks), NFC-V at most 57*4 = 228 blocks -- more than any
// ICODE variant here. Sizing this by the Mifare case instead silently truncated an
// NTAG216 dump to 188 of its 226 pages, which is exactly the kind of shifted/short image
// that corrupts a consumer's parsing without failing loudly.
static const int NFC_DUMP_MAX_ENTRIES = 57;
MifareBlockDump g_nfcDumpBlocks[NFC_DUMP_MAX_ENTRIES];
String g_nfcDumpTagType = "";     // "mifareClassic1k" | "ntag" | "nfcv"
String g_nfcDumpUid = "";
String g_nfcDumpVariant = "";     // NTAG only: "ntag213"/"ntag215"/"ntag216"
int g_nfcDumpUnitBytes = 16;      // addressable unit: 16 (Mifare block) or 4 (page/block)
int g_nfcDumpUnitCount = 0;       // units actually read (rows*4 overshoots on 4-byte carriers)
int g_nfcDumpCount = 0;
// Wall-clock duration of the last dump's auth+read loop alone (excludes the probe and
// the RF reset around it). Reported by /nfcdumpstatus so the cost of a full 16-sector
// dump can be measured on real hardware instead of estimated -- the deciding number
// for whether a raw-read endpoint can answer synchronously or has to be 202+polling.
uint32_t g_nfcDumpMs = 0;
int g_nfcDumpAuthOkSectors = 0;  // how many of the 16 sectors authenticated

// /nfcreadstart + /nfcreadstatus: raw tag image for an external parser (foreign vendor
// formats live outside this firmware -- see HARDWARE.md). Same request/poll handoff as
// the dump above; the actual RF work only ever runs inside pn5180Task.
//
// Keys are supplied per request and never stored: vendor tags derive them from the UID,
// so the caller that knows the derivation owns them, not the device.
volatile bool g_nfcReadReq = false;
volatile bool g_nfcReadPending = false;
volatile bool g_nfcReadDone = false;
volatile bool g_nfcReadOk = false;
volatile bool g_nfcReadRetryable = false;  // true = transient (tag moved), false = wrong key/format
volatile unsigned long g_nfcReadDoneAt = 0;
String g_nfcReadErr = "";
String g_nfcReadUid = "";        // echoed back so a caller can discard a swapped-tag result
String g_nfcReadTagType = "";    // "mifareClassic1k" | "ntag"
String g_nfcReadHex = "";        // full image, lowercase hex, no separators
int g_nfcReadStartPage = 0;      // NTAG only: always 0 (absolute offsets, see the dump fn)
String g_nfcReadNtagVariant = "";  // NTAG only: chip variant, diagnostics only
uint16_t g_nfcReadSectorMask = 0xFFFF;   // which sectors were requested
uint16_t g_nfcReadSectorsOk = 0;         // bit N = sector N authenticated
uint32_t g_nfcReadMs = 0;
// 16 sectors x 6 bytes, filled from the request. g_nfcReadHasKeyB stays false when the
// caller sent none, which keeps the single-auth fast path.
uint8_t g_nfcReadKeyA[16][6];
uint8_t g_nfcReadKeyB[16][6];
volatile bool g_nfcReadHasKeyA = false;
volatile bool g_nfcReadHasKeyB = false;
// Suppresses the TFT load flow for a FOREIGN tag while it's being read over HTTP (see
// flowOnTagPresent). Deliberately longer than the write path's 3 s: a caller chains
// /nfcprobe and /nfcreadstart, a full dump alone can take ~2.5 s, and a user may press
// "read tag" repeatedly -- the window has to span the gaps between those, not just one
// read, or the flow fires in between and the suppression is pointless.
volatile unsigned long g_nfcReadSuppressUntil = 0;

// Extended tag data (plan C.4 "Lesekosten beachten"): the Mifare Extended / NTAG
// OpenSpool read costs 1-3 extra authenticated block reads or NDEF page reads on top
// of the already-running poll -- running it every 500ms poll tick would be wasted work
// for a tag that isn't changing. Cached per UID: only re-run when the UID changes (new
// tag, or tag removed+re-placed), same "handle it once" pattern as g_lastUid below.
String g_nfcExtCacheUid = "";        // UID this cache was computed for ("" = none/stale)
bool g_nfcExtCacheHasExtended = false;
SpoolTagData g_nfcExtCacheData;
String g_nfcExtCacheFormatLabel = "";   // human-readable, for the web UI
String g_nfcExtCacheWriteFormat = "";   // machine id, for the web UI ("octoscaleExtended"/"openSpool"/"nfcvExtended")
int g_nfcExtCacheCapacityBytes = 0;
// Occupancy of a tag we could NOT parse: distinguishes "blank, safe to write" from
// "carries somebody else's data" -- hasExtendedData=false alone conflates the two, so
// a foreign vendor tag (Bambu et al.) currently looks exactly like a fresh one and
// nothing stops a write from destroying it. Values: "empty" (nothing on it),
// "foreign" (data present, not a format we know), "" (not determined / it IS ours).
// For NTAG/NFC-V this comes from the Capability Container the probe already read; for
// Mifare Classic there is no CC, so a failed default-key auth stands in for it: every
// tag OctoScale writes uses the factory key, so "key rejected" means "not ours".
String g_nfcExtCacheOccupancy = "";

// Maps a probed tag to the format the firmware would use if asked to write spool data
// to it right now (mirrors pn5180WriteSpoolTag's dispatch, without actually writing).
// ntagPages/ntagPagesPresent come from the probe that just ran (PN5180ProbeResult) --
// see the NTAG branch below for why they can't be re-queried here.
static void nfcClassifyTag(PN5180TagType type, uint16_t atqa, uint8_t sak,
                           String &writeFormatOut, String &formatLabelOut, int &capacityBytesOut,
                           int ntagPages = 0, bool ntagPagesPresent = false) {
  if (type == PN5180_TAG_NFCV) {
    writeFormatOut = "nfcvExtended";
    formatLabelOut = "Extended";
    capacityBytesOut = PN5180_ID_BYTES + 8 + 8 + 16 + 48;  // legacy anchor + magic/num/phys/strings blocks
  } else if (type == PN5180_TAG_NFCA) {
    bool isClassic = (sak == 0x08 || sak == 0x18 || sak == 0x09 || sak == 0x28);
    if (isClassic) {
      writeFormatOut = "octoscaleExtended";
      formatLabelOut = "Extended";
      capacityBytesOut = 16 * 5;  // blocks 4,8,9,12,13,14 minus block 4's own 12 already counted elsewhere -> approx sector capacity
    } else {
      writeFormatOut = "openSpool"; formatLabelOut = "OpenSpool";
      // NTAG213/215/216 differ 6x in user memory (144/504/888 B) and only GET_VERSION
      // can tell them apart -- but that command is only answered right after the SELECT,
      // before any READ, and by the time this runs the card has already been read from
      // and deselected (pn5180Probe ends with reset()+setupRF()). Calling it here always
      // timed out and silently reported the NTAG213 fallback for every tag. The probe
      // already asks at the one moment it works, so take its answer instead of asking
      // again at the wrong one; numPagesPresent=false means it genuinely couldn't tell.
      capacityBytesOut = ntagPagesPresent
                       ? (ntagPages - 4) * 4 - 22   // drop pages 0-3, minus NDEF/record overhead
                       : ntagUserBytes(NTAG_213) - 22;  // conservative fallback, same as the write path's
    }
  } else {
    writeFormatOut = "spoolIdNtag"; formatLabelOut = "unknown"; capacityBytesOut = 0;
  }
}

// OTA in progress (either ArduinoOTA/espota or web /update upload): the TFT menu locks
// to a dedicated "Software update" screen (progress bar, ignores all input) and the
// status LED cycles red/yellow/green with the progress instead of its normal states.
// Set from loop() (core 1, where both OTA paths run); read from pn5180Task (core 0).
volatile bool g_otaInProgress = false;
volatile uint8_t g_otaProgressPct = 0;   // 0..100, best-effort (web /update: unknown total -> stays 0 until done)

// Display backlight + timeout (screensaver). After g_blTimeoutSec with no activity
// (encoder/button, a real weight change, an NFC tag, web UI access) the display dims
// from g_blActive to g_blDim. Activity resets g_blActivity (millis). pn5180Task drives
// the brightness. All three values are NVS-persistent.
uint8_t g_blActive = 255;              // active brightness (0..255)
uint8_t g_blDim = 40;                  // dimmed brightness (~15%)
uint16_t g_blTimeoutSec = 30;          // seconds until dimming (0 = never dim)
volatile unsigned long g_blActivity = 0;   // last activity timestamp (millis); set by
                                           // the web UI + pn5180Task -> volatile

// Callable from anywhere: "something happened" -> keep/wake the display.
inline void displayTouch() { g_blActivity = millis(); }

// Logo screensaver (separate from backlight dimming above): after g_ssTimeoutSec idle
// AND the flow sitting at FLOW_IDLE, the TFT menu shows the logo full-screen. Any
// input/tag/state change exits it immediately. NVS-persistent, web UI toggle.
bool     g_ssEnabled = true;
uint16_t g_ssTimeoutSec = 60;          // 1 minute default

// Display off (third and last idle stage, after dimming and the logo screensaver):
// after g_offTimeoutSec of no activity the backlight goes to 0 and the ST7789 is put
// to sleep (see displaySleep()). Any activity wakes it. Off by default -- the two
// milder stages are what most users want, and a fully dark panel is easy to mistake
// for a crashed device, so it stays an opt-in. NVS-persistent like the others.
bool     g_offEnabled = false;
uint16_t g_offTimeoutSec = 300;        // 5 minutes default (when enabled)

// Buzzer control (NVS-persistent). Master on/off in the TFT menu + web UI; mode,
// volume, and frequency in the web UI only (an active buzzer ignores vol/freq).
bool     g_buzEnabled = true;   // signal tones on/off
uint8_t  g_buzMode    = 1;      // 0 = active (on/off), 1 = passive (LEDC tone)
uint8_t  g_buzVol     = 200;    // volume 0..255 (passive only)
uint16_t g_buzFreq    = 2700;   // base pitch in Hz (passive only)
#include "buzzer.h"             // after the g_buz* definitions it references as extern

// The PN5180 runs on its OWN task on core 0 (not in loop()!). Reason: the PN5180
// library has unbounded while-loops (setRF_on/BUSY waits) -> a reader hang would
// otherwise freeze the web server (loop, core 1) too. Writing (blocking + risky) is
// only triggered by the HTTP handler via a request flag; pn5180Task executes it and
// reports the result back. Cross-core -> volatile.
volatile bool g_nfcWriteReq = false;    // HTTP handler -> task: please write this ID
volatile long g_nfcWriteId = -1;        // ID to write
volatile bool g_nfcWriteDone = false;   // task -> handler: done
volatile bool g_nfcWritePending = false;  // a write is in flight (set on start, cleared once /nfcwritestatus consumes the result)
volatile unsigned long g_nfcWriteDoneAt = 0;  // millis() when done was set (0 = n/a); auto-clears pending if never polled
// After a write, the tag is re-read once (forced by g_lastUid=""); suppress the load
// flow from auto-triggering on that re-read (user is in the write UI, not loading).
volatile unsigned long g_nfcWriteSuppressUntil = 0;
// A flow trigger that arrived while the post-write suppression window was still open.
// Parked here instead of dropped: the forced re-read after a write lands inside that
// window, and the poll's "same UID -> already handled" gate means it is never offered
// again. pn5180Task retries it once the window closes. Not g_lastUid="" -- that would
// re-run the whole expensive path (extended read + beep) on every poll tick.
String g_flowRetryUid = "";
long g_flowRetryId = -1;
volatile bool g_nfcWriteOk = false;     // result
String g_nfcWriteErr = "";              // error text (task writes, handler reads after Done)

// POST /nfcwritespool and POST /nfcerase: two more write modes that share
// g_nfcWriteReq/Pending/Done/Ok/DoneAt/SuppressUntil above (only one write in flight
// at a time, any kind) but differ in what pn5180Task runs and what it reports back.
// g_nfcWriteKind distinguishes which of the three write functions pn5180Task should run.
enum NfcWriteKind { NFCWRITE_ID, NFCWRITE_SPOOL, NFCWRITE_ERASE };
volatile NfcWriteKind g_nfcWriteKind = NFCWRITE_ID;
// Set when a write is refused because the chosen format's payload does not fit the tag.
// Reported as structured fields on /nfcwritestatus so a caller can react ("pick a bigger
// tag" / "switch format") without having to parse the error string.
volatile int g_nfcWriteCapAvail = -1;   // bytes the tag offers for the payload region
volatile int g_nfcWriteCapNeeded = -1;  // bytes this spool's data actually needs
SpoolTagData g_nfcWriteSpoolData;          // filled by the HTTP handler before g_nfcWriteReq is set
// Which of the two NFC-V Extended formats to use, per-request (SpoolManagerExtended plugin
// setting, sent with every /nfcwritespool call) -- "extended" (default, OctoScale's own
// binary layout) or "openSpool" (standard Type 5 NDEF, generic-phone-app readable).
// Irrelevant for Mifare, which has only one Extended format.
String g_nfcWriteNfcvFormat = "extended";
// Same idea for NTAG -- "openSpool" (default/backward-compatible, standard NDEF) or
// "extended" (ntagExtended, OctoScale's own binary layout, NTAG215/216 only).
String g_nfcWriteNtagFormat = "openSpool";
String g_nfcWriteFormat = "";              // task writes: "octoscaleExtended" | "openSpool" | "nfcvExtended" | "ntagExtended"
int g_nfcWriteBytesWritten = 0;            // task writes
String g_nfcWriteDroppedFields = "";       // task writes (comma-separated field names)
// Set fields the chosen format cannot carry at all (vs. droppedFields = didn't fit).
String g_nfcWriteUnsupportedFields = "";
String g_nfcWriteUid = "";                 // task writes: UID of the tag that was just written (hex, no separator)

// Result handoff for the TFT result screen. g_nfcWriteDone can NOT be used for this:
// it is cleared by the /nfcwritestatus handler (core 1) as soon as the caller polls,
// and SpoolManagerExtended polls every 500ms. Since the blocking write runs inside pn5180Task
// -- the same task as menuTick() -- the flag is often already gone by the time the
// menu gets its next tick, so a fast write (Mifare/NFC-V, ~2-3s) would show the "in
// progress" screen and then no result at all. pn5180Task therefore latches the result
// here itself; only the menu clears it, once its display window has elapsed.
volatile bool g_nfcMenuResultPending = false;  // set by pn5180Task, cleared by menuTick

// A successful OpenSpool write can still lose almost everything: the JSON is built to
// fit the tag's budget by dropping fields from the back of a priority list, and on a
// small NFC-V tag (e.g. a 112-byte SLI-X, ~86 B of usable JSON) that can strip it down
// to the mandatory core (protocol/version/os_db_id). Reporting a bare "OK" there is
// misleading -- the tag reads back as little more than an ID. Judged by content, not
// by count: material and color are what make a tag useful to a reader, so losing
// either means the write deserves a warning even though it technically succeeded.
// Returns "" when nothing worth warning about was dropped.
//
// Two shapes of drop worth two different messages: OpenSpool's whole-field dropping
// (checked above, JSON-key substrings) loses core spool identity (material/color) --
// alarming, tag barely usable. Extended's v3 fields (octoscaleExtended/nfcvExtended)
// never drop a WHOLE field this way -- droppedFieldsOut there only ever means a text
// field got shortened to fit its byte cap (see pn5180WriteMifareExtended's v3 string
// buffer), so a plain field-name list with no quoting -- reported separately, less
// alarming wording, since numeric/core fields are never affected.
// Fields the caller supplied that the chosen format has no place for. Distinct from
// droppedFields: that means "didn't fit, a bigger tag would help", this means "this
// format cannot carry it at all, only a different format would". Both end in the same
// outcome -- the value is not on the tag -- but only one of them is the user's to fix
// by choosing different hardware, so a caller has to be able to tell them apart.
//
// Reports only fields that were actually SET (past their -1/"" sentinel). Listing every
// theoretically-unsupported field would bury the one the user cares about.
static String nfcWriteUnsupportedFields(const SpoolTagData &d, const String &format) {
  String out;
  auto add = [&](const char *name) { if (out.length()) out += ","; out += name; };

  // Drying data and opacity: OpenPrintTag has all three (spec keys 57/58/27), tigerTag
  // has the two drying fields but not td, and since the Extended v5/v3/v4 layouts all
  // three Extended carriers have all three too. What is left below genuinely has
  // nowhere to put them -- which is the case that prompted this function: a successful
  // write plus values quietly left behind.
  bool extHasDrying = (format == "octoscaleExtended" || format == "ntagExtended" ||
                       format == "nfcvExtended");
  if (format != "nfcvOpenPrintTag" && format != "tigerTag" && !extHasDrying) {
    if (d.dryingTemperature >= 0) add("dryingTemperature");
    if (d.dryingTime >= 0) add("dryingTime");
  }
  // tigerTag stores dryingTemperature/dryingTime (bytes 28/29) but not td -- see
  // pn5180WriteNtagTigerTag. The Extended layouts do carry td.
  if (format != "nfcvOpenPrintTag" && !extHasDrying && d.td >= 0) add("td");
  // OpenSpool carries a fixed 12-key JSON schema; everything below has no key there.
  // (OpenPrintTag's own gaps are not enumerated here -- it takes the widest field set
  // of all our formats, and its misses are the spec's, not a layout limitation.)
  // Only octoscaleExtended and ntagExtended carry a time of day -- their layouts had
  // reserved bytes to put it in. nfcvExtended is NOT in this list: it stores no date
  // fields at all (no firstUse/lastUse/purchasedOn on the wire), so a time of day there
  // would have nothing to qualify. OpenPrintTag's spec has no field for it either, and
  // OpenSpool's 12-key schema is fixed.
  if (format != "octoscaleExtended" && format != "ntagExtended") {
    if (d.firstUseMinuteOfDay >= 0) add("firstUseMinuteOfDay");
    if (d.lastUseMinuteOfDay >= 0) add("lastUseMinuteOfDay");
    if (d.purchasedOnMinuteOfDay >= 0) add("purchasedOnMinuteOfDay");
  }
  if (format == "openSpool" || format == "nfcvOpenSpool") {
    if (d.density >= 0) add("density");
    if (d.spoolWeight >= 0) add("spoolWeight");
    if (d.usedWeight >= 0) add("usedWeight");
    if (d.temperature >= 0) add("temperature");
    if (d.bedTemperature >= 0) add("bedTemperature");
    if (d.enclosureTemperature >= 0) add("enclosureTemperature");
    if (d.remainingWeight >= 0) add("remainingWeight");
    if (d.totalLength >= 0) add("totalLength");
    if (d.usedLength >= 0) add("usedLength");
    if (d.cost >= 0) add("cost");
    if (d.code.length()) add("code");
    if (d.batchNumber.length()) add("batchNumber");
    if (d.purchasedFrom.length()) add("purchasedFrom");
    if (d.finish.length()) add("finish");
    if (d.displayName.length()) add("displayName");
  }
  // TigerTag Standard: fixed 80-byte foreign layout (see pn5180WriteNtagTigerTag).
  // Only the 6 registry IDs, color, one weight-or-length "measure" value, and 6
  // temperature/drying bytes have a home there -- everything else this codebase
  // otherwise carries has nowhere to go on this format.
  if (format == "tigerTag") {
    if (d.vendor.length()) add("vendor");
    if (d.colorName.length()) add("colorName");
    if (d.density >= 0) add("density");
    if (d.diameterTolerance >= 0) add("diameterTolerance");
    if (d.spoolWeight >= 0) add("spoolWeight");
    if (d.usedWeight >= 0) add("usedWeight");
    if (d.remainingWeight >= 0) add("remainingWeight");
    if (d.usedLength >= 0) add("usedLength");
    if (d.totalLength >= 0 && d.totalWeight >= 0) add("totalLength");  // measure slot already used by totalWeight
    if (d.enclosureTemperature >= 0) add("enclosureTemperature");
    if (d.offsetTemperature != 0) add("offsetTemperature");
    if (d.offsetBedTemperature != 0) add("offsetBedTemperature");
    if (d.offsetEnclosureTemperature != 0) add("offsetEnclosureTemperature");
    if (d.cost >= 0) add("cost");
    if (d.code.length()) add("code");
    if (d.batchNumber.length()) add("batchNumber");
    if (d.purchasedFrom.length()) add("purchasedFrom");
    if (d.finish.length()) add("finish");
    if (d.displayName.length()) add("displayName");
    if (d.firstUse >= 0) add("firstUse");
    if (d.lastUse >= 0) add("lastUse");
    if (d.purchasedOn >= 0) add("purchasedOn");
  }
  return out;
}

static String nfcWriteDropWarning(const String &dropped, const String &format) {
  if (!dropped.length()) return "";
  if (format == "octoscaleExtended" || format == "nfcvExtended" || format == "ntagExtended") {
    return "some text fields were shortened to fit the tag: " + dropped;
  }
  bool lostType  = dropped.indexOf("\"type\"") >= 0;
  bool lostColor = dropped.indexOf("\"color_hex\"") >= 0;
  if (!lostType && !lostColor) return "";
  return F("tag too small for this format - only the spool ID was stored, "
           "no material/color/temperatures. Use the Extended format for this tag.");
}

// ---- Load-flow state machine ---------------------------------------------
// UI-agnostic: the web UI and the TFT menu drive the same state machine via the
// /flow/* endpoints. pn5180Task sets the starting point on a new tag.
enum FlowState {
  FLOW_IDLE,          // nothing present / starting state
  FLOW_DB_CHECK,      // spool is being checked against the DB
  FLOW_UNKNOWN,       // spool not in the DB
  FLOW_ASK_ACTION,    // spool known -> load or weigh?
  FLOW_ASK_PRINTER,   // load chosen -> choose printer
  FLOW_ASK_TOOL,      // printer chosen -> choose tool
  FLOW_LOADING,       // the load URL is being called
  FLOW_WEIGH_CONFIRM, // weigh chosen -> confirm the reading
  FLOW_WEIGHING,      // the weight is being written back
  FLOW_DONE,          // loaded or weighed successfully
  FLOW_ERROR          // error (message in g_flowMsg)
};
volatile FlowState g_flowState = FLOW_IDLE;
long g_flowSpoolId = -1;      // databaseId of the current flow
String g_flowSpoolName = "";  // display name from the DB
int g_flowPrinter = -1;       // chosen OctoPrint instance (index)
int g_flowToolCount = 0;      // tool count of the chosen printer (live)
int g_flowTool = -1;          // chosen tool
String g_flowMsg = "";        // status/error message for the UI
volatile bool g_flowDbRequest = false;  // DB-check trigger from pn5180Task
// FLOW_UNKNOWN reached with a foreign vendor tag (occupancy=="foreign") rather than a
// blank/unassigned one -- lets the TFT report it neutrally instead of as an error.
volatile bool g_flowForeignTag = false;
volatile bool g_flowEmptyTag = false;    // ... or a verified-blank one (occupancy=="empty")
// Foreign tag (e.g. Snapmaker U1): no databaseId payload on the tag, only a UID ->
// runDbCheck() resolves it via spoolExistsByCode() instead of spoolExists(id).
bool g_flowLookupByCode = false;
String g_flowLookupUid = "";

// TFT menu delegation: the three blocking flow transitions (flowDoPrinter/Tool/
// WeighSave) make octo* HTTP calls and may only run in the loop() task (core 1). The
// menu (pn5180Task, core 0) only sets this flag; loop() executes it. Same pattern as
// g_flowDbRequest. Cross-core -> volatile.
enum FlowMenuAction { FMA_NONE = 0, FMA_PRINTER, FMA_TOOL, FMA_WEIGH_SAVE };
volatile FlowMenuAction g_flowMenuReq = FMA_NONE;
volatile int g_flowMenuArg = 0;   // printer idx (FMA_PRINTER) or tool n (FMA_TOOL)

// DB reachability (for the TFT footer + system page): updated periodically in loop()
// (core 1) by pinging the DB source instance (blocking HTTP call -> only there). The
// TFT task only reads the cached flag. -1 = never checked, 0 = offline, 1 = reachable.
volatile int g_dbReachable = -1;

// Reference values for the spool from the DB (-1 = not set). Without the empty
// weight, remaining filament can't be derived from the scale's gross reading -> the
// weigh step reports that instead of writing a meaningless value.
float g_flowSpoolWeight = -1.0f;   // empty spool weight (g)
float g_flowTotalWeight = -1.0f;   // original filament amount (g)
float g_flowGrossWeight = 0.0f;    // reading to be confirmed (g)

// Extra info from the DB for display (the "what to do" screen + web UI).
String g_flowVendor = "";          // manufacturer
String g_flowMaterial = "";        // material (PLA/PETG/...)
float  g_flowRemaining = -1.0f;    // remaining weight (g), computed by the plugin (-1 = unknown)
String g_flowColor = "";           // color CODE ("#rrggbb", ";"-multi, "rainbow", "transparent[:..]")
String g_flowColorName = "";       // color display name (possibly localized)

// Timeout (s) for how long the selection buttons (ask_action/ask_printer/ask_tool)
// stay up after the TAG IS REMOVED, before the flow falls back to idle. Lets the user
// briefly read the tag, set the spool aside, and then click. 0 = infinite (never
// auto-resets). Loaded from NVS.
uint8_t g_flowAskTimeoutSec = 30;  // default 30s

// Timestamp (millis) since an ask_* state has had NO tag present, i.e. the auto-reset
// countdown is running. 0 = tag is (still) present OR no countdown active. Set by the
// PN5180 poll, read by flowStatusJson (the timer bar in the web UI). volatile:
// cross-task access (pn5180Task writes, the HTTP task reads).
volatile unsigned long g_flowAskGoneSince = 0;

// ---- WS2812 status LED (1 pixel) -----------------------------------------
// The idle color reflects the base state (boot/AP/connected). Events (NFC read,
// write, OK, error) flash a color briefly and then fall back to the idle color.
// Fully non-blocking: ledTick() in loop() handles the timing.
enum LedState { LED_BOOT, LED_AP, LED_CONNECTED };
volatile LedState g_ledState = LED_BOOT;

// Written from pn5180Task, read from loop/ledTick -> volatile (separate cores).
static volatile uint32_t g_flashColor = 0;      // active flash color (0 = no flash)
static volatile unsigned long g_flashUntil = 0; // millis until the flash ends
const uint8_t LED_BRIGHT = 40;            // base brightness (0-255), dimmed

// Global LED brightness, 0-255, applied in ledShow() as a final scale over whatever
// colour the caller produced (see ledScale()). Deliberately a post-multiplier rather
// than something the colour-producing code knows about: every status colour, blink
// pattern and flash keeps its own ratios, they just come out brighter or dimmer as a
// whole. 255 = the colours exactly as written (the previous behaviour, hence the
// default). NVS-persistent, set from the web UI.
uint8_t g_ledBrightness = 255;

// Onboard WS2812 on/off. The external LED on the enclosure front is the one the user
// actually sees; the S3's own LED ends up inside the closed case, where it is only
// visible as a glow through the seams. This switches that one off without touching
// the external mirror. Brightness (above) applies to both regardless.
bool g_ledOnboard = true;

// The plain single-colour LED on GPIO 2 (LED_PIN), lit while WiFi is connected. Its
// own switch: it is a bare on/off indicator, unaffected by the brightness setting
// (no PWM on it) and independent of the RGB strands above.
bool g_ledPin2 = true;

// Applies g_ledPin2 to the pin. Called whenever the flag or the connection state
// changes -- the LED only means "WiFi connected", so it is lit exactly when both are
// true. Cheap enough to just recompute rather than track a previous value.
static inline void ledPin2Apply() {
  digitalWrite(LED_PIN, (g_ledPin2 && WiFi.status() == WL_CONNECTED) ? HIGH : LOW);
}

// Idle color matching the base state (dimmed). While connected the LED pulses a very
// faint green ("breathing") as a subtle heartbeat; a flash (brighter) stands out
// clearly against it.
const uint8_t LED_BREATH_MAX = 18;  // peak brightness of the idle pulse (dim)
static uint32_t idleColor() {
  // Software update in progress: a steady 1s blue/off blink, deliberately NOT tied to
  // g_otaProgressPct. It used to be a traffic light (red -> yellow -> green by percent)
  // and later that same traffic light blinking at 500ms -- but progress-driven color
  // plus blinking reads as chaotic on a single pixel: the stage can flip in the middle
  // of a blink, so you see a lone yellow flash between reds and cannot tell an update
  // from a fault. One color, one rhythm answers the only question this LED can usefully
  // answer mid-flash ("is it still alive?"). The percentage is on the web UI, which is
  // where a number belongs. Highest priority, overrides everything else.
  if (g_otaInProgress) {
    return ((millis() % 1000) < 500) ? pixel.Color(0, 0, LED_BRIGHT * 2) : 0;
  }
  // Tag write in flight: blink blue for as long as it actually takes. A write is the
  // one operation that both blocks the reader and must not be interrupted by pulling
  // the tag, so the LED has to stay honest about "still working" instead of running
  // out early. This used to be a fixed 3s ledFlash() started at write time -- but an
  // NTAG Extended write measured 11s on real hardware, so the flash expired mid-write
  // and the LED fell back to idle green while the tag was still being written: exactly
  // the moment a user is most likely to lift it. Driven by the pending flag rather than
  // a duration, so it ends when the write ends, not before.
  // Covers all three: /nfcwriteid, /nfcwritespool and /nfcerase share this flag, and
  // an erase is just as interruption-sensitive as a write.
  if (g_nfcWritePending && !g_nfcWriteDone) {
    // ~500ms period on/off -- clearly a blink, not the DB check's steady cyan.
    bool on = (millis() % 500) < 250;
    return on ? pixel.Color(0, 0, LED_BRIGHT * 2) : 0;
  }
  // Raw dump in flight: same blue blink, same reasoning. A full-image dump is a
  // multi-second RF job that must not be interrupted by lifting the tag (an NTAG215
  // page walk measured ~1.7s, a 16-sector Mifare auth+read loop noticeably longer),
  // so it gets the same "still working, leave the tag alone" signal as a write rather
  // than a fixed-duration flash that could expire mid-dump.
  if (g_nfcDumpPending && !g_nfcDumpDone) {
    bool on = (millis() % 500) < 250;
    return on ? pixel.Color(0, 0, LED_BRIGHT * 2) : 0;
  }
  // DB check in flight: solid cyan overrides the normal idle look, regardless of WiFi
  // state, so the LED clearly shows "working" instead of sitting dark/faint. Not a
  // pulse: spoolExists()/octoSpoolInfo() block on HTTPClient::GET(), which has no
  // progress callback -> ledTick() can't animate anything while that call is in
  // flight, only right before/after it. A steady color is the honest signal here.
  if (g_flowState == FLOW_DB_CHECK) {
    return pixel.Color(0, LED_BRIGHT * 2, LED_BRIGHT * 2);  // solid cyan
  }
  switch (g_ledState) {
    case LED_AP:
      return pixel.Color(LED_BRIGHT, LED_BRIGHT / 2, 0);  // orange (static)
    case LED_CONNECTED: {
      // triangle pulse, ~4s period, 0..LED_BREATH_MAX
      unsigned long ph = millis() % 4000;
      uint8_t g = ph < 2000 ? (ph * LED_BREATH_MAX) / 2000
                            : ((4000 - ph) * LED_BREATH_MAX) / 2000;
      return pixel.Color(0, g, 0);  // faint green, breathing
    }
    default:
      return pixel.Color(0, 0, LED_BRIGHT);  // blue (boot, static)
  }
}

// Triggers a brief color flash (ms), falls back to the idle color afterward.
static void ledFlash(uint32_t color, unsigned long ms) {
  g_flashColor = color;
  g_flashUntil = millis() + ms;
}

// Non-blocking: sets the flash color or the idle color. Called from BOTH loop() (core
// 1) and pn5180Task (core 0) -- same reasoning as buzzerTick() below. loop() blocks for
// several seconds at a time on synchronous HTTPClient calls (the 60s DB-reachability
// ping, printer polls, etc.); a short ledFlash() (800ms-1.5s) started right before one
// of those could run out and revert to idle before loop() ever got back around to
// calling this, so the flash would never actually appear on the LED. pn5180Task runs at
// ~100Hz and isn't affected by loop()'s blocking calls, so it picks up the slack.
// Serializes pixel.show() across cores. Adafruit_NeoPixel's ESP32 backend does a full
// rmt_driver_install()/rmt_driver_uninstall() cycle on EVERY show() (see esp.c), and
// ledTick() is deliberately called from BOTH loop() (core 1) and pn5180Task (core 0)
// so the LED keeps animating while loop() blocks on HTTP. Those two can land in
// espShow() at the same time: one core installs the RMT driver while the other is
// uninstalling it, and rmt_driver_install() then sends to a queue that is already
// gone -> "assert failed: xQueueGenericSend queue.c:820 (pxQueue)" and a reboot.
// Observed from both sides -- one crash backtrace through loop(), a later one through
// pn5180Task. Whoever holds the lock finishes its cycle before the other starts; a
// skipped frame is invisible on a status LED, so the loser simply drops its update
// rather than waiting (never block pn5180Task on the LED). The lock itself lives in
// ledShow() below, which is the only thing that touches either pixel.
static portMUX_TYPE g_ledMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool g_ledShowBusy = false;

// Single funnel for BOTH pixels. Everything that wants to change the LED goes through
// here -- ledTick(), buzzerLedSet(), setup() and the pre-blocking-HTTP paint in
// runDbCheck(). Two reasons it must be the only door:
//   1) The external pixel is a mirror, not an independent light. Setting one strand
//      without the other is always a bug, so there is no API that can do it.
//   2) The RMT lock now has to cover twice the work. pixelExt.show() runs the exact
//      same rmt_driver_install()/uninstall() cycle as pixel.show() (Adafruit's ESP32
//      backend does that per frame), so with two strands there are two install/uninstall
//      pairs per update -- twice the window for the queue.c:820 race that already bit
//      us here. Three call sites used to bypass the lock entirely; they now don't.
// Returns false if the other core holds the lock: the caller's frame is dropped rather
// than waited on (a skipped status-LED frame is invisible, and pn5180Task must never
// block on the LED).
// Not static: buzzer.h forward-declares it (included above, before this definition).
// Scales all three channels by g_ledBrightness/255. Done here, on the packed colour,
// instead of via Adafruit's setBrightness(): that one re-scales its own stored pixel
// buffer on every show() and rounds each time, which visibly shifts hue on the very
// low values this project uses (LED_BRIGHT/2 = 20 of 255). One multiply per channel
// against the caller's original value has no such drift. +127 rounds to nearest.
static inline uint32_t ledScale(uint32_t c) {
  uint8_t b = g_ledBrightness;
  if (b == 255) return c;
  uint8_t r = (uint8_t)(((((c >> 16) & 0xFF) * b) + 127) / 255);
  uint8_t g = (uint8_t)((((((c >> 8) & 0xFF) * b)) + 127) / 255);
  uint8_t bl = (uint8_t)((((c & 0xFF) * b) + 127) / 255);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | bl;
}

bool ledShow(uint32_t color) {
  bool mine = false;
  taskENTER_CRITICAL(&g_ledMux);
  if (!g_ledShowBusy) { g_ledShowBusy = true; mine = true; }
  taskEXIT_CRITICAL(&g_ledMux);
  if (!mine) return false;
  color = ledScale(color);
  // The onboard strand can be muted independently (g_ledOnboard). show() still runs
  // for it -- the WS2812 holds its last value until clocked again, so "off" has to be
  // written out, not merely skipped.
  pixel.setPixelColor(0, g_ledOnboard ? color : 0);
  pixel.show();
  pixelExt.setPixelColor(0, color);
  pixelExt.show();
  taskENTER_CRITICAL(&g_ledMux);
  g_ledShowBusy = false;
  taskEXIT_CRITICAL(&g_ledMux);
  return true;
}

// Last colour actually pushed to the strands. File-scope (not a static inside
// ledTick()) so a brightness change can invalidate it -- see the /led handler: the
// colour itself doesn't change when only the brightness does, so without forcing a
// mismatch here ledTick() would consider the LED up to date and the new brightness
// would not appear until the next status change.
static volatile uint32_t g_ledLastShown = 0xFFFFFFFF;

static void ledTick() {
  // Test menu (physical screen or web UI) owns the pixels directly right now -- back
  // off completely so ledTick() doesn't fight it every ~10ms. See g_ledTestActive.
  if (g_ledTestActive) return;
  // OTA in progress overrides everything (including a buzzer tone mid-flight) -> the
  // traffic-light color is the only thing the LED should show right now.
  if (!g_otaInProgress && g_buzLedColor) return;  // a buzzer tone is driving the LED
  uint32_t want;
  if (g_otaInProgress) {
    want = idleColor();  // traffic-light branch inside idleColor() takes over
  } else if (g_flashColor && millis() < g_flashUntil) {
    want = g_flashColor;
  } else {
    g_flashColor = 0;
    want = idleColor();
  }
  if (want != g_ledLastShown) {
    // Claim the RMT driver for this update; if the other core is mid-show(), skip this
    // frame instead of racing it. lastShown is only advanced by the core that actually
    // draws, so the skipped update is retried on the next tick (~10ms) rather than lost.
    // lastShown is only advanced when we actually drew, so a frame lost to the other
    // core is retried on the next tick (~10ms) rather than dropped for good.
    if (ledShow(want)) g_ledLastShown = want;
  }
}

// Saves the calibration factor to NVS and applies it live.
void applyCalFactor(float f) {
  g_calFactor = f;
  scale.set_scale(g_calFactor);
  prefs.begin("octoscale", false);
  prefs.putFloat("calFactor", g_calFactor);
  prefs.end();
  Serial.printf("Calibration factor saved: %.4f\n", g_calFactor);
}


// ---- Load flow: status + DB check ----------------------------------------
// Flow state names for the UI (order matches enum FlowState)
static const char *flowStateName(FlowState s) {
  switch (s) {
    case FLOW_IDLE:          return "idle";
    case FLOW_DB_CHECK:      return "db_check";
    case FLOW_UNKNOWN:       return "unknown";
    case FLOW_ASK_ACTION:    return "ask_action";
    case FLOW_ASK_PRINTER:   return "ask_printer";
    case FLOW_ASK_TOOL:      return "ask_tool";
    case FLOW_LOADING:       return "loading";
    case FLOW_WEIGH_CONFIRM: return "weigh_confirm";
    case FLOW_WEIGHING:      return "weighing";
    case FLOW_DONE:          return "done";
    case FLOW_ERROR:         return "error";
    default:               return "idle";
  }
}

// Current flow state + context as JSON (polled by the web UI/display).
static String flowStatusJson() {
  JsonDocument doc;
  doc["state"] = flowStateName(g_flowState);
  doc["spoolId"] = g_flowSpoolId;
  doc["spoolName"] = g_flowSpoolName;
  doc["vendor"] = g_flowVendor;
  doc["material"] = g_flowMaterial;
  doc["remaining"] = g_flowRemaining;
  doc["color"] = g_flowColor;
  doc["colorName"] = g_flowColorName;
  doc["printer"] = g_flowPrinter;
  doc["toolCount"] = g_flowToolCount;
  doc["tool"] = g_flowTool;
  doc["msg"] = g_flowMsg;
  // Weigh context: the UI shows what would be saved during weigh_confirm. grossWeight
  // in weigh_confirm is the LIVE scale reading (the spool is on it right now, the UI
  // polls and watches the value rise/settle); /flow/weigh saves that same live value.
  // 0 in every other state.
  doc["spoolWeight"] = g_flowSpoolWeight;
  doc["totalWeight"] = g_flowTotalWeight;
  doc["grossWeight"] = (g_flowState == FLOW_WEIGH_CONFIRM) ? g_weight : 0.0f;
  doc["canWeigh"] = (g_flowSpoolWeight >= 0.0f && g_flowTotalWeight >= 0.0f);
  // Countdown timer for the UI bar (only relevant in ask_* states):
  //  askTimeoutSec = configured timeout (0 = no timer -> UI shows no bar).
  //  askRemainMs   = ms remaining until auto-reset, but only once the tag is GONE
  //                  (countdown running). While the tag is present
  //                  (g_flowAskGoneSince==0) = -1 -> UI shows a full, static bar.
  {
    bool ask = (g_flowState == FLOW_ASK_ACTION || g_flowState == FLOW_ASK_PRINTER ||
                g_flowState == FLOW_ASK_TOOL);
    doc["askTimeoutSec"] = (ask ? g_flowAskTimeoutSec : 0);
    long remain = -1;  // -1 = countdown not running (tag present) or no timer
    if (ask && g_flowAskTimeoutSec > 0 && g_flowAskGoneSince != 0) {
      unsigned long total = (unsigned long)g_flowAskTimeoutSec * 1000UL;
      unsigned long elapsed = millis() - g_flowAskGoneSince;
      remain = (elapsed >= total) ? 0 : (long)(total - elapsed);
    }
    doc["askRemainMs"] = remain;
  }
  // Include the printer list directly so the UI can render buttons immediately in the
  // ASK_PRINTER state (no second request needed).
  JsonArray arr = doc["printers"].to<JsonArray>();
  for (uint8_t i = 0; i < g_octoCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["idx"] = i;
    o["name"] = g_octo[i].name.length() ? g_octo[i].name : g_octo[i].host;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// Runs the DB check (BLOCKING -> only call from loop(), never from pn5180Task).
// Triggered via g_flowDbRequest from pn5180Task.
static void runDbCheck() {
  g_flowDbRequest = false;
  // spoolExists() blocks (HTTP) -> ledTick() won't run again until it returns, so set
  // the solid "working" color up front instead of only after the fact.
  ledShow(idleColor());
  String name, err;
  bool known;
  if (g_flowLookupByCode) {
    dbgLogf("HTTP: GET spool/byCode/%s (instance %u)", g_flowLookupUid.c_str(), g_dbInstance);
    // Foreign tag: resolve UID -> databaseId first, then continue exactly like a
    // native tag (g_flowSpoolId now holds the real databaseId for the rest of the flow).
    long id = -1;
    known = spoolExistsByCode(g_flowLookupUid, id, name, err, &g_flowSpoolWeight,
                              &g_flowTotalWeight, &g_flowVendor, &g_flowMaterial,
                              &g_flowRemaining, &g_flowColor, &g_flowColorName);
    if (known) g_flowSpoolId = id;
    dbgLogf("HTTP: byCode -> known=%d databaseId=%ld instance=%d err=%s",
            known, id, g_dbUsedInstance, err.c_str());
  } else {
    dbgLogf("HTTP: GET spool/%ld (instance %u)", g_flowSpoolId, g_dbInstance);
    known = spoolExists(g_flowSpoolId, name, err, &g_flowSpoolWeight,
                        &g_flowTotalWeight, &g_flowVendor, &g_flowMaterial,
                        &g_flowRemaining, &g_flowColor, &g_flowColorName);
    dbgLogf("HTTP: spool/%ld -> known=%d instance=%d err=%s",
            g_flowSpoolId, known, g_dbUsedInstance, err.c_str());
  }
  if (known) {
    g_flowSpoolName = name;
    g_flowMsg = "";
    if (g_octoCount == 0) {
      g_flowState = FLOW_ERROR;
      g_flowMsg = "No OctoPrint instance configured";
      ledFlash(pixel.Color(LED_BRIGHT * 2, 0, 0), 1500);
    } else {
      // From here the user decides: load the spool or record a new weight.
      g_flowState = FLOW_ASK_ACTION;
      ledFlash(pixel.Color(0, LED_BRIGHT * 3, 0), 800);  // green: spool known
    }
  } else {
    g_flowSpoolName = "";
    // spoolExists distinguishes: empty err = DB reachable, spool not in it
    // (-> UNKNOWN); err set = the DB source(s) are unreachable (-> ERROR, so the user
    // can tell "offline" apart from "unknown spool").
    if (err.length()) {
      g_flowState = FLOW_ERROR;
      g_flowMsg = err;
    } else {
      g_flowState = FLOW_UNKNOWN;
      // A foreign vendor tag (Bambu, Snapmaker, ...) is not a fault -- it's a spool we
      // simply don't own yet. Announcing it in warning amber with an error tone made a
      // perfectly normal event look like a defect, and it was indistinguishable from a
      // blank tag. occupancy tells the two apart, so say which one it actually is.
      bool cacheFresh = (g_nfcExtCacheUid == g_pn5180Uid);
      g_flowForeignTag = cacheFresh && g_nfcExtCacheOccupancy == "foreign";
      g_flowEmptyTag   = cacheFresh && g_nfcExtCacheOccupancy == "empty";
      g_flowMsg = g_flowForeignTag  ? "Not in database - add it in the browser"
                : g_flowEmptyTag    ? "Ready to write"
                : g_flowLookupByCode ? "Tag UID not assigned to a spool"
                                     : "Spool not in database";
    }
    // Error tone only for a genuine fault (DB unreachable) or a tag that should have
    // been ours but isn't in the database. A vendor or blank tag is a normal thing to
    // put on the reader, so those get the neutral read beep instead.
    if (g_flowForeignTag || g_flowEmptyTag) buzzerRead();
    else buzzerError();
  }
}

// Resets the flow (cancel / after completion).
static void flowReset(bool rearmSameTag = true) {
  g_flowState = FLOW_IDLE;
  g_flowSpoolId = -1;
  g_flowSpoolName = "";
  g_flowForeignTag = false;
  g_flowEmptyTag = false;
  g_flowPrinter = -1;
  g_flowToolCount = 0;
  g_flowTool = -1;
  g_flowMsg = "";
  g_flowSpoolWeight = -1.0f;
  g_flowTotalWeight = -1.0f;
  g_flowGrossWeight = 0.0f;
  g_flowVendor = "";
  g_flowMaterial = "";
  g_flowRemaining = -1.0f;
  g_flowColor = "";
  g_flowColorName = "";
  g_flowLookupByCode = false;
  g_flowLookupUid = "";
  // rearmSameTag: clear the "already handled" latch so the very same tag triggers the
  // flow again on the next poll. Right when the tag was REMOVED (the poll's goneStreak
  // path) -- the next placement must be seen as new, even if it is the same spool.
  //
  // WRONG when the user backed out of the flow with the tag still ON the reader: the
  // latch would clear, the next poll (500 ms) would see a "new" tag and reopen the very
  // menu the user just dismissed, roughly every 1.5 s. That also froze the weight
  // readout, because flowOnTagPresent does a BLOCKING DB/OctoPrint lookup. The user
  // could never reach the live scale display to update a spool's weight on the device
  // -- the same task that draws the weight was stuck in HTTP. Keep the latch in that
  // case: the tag stays "handled" until it is physically lifted.
  if (rearmSameTag) g_lastUid = "";
}

// --- Blocking flow transitions (pure logic, NO server.send) -----------------
// Used by the /flow/* HTTP handlers AND (via delegation) by the TFT menu. Each of
// these three makes a BLOCKING octo* HTTP call -> may only run in the loop() task
// (core 1), never in pn5180Task (core 0). They only set g_flowState/g_flowMsg + LED.

// Printer chosen (idx): fetch tool count live -> ASK_TOOL or ERROR.
static void flowDoPrinter(int idx) {
  displayTouch();  // a flow action (web UI/TFT) counts as activity
  if (idx < 0 || idx >= g_octoCount) {
    g_flowState = FLOW_ERROR;
    g_flowMsg = "Invalid printer index";
    return;
  }
  dbgLogf("HTTP: GET toolCount (instance %d)", idx);
  String err;
  int count = octoToolCount((uint8_t)idx, err);  // BLOCKING (HTTP), but short
  dbgLogf("HTTP: toolCount -> count=%d err=%s", count, err.c_str());
  if (count <= 0) {
    g_flowState = FLOW_ERROR;
    g_flowMsg = err;
    ledFlash(pixel.Color(LED_BRIGHT * 2, 0, 0), 1500);
    return;
  }
  g_flowPrinter = idx;
  g_flowToolCount = count;
  if (count == 1) g_flowTool = 0;  // single tool -> pre-select it, state stays ASK_TOOL
  g_flowState = FLOW_ASK_TOOL;
}

// Tool chosen (n): call the SpoolManagerExtended load URL -> DONE/ERROR.
static void flowDoTool(int n) {
  displayTouch();
  if (n < 0 || n >= g_flowToolCount) {
    g_flowState = FLOW_ERROR;
    g_flowMsg = "Tool out of range";
    return;
  }
  g_flowTool = n;
  g_flowState = FLOW_LOADING;
  dbgLogf("HTTP: PUT loadSpool printer=%d tool=%d databaseId=%ld",
          g_flowPrinter, n, g_flowSpoolId);
  String err;
  bool ok = octoLoadSpool((uint8_t)g_flowPrinter, g_flowSpoolId, n, err);
  dbgLogf("HTTP: loadSpool -> ok=%d err=%s", ok, err.c_str());
  if (ok) {
    g_flowState = FLOW_DONE;
    g_flowMsg = "";
    buzzerSuccess();  // success tone + synced green LED pulse: spool loaded
  } else {
    g_flowState = FLOW_ERROR;
    g_flowMsg = err;
    buzzerError();    // error tone + synced red LED pulse
  }
}

// Reading confirmed: write the current scale weight to the SpoolManagerExtended DB.
static void flowDoWeighSave() {
  displayTouch();
  uint8_t idx = g_dbInstance < g_octoCount ? g_dbInstance : 0;
  g_flowGrossWeight = g_weight;  // live value (the spool is on the scale right now)
  g_flowState = FLOW_WEIGHING;
  dbgLogf("HTTP: PUT measuredWeight databaseId=%ld gross=%.1f (instance %u)",
          g_flowSpoolId, g_flowGrossWeight, idx);
  String err;
  bool ok = octoSetMeasuredWeight(idx, g_flowSpoolId, g_flowGrossWeight, err);
  dbgLogf("HTTP: measuredWeight -> ok=%d err=%s", ok, err.c_str());
  if (ok) {
    g_flowState = FLOW_DONE;
    g_flowMsg = "Weight saved";
    buzzerSuccess();  // success tone + synced green LED pulse: weight saved
  } else {
    g_flowState = FLOW_ERROR;
    g_flowMsg = err;
    buzzerError();    // error tone + synced red LED pulse
  }
}

// TFT menu: needs the flow globals + flowReset/flowDo* + g_flowMenuReq/g_tareReq
// (all defined above) and g_tft/pn5180IsReady/g_octo (from the includes above).
#include "menu.h"

void startWebServer() {
  server.on("/", []() { server.send_P(200, "text/html", INDEX_HTML); });

  server.on("/version", []() {
    server.send(200, "text/plain", "OctoScale v" FW_VERSION " (Build " FW_BUILD ")");
  });

  // Live backlight brightness test: /backlight?level=0-255 (PWM, thread-safe). Helps
  // narrow down whether flicker is brightness-/load-dependent (a hardware question).
  server.on("/backlight", []() {
    if (server.hasArg("level")) {
      int lv = server.arg("level").toInt();
      if (lv < 0) lv = 0;
      if (lv > 255) lv = 255;
      g_blActive = (uint8_t)lv;      // new active brightness
      displaySetBacklight((uint8_t)lv);
      displayTouch();                // using the slider counts as activity
      server.send(200, "text/plain", String("backlight=") + lv);
    } else {
      server.send(400, "text/plain", "level=0..255 missing");
    }
  });

  // Display settings (active/dim brightness + timeout). No args -> current values as
  // JSON. With args (active/dim/timeout) -> set + save to NVS.
  server.on("/display", []() {
    bool changed = false;
    if (server.hasArg("active")) {
      int v = server.arg("active").toInt(); if (v < 10) v = 10; if (v > 255) v = 255;
      g_blActive = (uint8_t)v; displaySetBacklight(g_blActive); changed = true;
    }
    if (server.hasArg("dim")) {
      int v = server.arg("dim").toInt(); if (v < 0) v = 0; if (v > 255) v = 255;
      g_blDim = (uint8_t)v; changed = true;
    }
    if (server.hasArg("timeout")) {
      int v = server.arg("timeout").toInt(); if (v < 0) v = 0; if (v > 3600) v = 3600;
      g_blTimeoutSec = (uint16_t)v; changed = true;
    }
    if (server.hasArg("ssEnabled")) {
      g_ssEnabled = (server.arg("ssEnabled") == "1"); changed = true;
    }
    if (server.hasArg("ssTimeout")) {
      int v = server.arg("ssTimeout").toInt(); if (v < 5) v = 5; if (v > 3600) v = 3600;
      g_ssTimeoutSec = (uint16_t)v; changed = true;
    }
    if (server.hasArg("offEnabled")) {
      g_offEnabled = (server.arg("offEnabled") == "1"); changed = true;
    }
    if (server.hasArg("offTimeout")) {
      // Lower bound 10s: below that the panel's ~140ms wake latency starts to dominate
      // (the ST7789 sleep-out timing), which makes the device feel sluggish rather than
      // power-saving. Upper bound matches the other two stages.
      int v = server.arg("offTimeout").toInt(); if (v < 10) v = 10; if (v > 3600) v = 3600;
      g_offTimeoutSec = (uint16_t)v; changed = true;
    }
    if (changed) {
      Preferences p; p.begin("octoscale", false);
      p.putUChar("blActive", g_blActive);
      p.putUChar("blDim", g_blDim);
      p.putUShort("blTimeout", g_blTimeoutSec);
      p.putBool("ssEnabled", g_ssEnabled);
      p.putUShort("ssTimeout", g_ssTimeoutSec);
      p.putBool("offEnabled", g_offEnabled);
      p.putUShort("offTimeout", g_offTimeoutSec);
      p.end();
      displayTouch();
    }
    JsonDocument doc;
    doc["active"] = g_blActive;
    doc["dim"] = g_blDim;
    doc["timeout"] = g_blTimeoutSec;
    doc["ssEnabled"] = g_ssEnabled;
    doc["ssTimeout"] = g_ssTimeoutSec;
    doc["offEnabled"] = g_offEnabled;
    doc["offTimeout"] = g_offTimeoutSec;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Buzzer settings. No args -> current values as JSON. With args, set + save to NVS:
  //   enabled=0/1, mode=0(active)/1(passive), vol=0..255, freq=100..8000, test=1 (test tone).
  server.on("/buzzer", []() {
    bool changed = false;
    if (server.hasArg("enabled")) {
      g_buzEnabled = server.arg("enabled").toInt() != 0; changed = true;
    }
    if (server.hasArg("mode")) {
      int v = server.arg("mode").toInt(); g_buzMode = (v == 1) ? 1 : 0; changed = true;
    }
    if (server.hasArg("vol")) {
      int v = server.arg("vol").toInt(); if (v < 0) v = 0; if (v > 255) v = 255;
      g_buzVol = (uint8_t)v; changed = true;
    }
    if (server.hasArg("freq")) {
      int v = server.arg("freq").toInt(); if (v < 100) v = 100; if (v > 8000) v = 8000;
      g_buzFreq = (uint16_t)v; changed = true;
    }
    if (changed) {
      Preferences p; p.begin("octoscale", false);
      p.putBool("buzEn", g_buzEnabled);
      p.putUChar("buzMode", g_buzMode);
      p.putUChar("buzVol", g_buzVol);
      p.putUShort("buzFreq", g_buzFreq);
      p.end();
    }
    if (server.hasArg("test") && server.arg("test").toInt() != 0) buzzerTest();
    JsonDocument doc;
    doc["enabled"] = g_buzEnabled;
    doc["mode"] = g_buzMode;
    doc["vol"] = g_buzVol;
    doc["freq"] = g_buzFreq;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Global LED brightness. No args -> current value as JSON; with ?level=N (0..255)
  // -> set + save. Scales every status colour, blink and flash uniformly in
  // ledShow()/ledScale() -- hues and patterns are untouched.
  server.on("/led", []() {
    bool changed = false;
    if (server.hasArg("level")) {
      int v = server.arg("level").toInt();
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      g_ledBrightness = (uint8_t)v; changed = true;
    }
    if (server.hasArg("onboard")) {
      g_ledOnboard = server.arg("onboard").toInt() != 0; changed = true;
    }
    if (server.hasArg("pin2")) {
      g_ledPin2 = server.arg("pin2").toInt() != 0; changed = true;
      ledPin2Apply();   // plain GPIO write, safe from either core
    }
    if (changed) {
      Preferences p; p.begin("octoscale", false);
      p.putUChar("ledBright", g_ledBrightness);
      p.putBool("ledOnboard", g_ledOnboard);
      p.putBool("ledPin2", g_ledPin2);
      p.end();
      // The colour itself is unchanged, only its scaling/routing -- so force ledTick()
      // (core 0) to repaint on its next pass rather than touching the strands here.
      g_ledLastShown = 0xFFFFFFFF;
    }
    JsonDocument doc;
    doc["level"] = g_ledBrightness;
    doc["onboard"] = g_ledOnboard;
    doc["pin2"] = g_ledPin2;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Diagnostics card (web UI Debug tab) -- LED test color. idx: 0-4 = kTestLedColors
  // index (menu.h), -1 = release the override (ledTick() resumes normal behavior).
  // Fire-and-forget: no state to report back, only a one-shot color change.
  server.on("/testled", []() {
    if (server.hasArg("idx")) {
      g_testLedReqColorIdx = server.arg("idx").toInt();
      g_testLedReq = true;
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // Diagnostics card -- TFT test pattern. pattern: 0-4 = full-screen color index, 10 =
  // font sample, 20 = grayscale ramp, -1 = release (back to whatever menuTick() would
  // normally be showing). Same fire-and-forget shape as /testled.
  server.on("/testtft", []() {
    if (server.hasArg("pattern")) {
      g_testTftReqPattern = server.arg("pattern").toInt();
      g_testTftReq = true;
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/weight", []() {
    server.send(200, "text/plain", String(g_weight, 1));
  });

  server.on("/tare", []() {
    // Don't tare here (would block loop() + conflict with scaleTask over the HX711)
    // -> just set the flag.
    g_tareReq = true;
    displayTouch();  // a web UI action counts as activity
    server.send(200, "text/plain", "OK");
    Serial.println("Tare requested (via web UI)");
  });

  server.on("/factor", []() {
    server.send(200, "text/plain", String(g_calFactor, 4));
  });

  // HX711 diagnostics for the web UI's Scale tab. The chip itself has no status
  // registers (unlike the PN5180) -- this is the raw ADC reading + a computed noise
  // estimate over the last few samples, plus the live is_ready()/last-read staleness.
  server.on("/scaleinfo", []() {
    JsonDocument doc;
    doc["ready"] = g_scaleReady;
    doc["hxReady"] = g_scaleHxReady;
    // Watchdog visibility: recoveries > 0 means the HX711 froze and was reset. A
    // rising count is the signal to look at wiring/supply -- the fix keeps the scale
    // usable but does not make the underlying fault go away.
    doc["recoveries"] = g_scaleRecoveries;
    // Cast to long: the ternary's other arm is uint32_t, so a bare -1 gets converted to
    // 4294967295 before ArduinoJson ever sees it -- "never recovered" would read as a
    // 49-day-old recovery.
    doc["lastRecoveryAgoMs"] = g_scaleLastRecoveryMs ? (long)(millis() - g_scaleLastRecoveryMs) : -1L;
    doc["stuckMs"] = g_scaleStuckSince ? (millis() - g_scaleStuckSince) : 0;
    doc["raw"] = g_scaleRawLast;
    doc["offset"] = scale.get_offset();
    doc["scaleFactor"] = g_calFactor;
    doc["lastReadAgoMs"] = g_scaleLastReadMs ? (millis() - g_scaleLastReadMs) : -1;
    // Sample std-dev over the rolling raw-value window -- a noisy/loose load cell or
    // bad wiring shows up here as a large number even while the weight readout itself
    // looks plausible (the 0.7/0.3 smoothing in scaleTask hides jitter from the UI).
    uint8_t n = g_scaleNoiseCount;
    if (n >= 2) {
      double sum = 0;
      for (uint8_t i = 0; i < n; i++) sum += g_scaleNoiseBuf[i];
      double mean = sum / n;
      double sq = 0;
      for (uint8_t i = 0; i < n; i++) { double d = g_scaleNoiseBuf[i] - mean; sq += d * d; }
      doc["noiseStdDev"] = sqrt(sq / n);
      doc["noiseSamples"] = n;
    } else {
      doc["noiseStdDev"] = nullptr;
      doc["noiseSamples"] = n;
    }
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Calibrate: a known weight is on the scale, ?weight=<real grams>
  // newFactor = raw value (tare-corrected, no factor) / real weight
  server.on("/calibrate", []() {
    if (!g_scaleReady) {
      server.send(503, "text/plain", "Scale not ready");
      return;
    }
    if (!server.hasArg("weight")) {
      server.send(400, "text/plain", "weight missing");
      return;
    }
    float known = server.arg("weight").toFloat();
    if (known <= 0) {
      server.send(400, "text/plain", "weight must be > 0");
      return;
    }
    long raw = scale.get_value(20);  // average raw minus the tare offset, no factor
    float newFactor = (float)raw / known;
    if (newFactor <= 0) {
      server.send(400, "text/plain", "Invalid - is the weight actually on the scale?");
      return;
    }
    applyCalFactor(newFactor);
    server.send(200, "text/plain", String(newFactor, 4));
    Serial.printf("Calibrated @ %.1f g -> factor %.4f\n", known, newFactor);
  });

  // 2-point calibration wizard, step 1/2: read the raw (tare-relative) value at a
  // reference weight, without saving anything yet. The web UI calls this once per
  // point, then computes+saves via /calibrate2.
  server.on("/calmeasure", []() {
    if (!g_scaleReady) { server.send(503, "text/plain", "Scale not ready"); return; }
    long raw = scale.get_value(20);
    server.send(200, "text/plain", String(raw));
  });

  // 2-point calibration: two (weight, raw) pairs -> fit only the slope (SCALE).
  // Offset/zero point stays owned by tare (re-measured every boot) -> untouched here.
  // scale = (raw2 - raw1) / (w2 - w1)
  server.on("/calibrate2", []() {
    if (!g_scaleReady) { server.send(503, "text/plain", "Scale not ready"); return; }
    if (!server.hasArg("w1") || !server.hasArg("raw1") || !server.hasArg("w2") || !server.hasArg("raw2")) {
      server.send(400, "text/plain", "w1/raw1/w2/raw2 missing");
      return;
    }
    float w1 = server.arg("w1").toFloat(), w2 = server.arg("w2").toFloat();
    long raw1 = server.arg("raw1").toInt(), raw2 = server.arg("raw2").toInt();
    if (w1 <= 0 || w2 <= 0) { server.send(400, "text/plain", "weights must be > 0"); return; }
    if (fabsf(w2 - w1) < 1.0f) { server.send(400, "text/plain", "the two weights are too close together"); return; }
    float newFactor = (float)(raw2 - raw1) / (w2 - w1);
    if (newFactor <= 0) {
      server.send(400, "text/plain", "Invalid - check the weights are on the scale in the right order");
      return;
    }
    applyCalFactor(newFactor);
    server.send(200, "text/plain", String(newFactor, 4));
    Serial.printf("2-point calibrated @ %.1fg/%.1fg -> factor %.4f\n", w1, w2, newFactor);
  });

  server.on("/setfactor", []() {
    if (!server.hasArg("value")) {
      server.send(400, "text/plain", "value missing");
      return;
    }
    float f = server.arg("value").toFloat();
    if (f <= 0) {
      server.send(400, "text/plain", "value must be > 0");
      return;
    }
    applyCalFactor(f);
    server.send(200, "text/plain", String(f, 4));
  });

  // PN5180 (ISO15693/NFC-V) test status: reports ready + current tag + UID.
  server.on("/nfc5180", []() {
    JsonDocument doc;
    doc["ready"] = pn5180IsReady();
    doc["present"] = g_pn5180Present;
    doc["uid"] = g_pn5180Uid;
    doc["lastRc"] = pn5180LastRc();      // last getInventory code (-1 = no tag)
    doc["okCount"] = pn5180OkCount();    // successful reads so far (>0 = it works!)
    // The databaseId found on the tag, as ASCII text (plan A.1's `data` field) -- the
    // legacy field the plugin's getOctoScaleNfcStatus originally expected here.
    doc["data"] = g_lastTagData;
    // Tag-type/format info (plan A.1), from the per-UID cache (see nfcClassifyTag).
    if (g_pn5180Present && g_nfcExtCacheUid == g_pn5180Uid) {
      doc["tagType"] = (g_nfcExtCacheWriteFormat == "nfcvExtended" || g_nfcExtCacheWriteFormat == "nfcvOpenSpool") ? "nfcv"
                       : g_nfcExtCacheWriteFormat == "octoscaleExtended" ? "mifareClassic1k"
                       : (g_nfcExtCacheWriteFormat == "openSpool" || g_nfcExtCacheWriteFormat == "ntagExtended" || g_nfcExtCacheWriteFormat == "tigerTag") ? "ntag" : "unknown";
      doc["capacityBytes"] = g_nfcExtCacheCapacityBytes;
      doc["writeFormat"] = g_nfcExtCacheWriteFormat;
      doc["formatLabel"] = g_nfcExtCacheFormatLabel;
    } else {
      doc["tagType"] = "unknown";
      doc["capacityBytes"] = 0;
      doc["writeFormat"] = "spoolIdNtag";
      doc["formatLabel"] = "unknown";
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Toggle NFC debug mode: /nfcdebug?on=1 enables, ?on=0 disables. No parameter just
  // reports the current state. Turning it off resets the reader to normal ISO15693.
  server.on("/nfcdebug", []() {
    if (server.hasArg("on")) {
      bool want = (server.arg("on") == "1" || server.arg("on") == "true");
      if (g_nfcDebug != want) {
        g_nfcDebug = want;
        g_nfcProbe = PN5180ProbeResult();  // discard the old result
        g_nfcDebugChangeReq = true;        // pn5180Task runs pn5180Recover() (shared SPI obj)
        Serial.printf("NFC debug: %s\n", g_nfcDebug ? "ON" : "OFF");
      }
    }
    JsonDocument doc;
    doc["debug"] = g_nfcDebug;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Toggle the TFT screen preview (web UI System tab): /menupreview?on=1 starts
  // stepping through every screen with the encoder, ?on=0 stops it and returns to
  // whatever the real state actually is. No parameter just reports current state +
  // how many screens there are (for the web UI's "n / total" readout).
  server.on("/menupreview", []() {
    if (server.hasArg("on")) {
      bool want = (server.arg("on") == "1" || server.arg("on") == "true");
      if (want != g_menuPreviewActive) {
        g_menuPreviewReqOn = want;
        g_menuPreviewReq = true;
        Serial.printf("Screen preview: %s (requested)\n", want ? "ON" : "OFF");
      }
    }
    // ?next=1 steps the preview forward WITHOUT the encoder -- lets a script cycle
    // through every screen (e.g. to spot-check the LED against each one) the same way
    // the knob would. Same cross-core delegation as everything else here: this handler
    // (core 1) only sets a flag, pn5180Task (core 0) does the actual step + redraw.
    if (g_menuPreviewActive && server.hasArg("next")) {
      g_menuPreviewStepReq = true;
    }
    JsonDocument doc;
    doc["active"] = g_menuPreviewActive;
    doc["index"] = g_menuPreviewIdx;
    doc["count"] = kMenuPreviewCount;
    doc["name"] = kMenuPreview[g_menuPreviewIdx].name;
    // What the real LED/buzzer are doing right now (same source the TFT overlay reads,
    // see mpDrawSignalOverlay) -- lets the web UI mirror the exact color/on-off state
    // instead of guessing from the screen name alone.
    uint32_t c = pixel.getPixelColor(0);
    uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    float boost = 1.0f;
    uint8_t maxc = max(r, max(g, b));
    if (maxc > 0) boost = min(255.0f / maxc, 4.0f);
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", (uint8_t)min(255.0f, r * boost),
             (uint8_t)min(255.0f, g * boost), (uint8_t)min(255.0f, b * boost));
    doc["led"] = (r || g || b) ? String(hex) : "";
    doc["buzzing"] = (g_buzLedColor != 0);
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // NFC debug result: what the reader saw on the last probe (only while debug is ON).
  server.on("/nfcprobe", []() {
    JsonDocument doc;
    doc["debug"] = g_nfcDebug;
    doc["ready"] = pn5180IsReady();
    bool present = (g_nfcProbe.type != PN5180_TAG_NONE);
    doc["present"] = present;
    doc["type"] = (int)g_nfcProbe.type;           // 0=none,1=NFC-V,2=NFC-A
    doc["typeName"] = pn5180TagTypeName(g_nfcProbe.type);
    doc["uid"] = g_nfcProbe.uid;
    if (g_nfcProbe.type == PN5180_TAG_NFCA) {
      char hx[7];
      snprintf(hx, sizeof(hx), "0x%04X", g_nfcProbe.atqa);
      doc["atqa"] = hx;
      snprintf(hx, sizeof(hx), "0x%02X", g_nfcProbe.sak);
      doc["sak"] = hx;
      doc["product"] = pn5180NfcaProduct(g_nfcProbe.atqa, g_nfcProbe.sak);
      // Page/block count: fixed by SAK for Mifare Classic, read via GET_VERSION for
      // NTAG (user memory size differs 6x across 213/215/216, see pn5180Probe). Omitted
      // (not 0) when neither path could determine it, so the WebUI can tell "unknown"
      // from "genuinely zero".
      if (g_nfcProbe.numPagesPresent) doc["numPages"] = g_nfcProbe.numPages;
    }
    if (g_nfcProbe.type == PN5180_TAG_NFCV) {
      doc["blockSize"] = g_nfcProbe.blockSize;
      doc["numBlocks"] = g_nfcProbe.numBlocks;
      // ISO15693 has no ATQA/SAK (that's an ISO14443-A concept) -- DSFID/AFI are its
      // closest equivalents: optional per-tag metadata bytes, not every tag sets them.
      if (g_nfcProbe.dsfidPresent) {
        char hx[5]; snprintf(hx, sizeof(hx), "0x%02X", g_nfcProbe.dsfid);
        doc["dsfid"] = hx;
      }
      if (g_nfcProbe.afiPresent) {
        char hx[5]; snprintf(hx, sizeof(hx), "0x%02X", g_nfcProbe.afi);
        doc["afi"] = hx;
      }
      // Raw only, not decoded to a chip name -- see PN5180ProbeResult::icRef's comment.
      if (g_nfcProbe.icRefPresent) {
        char hx[5]; snprintf(hx, sizeof(hx), "0x%02X", g_nfcProbe.icRef);
        doc["icRef"] = hx;
      }
    }
    // What the real (debug-off) flow would do with this tag: a parsed databaseId ->
    // normal load flow; none -> falls back to a UID lookup (spool/byCode) if present.
    doc["idParsed"] = g_nfcProbe.idParsed;
    doc["idText"] = g_nfcProbe.idText;
    // Capability Container state (NFC-A/NTAG + NFC-V only, debug mode only -- see
    // pn5180ProbeCcState). "virgin"/"ndef"/"other", "" = n/a (Mifare, or not read).
    doc["ccState"] = g_nfcProbe.ccState;
    doc["flowWouldUse"] = present
      ? (g_nfcProbe.idParsed >= 0 ? "databaseId" : "uid (byCode lookup)")
      : "none";
    // Tag-type/format info (plan A.1) -- from the per-UID cache, populated on every
    // new-tag event regardless of debug mode (see the poll loop). "data" mirrors
    // /nfc5180's field of the same name, for callers standardizing on /nfcprobe alone.
    doc["data"] = g_nfcProbe.idText;
    if (present && g_nfcExtCacheUid == g_nfcProbe.uid) {
      doc["tagType"] = (g_nfcExtCacheWriteFormat == "nfcvExtended" || g_nfcExtCacheWriteFormat == "nfcvOpenSpool") ? "nfcv"
                       : g_nfcExtCacheWriteFormat == "octoscaleExtended" ? "mifareClassic1k"
                       : (g_nfcExtCacheWriteFormat == "openSpool" || g_nfcExtCacheWriteFormat == "ntagExtended" || g_nfcExtCacheWriteFormat == "tigerTag") ? "ntag" : "unknown";
      doc["capacityBytes"] = g_nfcExtCacheCapacityBytes;
      doc["writeFormat"] = g_nfcExtCacheWriteFormat;
      doc["formatLabel"] = g_nfcExtCacheFormatLabel;
      doc["hasExtendedData"] = g_nfcExtCacheHasExtended;
      // "empty" | "foreign" | "" (ours, or undetermined) -- lets a caller refuse to
      // overwrite another vendor's tag, which hasExtendedData=false alone can't express.
      doc["occupancy"] = g_nfcExtCacheOccupancy;
      // Full Extended payload (debug view: "all data on the tag", not just the
      // databaseId) -- only meaningful when hasExtendedData, but harmless to include
      // either way (fields just read as their "not set" sentinel/blank string).
      JsonObject ext = doc["extended"].to<JsonObject>();
      const SpoolTagData &e = g_nfcExtCacheData;
      ext["databaseId"] = e.databaseId;
      ext["material"] = e.material;
      ext["vendor"] = e.vendor;
      ext["color"] = e.color;
      // v4 multi-colour, reported as a rebuilt grammar string plus the parsed parts.
      // colorFull is what a caller would write back verbatim; the array and the two
      // flags are there so a consumer doesn't have to re-parse the grammar itself.
      {
        String full;
        if (e.isRainbow) full = "rainbow";
        else {
          if (e.isTransparent) full = "transparent";
          char hx[10];
          for (int i = 0; i < e.colorCount && i < 3; i++) {
            snprintf(hx, sizeof(hx), "#%02X%02X%02X",
                     e.colorRgb[i][0], e.colorRgb[i][1], e.colorRgb[i][2]);
            if (i == 0) full += e.isTransparent ? ":" : "";
            else full += ";";
            full += hx;
          }
        }
        ext["colorFull"] = full;
        ext["colorCount"] = e.colorCount;
        ext["isTransparent"] = e.isTransparent;
        ext["isRainbow"] = e.isRainbow;
        JsonArray cols = ext["colors"].to<JsonArray>();
        for (int i = 0; i < e.colorCount && i < 3; i++) {
          char hx[10];
          snprintf(hx, sizeof(hx), "#%02X%02X%02X",
                   e.colorRgb[i][0], e.colorRgb[i][1], e.colorRgb[i][2]);
          cols.add(hx);
        }
      }
      ext["colorName"] = e.colorName;
      ext["diameter"] = e.diameter;
      ext["diameterTolerance"] = e.diameterTolerance;
      ext["density"] = e.density;
      ext["totalWeight"] = e.totalWeight;
      ext["spoolWeight"] = e.spoolWeight;
      ext["usedWeight"] = e.usedWeight;
      ext["temperature"] = e.temperature;
      ext["bedTemperature"] = e.bedTemperature;
      ext["enclosureTemperature"] = e.enclosureTemperature;
      ext["offsetTemperature"] = e.offsetTemperature;
      ext["offsetBedTemperature"] = e.offsetBedTemperature;
      ext["offsetEnclosureTemperature"] = e.offsetEnclosureTemperature;
      ext["temperatureMin"] = e.temperatureMin;
      ext["temperatureMax"] = e.temperatureMax;
      ext["bedTemperatureMin"] = e.bedTemperatureMin;
      ext["bedTemperatureMax"] = e.bedTemperatureMax;
      // octoscaleExtended v3 (Mifare Classic 1K only so far).
      ext["remainingWeight"] = e.remainingWeight;
      ext["totalLength"] = e.totalLength;
      ext["usedLength"] = e.usedLength;
      ext["code"] = e.code;
      ext["batchNumber"] = e.batchNumber;
      ext["firstUse"] = e.firstUse;
      ext["lastUse"] = e.lastUse;
      ext["firstUseMinuteOfDay"] = e.firstUseMinuteOfDay;
      ext["lastUseMinuteOfDay"] = e.lastUseMinuteOfDay;
      ext["purchasedOnMinuteOfDay"] = e.purchasedOnMinuteOfDay;
      ext["purchasedFrom"] = e.purchasedFrom;
      ext["purchasedOn"] = e.purchasedOn;
      ext["cost"] = e.cost;
      ext["finish"] = e.finish;
      ext["displayName"] = e.displayName;
      // OpenPrintTag-only -- stays at the -1 sentinel for every other format.
      ext["dryingTemperature"] = e.dryingTemperature;
      ext["dryingTime"] = e.dryingTime;  // minutes, spec unit
      ext["td"] = e.td;
      // TigerTag-only (foreign format, see pn5180ReadNtagTigerTag). -1 on every other
      // format/an unread TigerTag tag.
      ext["tigerTagMaterialId"] = e.tigerTagMaterialId;
      ext["tigerTagBrandId"] = e.tigerTagBrandId;
      ext["tigerTagAspectId"] = e.tigerTagAspectId;
      ext["tigerTagTypeId"] = e.tigerTagTypeId;
      ext["tigerTagDiameterId"] = e.tigerTagDiameterId;
      ext["tigerTagMeasureUnitId"] = e.tigerTagMeasureUnitId;
    } else {
      doc["tagType"] = "unknown";
      doc["capacityBytes"] = 0;
      doc["writeFormat"] = "spoolIdNtag";
      doc["formatLabel"] = "unknown";
      doc["hasExtendedData"] = false;
    }
    // Chip-level health/diagnostic status (not tag-related, doesn't need a tag present)
    // -- only read in debug mode, since it's a handful of extra SPI round-trips on
    // every 500ms poll otherwise pointless outside diagnostics.
    if (g_nfcDebug && pn5180IsReady()) {
      PN5180ChipStatus cs = pn5180ReadChipStatus();
      JsonObject chip = doc["chip"].to<JsonObject>();
      chip["ok"] = cs.ok;
      char hex[33]; int hn = 0;
      for (int i = 0; i < 16; i++) hn += snprintf(hex + hn, sizeof(hex) - hn, "%02X", cs.dieId[i]);
      chip["dieId"] = String(hex);
      // Raw bytes, not "major.minor" -- unlike PRODUCT_VERSION/FIRMWARE_VERSION (whose
      // major.minor layout the atrappmann library's own boot log already relies on),
      // NXP's public PN5180 docs don't pin down EEPROM_VERSION's byte meaning, and a
      // live read here came back looking implausible as a version number (0x91, 0x00)
      // -- shown as hex rather than guessing a format that might be wrong.
      char eepromVer[8];
      snprintf(eepromVer, sizeof(eepromVer), "0x%02X%02X", cs.eepromVersion[1], cs.eepromVersion[0]);
      chip["eepromVersion"] = eepromVer;
      chip["rfFieldOn"] = cs.rfFieldOn;
      static const char *kTransceiveNames[8] = {
        "idle", "waitTransmit", "transmitting", "waitReceive",
        "waitForData", "receiving", "loopback", "reserved"
      };
      chip["transceiveState"] = kTransceiveNames[cs.transceiveState & 0x07];
      chip["systemMode"] = cs.systemMode;
      char hx8[11];
      snprintf(hx8, sizeof(hx8), "0x%08lX", (unsigned long)cs.systemStatus);
      chip["systemStatusRaw"] = hx8;
      chip["overtempWarning"] = cs.overtempWarning;
      snprintf(hx8, sizeof(hx8), "0x%08lX", (unsigned long)cs.tempControl);
      chip["tempControlRaw"] = hx8;
      snprintf(hx8, sizeof(hx8), "0x%08lX", (unsigned long)cs.irqStatus);
      chip["irqStatusRaw"] = hx8;
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // POST /nfcdump: start a raw sector/block dump of the tag on the reader (web UI's
  // "Create dump" button, unknown-tag fallback). Async, same request/poll pattern as
  // the writes -- the actual Mifare auth+read only ever runs inside pn5180Task.
  server.on("/nfcdump", HTTP_POST, []() {
    if (!pn5180IsReady()) { server.send(503, "text/plain", "PN5180 not ready"); return; }
    if (g_nfcWritePending) { server.send(409, "text/plain", "A write is already in progress"); return; }
    if (g_nfcDumpPending) { server.send(409, "text/plain", "A dump is already in progress"); return; }
    // A raw read owns the reader just as exclusively as a write or a dump does.
    if (g_nfcReadPending) { server.send(409, "text/plain", "A read is already in progress"); return; }
    dbgLog("/nfcdump: request received");
    g_nfcDumpDone = false;
    g_nfcDumpPending = true;
    g_nfcDumpReq = true;  // pn5180Task takes over
    server.send(202, "text/plain", "started");
  });

  // Poll result of /nfcdump. Once consumed (done=true seen), g_nfcDumpPending stays
  // clear (already cleared by pn5180Task itself, unlike the write flow -- a dump has
  // no TFT lock screen to race with, so there's no reason to delay it).
  server.on("/nfcdumpstatus", []() {
    JsonDocument doc;
    doc["pending"] = g_nfcDumpPending;
    doc["done"] = g_nfcDumpDone;
    if (g_nfcDumpDone) {
      doc["ok"] = g_nfcDumpOk;
      doc["err"] = g_nfcDumpErr;
      doc["durationMs"] = g_nfcDumpMs;              // auth+read loop only, excludes probe/RF reset
      doc["authOkSectors"] = g_nfcDumpAuthOkSectors;  // Mifare only; 0 on NTAG/NFC-V (no auth concept)
      doc["tagType"] = g_nfcDumpTagType;   // "mifareClassic1k" | "ntag" | "nfcv"
      doc["uid"] = g_nfcDumpUid;
      // Bytes per addressable unit: 16 for a Mifare block, 4 for an NTAG page or an
      // NFC-V block. Each row below still carries 16 bytes -- on the 4-byte carriers
      // that is FOUR consecutive units packed together, and "block" names the first of
      // them. A consumer converts a row index to an absolute byte offset as
      // row * 16, and to a unit number as block (already absolute).
      doc["unitBytes"] = g_nfcDumpUnitBytes;
      doc["unitsPerRow"] = 16 / g_nfcDumpUnitBytes;
      // Exact unit count. The rows are 16 bytes each, so a carrier whose unit count is
      // not a multiple of 4 (NTAG215: 130 pages -> 33 rows = 132 pages' worth) has a
      // zero-padded tail in the last row. Without this a consumer cannot tell the
      // padding apart from real tag content and reads 8 bytes too many.
      doc["unitCount"] = g_nfcDumpUnitCount;
      if (g_nfcDumpVariant.length()) doc["ntagVariant"] = g_nfcDumpVariant;
      JsonArray blocks = doc["blocks"].to<JsonArray>();
      for (int i = 0; i < g_nfcDumpCount; i++) {
        JsonObject b = blocks.add<JsonObject>();
        b["block"] = g_nfcDumpBlocks[i].block;
        b["authOk"] = g_nfcDumpBlocks[i].sectorAuthOk;
        b["readOk"] = g_nfcDumpBlocks[i].readOk;
        if (g_nfcDumpBlocks[i].readOk) {
          char hex[33];
          for (int j = 0; j < 16; j++) snprintf(hex + j * 2, 3, "%02X", g_nfcDumpBlocks[i].data[j]);
          b["hex"] = String(hex);
        }
      }
      g_nfcDumpDone = false;  // consumed
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // POST /nfcreadstart: start a raw image read of the tag on the reader, for a parser
  // that lives outside this firmware. Body (JSON, all optional):
  //   keyA / keyB : 16 strings of 12 hex chars, indexed by sector. Omitted = factory
  //                 key. keyB is only tried where keyA was rejected.
  //   sectors     : array of sector numbers to read (Mifare only). Omitted = all 16.
  //                 Reading only what a parser needs is the main cost saving -- block
  //                 reads dominate the runtime, not authentication.
  // Answers 202 immediately; poll /nfcreadstatus for the result.
  server.on("/nfcreadstart", HTTP_POST, []() {
    if (!pn5180IsReady()) {
      server.send(503, "application/json",
                  "{\"ok\":false,\"error\":\"reader not ready\",\"retryable\":true}");
      return;
    }
    // One RF job at a time -- a read, a write and a dump all drive the same reader.
    if (g_nfcReadPending || g_nfcWritePending || g_nfcDumpPending) {
      server.send(409, "application/json",
                  "{\"ok\":false,\"error\":\"read already in progress\",\"retryable\":true}");
      return;
    }
    JsonDocument doc;
    if (server.hasArg("plain") && server.arg("plain").length() &&
        deserializeJson(doc, server.arg("plain"))) {
      server.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"invalid JSON\",\"retryable\":false}");
      return;
    }
    // Keys: 16 x 12 hex chars. A malformed entry is rejected outright rather than
    // silently falling back to the factory key -- a wrong key looks exactly like a
    // foreign tag in the result, which would be a very confusing way to fail.
    auto parseKeys = [&](const char *field, uint8_t dst[16][6], bool &haveOut) -> const char * {
      haveOut = false;
      if (!doc[field].is<JsonArray>()) return nullptr;
      JsonArray arr = doc[field].as<JsonArray>();
      if (arr.size() != 16) return "key array must have 16 entries";
      for (int s = 0; s < 16; s++) {
        String k = arr[s].as<String>();
        if (k.length() != 12) return "each key must be 12 hex chars";
        for (int b = 0; b < 6; b++) {
          char hx[3] = { k[b * 2], k[b * 2 + 1], 0 };
          char *end = nullptr;
          long v = strtol(hx, &end, 16);
          if (!end || *end) return "key contains non-hex characters";
          dst[s][b] = (uint8_t)v;
        }
      }
      haveOut = true;
      return nullptr;
    };
    bool haveA = false, haveB = false;
    const char *kerr = parseKeys("keyA", g_nfcReadKeyA, haveA);
    if (!kerr) kerr = parseKeys("keyB", g_nfcReadKeyB, haveB);
    if (kerr) {
      JsonDocument e;
      e["ok"] = false; e["error"] = kerr; e["retryable"] = false;
      String out; serializeJson(e, out);
      server.send(400, "application/json", out);
      return;
    }
    g_nfcReadHasKeyA = haveA;
    g_nfcReadHasKeyB = haveB;
    // Sector selection: absent = the whole card.
    uint16_t mask = 0xFFFF;
    if (doc["sectors"].is<JsonArray>()) {
      mask = 0;
      for (JsonVariant v : doc["sectors"].as<JsonArray>()) {
        int s = v.as<int>();
        if (s < 0 || s > 15) {
          server.send(400, "application/json",
                      "{\"ok\":false,\"error\":\"sector out of range 0-15\",\"retryable\":false}");
          return;
        }
        mask |= (uint16_t)(1u << s);
      }
      if (!mask) {
        server.send(400, "application/json",
                    "{\"ok\":false,\"error\":\"sectors list is empty\",\"retryable\":false}");
        return;
      }
    }
    g_nfcReadSectorMask = mask;
    g_nfcReadDone = false;
    g_nfcReadPending = true;
    g_nfcReadReq = true;  // pn5180Task takes over
    // Opened here rather than when the job finishes: the flow must not fire during the
    // read either. Re-armed at completion so the window covers the gap until the next
    // call in a chain (see g_nfcReadSuppressUntil).
    g_nfcReadSuppressUntil = millis() + 6000;
    dbgLogf("/nfcreadstart: keyA=%d keyB=%d mask=0x%04X", haveA, haveB, mask);
    server.send(202, "application/json", "{\"ok\":true,\"pending\":true}");
  });

  // Poll result of /nfcreadstart. `done` self-clears on the first read, matching
  // /nfcwritestatus -- a caller stops polling as soon as it sees done:true.
  server.on("/nfcreadstatus", []() {
    JsonDocument doc;
    doc["pending"] = g_nfcReadPending;
    doc["done"] = g_nfcReadDone;
    if (g_nfcReadDone) {
      doc["ok"] = g_nfcReadOk;
      doc["error"] = g_nfcReadErr;
      // Distinguishes "try again, the tag moved" from "this key/format is wrong, move
      // on to the next parser". Getting this wrong means either an endless retry loop
      // or giving up on a tag that was only briefly out of range.
      doc["retryable"] = g_nfcReadRetryable;
      doc["uid"] = g_nfcReadUid;
      doc["tagType"] = g_nfcReadTagType;
      doc["bytes"] = g_nfcReadHex;
      doc["byteCount"] = (int)(g_nfcReadHex.length() / 2);
      doc["durationMs"] = g_nfcReadMs;
      if (g_nfcReadTagType == "ntag") {
        doc["startPage"] = g_nfcReadStartPage;  // always 0 -- absolute page offsets
        doc["ntagVariant"] = g_nfcReadNtagVariant;  // diagnostics only, not for logic
      } else if (g_nfcReadTagType == "nfcv") {
        // Block-addressed, starting at block 0, so byte offsets are absolute like the
        // other two carriers. blockSize is fixed at 4 for every ISO15693 tag this
        // firmware talks to; blockCount is what the walk actually reached, which is the
        // honest size (getSystemInfo's numBlocks is not always backed by readable
        // blocks). No `sectors` array: ISO15693 has no sector or auth concept.
        doc["blockSize"] = NFCV_BLOCK_SIZE;
        doc["blockCount"] = (int)(g_nfcReadHex.length() / 2 / NFCV_BLOCK_SIZE);
      } else if (g_nfcReadTagType == "mifareClassic1k") {
        // Which sectors actually authenticated. A caller needs this to tell real data
        // apart from the zero-fill that stands in for an unreadable sector.
        JsonArray sec = doc["sectors"].to<JsonArray>();
        JsonArray failed = doc["authFailedSectors"].to<JsonArray>();
        for (int s = 0; s < 16; s++) {
          bool requested = g_nfcReadSectorMask & (1u << s);
          bool ok = g_nfcReadSectorsOk & (1u << s);
          sec.add(ok);
          if (requested && !ok) failed.add(s);
        }
      }
      g_nfcReadDone = false;      // consumed
      g_nfcReadPending = false;
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Encoder + button live status (for testing): position + cumulative press counts.
  server.on("/encoder", []() {
    JsonDocument doc;
    doc["pos"] = encoderPosition();
    doc["pushCount"] = encoderPushCount();
    doc["startCount"] = encoderStartCount();
    doc["pushDown"] = encoderPushDown();
    doc["startDown"] = encoderStartDown();
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Write spool fields to the tag: POST JSON body (plan A.2). The firmware picks the
  // format (Mifare Extended / NTAG OpenSpool / legacy ID-only for NFC-V) based on the
  // tag actually on the reader -- no fmt parameter, no client-side format choice.
  // Non-blocking, same start+poll pattern as /nfcwriteid (shares its pending state).
  server.on("/nfcwritespool", HTTP_POST, []() {
    if (!pn5180IsReady()) { server.send(503, "text/plain", "PN5180 not ready"); return; }
    // Structured like the foreign-tag 409 below -- both return 409, and a caller has to
    // tell "wait and retry" apart from "this tag is protected".
    if (g_nfcWritePending) {
      server.send(409, "application/json",
                  "{\"error\":\"write in progress\",\"retryable\":true,"
                  "\"message\":\"a write is already in progress\"}");
      return;
    }
    // Writing while a raw read is in flight would change the tag under the reader and
    // hand the caller a half-old, half-new image -- worse than simply making it wait.
    if (g_nfcReadPending) {
      server.send(409, "application/json",
                  "{\"error\":\"read in progress\",\"retryable\":true,"
                  "\"message\":\"a tag read is already in progress\"}");
      return;
    }
    String body = server.arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body)) { server.send(400, "text/plain", "invalid JSON"); return; }
    if (!doc["databaseId"].is<JsonVariant>()) { server.send(400, "text/plain", "databaseId required"); return; }

    // Refuse to overwrite a tag that carries another vendor's data (Bambu et al.).
    // Writing is irreversible -- a re-keyed vendor tag can't be restored once its
    // sectors are rewritten -- so the default is to stop, and the caller has to say
    // force=true to go ahead anyway. Only blocks on a POSITIVE "foreign" result: an
    // undetermined or empty tag, or a stale cache, writes as before.
    if (g_nfcExtCacheOccupancy == "foreign" && g_nfcExtCacheUid == g_pn5180Uid &&
        !(doc["force"] | false)) {
      // Structured body, not bare text: a caller has to tell this apart from the other
      // 409 on this endpoint ("write already in progress", which is transient and worth
      // retrying) -- the status code alone can't, and surfacing it as a generic HTTP
      // error makes a normal protective refusal look like a malfunction to the user.
      JsonDocument err;
      err["error"] = "foreign tag";
      err["occupancy"] = "foreign";
      err["uid"] = g_pn5180Uid;
      err["retryable"] = false;   // retrying unchanged will fail identically
      err["overridable"] = true;  // ... but resending with force=true will not
      err["message"] = "tag carries data in an unrecognized format (likely another "
                       "vendor's); writing would destroy it irreversibly";
      String out; serializeJson(err, out);
      server.send(409, "application/json", out);
      return;
    }

    SpoolTagData d;
    d.databaseId = doc["databaseId"] | -1L;
    if (d.databaseId < 0) { server.send(400, "text/plain", "databaseId must be >= 0"); return; }
    d.material   = String((const char *)(doc["material"]   | ""));
    d.vendor     = String((const char *)(doc["vendor"]     | ""));
    d.color      = String((const char *)(doc["color"]      | ""));
    // Parse SpoolManagerExtended's colour grammar into the v4 multi-colour fields.
    // d.color keeps the raw string (the single-colour write paths still parse it
    // themselves for the primary slot); these carry colours 2-3 and the flags.
    pn5180ParseColorGrammar(d.color, d.colorRgb, d.colorCount, d.isTransparent, d.isRainbow);
    d.colorName  = String((const char *)(doc["colorName"]  | ""));
    // Numeric fields: SpoolManagerExtended's own API returns weights as strings elsewhere in
    // this codebase (see octoSpoolInfo's comment), but the plugin sends this payload
    // reading straight off the peewee model (native types, not through the string-
    // producing Transformer) -- so these arrive as JSON numbers. Accept both forms
    // defensively (a JsonVariant that holds a numeric string still converts via |).
    d.diameter          = doc["diameter"]          | -1.0f;
    d.density            = doc["density"]           | -1.0f;
    d.totalWeight        = doc["totalWeight"]        | -1.0f;
    d.spoolWeight        = doc["spoolWeight"]        | -1.0f;
    d.usedWeight          = doc["usedWeight"]         | -1.0f;
    d.temperature         = doc["temperature"]        | -1;
    d.bedTemperature      = doc["bedTemperature"]      | -1;
    d.enclosureTemperature = doc["enclosureTemperature"] | -1;
    // Optional temperature range (SpoolManagerExtended's minTemperature/maxTemperature and
    // minBedTemperature/maxBedTemperature) -- independent of temperature/bedTemperature
    // above, which stay the target/default value, not the range's min (confirmed with
    // the SpoolManagerExtended plugin session: temperature/minTemperature/maxTemperature are
    // sent as three independent fields, potentially with three different values).
    d.temperatureMin    = doc["minTemperature"]    | -1;
    d.temperatureMax    = doc["maxTemperature"]    | -1;
    d.bedTemperatureMin = doc["minBedTemperature"] | -1;
    d.bedTemperatureMax = doc["maxBedTemperature"] | -1;
    // These 5 were already wired into the Mifare/NFC-V Extended wire format but never
    // reached this parsing block -- diameterTolerance/offsets simply had no way to
    // arrive from an HTTP caller until now (enclosureTemperature was already here).
    d.diameterTolerance          = doc["diameterTolerance"]          | -1.0f;
    d.offsetTemperature          = doc["offsetTemperature"]          | 0;
    d.offsetBedTemperature       = doc["offsetBedTemperature"]       | 0;
    d.offsetEnclosureTemperature = doc["offsetEnclosureTemperature"] | 0;

    // octoscaleExtended v3 (Mifare Classic 1K) -- SpoolManagerExtended fields with no prior
    // Extended-format equivalent. See HARDWARE.md's v3 section for the wire layout.
    d.remainingWeight = doc["remainingWeight"] | -1.0f;
    d.totalLength      = doc["totalLength"]     | -1L;
    d.usedLength        = doc["usedLength"]      | -1L;
    d.code              = String((const char *)(doc["code"]          | ""));
    d.batchNumber       = String((const char *)(doc["batchNumber"]   | ""));
    d.firstUse          = doc["firstUse"]        | -1L;
    d.lastUse           = doc["lastUse"]         | -1L;
    // Time of day for the three dates, 0..1439. Sent separately from the day count so
    // an older firmware simply ignores them and still gets the date right.
    d.firstUseMinuteOfDay    = doc["firstUseMinuteOfDay"]    | -1;
    d.lastUseMinuteOfDay     = doc["lastUseMinuteOfDay"]     | -1;
    d.purchasedOnMinuteOfDay = doc["purchasedOnMinuteOfDay"] | -1;
    d.purchasedFrom     = String((const char *)(doc["purchasedFrom"] | ""));
    d.purchasedOn       = doc["purchasedOn"]     | -1L;
    d.cost              = doc["cost"]            | -1.0f;
    d.finish            = String((const char *)(doc["finish"]        | ""));
    d.displayName       = String((const char *)(doc["displayName"]   | ""));
    // OpenPrintTag-only (spec keys 57/58/27). dryingTime arrives in MINUTES -- the
    // caller converts from whatever it stores, so the value here is what goes on the
    // tag verbatim. td is a dimensionless opacity number (0.1-100), not a length.
    d.dryingTemperature = doc["dryingTemperature"] | -1;
    d.dryingTime        = doc["dryingTime"]        | -1;
    d.td                = doc["td"]                | -1.0f;

    // TigerTag-only: pre-resolved registry IDs (SpoolManagerExtended owns the Material/Brand/
    // Aspect/Type/Diameter/MeasureUnit lookup, firmware just packs bytes). -1 = not
    // resolved.
    d.tigerTagMaterialId    = doc["tigerTagMaterialId"]    | -1L;
    d.tigerTagBrandId       = doc["tigerTagBrandId"]       | -1L;
    d.tigerTagAspectId      = doc["tigerTagAspectId"]      | -1L;
    d.tigerTagTypeId        = doc["tigerTagTypeId"]        | -1L;
    d.tigerTagDiameterId    = doc["tigerTagDiameterId"]    | -1L;
    d.tigerTagMeasureUnitId = doc["tigerTagMeasureUnitId"] | -1L;

    // Optional: which NFC-V Extended format to use, if the tag turns out to be NFC-V.
    // "extended" (default), "openSpool", or "openPrintTag". Any other/missing value
    // falls back to "extended" -- unrecognized values are treated as "no preference
    // stated" rather than rejecting the whole request over one optional field.
    String nfcvFmt = String((const char *)(doc["preferredNfcvFormat"] | "extended"));
    if (nfcvFmt != "openSpool" && nfcvFmt != "openPrintTag") nfcvFmt = "extended";
    // Same idea for NTAG -- "openSpool" (default) or "extended" (ntagExtended).
    // Separate parameter from preferredNfcvFormat: the value sets don't fully overlap
    // (NFC-V has the extra "openPrintTag" option NTAG doesn't) and SpoolManagerExtended keeps
    // them as two independent settings, so a shared wire name would only reintroduce
    // an artificial coupling that isn't there on either side.
    String ntagFmt = String((const char *)(doc["preferredNtagFormat"] | "openSpool"));
    if (ntagFmt != "extended" && ntagFmt != "tigerTag") ntagFmt = "openSpool";

    dbgLogf("/nfcwritespool: request received, databaseId=%ld preferredNfcvFormat=%s preferredNtagFormat=%s",
            d.databaseId, nfcvFmt.c_str(), ntagFmt.c_str());
    // No ledFlash here: idleColor() blinks blue for the whole write (see there). A
    // fixed-duration flash would sit on top of it and expire mid-write.
    g_nfcWriteDone = false;
    g_nfcWritePending = true;
    g_nfcWriteDoneAt = millis();
    g_nfcWriteSpoolData = d;
    g_nfcWriteNfcvFormat = nfcvFmt;
    g_nfcWriteNtagFormat = ntagFmt;
    g_nfcWriteKind = NFCWRITE_SPOOL;
    g_nfcWriteReq = true;  // pn5180Task takes over
    server.send(202, "text/plain", "started");
  });

  // Start writing the databaseId to the tag: ?id=<N> (the tag must be present).
  // Non-blocking: kicks off pn5180Task via a request flag and returns immediately.
  // Poll /nfcwritestatus for the result (same pattern as /flow/status).
  server.on("/nfcwriteid", []() {
    if (!server.hasArg("id")) {
      server.send(400, "text/plain", "id missing");
      return;
    }
    long id = server.arg("id").toInt();
    if (id < 0) {
      server.send(400, "text/plain", "id must be >= 0");
      return;
    }
    if (!pn5180IsReady()) {
      server.send(503, "text/plain", "PN5180 not ready");
      return;
    }
    if (g_nfcWritePending) {
      server.send(409, "text/plain", "A write is already in progress");
      return;
    }
    dbgLogf("/nfcwriteid: request received, id=%ld", id);
    // No ledFlash here: idleColor() blinks blue for the whole write (see there). A
    // fixed-duration flash would sit on top of it and expire mid-write.
    g_nfcWriteDone = false;
    g_nfcWritePending = true;
    g_nfcWriteDoneAt = millis();  // also used as a "request started" fallback timeout
    g_nfcWriteId = id;
    g_nfcWriteKind = NFCWRITE_ID;
    g_nfcWriteReq = true;  // pn5180Task takes over
    server.send(202, "text/plain", "started");
  });

  // Erase the tag: wipe the legacy anchor + all Extended-format areas OctoScale could
  // have written (see pn5180EraseTag's comment for exactly what "erase" means per tag
  // type -- notably, the NTAG/NFC-V-OpenSpool Capability Container is OTP hardware and
  // can never be un-set, only the NDEF content itself is cleared). Same async
  // request/poll protocol as the other two writes, so it gets the same TFT lock screen.
  server.on("/nfcerase", HTTP_POST, []() {
    if (!pn5180IsReady()) { server.send(503, "text/plain", "PN5180 not ready"); return; }
    if (g_nfcWritePending) { server.send(409, "text/plain", "A write is already in progress"); return; }
    dbgLog("/nfcerase: request received");
    // No ledFlash here: erase shares g_nfcWritePending with the two write paths, so
    // idleColor() already blinks blue for exactly as long as the erase runs.
    g_nfcWriteDone = false;
    g_nfcWritePending = true;
    g_nfcWriteDoneAt = millis();
    g_nfcWriteKind = NFCWRITE_ERASE;
    g_nfcWriteReq = true;  // pn5180Task takes over
    server.send(202, "text/plain", "started");
  });

  // Poll result of a write started via /nfcwriteid, /nfcwritespool, OR /nfcerase. Once consumed
  // (done=true seen), g_nfcWritePending clears so a new write can start.
  server.on("/nfcwritestatus", []() {
    JsonDocument doc;
    doc["pending"] = g_nfcWritePending && !g_nfcWriteDone;
    doc["done"] = g_nfcWriteDone;
    if (g_nfcWriteDone) {
      doc["ok"] = g_nfcWriteOk;
      doc["uid"] = g_nfcWriteUid;
      if (g_nfcWriteKind == NFCWRITE_SPOOL) {
        doc["format"] = g_nfcWriteFormat;
        doc["bytesWritten"] = g_nfcWriteBytesWritten;
        // Capacity refusal (currently OpenPrintTag only): structured so a caller can
        // tell "this tag is too small for the data you picked" apart from a transport
        // failure, and show the two numbers, without parsing the error string.
        if (g_nfcWriteCapAvail >= 0 || g_nfcWriteCapNeeded >= 0) {
          doc["capacityAvailable"] = g_nfcWriteCapAvail;
          doc["capacityNeeded"] = g_nfcWriteCapNeeded;
          if (!g_nfcWriteOk && g_nfcWriteCapNeeded > g_nfcWriteCapAvail)
            doc["failureReason"] = "tagTooSmall";
        }
        JsonArray dropped = doc["droppedFields"].to<JsonArray>();
        if (g_nfcWriteDroppedFields.length()) {
          int start = 0;
          while (start < (int)g_nfcWriteDroppedFields.length()) {
            int comma = g_nfcWriteDroppedFields.indexOf(',', start);
            if (comma < 0) comma = g_nfcWriteDroppedFields.length();
            dropped.add(g_nfcWriteDroppedFields.substring(start, comma));
            start = comma + 1;
          }
        }
        // Values the caller sent that this format has no field for -- written as a
        // separate list because the remedy differs: droppedFields wants a bigger tag,
        // unsupportedFields wants a different format.
        JsonArray unsup = doc["unsupportedFields"].to<JsonArray>();
        if (g_nfcWriteUnsupportedFields.length()) {
          int start = 0;
          while (start < (int)g_nfcWriteUnsupportedFields.length()) {
            int comma = g_nfcWriteUnsupportedFields.indexOf(',', start);
            if (comma < 0) comma = g_nfcWriteUnsupportedFields.length();
            unsup.add(g_nfcWriteUnsupportedFields.substring(start, comma));
            start = comma + 1;
          }
        }
        doc["error"] = g_nfcWriteOk ? "" : g_nfcWriteErr;
        // `warning` used to also append a raw, unformatted "<format> cannot store:
        // <rawKeys>" note here -- pure duplication of the already-structured
        // `unsupportedFields` array above, and worse to read (no field labels, comma-
        // joined with no spaces). A client parsing unsupportedFields (as SpoolManagerExtended
        // does, with proper labels) got the same information twice, once garbled. Left
        // in only as the genuine truncation warning now; unsupportedFields is the sole
        // source for "fields this format can't store".
        String warn = g_nfcWriteOk ? nfcWriteDropWarning(g_nfcWriteDroppedFields, g_nfcWriteFormat) : String("");
        doc["warning"] = warn;
        // `msg` still needs to mention unsupported fields even though `warning` no
        // longer does -- a caller reading only `msg` (not the structured
        // unsupportedFields array) would otherwise see a plain success message for a
        // write that silently left data behind. `warning` itself stays label-free
        // (unsupportedFields is the array a caller should parse for that), but `msg`
        // is prose already, so folding the raw key list in there is the same
        // trade-off `msg` always made for the drop-warning case just below it.
        String msgSuffix = warn;
        if (g_nfcWriteOk && g_nfcWriteUnsupportedFields.length()) {
          String note = g_nfcWriteFormat + " cannot store: " + g_nfcWriteUnsupportedFields;
          msgSuffix = msgSuffix.length() ? (msgSuffix + "; " + note) : note;
        }
        doc["msg"] = !g_nfcWriteOk ? ("Write/verify failed: " + g_nfcWriteErr)
                   : msgSuffix.length() ? ("Written, but " + msgSuffix)
                   : ("Spool data written (" + g_nfcWriteFormat + ", " +
                      String(g_nfcWriteBytesWritten) + " B)");
      } else if (g_nfcWriteKind == NFCWRITE_ERASE) {
        doc["format"] = g_nfcWriteFormat;
        doc["error"] = g_nfcWriteOk ? "" : g_nfcWriteErr;
        doc["msg"] = g_nfcWriteOk ? "Tag erased" : ("Erase failed: " + g_nfcWriteErr);
      } else {
        doc["msg"] = g_nfcWriteOk ? ("ID written & verified: " + String(g_nfcWriteId))
                                   : ("Write/verify failed: " + g_nfcWriteErr);
      }
      // Green = clean write, amber = written but stripped down (see nfcWriteDropWarning),
      // red = failed. The amber case would otherwise look identical to a full success.
      if (!g_nfcWriteOk) ledFlash(pixel.Color(LED_BRIGHT * 2, 0, 0), 1500);           // red
      else if (g_nfcWriteKind == NFCWRITE_SPOOL &&
               nfcWriteDropWarning(g_nfcWriteDroppedFields, g_nfcWriteFormat).length())
        ledFlash(pixel.Color(LED_BRIGHT * 3, LED_BRIGHT * 2, 0), 1500);               // amber
      else ledFlash(pixel.Color(0, LED_BRIGHT * 3, 0), 1500);                          // green
      g_nfcWriteDone = false;     // consumed -> next poll reports idle
      g_nfcWritePending = false; // /nfcwriteid|/nfcwritespool|/nfcerase can accept a new request
      g_nfcWriteDoneAt = 0;
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // ---- Load-flow endpoints ------------------------------------------------
  server.on("/flow/status", []() {
    server.send(200, "application/json", flowStatusJson());
  });

  // Action chosen: ?do=load|weigh
  //  load  -> ASK_PRINTER
  //  weigh -> freeze the current weight and let the user confirm it (WEIGH_CONFIRM).
  // The reading is captured here so the UI confirms exactly the value it shows -
  // otherwise the weight could drift between display and click (hand still on the
  // scale, settling).
  server.on("/flow/action", []() {
    if (g_flowState != FLOW_ASK_ACTION) {
      server.send(409, "text/plain", "Wrong flow state");
      return;
    }
    String what = server.arg("do");
    if (what == "load") {
      g_flowState = FLOW_ASK_PRINTER;
      server.send(200, "application/json", flowStatusJson());
      return;
    }
    if (what == "weigh") {
      if (g_flowSpoolWeight < 0.0f || g_flowTotalWeight < 0.0f) {
        // without reference values, any weight written would be a guess
        g_flowState = FLOW_ERROR;
        g_flowMsg = "Spool has no empty/total weight in the database";
        ledFlash(pixel.Color(LED_BRIGHT * 2, 0, 0), 1500);
        server.send(200, "application/json", flowStatusJson());
        return;
      }
      // NOT frozen here: the user still needs to take the spool off the reader and
      // put it on the scale. weigh_confirm shows the live scale reading
      // (flowStatusJson returns g_weight); only /flow/weigh captures the final value.
      g_flowState = FLOW_WEIGH_CONFIRM;
      server.send(200, "application/json", flowStatusJson());
      return;
    }
    server.send(400, "text/plain", "do must be load or weigh");
  });

  // Re-capture the reading from the current scale value (user adjusted the spool)
  server.on("/flow/reweigh", []() {
    if (g_flowState != FLOW_WEIGH_CONFIRM) {
      server.send(409, "text/plain", "Wrong flow state");
      return;
    }
    g_flowGrossWeight = g_weight;
    server.send(200, "application/json", flowStatusJson());
  });

  // Reading confirmed -> write it to the SpoolManagerExtended DB via PUT
  server.on("/flow/weigh", []() {
    if (g_flowState != FLOW_WEIGH_CONFIRM) {
      server.send(409, "text/plain", "Wrong flow state");
      return;
    }
    flowDoWeighSave();  // BLOCKING (octoSetMeasuredWeight) — runs in the loop() task
    server.send(200, "application/json", flowStatusJson());
  });

  // Printer chosen: ?idx=<i> -> fetch tool count live -> ASK_TOOL
  server.on("/flow/printer", []() {
    if (g_flowState != FLOW_ASK_PRINTER) {
      server.send(409, "text/plain", "Wrong flow state");
      return;
    }
    if (!server.hasArg("idx")) {
      server.send(400, "text/plain", "idx missing");
      return;
    }
    int idx = server.arg("idx").toInt();
    if (idx < 0 || idx >= g_octoCount) {
      server.send(400, "text/plain", "idx invalid");
      return;
    }
    flowDoPrinter(idx);  // BLOCKING (octoToolCount) — runs in the loop() task
    if (g_flowState == FLOW_ERROR) {
      server.send(502, "text/plain", g_flowMsg);
      return;
    }
    server.send(200, "application/json", flowStatusJson());
  });

  // Tool chosen: ?n=<N> -> call the SpoolManagerExtended load URL -> DONE/ERROR
  server.on("/flow/tool", []() {
    if (g_flowState != FLOW_ASK_TOOL) {
      server.send(409, "text/plain", "Wrong flow state");
      return;
    }
    if (!server.hasArg("n")) {
      server.send(400, "text/plain", "n missing");
      return;
    }
    int n = server.arg("n").toInt();
    if (n < 0 || n >= g_flowToolCount) {
      server.send(400, "text/plain", "Tool out of range");
      return;
    }
    flowDoTool(n);  // BLOCKING (octoLoadSpool) — runs in the loop() task
    server.send(200, "application/json", flowStatusJson());
  });

  server.on("/flow/cancel", []() {
    // Same reasoning as the TFT's back button (see flowReset): the user dismissed the
    // flow while the tag is most likely still on the reader, so keep it "handled"
    // rather than reopening the flow on the next poll.
    flowReset(false);
    server.send(200, "application/json", flowStatusJson());
  });

  // Read/set the ask-timeout: no arg -> current value; ?sec=<0-60> -> set it
  // (0 = infinite). Persisted in NVS (key askTimeout).
  server.on("/flow/timeout", []() {
    if (server.hasArg("sec")) {
      int s = server.arg("sec").toInt();
      if (s < 0) s = 0;
      if (s > 60) s = 60;
      g_flowAskTimeoutSec = (uint8_t)s;
      Preferences p;
      p.begin("octoscale", false);
      p.putUChar("askTimeout", g_flowAskTimeoutSec);
      p.end();
    }
    JsonDocument doc;
    doc["sec"] = g_flowAskTimeoutSec;  // 0 = infinite
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // ---- OctoPrint instance management ---------------------------------------
  server.on("/octoprint/list", []() {
    server.send(200, "application/json", octoListJson());
  });

  server.on("/octoprint/add", HTTP_POST, []() {
    String host = server.arg("host");
    if (host.length() == 0) {
      server.send(400, "text/plain", "host missing");
      return;
    }
    uint16_t port = server.hasArg("port") ? server.arg("port").toInt() : 80;
    if (!octoAdd(server.arg("name"), host, port, server.arg("apikey"))) {
      server.send(507, "text/plain", "Full (max " + String(OCTO_MAX_INSTANCES) + ")");
      return;
    }
    octoRefreshDbIds();  // refresh dbId for the new (+ all) instances
    server.send(200, "application/json", octoListJson());
  });

  // Test an OctoPrint instance: ?idx=<i> -> fetch tool count live (checks
  // reachability + API key). Useful during setup.
  server.on("/octoprint/test", []() {
    if (!server.hasArg("idx")) {
      server.send(400, "text/plain", "idx missing");
      return;
    }
    int idx = server.arg("idx").toInt();
    String err;
    int count = octoToolCount((uint8_t)idx, err);
    JsonDocument doc;
    doc["idx"] = idx;
    doc["toolCount"] = count;
    doc["error"] = err;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Fetch an instance's DB identity (SpoolManagerExtended databaseInfo): ?idx=<i>
  // Returns external + dbId, to see which instances share the same DB.
  server.on("/octoprint/dbinfo", []() {
    if (!server.hasArg("idx")) {
      server.send(400, "text/plain", "idx missing");
      return;
    }
    int idx = server.arg("idx").toInt();
    String dbId, err;
    bool external = false;
    int r = octoDbInfo((uint8_t)idx, dbId, external, err);
    JsonDocument doc;
    doc["idx"] = idx;
    doc["result"] = r;  // 1=ok, 0=endpoint missing (old plugin), -1=error
    doc["external"] = external;
    doc["dbId"] = dbId;
    doc["error"] = err;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Manually set an instance's dbId: ?idx=<i>&dbId=<string>. For when an instance
  // was offline while being added/at boot (databaseInfo unreachable) but its fallback
  // group is known anyway. An empty dbId means local/no fallback.
  server.on("/octoprint/setdbid", []() {
    if (!server.hasArg("idx")) {
      server.send(400, "text/plain", "idx missing");
      return;
    }
    int idx = server.arg("idx").toInt();
    if (idx < 0 || idx >= g_octoCount) {
      server.send(400, "text/plain", "idx invalid");
      return;
    }
    g_octo[idx].dbId = server.arg("dbId");
    octoSave();
    server.send(200, "application/json", octoListJson());
  });

  // Re-fetch dbId for all instances (query databaseInfo + cache). Useful when an
  // instance was offline at boot or the DB config changed.
  server.on("/octoprint/refreshdb", []() {
    int n = octoRefreshDbIds();
    JsonDocument doc;
    doc["updated"] = n;
    JsonArray arr = doc["instances"].to<JsonArray>();
    for (uint8_t i = 0; i < g_octoCount; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["idx"] = i;
      o["name"] = g_octo[i].name;
      o["dbId"] = g_octo[i].dbId;
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Edit an existing OctoPrint instance: idx (required), name/host/port/apikey. An
  // empty apikey means "keep the current key" (see octoUpdate) -- the web UI's edit
  // form never round-trips the real key, so an untouched field must not clear it.
  // Send apikey=<anything> to actually change it, including an explicit empty value
  // via clearkey=1 (mirrors the "no API key needed" case /octoprint/add also allows).
  server.on("/octoprint/edit", HTTP_POST, []() {
    if (!server.hasArg("idx")) {
      server.send(400, "text/plain", "idx missing");
      return;
    }
    int idx = server.arg("idx").toInt();
    if (idx < 0 || idx >= g_octoCount) {
      server.send(400, "text/plain", "idx invalid");
      return;
    }
    String host = server.arg("host");
    if (host.length() == 0) {
      server.send(400, "text/plain", "host missing");
      return;
    }
    uint16_t port = server.hasArg("port") ? server.arg("port").toInt() : 80;
    bool keepKey = !server.hasArg("apikey") && !server.hasArg("clearkey");
    if (!octoUpdate((uint8_t)idx, server.arg("name"), host, port, server.arg("apikey"), keepKey)) {
      server.send(400, "text/plain", "update failed");
      return;
    }
    octoRefreshDbIds();  // host/port may have changed -> dbId could differ now
    server.send(200, "application/json", octoListJson());
  });

  server.on("/octoprint/del", []() {
    if (!server.hasArg("idx")) {
      server.send(400, "text/plain", "idx missing");
      return;
    }
    if (!octoDel(server.arg("idx").toInt())) {
      server.send(400, "text/plain", "idx invalid");
      return;
    }
    server.send(200, "application/json", octoListJson());
  });

  // ---- Spool DB source (via the SpoolManagerExtended HTTP bridge) -------------------
  server.on("/db/get", []() {
    server.send(200, "application/json", dbCfgJson());
  });

  server.on("/db/set", []() {
    if (!server.hasArg("idx")) {
      server.send(400, "text/plain", "idx missing");
      return;
    }
    int idx = server.arg("idx").toInt();
    if (idx < 0 || idx >= g_octoCount) {
      server.send(400, "text/plain", "idx invalid");
      return;
    }
    dbSetInstance((uint8_t)idx);
    server.send(200, "application/json", dbCfgJson());
  });

  // Test a spool lookup directly: ?id=<N> -> checks spoolExists via SpoolManagerExtended.
  // Useful during setup without a physical tag.
  server.on("/db/test", []() {
    long id = server.hasArg("id") ? server.arg("id").toInt() : 1;
    String name, err;
    bool ok = spoolExists(id, name, err);
    JsonDocument doc;
    doc["found"] = ok;
    doc["id"] = id;
    doc["name"] = name;
    doc["error"] = err;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // ---- Config backup / restore ----------------------------------------------
  // Export: GET /backup?pw=<passphrase> -> JSON download. With pw, the OctoPrint API
  // keys are AES-256-CBC encrypted (PBKDF2 key). WITHOUT pw the keys are omitted
  // (never a plaintext export). Content-Disposition makes the browser download it.
  server.on("/backup", []() {
    String pw = server.arg("pw");
    String json = bkExport(pw);
    server.sendHeader("Content-Disposition",
                      "attachment; filename=octoscale-backup.json");
    server.send(200, "application/json", json);
  });

  // Import: POST /restore?pw=<passphrase> with the backup JSON in the body.
  // Overwrites the config + NVS. Fully validated before anything is written (a wrong
  // password touches nothing). A reboot afterward is recommended.
  server.on("/restore", HTTP_POST, []() {
    String body = server.arg("plain");
    if (body.length() == 0) {
      server.send(400, "text/plain", "No backup body");
      return;
    }
    String err;
    if (!bkImport(body, server.arg("pw"), err)) {
      server.send(400, "text/plain", err.length() ? err : "Import failed");
      return;
    }
    server.send(200, "application/json", octoListJson());
  });

  // ---- Debug log console (web UI, opt-in via checkbox) -----------------------
  // Persistent across reboots (NVS) -> stays on until explicitly turned off again.
  server.on("/debuglog", []() {
    if (server.hasArg("on")) {
      bool en = (server.arg("on") == "1");
      if (en != g_dbgLogEnabled) {
        g_dbgLogEnabled = en;
        Preferences p;
        p.begin("octoscale", false);
        p.putBool("dbgLogEn", en);
        p.end();
      }
    }
    // Serializing the whole ring buffer needs a contiguous String roughly the size of
    // the buffer itself. That used to be attempted unconditionally and could fail on a
    // low heap, killing the web server outright (firmware kept running, WiFi/HTTP dead
    // until reset -- see DBGLOG_LINES' comment). Bail out with a normal HTTP error
    // instead of taking the server down: the caller sees a clear message, and the
    // buffer stays intact for a later retry once memory frees up.
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < DBGLOG_MIN_HEAP_FOR_DUMP) {
      server.send(503, "application/json",
                  "{\"error\":\"low memory, debug log not dumped\",\"heapFree\":" +
                  String(freeHeap) + "}");
      return;
    }
    JsonDocument doc;
    doc["enabled"] = g_dbgLogEnabled;
    doc["seq"] = g_dbgLogSeq;
    JsonArray lines = doc["lines"].to<JsonArray>();
    // Oldest-to-newest: start right after the write head (that's the oldest slot).
    for (int i = 0; i < DBGLOG_LINES; i++) {
      String &l = g_dbgLogBuf[(g_dbgLogHead + i) % DBGLOG_LINES];
      if (l.length()) lines.add(l);
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Raw Mifare Crypto1 auth frame tracing -- deliberately separate from the debug log's
  // own on/off switch, because a resting Mifare tag re-authenticates on every 500 ms
  // poll and these two lines per auth otherwise flood the ring buffer (see the callback
  // in pn5180nfc.h). Not persisted: always back off after a reboot.
  server.on("/nfcauthtrace", []() {
    if (server.hasArg("on")) g_pn5180AuthTrace = (server.arg("on") == "1");
    server.send(200, "application/json",
                String("{\"authTrace\":") + (g_pn5180AuthTrace ? "true" : "false") + "}");
  });

  // ---- System metrics (RAM / flash / CPU / uptime) ---------------------------
  server.on("/system", []() {
    JsonDocument doc;
    // Subsystem status (for the web UI status bar, matches the display footer)
    doc["scaleReady"] = g_scaleReady;
    doc["nfcReady"] = pn5180IsReady();
    doc["dbReachable"] = g_dbReachable;   // -1 unknown, 0 offline, 1 reachable
    // Heap (RAM): free / total / all-time minimum (watermark)
    uint32_t heapTotal = ESP.getHeapSize();
    uint32_t heapFree = ESP.getFreeHeap();
    doc["heapTotal"] = heapTotal;
    doc["heapFree"] = heapFree;
    doc["heapUsed"] = heapTotal - heapFree;
    doc["heapMinFree"] = ESP.getMinFreeHeap();  // lowest value ever seen
    doc["psramTotal"] = ESP.getPsramSize();     // 0 if PSRAM isn't present
    doc["psramFree"] = ESP.getFreePsram();
    doc["psramMinFree"] = ESP.getMinFreePsram();   // watermark (lowest ever)
    // Flash: sketch size vs. space available to the sketch (app partition)
    uint32_t sketchUsed = ESP.getSketchSize();
    uint32_t sketchFree = ESP.getFreeSketchSpace();
    doc["sketchUsed"] = sketchUsed;
    doc["sketchTotal"] = sketchUsed + sketchFree;  // total app partition
    doc["flashChip"] = ESP.getFlashChipSize();     // physical flash chip
    doc["flashSpeedMhz"] = ESP.getFlashChipSpeed() / 1000000;
    // OTA: size of the partition the NEXT OTA firmware would land in.
    const esp_partition_t *nextOta = esp_ota_get_next_update_partition(nullptr);
    doc["otaPartSize"] = nextOta ? nextOta->size : 0;
    // CPU
    doc["cpuLoad"] = g_cpuLoad;                     // rough approximation (loop task)
    doc["cpuFreqMhz"] = getCpuFrequencyMhz();
    doc["temp"] = temperatureRead();               // internal chip temp (rough)
    // Chip info (model / revision / cores / SDK)
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    const char *model = "ESP32";
    switch (ci.model) {
      case CHIP_ESP32:   model = "ESP32";    break;
      case CHIP_ESP32S2: model = "ESP32-S2"; break;
      case CHIP_ESP32S3: model = "ESP32-S3"; break;
      case CHIP_ESP32C3: model = "ESP32-C3"; break;
      default: break;
    }
    doc["chipModel"] = model;
    doc["chipRev"] = ci.revision;
    doc["chipCores"] = ci.cores;
    doc["sdkVersion"] = esp_get_idf_version();
    doc["resetReason"] = (int)esp_reset_reason();  // 1=POWERON,3=SW,6=OTA,... (UI mappt)
    doc["taskCount"] = (uint32_t)uxTaskGetNumberOfTasks();
    // Pro-Core-Auslastung (Dual-Core)
    doc["core0Load"] = g_coreLoad[0];
    doc["core1Load"] = g_coreLoad[1];
    // Uptime
    doc["uptimeSec"] = (uint32_t)(millis() / 1000);
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // ---- WiFi status + reconfiguration -----------------------------------------
  server.on("/wifi/status", []() {
    int rssi = WiFi.RSSI();
    // RSSI (dBm) roughly mapped to percent: -50 dBm=100%, -100 dBm=0% (linear approx).
    int quality = 0;
    if (rssi <= -100) quality = 0;
    else if (rssi >= -50) quality = 100;
    else quality = 2 * (rssi + 100);
    JsonDocument doc;
    doc["connected"] = WiFi.status() == WL_CONNECTED;
    doc["ssid"] = WiFi.SSID();
    doc["rssi"] = rssi;
    doc["quality"] = quality;
    doc["ip"] = WiFi.localIP().toString();
    doc["mac"] = WiFi.macAddress();
    doc["host"] = HOSTNAME;
    doc["channel"] = WiFi.channel();       // 2.4 GHz channel
    doc["bssid"] = WiFi.BSSIDstr();        // MAC of the connected AP
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Reconfigure WiFi: clear saved creds + reboot -> on restart, WiFiManager opens the
  // AP config portal ("OctoScale-Setup") automatically since no creds remain. Reboot
  // is used instead of running the portal alongside the live web server.
  server.on("/wifi/portal", []() {
    server.send(200, "text/html",
                "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                "<style>body{text-align:center;font-family:verdana;background:#060606;color:#fff}"
                ".msg{max-width:400px;margin:40px auto;padding:20px;background:#282828;"
                "border:1px solid #555;border-left:5px solid #1fa3ec;border-radius:.3rem;text-align:left}</style>"
                "</head><body><div class='msg'><strong>WiFi setup started</strong><br/><br/>"
                "The device is restarting and will open the setup network "
                "<b>\"OctoScale-Setup\"</b>.<br/><br/>Connect to it (phone/PC) and "
                "choose your WiFi. OctoScale will be reachable at its IP again afterward."
                "</div></body></html>");
    Serial.println("WiFi portal requested via web UI -> resetSettings + reboot");
    delay(500);
    WiFiManager wm;
    wm.resetSettings();  // clear saved creds
    delay(200);
    ESP.restart();
  });

  // Web OTA from a URL: POST /updateurl?url=<http-URL-to-.bin>
  // The ESP downloads the firmware itself from the URL and flashes it (HTTPUpdate).
  // Blocks during the download (a few seconds on the LAN). Reboots on success.
  // HTTP only (no TLS) — consistent with the rest of the communication (OctoPrint,
  // web UI, backup all run unencrypted on the local network). The update is
  // protected against corrupted downloads via MD5/length, but the source isn't
  // authenticated -> only use trusted URLs on your own LAN.
  server.on("/updateurl", HTTP_POST, []() {
    String url = server.arg("url");
    url.trim();
    if (!url.startsWith("http://")) {
      server.send(400, "text/plain", "url missing or not http://");
      return;
    }
    Serial.printf("URL OTA: %s\n", url.c_str());
    httpUpdate.rebootOnUpdate(false);  // we reboot ourselves AFTER the response
    g_otaProgressPct = 0;
    g_otaInProgress = true;  // pn5180Task locks the TFT/encoder on the next tick
    httpUpdate.onProgress([](int done, int total) {
      g_otaProgressPct = total ? (uint8_t)((done * 100) / total) : 0;
    });
    WiFiClient cli;
    t_httpUpdate_return ret = httpUpdate.update(cli, url);
    g_otaInProgress = false;
    if (ret == HTTP_UPDATE_OK) {
      server.send(200, "text/plain", "Update OK - restarting");
      Serial.println("URL OTA OK -> reboot");
      delay(500);
      ESP.restart();
    } else {
      String err = String(httpUpdate.getLastError()) + " " + httpUpdate.getLastErrorString();
      Serial.printf("URL OTA error: %s\n", err.c_str());
      server.send(502, "text/plain", "Update failed: " + err);
    }
  });

  // Web OTA: POST /update (result page + reboot on success)
  server.on(
      "/update", HTTP_POST,
      []() {
        bool ok = !Update.hasError();
        server.send(
            200, "text/html",
            ok ? "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                 "<meta http-equiv='refresh' content='8;url=/'>"
                 "<style>body{text-align:center;font-family:verdana;background:#060606;color:#fff}"
                 ".msg{max-width:400px;margin:40px auto;padding:20px;background:#282828;"
                 "border:1px solid #555;border-left:5px solid #5cb85c;border-radius:.3rem;text-align:left}</style>"
                 "</head><body><div class='msg'><strong>Update OK</strong><br/>Restarting...</div></body></html>"
               : "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                 "<style>body{text-align:center;font-family:verdana;background:#060606;color:#fff}"
                 ".msg{max-width:400px;margin:40px auto;padding:20px;background:#282828;"
                 "border:1px solid #555;border-left:5px solid #dc3630;border-radius:.3rem;text-align:left}</style>"
                 "</head><body><div class='msg'><strong>Update failed</strong><br/>"
                 "Restart and try again</div></body></html>");
        if (ok) {
          delay(500);
          ESP.restart();
        }
      },
      []() {  // upload handler (chunked)
        HTTPUpload &up = server.upload();
        if (up.status == UPLOAD_FILE_START) {
          Serial.printf("Web OTA: %s\n", up.filename.c_str());
          g_otaProgressPct = 0;
          g_otaInProgress = true;  // pn5180Task locks the TFT/encoder on the next tick
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
        } else if (up.status == UPLOAD_FILE_WRITE) {
          if (Update.write(up.buf, up.currentSize) != up.currentSize)
            Update.printError(Serial);
        } else if (up.status == UPLOAD_FILE_END) {
          if (Update.end(true))
            Serial.printf("Web OTA OK: %u bytes\n", up.totalSize);
          else
            Update.printError(Serial);
          g_otaInProgress = false;
        }
      });

  server.begin();
  Serial.println("HTTP server running (port 80), web OTA at /update");
}

void startArduinoOTA() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.onStart([]() {
    Serial.println("ArduinoOTA: start");
    g_otaProgressPct = 0;
    g_otaInProgress = true;  // pn5180Task locks the TFT/encoder on the next tick
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nArduinoOTA: end");
    g_otaInProgress = false;
  });
  ArduinoOTA.onProgress([](unsigned int prog, unsigned int total) {
    Serial.printf("ArduinoOTA: %u%%\r", (prog * 100) / total);
    g_otaProgressPct = total ? (uint8_t)((prog * 100) / total) : 0;
  });
  ArduinoOTA.onError([](ota_error_t err) {
    Serial.printf("ArduinoOTA error[%u]\n", err);
    g_otaInProgress = false;
  });
  ArduinoOTA.begin();
  Serial.println("ArduinoOTA ready (espota push)");
}

void pn5180Task(void *param);  // forward declaration (defined after setup())
void dbPingTask(void *param);  // forward declaration (defined after setup())

// HX711 in its own task. scale.get_units() blocks briefly -> shouldn't run in loop(),
// so HTTP/OTA stay free. Pinned to core 0, loop/WiFi on core 1.
void scaleTask(void *param) {
  unsigned long lastPrint = 0;
  for (;;) {
    // Handle a tare request (web UI/TFT) here -> no concurrent HX711 access.
    if (g_tareReq) {
      g_tareReq = false;
      if (g_scaleReady) { scale.tare(20); g_weight = 0; Serial.println("Tared (via flag)"); }
    }
    g_scaleHxReady = g_scaleReady && scale.is_ready();
    // Watchdog: has the raw reading moved at all recently? Runs OUTSIDE the
    // is_ready() branch on purpose -- the failure mode being caught is precisely one
    // where that branch stops being entered, so a check inside it would never run.
    if (g_scaleReady) {
      // Anchor, not "previous sample": the window only restarts once the reading has
      // actually travelled SCALE_STUCK_TOLERANCE counts away from where it started.
      // Comparing against the previous sample instead would let a slow drift of one
      // count at a time reset the timer forever and never catch a real freeze.
      static long stuckAnchorRaw = 0;
      static bool haveAnchor = false;
      uint32_t now = millis();
      long raw = g_scaleRawLast;
      if (!haveAnchor) { stuckAnchorRaw = raw; haveAnchor = true; g_scaleStuckSince = now; }
      else if (labs(raw - stuckAnchorRaw) > SCALE_STUCK_TOLERANCE) {
        stuckAnchorRaw = raw; g_scaleStuckSince = now;
      }
      else if (g_scaleStuckSince && (now - g_scaleStuckSince) >= SCALE_STUCK_MS) {
        // Frozen. power_down() holds SCK high >60us, which is the HX711's own reset:
        // it drops the internal analog front end and the serial state machine, so a
        // chip wedged mid-transfer starts clean instead of waiting for a clock edge
        // that already went by. Cheaper and far less invasive than rebooting the board,
        // and it keeps tare/calibration (those live in the HX711 driver, not the chip).
        scale.power_down();
        vTaskDelay(pdMS_TO_TICKS(2));   // datasheet: >60us; 2ms is generous and free here
        scale.power_up();
        g_scaleRecoveries++;
        g_scaleLastRecoveryMs = now;
        g_scaleStuckSince = now;        // give it a fresh window before trying again
        stuckAnchorRaw = raw;           // re-anchor too, else the next window compares
                                        // against a value from before the reset
        dbgLogf("Scale: HX711 frozen for %ums (raw pinned at %ld +/-%ld) -> power-cycled, recovery #%u",
                SCALE_STUCK_MS, stuckAnchorRaw, SCALE_STUCK_TOLERANCE,
                (unsigned)g_scaleRecoveries);
        Serial.printf("Scale: HX711 frozen -> power-cycled (#%u)\n", (unsigned)g_scaleRecoveries);
      }
    }
    if (g_scaleHxReady) {
      float grams = scale.get_units(1);
      if (grams < 0) grams = 0;
      g_weight = g_weight * 0.7f + grams * 0.3f;  // light smoothing (1 sample)
      // Diagnostics: raw (unscaled, untared) ADC value + rolling noise window. Cheap --
      // reuses the conversion already clocked out for get_units() above, no extra HX711
      // transaction (get_value() would trigger a second blocking read).
      long raw = (long)(grams * g_calFactor) + scale.get_offset();
      g_scaleRawLast = raw;
      g_scaleLastReadMs = millis();
      g_scaleNoiseBuf[g_scaleNoiseIdx] = raw;
      g_scaleNoiseIdx = (g_scaleNoiseIdx + 1) % SCALE_NOISE_WINDOW;
      if (g_scaleNoiseCount < SCALE_NOISE_WINDOW) g_scaleNoiseCount++;
      // Weight trace only when the debug log is armed AND the weight actually changed:
      // the board normally runs on external power with no serial monitor attached, so
      // an unconditional 2 Hz printf just formats a string into the void. dbgLog is
      // fully inert while disabled (see dbglog.h). The change filter matters because
      // this is a 2 Hz line in a 400-entry ring buffer -- logging a resting, unchanged
      // scale reading pushed every actual diagnostic line out of the buffer within
      // about three minutes.
      if (g_dbgLogEnabled && millis() - lastPrint >= 500) {
        lastPrint = millis();
        static float lastLoggedWeight = -9999.0f;
        if (fabsf(g_weight - lastLoggedWeight) >= 0.5f) {
          lastLoggedWeight = g_weight;
          dbgLogf("Weight: %.1f g", g_weight);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// --- Reader-neutral flow-trigger helpers -----------------------------------
// Used by the PN5180 poll (pn5180Task) to feed the load-flow state machine.

// New tag with a valid ID -> start the load flow, but only from a RESTING state
// (IDLE/DONE/UNKNOWN/ERROR). This way a new tag never hijacks an ongoing interaction.
// uid: the tag's raw UID (hex, big-endian) — only needed as a fallback when id < 0 (no
// OctoScale databaseId payload on the tag, e.g. a foreign/manufacturer tag).
static void flowOnTagPresent(long id, const String &uid) {
  bool flowIdle = (g_flowState == FLOW_IDLE || g_flowState == FLOW_DONE ||
                   g_flowState == FLOW_UNKNOWN || g_flowState == FLOW_ERROR);
  if (g_nfcDebug) return;  // debug mode: never auto-trigger the load flow
  // Just-written tag -> don't auto-load. But DON'T just drop the trigger: the forced
  // re-read after a write (g_lastUid="") lands ~2s later, i.e. inside this very window,
  // and afterwards uidHex == g_lastUid means the poll never calls us again. The flow
  // then sat in FLOW_UNKNOWN showing the PRE-write state ("Ready to write") on a tag
  // that had just been written successfully, until the tag was physically removed and
  // replaced. Clearing g_lastUid re-arms the poll so the trigger is retried once the
  // window has passed, instead of being lost.
  if (millis() < g_nfcWriteSuppressUntil) {
    g_flowRetryUid = uid;
    g_flowRetryId = id;
    return;
  }
  // Someone is reading this tag over HTTP right now AND it's a foreign vendor tag ->
  // they're adding it from the browser, and the device announcing it too would be two
  // interfaces narrating one action. Deliberately narrow: a KNOWN or BLANK tag still
  // starts the flow normally, because there the TFT reaction IS the feature, and a
  // device that stops responding to a tag reads as broken.
  if (millis() < g_nfcReadSuppressUntil && g_nfcExtCacheOccupancy == "foreign" &&
      g_nfcExtCacheUid == uid) {
    dbgLogf("Flow: suppressed for foreign tag %s (raw read in progress)", uid.c_str());
    return;
  }
  if (!flowIdle) return;
  if (id >= 0) {
    g_flowLookupByCode = false;
    g_flowSpoolId = id;
    dbgLogf("Flow: tag present, databaseId=%ld -> DB check", id);
  } else if (uid.length()) {
    // No databaseId payload -> try resolving the tag by its UID (SpoolManagerExtended `code`).
    g_flowLookupByCode = true;
    g_flowLookupUid = uid;
    g_flowSpoolId = -1;
    dbgLogf("Flow: tag present, no databaseId, uid=%s -> byCode lookup", uid.c_str());
  } else {
    return;
  }
  g_flowPrinter = -1;
  g_flowToolCount = 0;
  g_flowTool = -1;
  g_flowMsg = "";
  g_flowGrossWeight = 0.0f;
  g_flowState = FLOW_DB_CHECK;
  g_flowDbRequest = true;  // loop() runs the DB check
}

// Tag is gone -> clear a pending flow (end states reset right after debouncing,
// selection states only after g_flowAskTimeoutSec). Active states + WEIGH_CONFIRM are
// left untouched. Debounced via a persistent counter (a static owned by the caller).
static void flowOnTagGone(uint8_t &goneStreak) {
  unsigned long &askGoneSince = *(unsigned long *)&g_flowAskGoneSince;
  bool endState = (g_flowState == FLOW_DONE || g_flowState == FLOW_UNKNOWN ||
                   g_flowState == FLOW_ERROR);
  bool askState = (g_flowState == FLOW_ASK_ACTION || g_flowState == FLOW_ASK_PRINTER ||
                   g_flowState == FLOW_ASK_TOOL);
  if (endState) {
    askGoneSince = 0;
    // FLOW_ERROR gets extra time on screen (poll is 500ms -> ~8s) so the message is
    // actually readable; DONE/UNKNOWN reset quickly as before (~1.5s).
    uint8_t neededStreak = (g_flowState == FLOW_ERROR) ? 16 : 3;
    if (++goneStreak >= neededStreak) {
      goneStreak = 0;
      Serial.println("Tag removed -> flow reset (idle)");
      flowReset();
    }
  } else if (askState) {
    goneStreak = 0;
    if (g_flowAskTimeoutSec == 0) {
      askGoneSince = 0;
    } else {
      if (askGoneSince == 0) askGoneSince = millis();
      if (millis() - askGoneSince >= (unsigned long)g_flowAskTimeoutSec * 1000UL) {
        askGoneSince = 0;
        Serial.printf("Tag removed + %us timeout -> flow idle\n", g_flowAskTimeoutSec);
        flowReset();
      }
    }
  } else {
    goneStreak = 0;
    askGoneSince = 0;
  }
}

void startScale() {
  scale.begin(HX711_DOUT, HX711_SCK);
  if (!scale.wait_ready_timeout(2000)) {
    Serial.println("HX711 NOT found - check wiring");
    return;
  }
  prefs.begin("octoscale", true);  // load the saved factor (fallback = default)
  g_calFactor = prefs.getFloat("calFactor", DEFAULT_CALIBRATION_FACTOR);
  prefs.end();
  scale.set_scale(g_calFactor);
  Serial.printf("Calibration factor: %.4f\n", g_calFactor);
  Serial.println("Taring the scale (leave it empty)...");
  scale.tare(20);
  g_scaleReady = true;
  Serial.println("HX711 ready.");
}

// Raise the brownout-detector threshold above its power-on default. Rationale: two
// field incidents left ZERO trace anywhere -- no Serial output (not even the ROM
// bootloader banner), no HTTP response, no resetReason logged from the PRIOR boot
// (because the chip lost power before it could persist anything). That signature
// points at a brief supply dip that fell far enough, fast enough, to black out the
// UART itself -- consistent with the default threshold (~2.43V) being too close to
// the point where the MCU stops functioning at all. Raising the threshold makes the
// chip reset (and log ESP_RST_BROWNOUT, see /system's resetReason) at a higher, safer
// voltage -- trading "never see it happen" for "always get a diagnosable reset".
// Level is 0-7 (higher = higher trip voltage); 4 is comfortably above default without
// tripping on ordinary transient load (PN5180 RF field, buzzer, TFT backlight).
static void raiseBrownoutThreshold() {
  brownout_hal_config_t cfg = {
    .threshold = 4,
    .enabled = true,
    .reset_enabled = true,
    .flash_power_down = true,
    .rf_power_down = true,
  };
  brownout_hal_config(&cfg);
}

void setup() {
  raiseBrownoutThreshold();
  Serial.begin(115200);
  // Native USB-CDC (S3): wait for the monitor ONLY with a timeout! A bare
  // "while(!Serial)" blocks the boot forever if no serial host is connected -> the
  // board would NEVER boot on external power / after a reset (hangs before WiFi).
  // 1.5s is enough for an open monitor to see the boot banner; without one, boot
  // continues normally after the timeout.
  unsigned long serWait = millis();
  while (!Serial && millis() - serWait < 1500) delay(10);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  pixel.begin();
  pixel.setBrightness(255);  // dimming is done via the color values (LED_BRIGHT)
  pixelExt.begin();
  pixelExt.setBrightness(255);
  g_ledState = LED_BOOT;     // blue: boot / WiFi connecting
  ledTick();

  Serial.println("\n========================================");
  Serial.printf("  OctoScale  v%s\n", FW_VERSION);
  Serial.printf("  Build: %s\n", FW_BUILD);
  Serial.println("========================================");

  // SPI coexistence: TFT_eSPI uses HSPI (USE_HSPI_PORT), the PN5180 library uses the
  // global SPI = FSPI on the S3 -> two INDEPENDENT SPI controllers. They only share
  // the physical pins (11/12/13) via the GPIO matrix; separate CS lines (TFT=38,
  // PN5180 NSS=10) select who's talking. Both can be initialized in parallel.
  // Initialize the PN5180 FIRST (before the display), so its FSPI instance is settled
  // before TFT_eSPI sets up its HSPI bus.
  Serial.println("PN5180: init...");
  pn5180Init();

  if (displayInit()) {
    displaySplash(FW_VERSION);
    Serial.println("TFT: init ok (HSPI) + splash");
  }

  encoderInit();  // EC11 A/B (ISR) + PUSH + KO (INPUT_PULLUP)
  Serial.println("Encoder: init");
  menuInit();     // TFT menu (takes over the display after the splash)

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);

  WiFiManager wm;
  // wm.resetSettings();  // uncomment to clear saved creds
  wm.setConfigPortalTimeout(180);  // 3 min in the AP portal, then reboot
  // Robust connect: otherwise WiFiManager gives up after ONE short attempt (~4s) and
  // starts the AP immediately -> "occasionally boots into AP mode" if the router
  // answers slowly.
  wm.setConnectTimeout(20);   // wait up to 20s per connection attempt
  wm.setConnectRetries(3);    // 3 attempts before the AP portal starts
  wm.setWiFiAutoReconnect(true);  // auto-reconnect on WiFi loss
  wm.setAPCallback([](WiFiManager *) {  // config portal open -> orange status LED
    g_ledState = LED_AP;
    ledTick();
  });

  Serial.printf("Connecting... falling back to AP \"%s\" (portal with WiFi scan)\n", AP_NAME);
  if (!wm.autoConnect(AP_NAME)) {
    Serial.println("No WiFi + portal timeout -> reboot");
    delay(1000);
    ESP.restart();
  }

  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
  ledPin2Apply();               // GPIO 2 LED on = connected (unless switched off)
  g_ledState = LED_CONNECTED;   // green status LED
  ledTick();

  if (MDNS.begin(HOSTNAME)) {
    Serial.printf("mDNS: http://%s.local\n", HOSTNAME);
  }

  octoLoad();  // load persistent config: OctoPrint instances + DB source
  dbLoadCfg();
  {
    Preferences p;
    p.begin("octoscale", true);
    g_flowAskTimeoutSec = p.getUChar("askTimeout", 30);  // default 30s
    g_blActive = p.getUChar("blActive", 255);
    g_blDim = p.getUChar("blDim", 40);
    g_blTimeoutSec = p.getUShort("blTimeout", 30);
    g_buzEnabled = p.getBool("buzEn", true);
    g_buzMode = p.getUChar("buzMode", 1);  // default passive
    g_buzVol = p.getUChar("buzVol", 200);
    g_buzFreq = p.getUShort("buzFreq", 2700);
    g_ssEnabled = p.getBool("ssEnabled", true);
    g_ssTimeoutSec = p.getUShort("ssTimeout", 60);
    g_offEnabled = p.getBool("offEnabled", false);
    g_offTimeoutSec = p.getUShort("offTimeout", 300);
    g_ledBrightness = p.getUChar("ledBright", 255);
    g_ledOnboard = p.getBool("ledOnboard", true);
    g_ledPin2 = p.getBool("ledPin2", true);
    g_dbgLogEnabled = p.getBool("dbgLogEn", false);
    g_menuDark = p.getBool("menuDark", true);   // TFT theme, dark by default
    p.end();
  }
  menuApplyTheme();  // the palette globals still hold the compile-time default
  ledPin2Apply();    // WiFi connected before the NVS load above -> re-apply the saved
                     // setting, otherwise a disabled GPIO 2 LED would stay lit
  displaySetBacklight(g_blActive);  // apply the saved active brightness
  displayTouch();                   // start the timeout countdown now (not dimmed)
  buzzerInit();
  Serial.printf("OctoPrint instances: %u, DB source: instance %u, ask timeout: %us\n",
                g_octoCount, g_dbInstance, g_flowAskTimeoutSec);

  startScale();
  // Core split (deliberately separated: blocking sensor I/O away from networking):
  //   core 0 = scaleTask (HX711) + pn5180Task (NFC poll + TFT menu + encoder) +
  //            dbPingTask (DB reachability) + idle0
  //   core 1 = loop() (WiFi/HTTP/OTA/flow delegation) + idle1
  xTaskCreatePinnedToCore(scaleTask, "scaleTask", 4096, nullptr, 1, nullptr, 0);
  // The PN5180 task MUST run on core 0: the library has unbounded while-loops -> a
  // reader hang would otherwise freeze the web server (core 1). The TFT lives in the
  // same task because only ONE task may touch the TFT (SPI). Priority 1 like
  // scaleTask (cooperative via vTaskDelay, no starvation). 8 KB stack: TFT_eSPI draws
  // + string building need more.
  xTaskCreatePinnedToCore(pn5180Task, "pn5180Task", 8192, nullptr, 1, nullptr, 0);
  Serial.println("PN5180 task started (core 0)");
  // Also core 0 (see dbPingTask's own comment) -- small stack, it only ever does one
  // HTTPClient GET + JSON parse, nothing TFT/SPI-related.
  xTaskCreatePinnedToCore(dbPingTask, "dbPingTask", 6144, nullptr, 1, nullptr, 0);
  startArduinoOTA();
  startWebServer();

  // Dual-core idle monitor: one lowest-priority counting task per core (see above).
  xTaskCreatePinnedToCore(idleCounterTask, "idle0", 1024, (void *)0, 0, nullptr, 0);
  xTaskCreatePinnedToCore(idleCounterTask, "idle1", 1024, (void *)1, 0, nullptr, 1);

  // Refresh the instances' DB identities (for auto-fallback groups). Blocks briefly
  // (HTTP per instance), hence after the web server starts. Offline instances keep
  // their cached (NVS) value -> the fallback stays usable.
  if (WiFi.status() == WL_CONNECTED && g_octoCount > 0) {
    int n = octoRefreshDbIds();
    Serial.printf("DB identities refreshed: %d/%u instances\n", n, g_octoCount);
  }

  Serial.println("Ready.");
}

// DB reachability ping (core 0): pings the DB source instance every 60s via
// octoToolCount() (a "light" GET, but HTTPClient's connect+read timeout is 3-4s, and an
// unreachable printer hits that timeout on every single ping). Used to run inline in
// loop() (core 1) -- that meant server.handleClient(), ledTick(), the OTA handler, and
// the DB-check/flow delegation ALL froze for the full timeout every 60s whenever a
// configured printer was offline. A dedicated task means only this ping stalls; the web
// server and everything else in loop() keep running. Deliberately NOT folded into
// pn5180Task: that task's whole purpose is keeping the NFC reader's ~100Hz responsiveness
// away from anything that can block for seconds (see pn5180Task's own comment) -- an
// HTTP timeout is exactly the kind of stall it exists to avoid.
void dbPingTask(void *param) {
  for (;;) {
    if (WiFi.isConnected() && g_octoCount > 0) {
      uint8_t idx = g_dbInstance < g_octoCount ? g_dbInstance : 0;
      String err;
      g_dbReachable = (octoToolCount(idx, err) > 0) ? 1 : 0;
    } else {
      g_dbReachable = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(60000));
  }
}

// PN5180 task (core 0): encapsulates ALL blocking reader access (poll, debug probe,
// write) away from loop()/the web server (core 1). The PN5180 library has unbounded
// while-loops -> a reader hang only freezes this task, not the web UI. Encoder
// debouncing runs here too (lightweight, same cadence).
void pn5180Task(void *param) {
  static uint8_t s3GoneStreak = 0;
  for (;;) {
    // 0) Handle the encoder + buttons FIRST (every task iteration, ~100 Hz), BEFORE
    //    the blocking NFC read. Otherwise a slow PN5180 read (a present/shaky tag
    //    with unbounded RF waits) would delay the PUSH/KO reaction -> "have to click
    //    several times". The menu shares g_flowState with the web UI; blocking
    //    transitions are delegated to loop() (core 1) via g_flowMenuReq.
    encoderTick();
    menuTick(encoderTakeDelta(), encoderPushPressed(), encoderStartPressed());
    buzzerTick();   // advance the buzzer sequence (non-blocking)
    ledTick();      // advance the status LED (non-blocking) -- see ledTick()'s comment:
                     // loop() can block for seconds on HTTP calls, this task can't

    // 1) Write request from the HTTP handler? (blocking, so it runs here in the task)
    if (g_nfcWriteReq) {
      g_nfcWriteReq = false;
      // Clear the menu's "result already shown" latch for this new write. Done here
      // (same task as menuTick, so no race) rather than in the HTTP handlers, which
      // run on core 1 and would have to touch menu-owned state.
      g_menuNfcResultDismissed = false;
      // Also cut short a PREVIOUS write's result screen if it's still in its 5s window
      // (g_nfcMenuResultPending true) when this new write starts -- otherwise menuTick's
      // lock-screen branch stays gated on "!g_nfcMenuResultPending" and can't take over
      // until the old result's timer runs out, so the new write's lock screen would never
      // show at all and the old result would sit well past its own write finishing.
      // Found via soak testing: "Tag written" from write N held the full 5s regardless
      // of write N+1 already starting underneath it.
      g_nfcMenuResultPending = false;
      g_nfcWriteCapAvail = -1;   // stale values must not leak into this write's result
      g_nfcWriteCapNeeded = -1;
      // Force the lock screen onto the TFT for THIS write before the blocking call
      // below runs. menuTick() at the top of the loop only sees g_nfcWritePending if
      // some earlier iteration happened to catch it first -- back-to-back writes (e.g.
      // SpoolManagerExtended firing the next /nfcwritespool right away) can have pn5180Task pick
      // up g_nfcWriteReq in the SAME iteration that g_nfcWritePending went true, before
      // menuTick ever saw it. Without this extra call, the screen would then stay on
      // whatever was showing (e.g. the spool-assign screen) straight through the write
      // and jump directly to the result screen once it's done -- found via soak testing.
      menuTick(0, false, false);
      bool ok;
      String err;
      if (g_nfcWriteKind == NFCWRITE_SPOOL) {
        dbgLogf("pn5180Task: spool write request picked up, databaseId=%ld", g_nfcWriteSpoolData.databaseId);
        String format, dropped;
        int bytesWritten = 0;
        int capAvail = -1, capNeeded = -1;
        ok = pn5180WriteSpoolTagOpt(g_nfcWriteSpoolData, format, bytesWritten, dropped, err,
                                    g_nfcWriteNfcvFormat, g_nfcWriteNtagFormat,
                                    &capAvail, &capNeeded);
        g_nfcWriteCapAvail = capAvail;
        g_nfcWriteCapNeeded = capNeeded;
        g_nfcWriteFormat = format;
        g_nfcWriteBytesWritten = bytesWritten;
        g_nfcWriteDroppedFields = dropped;
        // Only meaningful once the format is known, and only on a successful write --
        // a failed write lost everything, not just the unsupported subset.
        g_nfcWriteUnsupportedFields = ok ? nfcWriteUnsupportedFields(g_nfcWriteSpoolData, format) : String("");
        if (g_nfcWriteUnsupportedFields.length())
          dbgLog("pn5180Task: fields unsupported by " + format + ": " + g_nfcWriteUnsupportedFields);
        // Dropped-field list on its own line via dbgLog(String): dbgLogf() formats into
        // a fixed 160-byte buffer, and a heavily-dropped OpenSpool write (small NFC-V
        // tag) produces a list long enough to be cut off mid-name there.
        dbgLogf("pn5180Task: spool write result ok=%d format=%s bytes=%d err=%s",
                ok, format.c_str(), bytesWritten, err.c_str());
        if (dropped.length()) dbgLog("pn5180Task: dropped fields: " + dropped);
      } else if (g_nfcWriteKind == NFCWRITE_ERASE) {
        dbgLog("pn5180Task: erase request picked up");
        String format;
        ok = pn5180EraseTag(format, err);
        g_nfcWriteFormat = format;
        dbgLogf("pn5180Task: erase result ok=%d format=%s err=%s", ok, format.c_str(), err.c_str());
      } else {
        dbgLogf("pn5180Task: write request picked up, id=%ld", g_nfcWriteId);
        ok = pn5180WriteLegacyId(g_nfcWriteId, err);  // probes once, dispatches by tag kind
        dbgLogf("pn5180Task: write result ok=%d err=%s", ok, err.c_str());
      }
      g_nfcWriteErr = err;
      g_nfcWriteOk = ok;
      g_nfcWriteUid = g_pn5180Uid;  // UID of the tag on the reader during this write
      if (ok) buzzerSuccess(); else buzzerError();  // write OK/error
      g_lastUid = "";            // force a re-read on the next poll
      // Debug mode's poll (1b above) has its OWN "did the tag change" cache
      // (g_nfcProbe.type/uid, compared against a fresh probe each tick) -- g_lastUid
      // alone doesn't touch it. A same-UID rewrite (the normal case: same physical tag,
      // new content) would otherwise leave /nfcprobe showing pre-write format/data
      // until the tag is physically removed and re-placed. Found via a live OpenPrintTag
      // rewrite test: /nfcprobe kept reporting the tag's previous format after a
      // successful write.
      g_nfcProbe = PN5180ProbeResult();
      g_nfcWriteSuppressUntil = millis() + 3000;  // don't auto-start the load flow on it
      // Latch the result for the TFT BEFORE publishing Done: once Done is set, the HTTP
      // handler on core 1 may consume and clear it at any moment (see the comment on
      // g_nfcMenuResultPending), and the menu would then never see that a write finished.
      g_nfcMenuResultPending = true;
      g_nfcWriteDone = true;     // the handler may answer now
      g_nfcWriteDoneAt = millis();
    }
    // Nobody polled /nfcwritestatus to consume the result (e.g. the page was reloaded
    // mid-poll), or the request never got picked up -> don't wedge future writes
    // behind a stale pending flag forever.
    if (g_nfcWritePending && g_nfcWriteDoneAt && millis() - g_nfcWriteDoneAt > 15000) {
      g_nfcWriteReq = false;
      g_nfcWriteDone = false;
      g_nfcWritePending = false;
      g_nfcWriteDoneAt = 0;
    }

    // 1a) Raw dump request from the web UI's "Create dump" button (unknown-tag debug
    // fallback). Mifare Classic only -- re-selects the tag itself (a fresh RF session
    // is needed for the per-sector auth loop; the last probe's selection is long gone
    // by the time the button click reaches here) and reports what it found.
    if (g_nfcDumpReq) {
      g_nfcDumpReq = false;
      g_nfcDumpErr = "";
      g_nfcDumpCount = 0;
      // Full probe (not pn5180ProbeNfcA): NFC-V has to be recognised here too, and the
      // probe tries ISO15693 first -- an NFC-A-only probe would report "no tag present"
      // for every ICODE tag on the reader.
      PN5180ProbeResult pr;
      bool ok = pn5180Probe(pr);
      g_nfcDumpTagType = "";
      g_nfcDumpUid = pr.uid;
      g_nfcDumpUnitBytes = 16;
      g_nfcDumpVariant = "";
      if (!ok || pr.uid.length() == 0) {
        g_nfcDumpErr = "no tag present";
        ok = false;
      } else if (pr.type == PN5180_TAG_NFCV) {
        // --- NFC-V (ISO15693): keyless block walk, 4-byte blocks packed 4-per-entry
        // so the JSON shape stays identical to the Mifare one (16 bytes per row).
        g_nfcDumpTagType = "nfcv";
        g_nfcDumpUnitBytes = NFCV_BLOCK_SIZE;
        dbgLogf("pn5180Task: dump request picked up (NFC-V), uid=%s", pr.uid.c_str());
        uint8_t uidRaw[8];
        if (!pn5180UidHexToBytes8Reversed(pr.uid, uidRaw)) {
          g_nfcDumpErr = "could not decode NFC-V UID";
          ok = false;
        } else {
          uint32_t dumpT0 = millis();
          // Cap at the struct's capacity (47 entries x 16 B = 188 blocks of 4 B).
          int maxBlocks = min((int)(NFC_DUMP_MAX_ENTRIES * 16 / NFCV_BLOCK_SIZE),
                              pr.numBlocks > 0 ? (int)pr.numBlocks : 64);
          static uint8_t nfcvBuf[NFC_DUMP_MAX_ENTRIES * 16];
          memset(nfcvBuf, 0, sizeof(nfcvBuf));
          int gotBlocks = pn5180DumpNfcvBlocks(uidRaw, nfcvBuf, maxBlocks);
          g_nfcDumpMs = millis() - dumpT0;
          // Pack into 16-byte rows; a partial trailing row is kept (zero-padded) so no
          // read data is dropped, and readOk marks how far the tag actually went.
          g_nfcDumpCount = (gotBlocks * NFCV_BLOCK_SIZE + 15) / 16;
          g_nfcDumpUnitCount = gotBlocks;
          for (int i = 0; i < g_nfcDumpCount; i++) {
            g_nfcDumpBlocks[i].block = (uint8_t)(i * 16 / NFCV_BLOCK_SIZE);  // first block in row
            g_nfcDumpBlocks[i].sectorAuthOk = true;   // no auth concept on ISO15693
            g_nfcDumpBlocks[i].readOk = true;
            memcpy(g_nfcDumpBlocks[i].data, nfcvBuf + i * 16, 16);
          }
          g_nfcDumpAuthOkSectors = 0;  // not applicable
          ok = gotBlocks > 0;
          if (!ok) g_nfcDumpErr = "dump failed (no blocks read)";
          dbgLogf("pn5180Task: NFC-V dump ok=%d blocks=%d rows=%d ms=%lu",
                  ok, gotBlocks, g_nfcDumpCount, (unsigned long)g_nfcDumpMs);
        }
      } else if (pr.type == PN5180_TAG_NFCA &&
                 !(pr.sak == 0x08 || pr.sak == 0x18 || pr.sak == 0x09 || pr.sak == 0x28)) {
        // --- NTAG / Ultralight: keyless page walk from page 0, 4-byte pages packed
        // 4-per-entry. Covers NTAG213/215/216 -- the size comes from GET_VERSION, and
        // the walk stops at the first unreadable page regardless.
        g_nfcDumpTagType = "ntag";
        g_nfcDumpUnitBytes = 4;
        // pn5180Probe() ends with reset()+setupRF(), so the card is NO LONGER selected
        // here -- and both GET_VERSION and the page walk require a live selection. Without
        // this re-select the variant came back NTAG_UNKNOWN (reported as "ntag213", the
        // fallback) and every page read failed: "dump failed (no pages read)". Same
        // re-select the /nfcreadstart path does for exactly this reason.
        PN5180ProbeResult reSel;
        uint32_t dumpT0 = millis();
        int gotPages = 0;
        NtagVariant variant = NTAG_UNKNOWN;
        int maxPages = 0;
        static uint8_t ntagBuf[NFC_DUMP_MAX_ENTRIES * 16];
        memset(ntagBuf, 0, sizeof(ntagBuf));
        if (!pn5180ProbeNfcA(reSel) || reSel.uid != pr.uid) {
          g_nfcDumpErr = "tag moved away before the dump could start";
        } else {
          variant = pn5180NtagGetVersion();   // must follow the SELECT directly
          g_nfcDumpVariant = ntagVariantName(variant);
          dbgLogf("pn5180Task: dump request picked up (NTAG %s), uid=%s",
                  g_nfcDumpVariant.c_str(), pr.uid.c_str());
          // User bytes + the 4 header pages (UID/lock/CC), capped by the struct.
          maxPages = ntagUserBytes(variant) / 4 + 4;
          if (maxPages > NFC_DUMP_MAX_ENTRIES * 4) maxPages = NFC_DUMP_MAX_ENTRIES * 4;
          gotPages = pn5180DumpNtagPages(ntagBuf, maxPages);
        }
        g_nfcDumpMs = millis() - dumpT0;
        g_nfcDumpCount = (gotPages * 4 + 15) / 16;
        g_nfcDumpUnitCount = gotPages;
        for (int i = 0; i < g_nfcDumpCount; i++) {
          g_nfcDumpBlocks[i].block = (uint8_t)(i * 4);  // first page in row
          g_nfcDumpBlocks[i].sectorAuthOk = true;   // no auth concept on NTAG
          g_nfcDumpBlocks[i].readOk = true;
          memcpy(g_nfcDumpBlocks[i].data, ntagBuf + i * 16, 16);
        }
        g_nfcDumpAuthOkSectors = 0;  // not applicable
        ok = gotPages > 0;
        if (!ok && g_nfcDumpErr.length() == 0) g_nfcDumpErr = "dump failed (no pages read)";
        dbgLogf("pn5180Task: NTAG dump ok=%d pages=%d rows=%d ms=%lu",
                ok, gotPages, g_nfcDumpCount, (unsigned long)g_nfcDumpMs);
      } else {
        g_nfcDumpTagType = "mifareClassic1k";
        dbgLogf("pn5180Task: dump request picked up, uid=%s", pr.uid.c_str());
        uint32_t dumpT0 = millis();
        g_nfcDumpCount = pn5180DumpMifareClassic1k(pr.uid, g_nfcDumpBlocks);
        g_nfcDumpUnitCount = g_nfcDumpCount;   // 1 unit per row on Mifare
        g_nfcDumpMs = millis() - dumpT0;
        // Count distinct sectors that authenticated (blocks carry the per-sector result,
        // 3 data blocks per sector) -- separates "wrong key everywhere" from a partial
        // read, which is the difference between the failure-case and success-case timing.
        g_nfcDumpAuthOkSectors = 0;
        for (int i = 0; i < g_nfcDumpCount; i += 3)
          if (g_nfcDumpBlocks[i].sectorAuthOk) g_nfcDumpAuthOkSectors++;
        ok = g_nfcDumpCount > 0;
        if (!ok) g_nfcDumpErr = "dump failed (no blocks read)";
        dbgLogf("pn5180Task: dump result ok=%d blocks=%d authSectors=%d/16 ms=%lu",
                ok, g_nfcDumpCount, g_nfcDumpAuthOkSectors, (unsigned long)g_nfcDumpMs);
      }
      g_pn5180->reset(); g_pn5180->setupRF();  // back to normal ISO15693 operation
      g_nfcDumpOk = ok;
      g_nfcDumpDone = true;
      g_nfcDumpPending = false;
    }

    // 1a2) Raw image read for an external parser (/nfcreadstart). Same structure as the
    // dump above, but the keys come from the request and the result is one hex string.
    if (g_nfcReadReq) {
      g_nfcReadReq = false;
      g_nfcReadErr = "";
      g_nfcReadHex = "";
      g_nfcReadUid = "";
      g_nfcReadTagType = "";
      g_nfcReadSectorsOk = 0;
      g_nfcReadMs = 0;
      g_nfcReadStartPage = 0;
      g_nfcReadNtagVariant = "";
      bool ok = false;
      // Default to retryable: everything that goes wrong before we have actually
      // authenticated is a transport/presence problem, i.e. worth trying again. Only a
      // clean "the tag answered and rejected every key" is non-retryable.
      bool retryable = true;
      PN5180ProbeResult pr;
      if (!pn5180ProbeNfcA(pr) || pr.uid.length() == 0) {
        // Nothing on NFC-A -- try ISO15693 before concluding the reader is empty.
        // pn5180ProbeNfcA() leaves the reader in type-A mode (it does NOT restore
        // ISO15693 on failure), so getInventory() here would ALWAYS fail without the
        // reset()+setupRF() below, regardless of what is actually on the reader. That
        // was a real bug: an NFC-V tag reported "no tag present" with retryable:true,
        // sending callers into an endless retry on a tag sitting right there --
        // /nfcprobe saw it fine, because pn5180Probe() switches modes in this order.
        g_pn5180->reset(); g_pn5180->setupRF();
        uint8_t nfcvUid[8] = {0};
        bool isNfcv = (g_pn5180->getInventory(nfcvUid) == ISO15693_EC_OK);
        if (isNfcv) {
          // NFC-V raw read: keyless, UID-addressed block walk, same primitive the dump
          // uses. No sector/auth concept, so `sectors` (a Mifare notion, 0-15) does not
          // apply here and is ignored -- the whole tag is returned, exactly like the
          // NTAG page walk does.
          g_nfcReadTagType = "nfcv";
          char ubuf[3];
          for (int i = 7; i >= 0; i--) { snprintf(ubuf, sizeof(ubuf), "%02X", nfcvUid[i]); g_nfcReadUid += ubuf; }
          uint32_t t0 = millis();
          static uint8_t nfcvBuf[NFC_DUMP_MAX_ENTRIES * 16];
          memset(nfcvBuf, 0, sizeof(nfcvBuf));
          int maxBlocks = (int)(sizeof(nfcvBuf) / NFCV_BLOCK_SIZE);
          int gotBlocks = pn5180DumpNfcvBlocks(nfcvUid, nfcvBuf, maxBlocks);
          g_nfcReadMs = millis() - t0;
          if (gotBlocks > 0) {
            int nbytes = gotBlocks * NFCV_BLOCK_SIZE;
            g_nfcReadHex.reserve(nbytes * 2 + 1);
            char hx[3];
            for (int i = 0; i < nbytes; i++) {
              snprintf(hx, sizeof(hx), "%02x", nfcvBuf[i]);
              g_nfcReadHex += hx;
            }
            ok = true;
          } else {
            // Inventory answered but not a single block read -> the tag left the field
            // between the two, which is worth retrying.
            g_nfcReadErr = "NFC-V tag answered inventory but no block could be read";
          }
          g_pn5180->reset(); g_pn5180->setupRF();
        } else {
          g_nfcReadErr = "no tag present";
        }
      } else {
        g_nfcReadUid = pr.uid;
        bool isClassic = (pr.sak == 0x08 || pr.sak == 0x18 || pr.sak == 0x09 || pr.sak == 0x28);
        uint32_t t0 = millis();
        if (isClassic) {
          g_nfcReadTagType = "mifareClassic1k";
          static MifareBlockDump blocks[64];
          int n = pn5180DumpMifareClassic1kEx(
              pr.uid, blocks,
              g_nfcReadHasKeyA ? g_nfcReadKeyA : nullptr,
              g_nfcReadHasKeyB ? g_nfcReadKeyB : nullptr,
              g_nfcReadSectorMask, true);  // trailers kept: consumers index the full 1K image
          g_nfcReadMs = millis() - t0;
          // Emit every requested block in order, zero-filling what could not be read,
          // so byte offsets stay meaningful regardless of which sectors failed.
          g_nfcReadHex.reserve(n * 32 + 1);
          char hx[3];
          for (int i = 0; i < n; i++) {
            if (blocks[i].sectorAuthOk) g_nfcReadSectorsOk |= (uint16_t)(1u << (blocks[i].block / 4));
            for (int j = 0; j < 16; j++) {
              snprintf(hx, sizeof(hx), "%02x", blocks[i].readOk ? blocks[i].data[j] : 0);
              g_nfcReadHex += hx;
            }
          }
          if (g_nfcReadSectorsOk) {
            ok = true;
          } else {
            // Zero sectors authenticated has TWO causes that must not be conflated: the
            // tag rejected the keys (wrong format -> try the next parser, retryable
            // false), or the tag left the field mid-dump (retryable true). Both look
            // identical in the auth results, so ask whether the tag is still there --
            // if it answers a fresh select, the keys really were wrong.
            PN5180ProbeResult still;
            if (pn5180ProbeNfcA(still) && still.uid == pr.uid) {
              g_nfcReadErr = "authentication failed";
              retryable = false;
            } else {
              g_nfcReadErr = "tag removed during read";
              retryable = true;
            }
          }
        } else {
          g_nfcReadTagType = "ntag";
          // Bound the walk by the tag's real size. Reading past the last page is not a
          // clean failure on these chips -- many wrap back to page 0, which would hand
          // the caller a dump with duplicated content and no indication anything was
          // wrong. GET_VERSION only answers while the tag is freshly selected and
          // nothing has read from it yet, so it has to happen before the walk.
          // Ultralight (and anything that doesn't answer GET_VERSION) falls back to the
          // NTAG213 size -- the smallest, so the walk stops early rather than over-reads.
          NtagVariant variant = pn5180NtagGetVersion();
          int maxPages = (variant != NTAG_UNKNOWN) ? (ntagUserBytes(variant) / 4 + 4)
                                                   : (ntagUserBytes(NTAG_213) / 4 + 4);
          // Reported for diagnostics only -- "which chip is this" is the first question
          // when a user says a tag isn't recognised. "unknown" = didn't answer
          // GET_VERSION (Ultralight and clones), which is itself the useful signal.
          g_nfcReadNtagVariant = ntagVariantName(variant);
          static uint8_t pages[240 * 4];
          if (maxPages > 240) maxPages = 240;
          // GET_VERSION consumed the "freshly selected" window -- re-select before reading.
          PN5180ProbeResult reSel;
          bool reSelected = pn5180ProbeNfcA(reSel) && reSel.uid == pr.uid;
          int got = reSelected ? pn5180DumpNtagPages(pages, maxPages) : 0;
          g_nfcReadMs = millis() - t0;
          if (got > 0) {
            g_nfcReadHex.reserve(got * 8 + 1);
            char hx[3];
            for (int i = 0; i < got * 4; i++) {
              snprintf(hx, sizeof(hx), "%02x", pages[i]);
              g_nfcReadHex += hx;
            }
            ok = true;
          } else {
            // No keys involved here, so a total read failure is always a transport or
            // presence problem -- but say which, so the caller's log is useful.
            PN5180ProbeResult still;
            bool present = pn5180ProbeNfcA(still) && still.uid == pr.uid;
            g_nfcReadErr = present ? "tag unreadable" : "tag removed during read";
          }
        }
      }
      g_pn5180->reset(); g_pn5180->setupRF();
      g_nfcReadOk = ok;
      g_nfcReadRetryable = ok ? false : retryable;
      g_nfcReadDoneAt = millis();
      g_nfcReadSuppressUntil = millis() + 6000;  // re-arm from the END of the read
      g_nfcReadDone = true;
      dbgLogf("pn5180Task: read result ok=%d type=%s bytes=%d sectorsOk=0x%04X ms=%lu err=%s",
              ok, g_nfcReadTagType.c_str(), (int)(g_nfcReadHex.length() / 2),
              g_nfcReadSectorsOk, (unsigned long)g_nfcReadMs, g_nfcReadErr.c_str());
    }
    // Nobody polled /nfcreadstatus (page reloaded mid-poll, client gone) -> don't wedge
    // future reads behind a stale pending flag. Same guard the write flow has.
    if (g_nfcReadPending && g_nfcReadDoneAt && millis() - g_nfcReadDoneAt > 15000) {
      g_nfcReadPending = false;
      g_nfcReadDone = false;
      g_nfcReadDoneAt = 0;
    }

    // 1b) NFC-debug mode toggled from the web UI -> recover the reader here (shared
    // SPI object with the poll below, must not run concurrently from another core).
    if (g_nfcDebugChangeReq) {
      g_nfcDebugChangeReq = false;
      pn5180Recover();
    }

    // 1c) Screen preview toggled from the web UI -> the actual TFT draw happens here
    // (core 0), never in the HTTP handler (core 1).
    if (g_menuPreviewReq) {
      g_menuPreviewReq = false;
      menuPreviewSetActive(g_menuPreviewReqOn);
    }
    if (g_menuPreviewStepReq) {
      g_menuPreviewStepReq = false;
      if (g_menuPreviewActive) {
        menuMoveCursor(g_menuPreviewIdx, kMenuPreviewCount, 1);
        menuPreviewDraw();
      }
    }

    // 1d) Diagnostics card (web UI Debug tab) -- LED test color. Reuses the exact same
    // g_ledTestActive override + ledShow() that the physical MENU_TEST_LED screen uses
    // (menu.h), so the two paths can never fight over which one "owns" the pixels.
    if (g_testLedReq) {
      g_testLedReq = false;
      if (g_testLedReqColorIdx < 0 || g_testLedReqColorIdx >= TEST_LED_COUNT) {
        g_ledTestActive = false;  // release -> ledTick() resumes normal behavior
      } else {
        g_ledTestActive = true;
        const TestLedColor &c = kTestLedColors[g_testLedReqColorIdx];
        ledShow(pixel.Color(c.r * LED_BRIGHT, c.g * LED_BRIGHT, c.b * LED_BRIGHT));
      }
    }

    // 1e) Diagnostics card -- TFT test pattern. Draws directly here (core 0, same
    // rule as the screen preview above) and sets g_tftTestWebActive so menuTick()'s
    // normal redraw logic backs off while a web-triggered pattern is showing (see the
    // guard near the top of menuTick()).
    if (g_testTftReq) {
      g_testTftReq = false;
      if (g_testTftReqPattern < 0) {
        g_tftTestWebActive = false;
        g_menuForceRedraw = true;  // repaint whatever the real state actually is
      } else {
        g_tftTestWebActive = true;
        if (g_testTftReqPattern >= 0 && g_testTftReqPattern < TEST_TFT_COLOR_COUNT) {
          g_testTftColorIdx = g_testTftReqPattern;
          menuRenderTestTftColors();
        } else if (g_testTftReqPattern == 10) {
          menuRenderTestTftFont();
        } else if (g_testTftReqPattern == 20) {
          menuRenderTestTftGray();
        }
      }
    }

    // 2) Tag poll every 500ms. Skipped entirely while the screen preview is active: the
    // preview promises "read-only, no NFC/octo/HTTP call is ever made while this is
    // active" (see menuPreviewDraw's comment), but the poll -- and everything a real
    // tag triggers underneath it (flowOnTagPresent -> DB check -> blocking OctoPrint
    // HTTP calls in loop()) -- previously kept running in the background the whole
    // time, invisible under the preview's TFT overlay. Found via a live serial trace
    // during preview auto-cycling: a tag placed on the reader mid-preview quietly
    // walked all the way through the load flow while the screen showed an unrelated
    // preview frame. lastPoll is deliberately NOT updated here, so polling resumes
    // immediately (not delayed by up to 500ms) once the preview ends.
    static uint32_t lastPoll = 0;
    if (!g_menuPreviewActive && millis() - lastPoll >= 500) {
      lastPoll = millis();
      if (g_nfcDebug) {
        // DEBUG MODE: reads + determines the tag TYPE (NFC-V and NFC-A), no flow.
        // readCc=true: also reads the Capability Container inline, while the card is
        // still selected (see pn5180Probe's comment -- a separate re-select pass here
        // was tried first and caused RF churn that occasionally corrupted the NEXT
        // poll's anticollision read; reading it inline avoids that entirely, and is
        // cheap enough to just always do in debug mode rather than gating on a UID
        // change).
        PN5180ProbeResult pr;
        pn5180Probe(pr, /*readCc=*/true);
        // Normal mode sets g_pn5180Uid unconditionally on every successful read (see
        // the else branch below); debug mode used to skip it entirely, since it never
        // runs that branch -- left g_pn5180Uid stale/empty for anything that reads it
        // (e.g. g_nfcWriteUid, captured from here right after a write). Found via a
        // live OpenPrintTag write test with debug mode on: /nfcwritestatus reported an
        // empty uid despite a tag being present and the write succeeding.
        g_pn5180Present = (pr.type != PN5180_TAG_NONE);
        g_pn5180Uid = pr.uid;
        if (pr.type != g_nfcProbe.type || pr.uid != g_nfcProbe.uid) {
          if (pr.type == PN5180_TAG_NONE) Serial.println("NFC debug: no tag");
          else Serial.printf("NFC debug: %s  UID=%s\n",
                             pn5180TagTypeName(pr.type), pr.uid.c_str());
          dbgLogf("NFC read: type=%s uid=%s id=%ld cc=%s", pn5180TagTypeName(pr.type),
                  pr.uid.c_str(), pr.idParsed, pr.ccState.c_str());
          // Also populate the Extended-read cache here (same pn5180ReadSpoolEx() the
          // normal poll uses) -- lets the web UI's NFC debug view show the FULL
          // Extended payload (material/vendor/color/weights/temps), not just
          // type/UID/CC. Same once-per-UID-change cost gating as the normal poll.
          g_nfcExtCacheUid = "";
          g_nfcExtCacheData = SpoolTagData();
          g_nfcExtCacheHasExtended = false;
          g_nfcExtCacheOccupancy = "";
          if (pr.type != PN5180_TAG_NONE) {
            nfcClassifyTag(pr.type, pr.atqa, pr.sak, g_nfcExtCacheWriteFormat,
                           g_nfcExtCacheFormatLabel, g_nfcExtCacheCapacityBytes,
                           pr.numPages, pr.numPagesPresent);
            g_nfcExtCacheUid = pr.uid;
            bool dummyHasExt; PN5180TagType dummyType; long dummyId; String dummyUid, dummyText;
            String nfcvFormatFound;
            int nfcvCapacityFound = -1;
            if (pn5180ReadSpoolExOpt(dummyType, dummyId, dummyUid, dummyText, g_nfcExtCacheData,
                                  dummyHasExt, &nfcvFormatFound, &nfcvCapacityFound)) {
              g_nfcExtCacheHasExtended = dummyHasExt;
              if (dummyHasExt && pr.type == PN5180_TAG_NFCV && nfcvFormatFound.length()) {
                g_nfcExtCacheWriteFormat = nfcvFormatFound;
                g_nfcExtCacheFormatLabel = (nfcvFormatFound == "nfcvOpenSpool")
                  ? "OpenSpool"
                  : (nfcvFormatFound == "nfcvOpenPrintTag")
                  ? "OpenPrintTag"
                  : "Extended";
                // nfcvOpenSpool's real budget differs from nfcClassifyTag()'s
                // nfcvExtended-sized default -- show the tag's actual NDEF capacity.
                if (nfcvFormatFound == "nfcvOpenSpool" && nfcvCapacityFound >= 0)
                  g_nfcExtCacheCapacityBytes = nfcvCapacityFound;
              } else if (dummyHasExt && pr.type == PN5180_TAG_NFCA &&
                         (nfcvFormatFound == "ntagExtended" || nfcvFormatFound == "tigerTag")) {
                // nfcClassifyTag() always predicts openSpool for NTAG (its only default
                // before v3.1) -- override with the real format once an actual read
                // confirms ntagExtended/tigerTag, same "actual read result beats the
                // earlier guess" pattern as the NFC-V branch above. Capacity stays
                // whatever nfcClassifyTag() computed (openSpool's NDEF-budget math);
                // neither ntagExtended nor tigerTag has NDEF overhead, but this cache is
                // only a display value -- not worth a second capacity formula here.
                g_nfcExtCacheWriteFormat = nfcvFormatFound;
                g_nfcExtCacheFormatLabel = (nfcvFormatFound == "tigerTag") ? "TigerTag" : "Extended";
              }
            }
            // Nothing we know could parse this tag -- decide whether it is blank or
            // carries a foreign vendor's data, so callers can refuse to overwrite it.
            // Same three probes the normal poll path uses, so both paths answer
            // identically (they did not before: this one relied on the probe's ccState,
            // which cannot tell a formatted-but-empty NDEF tag from one with content --
            // and NFC-V had no block reader here at all, so it kept that weaker
            // ccState fallback long after the other two carriers got a real check).
            if (!g_nfcExtCacheHasExtended) {
              if (pr.type == PN5180_TAG_NFCA) {
                bool isClassic = (pr.sak == 0x08 || pr.sak == 0x18 ||
                                  pr.sak == 0x09 || pr.sak == 0x28);
                PN5180ProbeResult occSel;
                if (pn5180ProbeNfcA(occSel) && occSel.uid == pr.uid) {
                  g_nfcExtCacheOccupancy = isClassic ? pn5180MifareOccupancy(pr.uid)
                                                     : pn5180NtagOccupancy();
                }
                g_pn5180->reset(); g_pn5180->setupRF();
              }
              else if (pr.type == PN5180_TAG_NFCV) {
                uint8_t uidRaw[8];
                if (pn5180UidHexToBytes8Reversed(pr.uid, uidRaw))
                  g_nfcExtCacheOccupancy = pn5180NfcvOccupancy(uidRaw);
              }
              // ccState fallback for anything neither branch covered (read failed).
              else if (pr.ccState == "virgin") g_nfcExtCacheOccupancy = "empty";
              else if (pr.ccState.length()) g_nfcExtCacheOccupancy = "foreign";
            }
          }
        }
        g_nfcProbe = pr;
      } else {
        // NORMAL OPERATION: read a tag (NFC-V OR NFC-A) + trigger the load flow.
        PN5180TagType type;
        long id;
        String uidHex, idText;
        if (pn5180ReadSpool(type, id, uidHex, idText)) {
          s3GoneStreak = 0;
          g_pn5180Present = true;
          g_pn5180Uid = uidHex;
          g_nfcProbe.type = type;  // tag type for the TFT (short readout under the weight)
          g_nfcProbe.uid = uidHex;
          g_flowAskGoneSince = 0;  // tag present -> the UI's auto-reset countdown is off
          if (uidHex != g_lastUid) {  // new tag -> handle it once
            g_lastUid = uidHex;

            // Extended-read + tag classification BEFORE the flow starts (moved ahead of
            // flowOnTagPresent -- previously this ran after, so its databaseId (e.g. from
            // OpenSpool's os_db_id) never reached the flow; the flow always saw the
            // legacy-only id from pn5180ReadSpool() above, which is -1 on formats whose
            // legacy ID area was overwritten (OpenSpool's NDEF replaces pages 4-6) -> the
            // flow fell back to a UID lookup even though a valid databaseId was on the
            // tag). Only costs 1-3 extra reads, once per new tag (plan C.4 "Lesekosten
            // beachten"), same as before -- just reordered.
            PN5180ProbeResult pr2;
            uint16_t atqa = 0; uint8_t sak = 0;
            int ntagPages = 0; bool ntagPagesPresent = false;
            if (type == PN5180_TAG_NFCA && pn5180ProbeNfcA(pr2)) {
              atqa = pr2.atqa; sak = pr2.sak;
              // GET_VERSION here, while the card is still selected and nothing has read
              // from it yet -- that is the only window in which an NTAG answers it (see
              // pn5180Probe's NTAG branch). Skipped for Mifare Classic, which doesn't
              // implement the command at all.
              bool isClassic = (sak == 0x08 || sak == 0x18 || sak == 0x09 || sak == 0x28);
              if (!isClassic && (sak & 0x20) == 0) {
                NtagVariant v = pn5180NtagGetVersion();
                if (v != NTAG_UNKNOWN) { ntagPages = ntagUserBytes(v) / 4 + 4; ntagPagesPresent = true; }
              }
              g_pn5180->reset(); g_pn5180->setupRF();
            }
            g_nfcProbe.atqa = atqa; g_nfcProbe.sak = sak;
            g_nfcProbe.numPages = ntagPages; g_nfcProbe.numPagesPresent = ntagPagesPresent;
            nfcClassifyTag(type, atqa, sak, g_nfcExtCacheWriteFormat, g_nfcExtCacheFormatLabel,
                           g_nfcExtCacheCapacityBytes, ntagPages, ntagPagesPresent);

            g_nfcExtCacheUid = uidHex;
            g_nfcExtCacheData = SpoolTagData();
            g_nfcExtCacheHasExtended = false;
            g_nfcExtCacheOccupancy = "";
            bool dummyHasExt; PN5180TagType dummyType; long extId; String dummyUid, dummyText;
            String nfcvFormatFound;
            int nfcvCapacityFound = -1;
            if (pn5180ReadSpoolExOpt(dummyType, extId, dummyUid, dummyText, g_nfcExtCacheData, dummyHasExt,
                                  &nfcvFormatFound, &nfcvCapacityFound)) {
              g_nfcExtCacheHasExtended = dummyHasExt;
              // Extended formats carry their own databaseId (Mifare sector 2, NFC-V
              // blocks 3-4, or OpenSpool's os_db_id) -- prefer it over the legacy-area
              // id when present, since the legacy area may be stale or overwritten.
              // OpenPrintTag carries none at all (extId stays untouched by optRead),
              // so this correctly leaves id/idText alone for those tags -- the flow
              // falls back to a UID lookup, same as OpenSpool tags with no os_db_id.
              if (dummyHasExt && extId >= 0) { id = extId; idText = String(extId); }
              // NFC-V has three possible Extended formats (nfcClassifyTag only knows
              // the tag TYPE, not which one is actually on it) -- override with what
              // the read actually found.
              if (dummyHasExt && type == PN5180_TAG_NFCV && nfcvFormatFound.length()) {
                g_nfcExtCacheWriteFormat = nfcvFormatFound;
                g_nfcExtCacheFormatLabel = (nfcvFormatFound == "nfcvOpenSpool")
                  ? "OpenSpool"
                  : (nfcvFormatFound == "nfcvOpenPrintTag")
                  ? "OpenPrintTag"
                  : "Extended";
                // nfcvOpenSpool's real budget differs from nfcClassifyTag()'s
                // nfcvExtended-sized default -- show the tag's actual NDEF capacity.
                if (nfcvFormatFound == "nfcvOpenSpool" && nfcvCapacityFound >= 0)
                  g_nfcExtCacheCapacityBytes = nfcvCapacityFound;
              } else if (dummyHasExt && type == PN5180_TAG_NFCA &&
                         (nfcvFormatFound == "ntagExtended" || nfcvFormatFound == "tigerTag")) {
                // Same override, NTAG side: nfcClassifyTag() always predicts openSpool
                // (its only default before v3.1) -- the actual read confirmed
                // ntagExtended/tigerTag, so reflect that instead of the guess.
                g_nfcExtCacheWriteFormat = nfcvFormatFound;
                g_nfcExtCacheFormatLabel = (nfcvFormatFound == "tigerTag") ? "TigerTag" : "Extended";
              }
            }
            // Blank vs. foreign, for an unparseable tag -- see g_nfcExtCacheOccupancy.
            // Both NFC-A families are covered here: Mifare Classic via its key probe,
            // NTAG/Ultralight via the Capability Container. NTAG used to be skipped on
            // this path (pn5180ReadSpool never reads the CC), which left occupancy blank
            // for every NTAG outside debug mode -- the overwrite protection then did not
            // apply to foreign NTAG tags at all. Both need the card selected, which the
            // reads above have since dropped, so re-select first.
            if (!g_nfcExtCacheHasExtended && type == PN5180_TAG_NFCA) {
              bool isClassic = (sak == 0x08 || sak == 0x18 || sak == 0x09 || sak == 0x28);
              PN5180ProbeResult occSel;
              if (pn5180ProbeNfcA(occSel) && occSel.uid == uidHex) {
                g_nfcExtCacheOccupancy = isClassic ? pn5180MifareOccupancy(uidHex)
                                                   : pn5180NtagOccupancy();
              }
              g_pn5180->reset(); g_pn5180->setupRF();  // back to normal ISO15693 polling
            }
            // NFC-V third carrier. No re-select needed (ISO15693 reads are UID-addressed,
            // not session-bound like Crypto1/NFC-A), so this is two block reads and no RF
            // mode switch. Runs inside the uidHex != g_lastUid branch, i.e. once per tag
            // placement -- a tag left on the reader costs nothing further.
            else if (!g_nfcExtCacheHasExtended && type == PN5180_TAG_NFCV) {
              uint8_t uidRaw[8];
              if (pn5180UidHexToBytes8Reversed(uidHex, uidRaw))
                g_nfcExtCacheOccupancy = pn5180NfcvOccupancy(uidRaw);
            }
            dbgLogf("NFC extended-read: uid=%s hasExtended=%d format=%s occupancy=%s",
                    uidHex.c_str(), g_nfcExtCacheHasExtended, g_nfcExtCacheWriteFormat.c_str(),
                    g_nfcExtCacheOccupancy.c_str());
            if (g_nfcExtCacheHasExtended) {
              const SpoolTagData &e = g_nfcExtCacheData;
              dbgLogf("NFC extended fields: dbId=%ld material=%s vendor=%s colorName=%s color=%s",
                      e.databaseId, e.material.c_str(), e.vendor.c_str(), e.colorName.c_str(), e.color.c_str());
              dbgLogf("NFC extended fields: diameter=%.3f density=%.3f totalW=%.1f spoolW=%.1f usedW=%.1f",
                      e.diameter, e.density, e.totalWeight, e.spoolWeight, e.usedWeight);
              dbgLogf("NFC extended fields: temp=%d bedTemp=%d enclTemp=%d",
                      e.temperature, e.bedTemperature, e.enclosureTemperature);
            }

            g_lastTagData = idText;
            g_lastSpoolId = id;
            buzzerRead();  // short beep + synced cyan LED pulse: NFC tag detected
            Serial.printf("PN5180 tag: %s (%s) -> id=%ld\n",
                          uidHex.c_str(), pn5180TagTypeName(type), id);
            dbgLogf("NFC read: %s type=%s id=%ld text=%s", uidHex.c_str(),
                    pn5180TagTypeName(type), id, idText.c_str());
            g_nfcProbe.idParsed = id; g_nfcProbe.idText = idText;
            flowOnTagPresent(id, uidHex);
          }
          // A trigger parked by the post-write suppression window: retry it once the
          // window has closed and the tag is still the same one. Without this the flow
          // keeps showing the pre-write state until the tag is physically re-placed.
          if (g_flowRetryUid.length() && g_flowRetryUid == uidHex &&
              millis() >= g_nfcWriteSuppressUntil) {
            long retryId = g_flowRetryId;
            g_flowRetryUid = ""; g_flowRetryId = -1;
            dbgLogf("Flow: retrying trigger for %s (post-write window closed)", uidHex.c_str());
            flowOnTagPresent(retryId, uidHex);
          }
        } else {
          if (g_pn5180Present) { Serial.println("PN5180 tag: removed"); dbgLog("NFC read: tag removed"); }
          g_pn5180Present = false;
          g_pn5180Uid = "";
          g_nfcExtCacheUid = "";  // tag gone -> cache invalid for whatever comes next
          g_nfcProbe = PN5180ProbeResult();  // reset the short type readout
          g_lastUid = "";
          g_lastTagData = "";
          flowOnTagGone(s3GoneStreak);
        }
      }
    }

    // 4) Backlight timeout (screensaver). Activity -> displayTouch():
    //    - encoder (position/click counters change)
    //    - a real weight change (above a threshold, not HX711 noise)
    //    - an NFC tag present
    //    (Web UI accesses call displayTouch() directly in their handlers.)
    {
      static long   blLastEncPos = 0;
      static uint32_t blLastPush = 0, blLastStart = 0;
      static float  blLastWeight = 0;
      static uint8_t blCurLevel = 255;

      long ep = encoderPosition();
      uint32_t pc = encoderPushCount(), sc = encoderStartCount();
      if (ep != blLastEncPos || pc != blLastPush || sc != blLastStart) {
        blLastEncPos = ep; blLastPush = pc; blLastStart = sc;
        displayTouch();
      }
      float wt = g_weight;
      if (fabsf(wt - blLastWeight) > 3.0f) {   // >3g = a real change, not noise
        blLastWeight = wt;
        displayTouch();
      }
      if (g_pn5180Present) displayTouch();

      // Determine the target brightness: active, or dimmed after the timeout (0 = never).
      // OTA in progress -> stay at full brightness (the update screen must stay
      // readable) and keep resetting the activity clock so dimming doesn't kick in
      // the moment the update finishes either. Same for NFC debug: it's used to read
      // small print (UID, capacity, format) off the screen while actively working with
      // a tag, and dimming mid-read (no encoder/weight activity while just watching the
      // screen) defeats the point -- stays full-bright for as long as debug mode is on,
      // not just while a tag happens to be present (that's g_nfcDebug alone, not
      // g_nfcDebug && tag-present -- the empty-reader "no tag" screen needs to be just
      // as readable while deciding where to place one).
      uint8_t target;
      if (g_otaInProgress || g_nfcDebug) {
        target = g_blActive;
        displayTouch();
      } else if (g_blTimeoutSec > 0 &&
                 millis() - g_blActivity >= (unsigned long)g_blTimeoutSec * 1000UL) {
        target = g_blDim;
      } else {
        target = g_blActive;
      }

      // Third idle stage: panel fully off. Checked against the same g_blActivity
      // clock as dimming and the screensaver, just with a longer timeout, so the
      // three stages cascade naturally (dim -> logo -> off) as idle time grows.
      // Suppressed entirely during OTA / NFC debug via the branch above, which keeps
      // touching the activity clock -- those two need a readable screen throughout.
      bool wantOff = g_offEnabled && g_offTimeoutSec > 0 &&
                     !(g_otaInProgress || g_nfcDebug) &&
                     millis() - g_blActivity >= (unsigned long)g_offTimeoutSec * 1000UL;
      if (wantOff) {
        if (!g_tftAsleep) {
          dbgLog("Display: off (idle timeout)");
          displaySleep();
          blCurLevel = 0;   // so the wake below always re-applies a real level
        }
      } else if (g_tftAsleep) {
        // Waking costs ~140ms of blocking delay inside displayWake() (ST7789 sleep-out
        // timing). That is fine here: it happens once per wake, not per tick, and this
        // task's other duties (NFC poll, menu, backlight) tolerate a single skipped
        // 10ms slot -- the scale runs in its own task and is unaffected.
        displayWake(target);
        blCurLevel = target;
        g_menuForceRedraw = true;   // repaint rather than reveal the stale pre-sleep frame
        dbgLog("Display: on (activity)");
      }

      if (!g_tftAsleep && target != blCurLevel) {
        blCurLevel = target;
        displaySetBacklight(target);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));  // 100 Hz is enough for encoder debounce + the menu
  }
}

void loop() {
  // Only HTTP/OTA in loop() -> never blocked by sensors. HX711 (scaleTask) and
  // PN5180 (pn5180Task) run in their own tasks on core 0, loop/WiFi on core 1.
  //
  // Rough CPU load approximation (loop task): measures how much time per second goes
  // into actual work (handleClient/DB/OTA) vs. the trailing vTaskDelay(2ms).
  // busy / (busy + idle) ~ load. No menuconfig needed, but it's only an approximation
  // (not the only activity on core 1 - the WiFi stack runs alongside).
  unsigned long t0 = micros();
  ArduinoOTA.handle();
  server.handleClient();
  buzzerTick();   // advance the buzzer sequence from the loop context too
  // The DB check runs here (loop task), not in pn5180Task: the SpoolManagerExtended HTTP
  // bridge call blocks briefly and shouldn't hold up the reader task. Only on a new
  // tag with a valid ID.
  if (g_flowDbRequest) runDbCheck();
  // TFT menu delegation: run blocking flow transitions in the loop() task (core 1).
  if (g_flowMenuReq != FMA_NONE) {
    FlowMenuAction a = g_flowMenuReq;
    int arg = g_flowMenuArg;
    g_flowMenuReq = FMA_NONE;
    if (a == FMA_PRINTER) flowDoPrinter(arg);
    else if (a == FMA_TOOL) flowDoTool(arg);
    else if (a == FMA_WEIGH_SAVE) flowDoWeighSave();
  }

  // DB reachability ping moved to its own task (dbPingTask, core 0) -- see there for
  // why: octoToolCount()'s HTTPClient call can block for the full 3-4s connect+read
  // timeout when the printer is unreachable, and that used to run right here in
  // loop(), freezing server.handleClient() (and everything else in loop(), including
  // ledTick()) for the whole timeout every 60s.
  ledTick();  // update the WS2812 status LED (flashes -> back to idle)

  // PN5180 poll, debug probe, encoder + writing run in pn5180Task (core 0), NOT here:
  // the PN5180 library blocks (unbounded while-loops) and would otherwise freeze the
  // web server.

  unsigned long busy = micros() - t0;  // time spent working this iteration

  g_cpuBusyAccum += busy;  // average the load over a 1s window
  unsigned long now = millis();
  if (now - g_cpuWinStart >= 1000) {
    unsigned long win = (now - g_cpuWinStart) * 1000UL;  // window length in us
    g_cpuLoad = win ? (uint8_t)min<unsigned long>(100, (g_cpuBusyAccum * 100UL) / win) : 0;
    g_cpuBusyAccum = 0;

    // Dual-core load from the idle-counter deltas (rate/s against a calibrated baseline).
    static uint32_t lastIdle[2] = {0, 0};
    for (int c = 0; c < 2; c++) {
      uint32_t cur = g_idleCtr[c];
      uint32_t rate = (uint32_t)(((uint64_t)(cur - lastIdle[c]) * 1000UL) / (now - g_cpuWinStart));
      lastIdle[c] = cur;
      if (rate > g_idleBaseline[c]) g_idleBaseline[c] = rate;  // learn the idle maximum
      uint32_t load = g_idleBaseline[c] ? 100 - min<uint32_t>(100, (rate * 100UL) / g_idleBaseline[c]) : 0;
      g_coreLoad[c] = (uint8_t)load;
    }
    g_cpuWinStart = now;
  }

  vTaskDelay(pdMS_TO_TICKS(2));  // give the WiFi stack CPU time (counts as idle)
}
