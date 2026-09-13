# OctoScale Project Guide

NFC filament scale for OctoPrint and SpoolManagerExtended. This page contains the hardware, assembly, firmware, setup, architecture, and contribution documentation formerly kept in the project README.

<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/OctoScale.jpg" width="380" alt="Assembled OctoScale with a spool on the scale">

## Features

### Scale

- HX711 and 5 kg load cell with live readout on the TFT and web UI.
- One-point quick calibration and two-point linearity-checked calibration.
- Calibration factor stored in NVS and retained across reboots and OTA updates.

### NFC

The PN5180 reads and writes NFC-A/NTAG/Ultralight, NFC-V/ISO 15693, and Mifare Classic 1K tags. Supported payload formats are auto-detected:

| Format | Tag | Notes |
| --- | --- | --- |
| `octoscaleExtended` | Mifare Classic 1K | Custom binary layout with CRC-8 commit marker |
| `ntagExtended` | NTAG | Same layout with NTAG page addressing |
| `nfcvExtended` | NFC-V | Same layout with NFC-V blocks |
| `openSpool` / `nfcvOpenSpool` | NTAG / NFC-V | OpenSpool NDEF/JSON, readable by third-party apps |
| `nfcvOpenPrintTag` | NFC-V | OpenPrintTag CBOR, including drying data |
| `tigerTag` | NTAG | TigerTag Standard, unsigned, big-endian 80-byte payload |

Foreign tags such as Snapmaker U1 tags fall back to a UID lookup in SpoolManagerExtended. NFC writes report `droppedFields` when fields do not fit and `unsupportedFields` when the selected format has no representation. Unknown tags can be inspected with a raw sector/block dump.

### Interface and connectivity

- ST7789 320x240 TFT with EC11 encoder menu mirroring the spool workflow.
- Boot splash, locked OTA progress screen, and independent light/dark display theme.
- Three idle stages: dim, bouncing-logo screensaver, and display/backlight off.
- Hidden device test menu: hold PUSH for 3 seconds on the System info screen.
- Web UI themes: OctoScale and OctoPrint, each with light/dark mode.
- Web UI covers weighing, calibration, NFC, OctoPrint instances, system, WiFi, and test points.
- Passive buzzer with event tones and mirrored onboard/external WS2812 status LEDs.
- Optional browser debug console for NFC and HTTP activity.
- WiFiManager provisioning with AP fallback and captive portal.
- OTA through `espota` or web upload/URL update.
- Multiple OctoPrint instances with database failover when instances share an external database.
- AES-256-CBC encrypted JSON configuration backup; API keys are never exported in plaintext.

## Hardware BOM

<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/hardware.jpg" width="830" alt="All OctoScale hardware components laid out">

| Qty | Part | Notes |
| --- | --- | --- |
| 1 | ESP32-S3-N16R8 (YD-ESP32-S3 / DevKitC-1) | 16 MB flash, 8 MB octal PSRAM; R8/OPI variant is the reference board |
| 1 | HX711 ADC breakout | 24-bit load-cell amplifier |
| 1 | 5 kg load cell | Straight bar type with acrylic mounting plates |
| 1 | PN5180 NFC module | Supports NFC-A and NFC-V/OpenPrintTag |
| 1 | ST7789 TFT 320x240 with EC11 encoder and 2 buttons | S11-05 combo module |
| 1 | Passive buzzer | Driven by LEDC PWM |
| 1 | 470 uF electrolytic capacitor | Mandatory; install at the PN5180 5 V input |
| 1 | USB-C power supply | 5 V, at least 1.5 A |
| 1 | USB-C breakout board | Main 5 V entry point |
| 1 | Mini-360 buck converter | Adjust output to 3.3 V before connecting loads |
| 1 | WS2812 RGB LED module | External enclosure status LED |
| 10 | ST2.9 self-tapping screws, about 9.7 mm | Mount printed parts; no heat-set inserts |
| 2 | ST2.2 self-tapping screws, about 6 mm | Mount the USB-C breakout |

