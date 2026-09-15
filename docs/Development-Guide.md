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

`pn5180Task` iterates at roughly 100 Hz for encoder debounce and menu work. Only the tag poll is throttled, by `PN5180_POLL_INTERVAL_MS` in `src/main.cpp`. Write, erase, and read requests are picked up on every iteration, so they are unaffected by that interval. Express any duration in this task in real milliseconds rather than as a count of poll ticks: a tick count silently changes meaning whenever the interval moves, which is how error screens once stayed up for a different length of time than intended.

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

Two more electrolytic capacitors buffer the rest of the build:

| Position | Part | Buffers |
| --- | --- | --- |
| ESP32-S3 `5V` to `GND` | 1500 uF, 10 V+ | the board's own supply through WiFi transmit bursts |
| Mini-360 `OUT+` to `GND` | 100 uF, 16 V+ | the 3.3 V rail feeding HX711 and PN5180 logic |

Mount both with short leads directly at the pins they buffer; a few centimetres of wire undoes most of the benefit. Observe polarity.

Sizing for the 5 V one: a WiFi transmit burst is roughly 300 mA for about 2 ms, so holding it to a 0.2 V dip would take about 3000 uF. 1500 uF does not cover that fully but damps it hard, and is the sensible stopping point — more capacitance mainly buys inrush trouble. The 3.3 V capacitor stays small because that rail carries logic loads only; the bulk buffering belongs on 5 V, where the current is actually drawn.

The 3.3 V capacitor belongs at the **converter output**, not at the ESP32-S3's `3V3` pin. That pin is an output of the board's own regulator and feeds nothing in this build — HX711 and PN5180 logic hang on the Mini-360. A large capacitance on a regulator's output also does it no favours.

Ceramic 100 nF capacitors in parallel with each electrolytic are good practice against high-frequency noise, but they are not what prevents brownouts — the electrolytics handle the slow, large dips. The reference build runs without them.

Watch the supply path itself, not just the power supply's rating. Every connector, switch, and metre of thin cable between the supply and the board adds series resistance, and a chain of them can limit a burst badly enough to reset the device while a multimeter still reads a healthy idle voltage. If the device resets unpredictably, read `resetReason` from `/system` first: value 9 is a brownout and points at the supply path rather than at firmware. Prefer a short, thick, direct connection, and put any remote power switching on the mains side of the 5 V supply rather than in the low-voltage path.

## Input handling

The EC11 encoder uses quadrature interrupts on both rotation pins. PUSH and KO use interrupt-latched button events with debounce. This prevents a button press from being lost while the reader is inside a blocking NFC operation. Preserve this event-latching behavior when adding controls.

The encoder and buttons use internal pull-ups; the other side of each switch connects to ground. If rotation direction is reversed, swap the A and B signals.

## NFC development

`src/pn5180nfc.h` supports NFC-A/NTAG/Ultralight, NFC-V/ISO 15693, and Mifare Classic 1K. The PN5180 library is vendored under `lib/PN5180 Library/` and contains project patches; consult `lib/README-patch.md` before changing it.

The reader supports database-ID writes as well as extended spool payloads. Extended payload formats are selected by tag family and can include material, vendor, color, diameter, weights, and temperatures. NFC-V and NTAG formats may use either the project layout or OpenSpool-compatible NDEF where supported.

Keep NFC work asynchronous at the HTTP boundary: start operations with the existing start endpoints, poll their status endpoints, and avoid long reader calls in request handlers. Unknown tags can be inspected through the raw dump/image paths without changing the normal spool flow.

### Writing spool data: two silent traps

`/nfcwritespool` takes a JSON body and ignores unknown keys without error, so a misspelled field produces a successful write that is missing data. Two cases are worth calling out because neither reports a problem.

**The write format is chosen by `preferredNtagFormat` / `preferredNfcvFormat`, not by a `format` key.** There is no `format` parameter; passing one is silently ignored. For NTAG the accepted values are `openSpool` (the default), `extended`, and `tigerTag`; for NFC-V they are `extended` (the default), `openSpool`, and `openPrintTag`. Because the NTAG default is OpenSpool rather than the project layout, omitting the parameter converts an extended tag to OpenSpool, which stores a single colour and cannot represent transparent or rainbow spools. Assert `formatLabel` from a following `/nfcprobe` call when testing writes — it names the format actually on the tag.

