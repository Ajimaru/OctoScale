# OctoScale Development Guide

This guide collects public development notes for the ESP32-S3-N16R8 target. It is derived from the project's hardware plan and intentionally excludes local machine paths, credentials, personal identifiers, network identifiers, and private test history.

## Target hardware

The supported reference board is an ESP32-S3-N16R8 development board:

- ESP32-S3, dual-core Xtensa LX7
- 16 MB flash
- 8 MB octal PSRAM (OPI)
- WiFi and BLE
- PlatformIO board: `esp32-s3-devkitc-1`
- Environments: `esp32s3` for USB and `esp32s3_ota` for OTA

The firmware does not use PSRAM directly, but the N16R8 memory configuration is required for the reference board to boot correctly. Other ESP32-S3 boards need matching flash and memory settings in `platformio.ini`.

## Build and flash

The first flash is performed over USB with the external power supply disconnected:

```bash
pio run -e esp32s3 -t upload
```

Regular updates use OTA:

```bash
pio run -e esp32s3_ota -t upload --upload-port <device-ip>
```

Use an IP address for OTA. Hostname resolution through mDNS can introduce long delays on some development hosts. Close the serial monitor before flashing; an open monitor can keep the serial port busy and prevent the bootloader handshake.

The reference board uses a WCH USB-UART bridge. The firmware is configured to use UART0 for the serial console rather than native USB-CDC. Keep the USB-CDC boot setting consistent with `platformio.ini` when changing board configurations.

## Boot and memory configuration

The ESP32-S3-N16R8 configuration requires:

- `board_build.arduino.memory_type = qio_opi`
- `-DBOARD_HAS_PSRAM`
- USB-CDC-on-boot disabled for the UART0 console configuration

Without the OPI memory setting, an N16R8 board may compile successfully but fail during boot. For boards without octal PSRAM, remove the PSRAM build flag and use the board's correct memory type. Boards with less than 8 MB flash need a custom partition layout and are not supported by the default OTA setup.

## Core split and ownership

The firmware deliberately separates blocking hardware work from networking:

- Core 0 runs `scaleTask` for the HX711 and `pn5180Task` for NFC polling, TFT menu work, and encoder input.
- Core 1 runs `loop()` with WiFi, HTTP, OTA, database checks, and flow delegation.
- Blocking NFC operations stay on core 0 because the PN5180 library contains unbounded wait loops.
- Flow actions that require blocking HTTP calls are passed to `loop()` through volatile state/flags.

A new hardware operation must not block the web server. Keep ownership of each peripheral within its task and communicate across cores with the existing state and flag pattern.

## Pin map

The reference pin assignment is collision-checked against ESP32-S3 strapping pins, USB pins, and OPI flash/PSRAM pins.

| Peripheral | Signal | GPIO / supply |
| --- | --- | --- |
| HX711 | `DOUT`, `SCK` | 5, 6; 3.3 V |
| PN5180 FSPI | `MOSI`, `SCK`, `MISO` | 11, 12, 13 |
| PN5180 | `NSS`, `BUSY`, `RST`, `IRQ` | 10, 14, 21, 47 |
| TFT HSPI | `SCL`, `SDA` | 42, 44 |
| TFT | `CS`, `DC`, `RES`, `BLK` | 38, 39, 40, 41 |
| Encoder | `A`, `B`, `PUSH` | 15, 16, 17 |
| Start button | `KO` | 18 |
| Passive buzzer | signal | 7 |
| Onboard WS2812 | data | 48 |
| External WS2812 | data | 9 |

Do not reuse GPIO 0, 3, 45, or 46 (strapping), GPIO 19 or 20 (USB), or GPIO 26-37 (OPI flash/PSRAM on the reference board). GPIO 44 is often labelled `RX` on the development board. GPIO 43 is the UART0 transmit pin and should remain reserved when serial monitoring is needed.

## SPI buses

The PN5180 and TFT must use separate SPI controllers:

- PN5180: FSPI, GPIO 11/12/13
- TFT: HSPI, GPIO 42/44

Sharing the buses can leave the PN5180 unable to read tags after TFT initialization. The TFT is write-only and therefore does not need MISO. Keep `USE_HSPI_PORT` enabled for the TFT_eSPI configuration.