Reference photos are available in `assets/` (`hardware.jpg`, `ESP32-S3-N16R8.jpg`, and the component photos). The printed parts are in `stl_files/`.

### Wiring harness

Use female-to-female (F-F) wires for headers, F-cut wires for solder pads, and plain wire where both ends are soldered.

| Qty | Type | Length | Connection |
| --- | --- | --- | --- |
| 7 | F-F | about 22 cm | PN5180 `SCK`, `MOSI`, `MISO`, `NSS`, `BUSY`, `RST`, `IRQ` to ESP32-S3 |
| 3 | F-cut | about 22 cm | PN5180 power to USB-C breakout/Mini-360 and ground |
| 2 | F-cut | about 12 cm | ESP32-S3 `5V` and `GND` to USB-C breakout |
| 2 | F-cut | about 12 cm | HX711 `VCC` and `GND` to the 3.3 V rail |
| 2 | F-cut | about 12 cm | TFT `VCC` and `GND` to the 5 V rail |
| 10 | F-F | about 12 cm | TFT `SCL`, `SDA`, `RES`, `DC`, `CS`, `BLK`, `A`, `B`, `PUSH`, `KO` |
| 2 | F-F | about 12 cm | Buzzer to ESP32-S3 |
| 2 | F-cut | about 12 cm | WS2812 `VCC` and `GND` to the 3.3 V rail |
| 1 | F-cut | about 12 cm | WS2812 `DIN` to GPIO 9 |
| 2 | Plain wire | about 3 cm | USB-C breakout to Mini-360 `IN+`/`IN-`; solder both ends |

The PN5180 uses the long wires because it sits below the spool. Other components stay near the controller.

### Board variants

<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/ESP32-S3-N16R8.jpg" width="420" alt="ESP32-S3-N16R8 development board">

The reference is an ESP32-S3-N16R8: 16 MB flash, 8 MB octal PSRAM. The firmware needs an ESP32-S3 and at least 8 MB flash for two OTA app slots. PSRAM is not used.

For an 8 MB board, set both `board_upload.flash_size` and `board_build.flash_size` to `8MB` in `platformio.ini`. For boards without octal PSRAM, remove `-DBOARD_HAS_PSRAM` and `board_build.arduino.memory_type = qio_opi`. Four MB flash is not supported without a custom partition table.

## Wiring

All pin numbers below are GPIO numbers and match `src/main.cpp`. The PN5180 and TFT use separate SPI buses.

### Power

<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/USB-C_breakout_board.jpg" width="300" alt="USB-C breakout board"> <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/Mini-360_buck_converter.jpg" width="300" alt="Mini-360 buck converter">

Power enters through the USB-C breakout. The 5 V rail powers the ESP32-S3, PN5180 RF section, and TFT. The Mini-360 converts 5 V to 3.3 V for the HX711, PN5180 logic, and external WS2812. Everything shares one ground.

Before connecting any load, feed the Mini-360 with 5 V and adjust `OUT+` to exactly 3.3 V with a multimeter. Connect the 470 uF capacitor directly across PN5180 `#5V` and `GND`.

Never power the ESP32-S3 from USB while external 5 V is connected to its `5V`/`VBUS`/`VIN` pin. Disconnect the external supply for the first USB flash and any later recovery flash.

### Signal wiring

<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/HX711_5kg_load_cell_acrylic_mounting_plates_combo.jpg" width="420" alt="5 kg load cell with HX711 breakout and acrylic mounting plates">