**The temperature-range fields are spelled differently on read and write.** `/nfcprobe` returns `temperatureMin`, `temperatureMax`, `bedTemperatureMin`, and `bedTemperatureMax`; `/nfcwritespool` expects `minTemperature`, `maxTemperature`, `minBedTemperature`, and `maxBedTemperature`. Every other field keeps its name in both directions, which is exactly what makes these four easy to miss. Feeding a probe response straight back into a write therefore drops these values silently; build write payloads from your own model rather than from a probe result.

### Colour fields

A spool colour is expressed as a small grammar rather than a single hex value: `#RRGGBB`, up to three colours separated by `;`, an optional leading `transparent:` prefix, the bare word `transparent` for an untinted spool, or the bare word `rainbow`.

Composition is the exact inverse of parsing, so a `colorFull` read back and written again is unchanged:

| `isRainbow` | `isTransparent` | `colorCount` | `colorFull` |
| --- | --- | --- | --- |
| true | any | any | `rainbow` |
| false | true | 0 | `transparent` |
| false | true | 2 | `transparent:#000000;#FFFFFF` |
| false | false | 1 | `#000000` |
| false | false | 0 | no colour — see below |

`transparent` with `colorCount == 0` is a legitimate state, not a missing value: an untinted transparent spool has no primary colour to report. A composer must not emit `#000000` there — the zeroed primary slot is unset, not black.

The last row is the one case where the grammar says nothing about representation, only about state. This firmware always emits the key with an empty string; a consumer that distinguishes "absent" from "present but empty" — the OctoPrint plugin omits the key entirely — is equally correct. **Treat an empty `colorFull` and an absent one as the same thing.** Neither is a colour, and reading one as the negation of the other invents a distinction the grammar does not make.

A consumer falling back to another field must test for **absence**, not for falsyness:

```js
const c = colorFull != null ? colorFull : color;   // correct
const c = colorFull ? colorFull : color;           // wrong: "" falls through
```

Both spellings behaved identically while black was still dropped on read, because a black spool left every colour field empty. Since black reads back as `#000000`, they diverge: the falsy test sends a legitimately colourless spool to a fallback field that is also empty, and on formats where `color` is populated it can resurrect a stale value the grammar had deliberately cleared. This is a live trap for anything written against the older behaviour.

**`colorFull` is the field to rely on.** It carries the complete colour information for every tag format and can be written back verbatim. `extended.color` is deliberately narrower — the primary colour as plain `#RRGGBB`, and empty for `rainbow` or bare `transparent`, which have no primary colour. `colorCount`, `colors[]`, `isTransparent`, and `isRainbow` expose the parsed parts so consumers need not re-parse the grammar.

Black is a real colour, not a missing one: a black spool reads back as `#000000`. Three zero bytes mean black, never "unset" — the older reading, which treated `0,0,0` as absence, silently dropped every black spool.

The one exception is an extended tag written before its carrier gained the colour-flag byte. On those tags "black" and "no colour set" are genuinely the same bytes and cannot be told apart, so the field stays empty rather than guessing a colour that could overwrite a real one on import.

**The version counters run per carrier and do not line up.** Each carrier gained the flag byte at its own version number, so there is no single "from v4" rule — reading one carrier's threshold as if it applied to the others is the mistake this table exists to prevent:

| Carrier | Colour flags from | Current version | Read-path gate |
| --- | --- | --- | --- |
| Mifare Classic Extended | **v4** | v5 | `block8[2] >= 0x04` |
| NFC-V Extended | **v3** | v4 | `isV3nfcv` |
| NTAG Extended | **v2** | v3 | `fixedBuf[2] >= 0x02` |

The gate governs only this ambiguity, not the colour field as a whole. Four cases, which is the whole behaviour:

| Tag | Bytes | Result |
| --- | --- | --- |
| Below gate | non-zero | the colour — a pre-gate tag still reports its primary colour |
| Below gate | `0,0,0` | empty — black and unset are indistinguishable here |
| At/above gate, `colorCount >= 1` | `0,0,0` | `#000000` — the flag byte confirms a colour is set, so this is black |
| At/above gate, `colorCount == 0` | any | empty — the flag byte positively states "no colour" |

