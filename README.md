<p align="center">
  <img src="assets/octoscale_logo.jpeg" alt="OctoScale" width="320">
</p>

<p align="center">
  <b>NFC filament scale for OctoPrint + SpoolManagerExtended.</b><br>
  Put a spool on the scale, tap its tag — the spool loads into your printer or its
  remaining weight goes back into the database. No typing, no guessing.
</p>

---

> ### ⚠️ Requires SpoolManagerExtended
>
> OctoScale is built specifically against
> **[OctoPrint-SpoolManagerExtended](https://github.com/Ajimaru/OctoPrint-SpoolManagerExtended)**
> and depends on API endpoints that the original SpoolManager plugin does not provide.
>
> **Without it, the device still works — but only as a standalone tool:**
> weighing, writing spool numbers to NFC tags, reading tags, and erasing them.
> The whole point of the thing — tag → database lookup → load into printer, and weight
> written back automatically — needs SpoolManagerExtended on the OctoPrint side.

## What it does

OctoScale is an ESP32-S3 device that combines a **load cell**, an **NFC reader**, a
**TFT menu** and a **web UI** into one appliance for filament management:

1. **Place a tagged spool on the scale.**
2. The PN5180 reads the tag and looks the spool up in **SpoolManagerExtended**.
3. Choose what to do — on the built-in display or in the browser:
   - **Load into printer** → picks printer + tool, tells OctoPrint which spool is now mounted.
   - **Save weight** → weighs the spool live, writes the remaining weight back to the database.

It also *writes* tags: a whole spool record — material, vendor, color, diameter,
weights, temperatures — is encoded onto the tag itself, so the data travels with the
spool even when the database isn't reachable.

## Features

### **Scale**

- HX711 + 5 kg load cell, live readout on display and web UI
- 1-point (quick) or 2-point (linearity-checked) calibration wizard
- Calibration factor persists in NVS across reboots and OTA updates

### **NFC (PN5180)**

- Reads and writes three tag families: **NFC-A/NTAG + Ultralight**, **NFC-V/ISO 15693**,
  and **Mifare Classic 1K**
- Six on-tag payload formats, auto-detected on read:

  | Format | Tag type | Notes |
  | --- | --- | --- |
  | `octoscaleExtended` | Mifare Classic 1K | custom binary layout, CRC-8 commit marker |
  | `ntagExtended` | NTAG | same layout, NTAG page addressing |
  | `nfcvExtended` | NFC-V | same layout, NFC-V blocks |
  | `openSpool` / `nfcvOpenSpool` | NTAG / NFC-V | [OpenSpool](https://openspool.io) NDEF/JSON — readable by third-party apps |
  | `nfcvOpenPrintTag` | NFC-V | [OpenPrintTag](https://specs.openprinttag.org/) CBOR — widest field set, incl. drying data |
  | `tigerTag` | NTAG | [TigerTag](https://tigertag.io) Standard (unsigned) — layout per the [Python SDK](https://github.com/TigerTag-Project/TigerTag-SDK-Python), big-endian 80-byte payload |

- Foreign tags (e.g. a Snapmaker U1 tag) fall back to a **UID lookup** in the database
- Writes report exactly what was lost: `droppedFields` (didn't fit — a bigger tag helps)
  vs. `unsupportedFields` (this format has no such field at all)
- Raw sector/block dump for unknown tags

### **Interface**

- ST7789 320×240 TFT with an EC11 encoder menu that mirrors the entire spool flow
- Boot splash, idle screensaver, locked progress screen during OTA
- Web UI (dark/light) — weight, calibration, NFC, OctoPrint instances, system, WiFi
- Passive buzzer with event tones, WS2812 status LED
- Opt-in debug console streaming NFC reads/writes and HTTP calls to the browser

### **Connectivity**

- WiFiManager with AP fallback and captive portal
- OTA over `espota`, plus web OTA (upload a `.bin` or let the ESP pull one from a URL)
- Multiple OctoPrint instances; automatic **DB failover** between instances that share
  the same external database
- Config backup/restore as JSON — API keys AES-256-CBC encrypted, never exported in
  plaintext

## Hardware BOM

| Qty | Part | Notes |
| --- | --- | --- |
| 1 | **ESP32-S3-N16R8** dev board (YD-ESP32-S3 / DevKitC-1) | 16 MB flash, 8 MB **octal** PSRAM. The R8/OPI variant is required. |
| 1 | **HX711** ADC breakout | 24-bit load cell amplifier |
| 1 | **5 kg load cell** | straight bar type |
| 1 | **PN5180** NFC module | covers NFC-A *and* NFC-V in one chip — an NFC-A-only reader cannot do OpenPrintTag |
| 1 | **ST7789 TFT 320×240** + **EC11 encoder** + 2 buttons | the *S11-05* combo module carries all of it on one connector |
| 1 | **Passive buzzer** | driven by LEDC PWM |
| 1 | **470 µF electrolytic capacitor** | **mandatory**, see power notes |
| 1 | **5 V external power supply** | ≥ 1 A; USB alone is not enough |
| — | WS2812 RGB LED | already onboard the S3 (GPIO 48) |

## Wiring

Everything below is wired against an **ESP32-S3-N16R8**. Pin numbers are GPIO numbers as
printed on the board, and match the constants in `src/main.cpp` exactly.

### 1. Power first

This is the part that decides whether the build works at all, so do it before any signal
wiring.

```diagram
   5 V external PSU (>= 1 A)
        |
        +----------------+---------------- PN5180  #5V    ── 470 µF ──┐
        |                |                                            |
        |                +---------------- TFT     VCC                |
        |                                                             |
       GND --------------+----------------------------------------- GND
        |                |
        |                +---------------- ESP32-S3 GND   (shared ground, mandatory)
        |
   ESP32-S3  3V3 pin ----+---------------- HX711   VCC
                         |
                         +---------------- PN5180  +3.3V  (logic supply)
```

- **The 5 V rail comes from an external supply, not from the S3.** Leave the S3's own
  5 V pin unconnected. The board's 5 V pin is USB pass-through only and collapses under
  the PN5180's RF bursts.
- **The 3.3 V rail is the S3's internal regulator** (`3V3` pin). The HX711 and the
  PN5180's logic side are a small enough load to sit on it comfortably.
- **Tie the external supply's GND to the S3's GND.** Without a common ground there is no
  valid reference for any SPI signal and nothing communicates.
- **Put the 470 µF capacitor directly across the PN5180's `#5V` and `GND`**, physically
  at the module, not near the supply.

### 2. Load cell → HX711

The load cell's four wires go to the HX711's input side. Colours are the common
convention; verify against your cell's datasheet.

| Load cell wire | HX711 pad |
| --- | --- |
| red | `E+` |
| black | `E-` |
| white | `A-` |
| green | `A+` |

If weight readings run backwards (negative when loaded), swap **white** and **green**.
The `B` channel stays unused.

### 3. HX711 → ESP32-S3

| HX711 | GPIO |
| --- | --- |
| `VCC` | `3V3` |
| `GND` | `GND` |
| `DT` / `DOUT` | **5** |
| `SCK` | **6** |

### 4. PN5180 NFC reader → ESP32-S3

The PN5180 sits on its **own SPI bus (FSPI)** and must not share pins with the display.
The module is labelled `#5V +3.3V RST NSS MOSI MISO SCK BUSY GND GPIO IRQ AUX REQ`.

| PN5180 | GPIO | Role |
| --- | --- | --- |
| `#5V` | external 5 V | RF power (+ 470 µF here) |
| `+3.3V` | `3V3` | logic supply |
| `GND` | `GND` | |
| `SCK` | **12** | SPI clock |
| `MOSI` | **11** | SPI data out |
| `MISO` | **13** | SPI data in |
| `NSS` | **10** | chip select, active low |
| `BUSY` | **14** | status input |
| `RST` | **21** | reset, active low |
| `IRQ` | **47** | interrupt |

`GPIO`, `AUX` and `REQ` stay **unconnected**.

### 5. Display + encoder + buttons → ESP32-S3

The *S11-05* combo module carries the ST7789 display, the EC11 encoder and both buttons
on a single connector: `GND VCC SCL SDA RES DC CS BLK A B PUSH KO`. The display runs on
its **own SPI bus (HSPI)**, separate from the PN5180.

| Module pin | GPIO | Role |
| --- | --- | --- |
| `VCC` | external 5 V | |
| `GND` | `GND` | |
| `SCL` | **42** | display SPI clock |
| `SDA` | **44** | display SPI data (write-only, no MISO) |
| `CS` | **38** | display chip select |
| `DC` | **39** | data/command |
| `RES` | **40** | display reset |
| `BLK` | **41** | backlight, PWM-dimmed |
| `A` | **15** | encoder rotation |
| `B` | **16** | encoder rotation |
| `PUSH` | **17** | encoder click |
| `KO` | **18** | start button |

Encoder and buttons use the S3's **internal pull-ups** — their common pin goes to `GND`,
no external resistors needed. If rotation counts the wrong way, swap `A` and `B`.

Using discrete parts instead of the combo module works the same way: display pins to the
ST7789 breakout, `A`/`B`/`PUSH` to the EC11's three pins on one side, `KO` to a push
button, and every switch's other leg to `GND`.

### 6. Buzzer

| Buzzer | GPIO |
| --- | --- |
| `+` | **7** |
| `−` | `GND` |

A **passive** buzzer is expected — the firmware generates tones via LEDC PWM. An active
buzzer works but plays only its own fixed pitch.

> GPIO 2 is already taken by the status LED, which is why the buzzer sits on GPIO 7.

### 7. RGB status LED

Nothing to wire — the WS2812 is already onboard on **GPIO 48**.

### Complete pin summary

| GPIO | Connected to |
| --- | --- |
| 5 / 6 | HX711 `DT` / `SCK` |
| 7 | Buzzer `+` |
| 10 | PN5180 `NSS` |
| 11 / 12 / 13 | PN5180 `MOSI` / `SCK` / `MISO` (FSPI) |
| 14 | PN5180 `BUSY` |
| 15 / 16 / 17 | Encoder `A` / `B` / `PUSH` |
| 18 | Start button `KO` |
| 21 | PN5180 `RST` |
| 38 / 39 / 40 / 41 | TFT `CS` / `DC` / `RES` / `BLK` |
| 42 / 44 | TFT `SCL` / `SDA` (HSPI) |
| 47 | PN5180 `IRQ` |
| 48 | WS2812 (onboard) |

**Off-limits on the S3** — do not repurpose these: strapping pins 0/3/45/46, USB 19/20,
and 26–37 (reserved for the OPI flash and PSRAM).

> ### Three things that will cost you an evening if you skip them
>
> 1. **470 µF capacitor directly at the PN5180's `#5V`/GND.** The RF transmitter draws
>    current spikes that collapse the rail (measured down to ~0.8 V) — the chip still
>    answers over SPI, but produces no RF field at all. Symptom: `getInventory rc=-1`,
>    no tag ever reads. Without the capacitor the reader works only sporadically.
> 2. **Two separate SPI buses.** Sharing PN5180 and TFT on one bus kills the reader the
>    moment the display initialises. PN5180 = FSPI (12/11/13), TFT = HSPI (42/44).
> 3. **`ARDUINO_USB_CDC_ON_BOOT=0`.** This board bridges serial over a WCH CH343 on
>    UART0, not native USB-CDC. With CDC on boot the S3 hangs *before* `setup()` — no
>    output, no AP, no clue why.
>
> All three are already handled in `platformio.ini`; they matter when you deviate.

## Enclosure

`stl_files/` — currently the **top part** (`OcroScale_Top_V4.stl`). Base and load-cell
mount are still in progress; the printable set is not complete yet.

## Build & flash

Requires [PlatformIO](https://platformio.org/).

```bash
# first flash, over USB
pio run -e esp32s3 -t upload

# every flash after that, over the air
pio run -e esp32s3_ota -t upload --upload-port <device-ip>
```

Use the **IP address**, not `octoscale.local` — mDNS resolution adds enough latency to
make espota time out.

On first boot the device opens a WiFi access point named **`OctoScale-Setup`**. Connect
to it, enter your WiFi credentials, then reach the web UI at `http://<device-ip>/`.

## Software requirements

- **OctoPrint** with **[OctoPrint-SpoolManagerExtended](https://github.com/Ajimaru/OctoPrint-SpoolManagerExtended)**

This is a hard dependency for everything database-related, not a nice-to-have. OctoScale
talks to these endpoints, most of which exist only in the Extended plugin:

| Endpoint | Used for |
| --- | --- |
| `GET /plugin/SpoolManagerExtended/spool/<id>` | look a spool up by the id stored on the tag |
| `GET /plugin/SpoolManagerExtended/spool/byCode/<uid>` | look a foreign tag up by its UID |
| `PUT /plugin/SpoolManagerExtended/spool/<id>/measuredWeight` | write the weighed value back |
| `GET /plugin/SpoolManagerExtended/selectSpoolByQRCode/<id>?tool=<n>` | load a spool into a printer/tool |
| `GET /plugin/SpoolManagerExtended/databaseInfo` | identify the database behind an instance (used for failover) |

All calls are API-key protected. Database access goes exclusively through this HTTP
bridge — OctoScale never talks to MySQL directly (the Arduino MySQL library crashes
against MariaDB 11.x).

### Running without SpoolManagerExtended

The firmware does not refuse to start, and these parts remain fully usable on their own:

- weighing, taring and calibrating the scale
- writing a spool number onto an NFC tag (`/nfcwriteid`)
- writing a complete spool record onto a tag, if you supply the values yourself
  (`POST /nfcwritespool`)
- reading and inspecting tags, including raw dumps of unknown ones
- erasing tags

What stops working is the entire automatic flow: no lookup, no printer/tool selection,
no weight written back — the scale becomes a scale with an NFC reader attached.

## Project layout

| Path | Contents |
| --- | --- |
| `src/main.cpp` | WiFi/OTA, HX711, PN5180 task, flow state machine, HTTP endpoints |
| `src/pn5180nfc.h` | NFC read/write for all tag types and payload formats |
| `src/openprinttag.h`, `src/cbor.h` | OpenPrintTag support + a minimal CBOR codec |
| `src/menu.h`, `src/display.h`, `src/encoder.h` | TFT menu, ST7789 driver, EC11 input |
| `src/web_ui.h` | Web UI as a PROGMEM HTML string |
| `src/octoprint.h`, `src/spooldb.h` | OctoPrint instances, SpoolManagerExtended lookups, DB failover |
| `src/backup.h` | Encrypted config backup/restore |
| `lib/PN5180 Library/` | Vendored + patched PN5180 driver (see `lib/README-patch.md`) |

**Core split:** the HX711 and PN5180 tasks are pinned to **core 0**, WiFi/HTTP/OTA run on
**core 1**. The PN5180 library contains unbounded wait loops, so a reader hang must never
be able to freeze the web server.

## License

**GNU Lesser General Public License v2.1 or later** — see [LICENSE](LICENSE).

Copyright © 2026 Ajimaru

This choice is dictated by the dependencies rather than freely picked. OctoScale
**vendors and modifies** the PN5180 library (`lib/PN5180 Library/`, LGPL-2.1) — a BUSY-pin
timeout fix and a new `mifareAuthenticate()` implementation, documented in
[`lib/README-patch.md`](lib/README-patch.md). Distributing a modified LGPL work keeps it
under LGPL, so the project as a whole adopts the same license instead of layering a
different one on top.

| Component | License | Usage |
| --- | --- | --- |
| PN5180 Library — © 2018 Andreas Trappmann | LGPL-2.1 | vendored **and modified** |
| Adafruit NeoPixel | LGPL-3.0 | linked |
| WiFiManager, HX711, ArduinoJson, TFT_eSPI | MIT | linked |

The tag formats OctoScale implements are open specifications, independently implemented
from their published documentation — no code was copied from
[OpenSpool](https://openspool.io),
[OpenPrintTag](https://github.com/OpenPrintTag/openprinttag-specification) or the
[TigerTag Python SDK](https://github.com/TigerTag-Project/TigerTag-SDK-Python) (Apache-2.0),
whose `tigertag/tag.py` served as the byte-layout reference for the `tigerTag` format.