| Component | Pin | ESP32-S3 / rail |
| --- | --- | --- |
| Load cell | red / black / white / green | HX711 `E+` / `E-` / `A-` / `A+` |
| HX711 | `VCC`, `GND`, `DT`, `SCK` | 3.3 V, GND, GPIO 5, GPIO 6 |
| PN5180 | `+5V`, `+3.3V`, `GND` | 5 V, 3.3 V, GND |
| PN5180 | `SCK`, `MOSI`, `MISO`, `NSS` | GPIO 12, 11, 13, 10 |
| PN5180 | `BUSY`, `RST`, `IRQ` | GPIO 14, 21, 47 |
| TFT/encoder module | `VCC`, `GND` | 5 V, GND |
| TFT | `SCL`, `SDA`, `CS`, `DC`, `RES`, `BLK` | GPIO 42, 44, 38, 39, 40, 41 |
| Encoder/buttons | `A`, `B`, `PUSH`, `KO` | GPIO 15, 16, 17, 18; other switch legs to GND |
| Buzzer | `+`, `-` | GPIO 7, GND |
| External WS2812 | `DIN`, `VCC`, `GND` | GPIO 9, 3.3 V, GND |

<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/PN5180_NFC_module.jpg" width="420" alt="PN5180 NFC module">
<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/ST7789_TFT_320x240_EC11_encoder_2buttons.jpg" width="420" alt="S11-05 module with ST7789 display, EC11 encoder, and two buttons">
<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/buzzer.jpg" width="420" alt="Passive buzzer">
<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/WS2812.jpg" width="420" alt="WS2812 RGB LED module">

GPIO 44 is often labelled `RX`; it is still GPIO 44. If encoder rotation is reversed, swap `A` and `B`. If weight becomes negative when loaded, swap the load-cell white and green wires. PN5180 `GPIO`, `AUX`, and `REQ` remain unconnected.

Complete summary: GPIO 5/6 HX711; 7 buzzer; 9 external WS2812; 10 PN5180 NSS; 11/12/13 PN5180 MOSI/SCK/MISO; 14 PN5180 BUSY; 15/16/17 encoder A/B/PUSH; 18 start button; 21 PN5180 RST; 38/39/40/41 TFT CS/DC/RES/BLK; 42/44 TFT SCL/SDA; 47 PN5180 IRQ; 48 onboard WS2812. Do not repurpose strapping pins 0/3/45/46, USB pins 19/20, or OPI pins 26-37. GPIO 8 remains free; GPIO 43 is the USB-serial TX pin.

## Enclosure

Printable parts are in `stl_files/`:

| File | Qty | Part | Settings |
| --- | --- | --- | --- |
| `OcroScale_Top.stl` | 1 | Top plate; spool rests here; PN5180 below | 0.2 mm layers, 0.25 mm first layer, 15% infill |
| `OctoScaleBottom.stl` | 1 | Base for load cell, ESP32-S3, and power | Same |
| `OctoScaleLit.stl` | 1 | Front lid for display and status LED | Same |
| `OctoScaleScreenBazel.stl` | 1 | Display bezel, 64 x 47 x 1.6 mm | Same |
| `OctoScaleKnobCover.stl` | 1 | EC11 knob cover, 13.5 mm diameter, 5.2 mm tall | Same |
| `OctoScaleButtonCover.stl` | 1 | Button cover, 13.5 mm diameter, 1.8 mm tall | Same |
| `OctoScaleSpacer.stl` | 10 | 5.5 x 5.5 x 3.5 mm screw spacer | Same |
| `OctoScaleLEDCover.stl` | 1 | 7 x 7 x 1.4 mm light window | Transparent filament, 99.99% infill |

Mounting order is printed part, board, spacer, screw. Component names are embossed on the printed parts. The external WS2812 has no dedicated seat: solder its wires, test it, then glue it inside `OctoScaleLit.stl` behind the front opening. Use transparent filament for `OctoScaleLEDCover.stl`; sparse infill makes the diffuser cloudy.

## Build & flash