## Power requirements

The external 5 V supply powers the PN5180 RF section and TFT. A regulated 3.3 V rail powers the HX711, PN5180 logic, and external WS2812. All modules must share ground.

The PN5180 needs a 470 uF capacitor directly across its 5 V and ground pins. RF current spikes can otherwise collapse the supply and appear as a software or SPI failure. Do not power the ESP32-S3 from USB while an external 5 V supply is connected to its 5 V/VBUS/VIN input.

## Input handling

The EC11 encoder uses quadrature interrupts on both rotation pins. PUSH and KO use interrupt-latched button events with debounce. This prevents a button press from being lost while the reader is inside a blocking NFC operation. Preserve this event-latching behavior when adding controls.

The encoder and buttons use internal pull-ups; the other side of each switch connects to ground. If rotation direction is reversed, swap the A and B signals.

## NFC development

`src/pn5180nfc.h` supports NFC-A/NTAG/Ultralight, NFC-V/ISO 15693, and Mifare Classic 1K. The PN5180 library is vendored under `lib/PN5180 Library/` and contains project patches; consult `lib/README-patch.md` before changing it.

The reader supports database-ID writes as well as extended spool payloads. Extended payload formats are selected by tag family and can include material, vendor, color, diameter, weights, and temperatures. NFC-V and NTAG formats may use either the project layout or OpenSpool-compatible NDEF where supported.

Keep NFC work asynchronous at the HTTP boundary: start operations with the existing start endpoints, poll their status endpoints, and avoid long reader calls in request handlers. Unknown tags can be inspected through the raw dump/image paths without changing the normal spool flow.

### Vendored PN5180 patches

The local PN5180 library contains two important project changes documented in `lib/README-patch.md`:

1. BUSY-pin waits have a timeout instead of an unbounded loop. A non-conforming tag must produce a clean failure rather than hanging the reader task indefinitely.
2. `mifareAuthenticate()` implements the PN5180 `MIFARE_AUTHENTICATE` host command, which is required for Mifare Classic sector access.

Recheck both changes when updating the vendored library. NFC-A probing also requires CRC to be disabled for REQA/ATQA and anticollision frames. Seven-byte UIDs need the cascade-level anticollision sequence. Mifare Classic writes use a two-step command/data exchange, while NTAG page writes require a fresh RF reset and re-selection for each page. After NFC-A work, restore the reader with `reset()` and `setupRF()` before returning to normal NFC-V polling.

The legacy database ID is an ASCII decimal value stored in the tag's legacy area. Extended payload reads happen before flow lookup and take priority over the legacy ID. This ordering matters because an OpenSpool NTAG payload can overwrite the legacy pages and stores its ID as `os_db_id` in the NDEF/JSON data instead.

## Display, LED, and buzzer

The TFT uses TFT_eSPI configured through `platformio.ini` build flags. The backlight is controlled by LEDC PWM on GPIO 41; do not hand the backlight pin to TFT_eSPI in a way that lets display initialization override the PWM.

The onboard WS2812 on GPIO 48 and the external enclosure LED on GPIO 9 are mirrors. Drive both through the shared LED helper so color and timing stay synchronized. The passive buzzer uses LEDC PWM and shares event state with the LED pulse logic.

The LED base states are boot/WiFi connection, AP provisioning, connected/idle, database check, NFC result, and OTA progress. OTA has priority over normal animation; buzzer-linked colors have priority over the idle breathing effect. Keep all normal pixel writes behind the shared LED helper so the two pixels cannot drift apart or compete for the RMT peripheral. The idle breathing effect is deliberately low brightness. If colors are wrong, check whether the module is wired RGB rather than GRB before changing firmware behavior.

The TFT has four important takeover screens: the boot splash, the idle logo screensaver, the OTA lock screen, and the NFC-write screen. OTA suppresses backlight dimming and consumes input until the update finishes. NFC writes are blocking inside the reader task, so the write screen is intentionally static; a timer animation would not advance while the task is inside the write call. A completed write screen remains until the HTTP status result is consumed.

## Debug console and memory