The last row looks like a bug and is not: above the gate, `colorCount == 0` is an assertion that no colour is set, not a gap in the data. It is also the state a bare `transparent` spool is in, which is why the primary bytes must not be written into slot 0 there.

Formats that encode absence explicitly have no such ambiguity and are never gated: OpenSpool and OpenPrintTag omit the field entirely when no colour is set (a missing JSON key / CBOR key 19), so zero bytes there are unambiguously black. TigerTag has no "not set" sentinel at all — `0,0,0` is black, and byte 19 is alpha, not a presence marker.

### Vendored PN5180 patches

The local PN5180 library contains four project changes documented in `lib/README-patch.md`:

1. BUSY-pin waits have a timeout instead of an unbounded loop. A non-conforming tag must produce a clean failure rather than hanging the reader task indefinitely.
2. `mifareAuthenticate()` implements the PN5180 `MIFARE_AUTHENTICATE` host command, which is required for Mifare Classic sector access.
3. The IRQ wait loops in `setRF_on()`, `setRF_off()`, and `reset()` have timeouts. Upstream spins on these without any bound, so an unresponsive chip starves the reader task on core 0 and trips the task watchdog — a reboot with no log at all when the device runs from external power with no serial host attached.
4. ISO 15693 command handling waits for the reception interrupt instead of a fixed `delay(10)`. The old fixed delay discarded answers from slower tags as "no card". The wait is split in two: first for the start of a frame, bounded tightly because that timeout is paid on every empty poll, then for reception to complete. Do not collapse these back into a single check — a write is only acknowledged after the tag's internal programming cycle, so the frame legitimately has not started yet when the command returns.

Recheck all four when updating the vendored library. NFC-A probing also requires CRC to be disabled for REQA/ATQA and anticollision frames. Seven-byte UIDs need the cascade-level anticollision sequence. Mifare Classic writes use a two-step command/data exchange, while NTAG page writes require a fresh RF reset and re-selection for each page. After NFC-A work, restore the reader with `reset()` and `setupRF()` before returning to normal NFC-V polling.

The legacy database ID is an ASCII decimal value stored in the tag's legacy area. Extended payload reads happen before flow lookup and take priority over the legacy ID. This ordering matters because an OpenSpool NTAG payload can overwrite the legacy pages and stores its ID as `os_db_id` in the NDEF/JSON data instead.

## Display, LED, and buzzer

The TFT uses TFT_eSPI configured through `platformio.ini` build flags. The backlight is controlled by LEDC PWM on GPIO 41; do not hand the backlight pin to TFT_eSPI in a way that lets display initialization override the PWM.

The onboard WS2812 on GPIO 48 and the external enclosure LED on GPIO 9 are mirrors. Drive both through the shared LED helper so color and timing stay synchronized. The passive buzzer uses LEDC PWM and shares event state with the LED pulse logic.

The LED base states are boot/WiFi connection, AP provisioning, connected/idle, database check, NFC result, and OTA progress. OTA has priority over normal animation; buzzer-linked colors have priority over the idle breathing effect. Keep all normal pixel writes behind the shared LED helper so the two pixels cannot drift apart or compete for the RMT peripheral. The idle breathing effect is deliberately low brightness. If colors are wrong, check whether the module is wired RGB rather than GRB before changing firmware behavior.

The TFT has four important takeover screens: the boot splash, the idle logo screensaver, the OTA lock screen, and the NFC-write screen. OTA suppresses backlight dimming and consumes input until the update finishes. NFC writes are blocking inside the reader task, so the write screen is intentionally static; a timer animation would not advance while the task is inside the write call. A completed write screen remains until the HTTP status result is consumed.

## Debug console and memory

The optional web debug console is an in-RAM ring buffer intended to replace serial monitoring when the device runs from external power. Logging is inert while disabled. Keep the buffer bounded and avoid serializing it when free heap is low: `/debuglog` should fail clearly rather than allocating a second large contiguous JSON string and taking down the web server. High-frequency traces, such as repeated successful Mifare authentication or unchanged weight readings, must be gated or rate-limited. Prefer chunked streaming if substantially more history is ever needed.

## WebUI polling and tab ownership

The WebUI is embedded in `src/web_ui.h`. Periodic status requests are owned by the tab that displays their data. The `currentTab` guard prevents hidden panels from polling, and the browser visibility guard stops all periodic requests while the page is hidden. Selecting a tab triggers one immediate refresh; the regular timer then continues only while that tab remains active.