Install [PlatformIO](https://platformio.org/). The patched PN5180 library is vendored in `lib/`; other dependencies are downloaded from `lib_deps` on the first build.

For the first flash, disconnect the external 5 V supply and connect USB:

```bash
pio run -e esp32s3 -t upload
```

On first boot, connect to the `OctoScale-Setup` access point, enter WiFi credentials, and note the assigned IP from the router or serial monitor:

```bash
pio device monitor -e esp32s3
```

The web UI is at `http://<device-ip>/`. Later firmware updates use OTA and do not need USB:

```bash
pio run -e esp32s3_ota -t upload --upload-port <device-ip>
```

Use the IP address for OTA rather than `octoscale.local` because mDNS can make `espota` time out.

## First-time setup

Open `http://<device-ip>/` and work through these steps in order.

1. **Calibrate.** Empty the scale and tare it. Use a known reference weight with either one-point calibration or two-point calibration using light and heavy references. Verify the result. The HX711 diagnostics panel shows chip readiness, raw ADC value, and noise; unstable noise usually indicates wiring or supply trouble.
2. **Add printers.** In Setup, add each OctoPrint instance with name, host, port, and API key from OctoPrint Settings > API.
3. **Choose the spool database.** Select the OctoPrint instance used for spool records. Instances sharing an external database can fail over; local SQLite instances cannot. Use the spool-ID lookup test.
4. **Review optional settings.** Configure selection timeout, display brightness and idle stages, LED brightness, buzzer mode/tones, and web/display themes.
5. **Test a spool.** Put a tagged spool on the scale. For a new tag, use NFC > Write ID to tag. The Debug tab can log reads and lookups.

### Software requirement

Database features require [OctoPrint-SpoolManagerExtended](https://github.com/Ajimaru/OctoPrint-SpoolManagerExtended). OctoScale uses its API endpoints for spool lookup by ID or UID, measured-weight updates, printer/tool loading, and database identification for failover. All calls use API keys; OctoScale never connects to MySQL directly.

Without the plugin, weighing, taring, calibration, reading, writing, and erasing NFC tags still work. Automatic lookup, printer/tool selection, and database weight updates do not.

## Project layout

| Path | Contents |
| --- | --- |
| `src/main.cpp` | WiFi/OTA, HX711, PN5180 task, flow state machine, HTTP endpoints |
| `src/pn5180nfc.h` | NFC read/write for tag types and payload formats |
| `src/openprinttag.h`, `src/cbor.h` | OpenPrintTag support and CBOR codec |
| `src/menu.h`, `src/display.h`, `src/encoder.h` | TFT menu, display driver, and input |
| `src/web_ui.h` | Web UI as a PROGMEM HTML string |
| `src/octoprint.h`, `src/spooldb.h` | OctoPrint instances, lookups, and DB failover |
| `src/backup.h` | Encrypted configuration backup/restore |
| `src/buzzer.h` | Event tones and buzzer modes |
| `src/dbglog.h` | In-RAM debug log ring buffer |
| `src/OctoFontMid.h`, `src/OctoFontBig.h` | Generated VLW fonts |
| `src/OctoLogo.h` | RGB565 logo bitmap |
| `src/version.h` | `FW_VERSION`, bump for releases |
| `tools/make_vlw.py` | Regenerates VLW font headers |
| `lib/PN5180 Library/` | Vendored and patched PN5180 driver |

The HX711 and PN5180 tasks run on core 0; WiFi, HTTP, and OTA run on core 1. This isolation prevents unbounded PN5180 wait loops from freezing the web server.

## Contributing

Bug reports and pull requests are welcome. Read [CONTRIBUTING.md](../CONTRIBUTING.md) for build requirements, core-split rules, and useful issue details. Security issues belong in [SECURITY.md](../SECURITY.md). Release changes are tracked in [CHANGELOG.md](../CHANGELOG.md).

## License

GNU Lesser General Public License v2.1 or later; see [LICENSE](../LICENSE). The PN5180 library in `lib/PN5180 Library/` is vendored and modified under LGPL-2.1. Adafruit NeoPixel is LGPL-3.0; WiFiManager, HX711, ArduinoJson, and TFT_eSPI are MIT. Tag formats are independently implemented from their published specifications.