The optional web debug console is an in-RAM ring buffer intended to replace serial monitoring when the device runs from external power. Logging is inert while disabled. Keep the buffer bounded and avoid serializing it when free heap is low: `/debuglog` should fail clearly rather than allocating a second large contiguous JSON string and taking down the web server. High-frequency traces, such as repeated successful Mifare authentication or unchanged weight readings, must be gated or rate-limited. Prefer chunked streaming if substantially more history is ever needed.

## Spool flow architecture

The UI-independent flow is:

`idle -> db_check -> ask_action -> ask_printer -> ask_tool -> loading` or `weigh_confirm -> weighing -> done/error`

The reader task detects the tag and raises a flow request. The networking loop performs the blocking SpoolManager call. Database lookup uses the database ID when available and falls back to the tag UID for foreign tags. Loading fetches the printer's tool count before selecting a tool. Weighing uses the live gross reading and only offers a remaining-weight calculation when empty and total spool weights are known.

Tag removal keeps action/printer/tool selection open for the configured timeout. The weighing states are exempt because the spool can hide its tag while it remains on the scale. Keep this state-machine behavior independent of whether the request came from the TFT or web UI.

## Persistent configuration and backup

Preferences are stored under the `octoscale` namespace. Important keys include the calibration factor, OctoPrint instance JSON, selected database instance, selection timeout, display brightness/timeouts, screensaver settings, buzzer settings, and debug-console enablement. WiFiManager stores WiFi credentials separately.

Configuration backup is JSON. API keys are encrypted only when a passphrase is supplied, using PBKDF2-HMAC-SHA256 and AES-256-CBC with a random salt and IV. Without a passphrase, keys are omitted rather than exported in plaintext. Restore validates and decrypts the complete file before writing anything to NVS, so a bad password must leave the running configuration unchanged. The backup protects the file, not an unencrypted LAN transport.

## WiFi and OctoPrint integration

WiFiManager provides normal station setup, AP fallback, captive portal, credential storage, reconnect attempts, and WiFi scanning. Keep connection retries and timeouts long enough for slow routers before starting the fallback AP.

OctoPrint communication goes through the SpoolManagerExtended HTTP bridge. API keys stay in NVS and are never written to development documentation. The device supports multiple OctoPrint instances and can fail over between instances that report the same external database identity. It does not connect directly to MySQL.

## Debugging checklist

1. Confirm the selected PlatformIO environment matches the board memory configuration.
2. Disconnect external power before USB flashing.
3. Close serial monitors before uploading.
4. Verify the PN5180 has external 5 V, common ground, and the local 470 uF capacitor.
5. Confirm PN5180 and TFT are on separate SPI buses.
6. Check the serial console for boot progress and WiFi/AP status.
7. Use the web UI diagnostics for HX711 readiness, raw value, noise, system metrics, and debug logging.
8. Test with the device IP before investigating hostname or mDNS delays.

## Code structure

| Path | Development responsibility |
| --- | --- |
| `src/main.cpp` | Tasks, WiFi/OTA, HTTP endpoints, flow state, peripheral setup |
| `src/pn5180nfc.h` | NFC detection, read/write operations, payload formats |
| `src/display.h` and `src/menu.h` | TFT initialization, menu, screensaver, OTA screen |
| `src/encoder.h` | Encoder ISR and debounced button events |
| `src/web_ui.h` | Embedded HTML, CSS, and JavaScript served from PROGMEM |
| `src/octoprint.h` and `src/spooldb.h` | OctoPrint instances, bridge calls, database failover |
| `src/backup.h` | Encrypted configuration backup and restore |
| `src/dbglog.h` | Optional in-RAM debug ring buffer |
| `platformio.ini` | Board, memory, pin, library, and build configuration |
| `lib/PN5180 Library/` | Vendored and patched PN5180 driver |

When changing public behavior, update the relevant Wiki guide and keep endpoint names, task ownership, and NVS behavior documented. Do not add local machine paths, API keys, WiFi credentials, MAC addresses, or private test data to committed documentation.

The documentation workflow ends here. Return to the [Features](https://github.com/Ajimaru/OctoScale/wiki/Features) page for the project overview.