| Tab | Endpoint | Interval |
| --- | --- | ---: |
| Operate | `/weight` | 500 ms |
| Operate | `/flow/status` | 800 ms |
| NFC | `/nfc5180` and `/flow/status` | 700 ms |
| Scale | `/scaleinfo` | 1000 ms |
| Setup | `/wifi/status` | 5000 ms |
| System | `/system` | 3000 ms |
| Debug | `/nfcdebug` | 700 ms |
| Debug diagnostics | `/nfc5180`, `/scaleinfo`, `/weight` | 1000 ms |

The Debug tab has two additional conditional loops. The menu preview polls `/menupreview` every 300 ms only while the preview is active. The debug console polls `/debuglog` every 500 ms only while debug logging is enabled. These loops stop when the Debug tab is left or the browser page is hidden.

The header status LEDs are the one deliberate exception to tab ownership: they poll `/system` and `/wifi/status` every 10 s regardless of the active tab, because they are visible from all of them. They are still gated on page visibility, and the visibility handler refreshes them on return so they do not show stale state.

Any self-rescheduling loop needs both guards, not just the visibility one. The preview auto-cycle in particular kept stepping the device's screens from a switched-away tab, and an active preview suppresses NFC polling on the device — so a forgotten auto-cycle silently stopped tag detection while the page looked idle. When adding a loop that reschedules itself with `setTimeout`, check `document.hidden` and `currentTab` on every iteration, and make sure the tab's `tabRefresh()` path restarts it.

The intervals are intentionally different: live weight and flow state need responsive updates on Operate, while WiFi and system metrics can use a lower rate. Avoid adding a global timer for a panel-specific endpoint. Add the endpoint to the owning tab's `tabRefresh()` path and wrap its periodic callback with `pv(callback, tabId)` so it cannot continue polling in the background.

The page also performs one-shot requests during startup and user actions, such as loading `/factor`, `/display`, `/led`, `/buzzer`, `/db/get`, and OctoPrint configuration. These are not periodic status polls and should remain tied to the relevant initialization or action unless a new live status requirement is introduced.

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
4. Verify the PN5180 has external 5 V, common ground, and the local 470 uF capacitor, and that the board's 5 V pin and the converter output are buffered.
5. For unexplained resets, read `resetReason` from `/system` before suspecting firmware: 9 is a brownout and points at the supply path, 6 is a task watchdog, 1 is a normal power-on.
6. Confirm PN5180 and TFT are on separate SPI buses.
7. Check the serial console for boot progress and WiFi/AP status.
8. Use the web UI diagnostics for HX711 readiness, raw value, noise, system metrics, and debug logging.
9. Test with the device IP before investigating hostname or mDNS delays.

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

When changing public behavior, update the relevant guide under `docs/` and keep endpoint names, task ownership, and NVS behavior documented. Do not add local machine paths, API keys, WiFi credentials, MAC addresses, or private test data to committed documentation.

## Documentation and wiki sync

`docs/` is the single source of truth. Every file there is a wiki page and the filename is the page name. The `wiki-sync` workflow copies `docs/*.md` to the GitHub wiki on every push to `main` that touches them, and can also be run manually from the Actions tab. **Edits made directly in the wiki are overwritten by the next sync** — change the file under `docs/` instead, or the change is lost the next time anyone edits documentation.

The wiki's `Home` page is not synced and has no counterpart in `docs/`. It is a short landing page with its own navigation and images, maintained in the wiki itself.

The workflow needs a repository secret named `WIKI_TOKEN` containing a personal access token with `repo` scope. GitHub's built-in `GITHUB_TOKEN` cannot write to a wiki, which is the only reason a separate token is required. Without the secret the workflow fails on its first step with an explicit message rather than silently doing nothing.

Anything that a consumer of the HTTP API can rely on belongs in this guide rather than only in a code comment. A promise that lives in the implementation holds only until someone rewrites the implementation; a documented one makes a later change the maintainer's problem instead of a downstream surprise. Any field the firmware accepts is part of the contract whether or not it is written down — the only choice is whether that contract is legible.

The documentation workflow ends here. Return to the [Features](Features) page for the project overview.
