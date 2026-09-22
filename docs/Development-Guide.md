# OctoScale Development Guide

This guide collects public development notes for the ESP32-S3-N16R8 target.

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

### Dev build numbering

Every local build appends a running number to the version it reports — `0.0.3-dev7` rather than `0.0.3` — and copies its images into `artifacts/`. Flashing the same release version twice is otherwise indistinguishable on the display, in the web UI and over HTTP, which makes "which build is on the device?" unanswerable during a test session.

`tools/dev_build.py` runs as a pre-script for both environments. It increments a counter, passes the result in as `FW_DEV_BUILD`, and after linking writes `artifacts/octoscale-<version>-<env>.bin`, the identical `-ota.bin`, and the matching `.elf`. Keep the `.elf`: a backtrace from the serial monitor cannot be resolved once the build it came from has been overwritten.

The counter lives in `artifacts/.devcounter`, outside version control by way of `artifacts/` already being ignored. A number that changes on every build would otherwise make every test flash look like a source change. `src/version.h` keeps the release number in `FW_VERSION_RELEASE` and falls back to it when `FW_DEV_BUILD` is absent, so a clean checkout, a CI build or a release build reports a plain version with no dev suffix. Rename one of those macros and the script's pattern has to follow, or the version silently reads as `0.0.0`.

Check what actually shipped by reading the binary rather than the build log, which only shows what the script intended:

```bash
strings artifacts/octoscale-0.0.3-dev1-esp32s3.bin | grep -o "0\.0\.3-dev[0-9]*"
```

The dev number separates local builds from each other; it does not make released versions unambiguous. Twenty-one commits shipped between the 0.0.2 and 0.0.3 bumps, all reporting the same version. Treat `fwVersion` and `currentVersion` as diagnostics rather than as a behaviour switch, and do not branch on them: a version that stays constant across that many commits separates nothing, and a later bump marks only devices built after it, never the ones already in the field. Where a consumer needs to know whether a capability exists, test for the field itself — `idSource` is simply absent on firmware that predates it.

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

The 3.3 V capacitor belongs at the **converter output**, not at the ESP32-S3's `3V3` pin. That pin is an output of the board's own regulator and feeds nothing in this build — HX711 and PN5180 logic hang on the Mini-360. A large capacitance on a regulator's output also does it no favours.

If the device resets unpredictably, read `resetReason` from `/system` first: value 9 is a brownout and points at the supply path rather than at firmware. Prefer a short, thick, direct connection, and put any remote power switching on the mains side of the 5 V supply rather than in the low-voltage path.

## Input handling

The EC11 encoder uses quadrature interrupts on both rotation pins. PUSH and KO use interrupt-latched button events with debounce. This prevents a button press from being lost while the reader is inside a blocking NFC operation. Preserve this event-latching behavior when adding controls.

The encoder and buttons use internal pull-ups; the other side of each switch connects to ground. If rotation direction is reversed, swap the A and B signals.

## NFC development

`src/pn5180nfc.h` supports NFC-A/NTAG/Ultralight, NFC-V/ISO 15693, and Mifare Classic 1K. The PN5180 library is vendored under `lib/PN5180 Library/` and contains project patches; consult `lib/README-patch.md` before changing it.

The reader supports database-ID writes as well as extended spool payloads. Extended payload formats are selected by tag family and can include material, vendor, color, diameter, weights, and temperatures. NFC-V and NTAG formats may use either the project layout or OpenSpool-compatible NDEF where supported.

Keep NFC work asynchronous at the HTTP boundary: start operations with the existing start endpoints, poll their status endpoints, and avoid long reader calls in request handlers. The three single-shot status endpoints — `/nfcwritestatus`, `/nfcdumpstatus` and `/nfcreadstatus` — each hand out their result exactly once and clear it, so a second reader cannot tell "someone already collected it" from "nothing happened". Pass `?peek=1` to read without consuming (and, on the write status, without the LED flash that announces the outcome) when something other than the issuing client wants to follow along. Keep the three consistent: `peek` was first added to the write status alone, and the gap on the dump endpoint was found by an external consumer rather than here. A fourth status of this shape needs the same flag. Unknown tags can be inspected through the raw dump/image paths without changing the normal spool flow.

### Vendored PN5180 patches

The local PN5180 library contains four project changes documented in `lib/README-patch.md`:

1. BUSY-pin waits have a timeout instead of an unbounded loop. A non-conforming tag must produce a clean failure rather than hanging the reader task indefinitely.
2. `mifareAuthenticate()` implements the PN5180 `MIFARE_AUTHENTICATE` host command, which is required for Mifare Classic sector access.
3. The IRQ wait loops in `setRF_on()`, `setRF_off()`, and `reset()` have timeouts. Upstream spins on these without any bound, so an unresponsive chip starves the reader task on core 0 and trips the task watchdog — a reboot with no log at all when the device runs from external power with no serial host attached.
4. ISO 15693 command handling waits for the reception interrupt instead of a fixed `delay(10)`. The old fixed delay discarded answers from slower tags as "no card". The wait is split in two: first for the start of a frame, bounded tightly because that timeout is paid on every empty poll, then for reception to complete. Do not collapse these back into a single check — a write is only acknowledged after the tag's internal programming cycle, so the frame legitimately has not started yet when the command returns.

Recheck all four when updating the vendored library. NFC-A probing also requires CRC to be disabled for REQA/ATQA and anticollision frames. Seven-byte UIDs need the cascade-level anticollision sequence. Mifare Classic writes use a two-step command/data exchange, while NTAG page writes require a fresh RF reset and re-selection for each page. After NFC-A work, restore the reader with `reset()` and `setupRF()` before returning to normal NFC-V polling.

The legacy database ID is an ASCII decimal value stored in the tag's legacy area. Extended payload reads happen before flow lookup and take priority over the legacy ID. This ordering matters because an OpenSpool NTAG payload can overwrite the legacy pages and stores its ID as `os_db_id` in the NDEF/JSON data instead.

### Trusting a parsed database ID

The legacy layout is digits followed by space padding, with no magic number and no checksum. Nothing in it identifies the tag as ours. A third-party tag that happens to carry ASCII digits at the same offset — an inventory number, for instance — parses exactly like a tag this firmware wrote, and no amount of reading that field harder can tell the two apart.

The parser therefore only rejects shapes our own writers cannot produce: trailing content that is not padding, more than one run of digits, a leading zero, or more than ten digits. Everything surviving that is plausible, not proven.

It also requires the digit run to end inside the buffer it was given, with at least one padding byte visible. `pn5180ReadNfcvId` passes however many bytes it managed to read, so a block failing mid-field used to yield a prefix of the real ID — a five-digit ID read as four bytes returned a different, entirely plausible, wrong spool, with no error anywhere. The ten-digit ceiling is mirrored in the three ID writers; a wider value would be written and verified, then read back as `-1` for the rest of the tag's life.

Proof, where it exists, comes from a second source. `occupancy` reads the Capability Container and the start of the data area, so it knows whether the tag carries a format this firmware wrote; the ID parser reads a fixed offset and knows nothing about the CC. `/nfcprobe` reports the combined judgement as `idSource`:

| `idSource` | Meaning |
| --- | --- |
| `extended` | The ID arrived with a verified format magic (`OX`, `OS`, OpenSpool NDEF, TigerTag). As trustworthy as the format it came from. |
| `legacy` | The ID came from the header-less field and passed the shape check. Plausible, unverifiable. |
| `unverified` | A `legacy` ID on a tag whose `occupancy` is `foreign`. The data area belongs to a format this firmware did not write, so the digits are most likely someone else's numbering. |
| `extendedNoId` | A verified format that carries no database ID of ours at all — TigerTag and OpenPrintTag both describe a spool without referencing this database. |
| `""` | No ID was parsed. |

`extendedNoId` is deliberately not `""`. Both mean "no usable ID", but the tag is not blank: it holds structured data from a format this firmware can read. A consumer that collapses the two will offer to overwrite a TigerTag.

A tag that has just been placed on the reader is published in two steps: type and UID appear as soon as it answers, because the display needs them at once, while the ID and the format fields are only filled after the Extended read and the occupancy probes have run — a few hundred milliseconds on NTAG. `/nfcprobe` reports `complete: false` for that window. A consumer that polls continuously can ignore the flag and watch the fields settle; one that asks once and acts on the answer should require `present && complete`, or it may see an Extended tag as an empty one and offer to overwrite it. Firmware older than this field omits it, so treat a missing value as unknown rather than as false.

`idSource` is a reported judgement, not an input to the flow. The load flow reaches its own verdict from the raw values — `hasExtendedData` and `occupancy` — and `flowWouldUse` only consults `idSource` to exclude `unverified`. Keep it that way: the firmware decides on what it read, the consumer receives the summary. It also means the current formats' habit of leaving `idParsed` at `-1` whenever `extendedNoId` applies is a property of those formats, not a guarantee of the field. A future format with both a verified magic and a populated legacy area would break that assumption, so do not build logic on it without checking the callers again.

`idParsed` keeps the raw value even when `idSource` is `unverified`, because `/nfcprobe` is a diagnostic endpoint and "there are digits here, but they are not credible" is worth more to a caller than a bare `-1` that cannot be told apart from an empty field. The judgement, not the value, is what consumers should branch on. `flowWouldUse` and the load flow both refuse an `unverified` ID and fall back to the UID lookup.

Note that `occupancy` alone is not that judgement. A tag written through `/nfcwriteid` onto previously NDEF-formatted material reports `foreign` while carrying a perfectly real ID, and an extended tag can report `foreign` because `occupancy` only inspects the CC and the first user page. Branching on `occupancy` instead of `idSource` discards both. Firmware older than this field omits it entirely, so a consumer that needs to support both should treat a missing `idSource` as "unknown, ask the user" rather than inferring one from `occupancy`.

`writeFormat` and `formatLabel` answer a different question again: which format this firmware *would* write if asked right now. They are derived from the tag family alone and say nothing about what is currently on the tag — every non-Classic NFC-A tag reports `openSpool` whether or not it holds any NDEF at all. Use `hasExtendedData`, `occupancy`, and `idSource` to find out what a tag actually carries.

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

`/weight` answers `<grams>|<zeroState>|<deviation g>` as plain text rather than a bare number. The zero-point verdict rides along on that poll instead of being fetched separately so the Operate tab's reading and its warning can never disagree on screen; a consumer that only wants the weight takes the first field.

The Debug tab has two additional conditional loops. The menu preview polls `/menupreview` every 300 ms only while the preview is active. The debug console polls `/debuglog` every 500 ms only while debug logging is enabled. These loops stop when the Debug tab is left or the browser page is hidden.

The header status LEDs are the one deliberate exception to tab ownership: they poll `/system` and `/wifi/status` every 10 s regardless of the active tab, because they are visible from all of them. They are still gated on page visibility, and the visibility handler refreshes them on return so they do not show stale state.

Any self-rescheduling loop needs both guards, not just the visibility one. The preview auto-cycle in particular kept stepping the device's screens from a switched-away tab, and an active preview suppresses NFC polling on the device — so a forgotten auto-cycle silently stopped tag detection while the page looked idle. When adding a loop that reschedules itself with `setTimeout`, check `document.hidden` and `currentTab` on every iteration, and make sure the tab's `tabRefresh()` path restarts it.

The intervals are intentionally different: live weight and flow state need responsive updates on Operate, while WiFi and system metrics can use a lower rate. Avoid adding a global timer for a panel-specific endpoint. Add the endpoint to the owning tab's `tabRefresh()` path and wrap its periodic callback with `pv(callback, tabId)` so it cannot continue polling in the background.

The page also performs one-shot requests during startup and user actions, such as loading `/factor`, `/display`, `/led`, `/buzzer`, `/db/get`, and OctoPrint configuration. These are not periodic status polls and should remain tied to the relevant initialization or action unless a new live status requirement is introduced.

## Spool flow architecture

The UI-independent flow is:

`idle -> db_check -> ask_action -> ask_printer -> ask_tool -> loading` or `weigh_confirm -> weighing -> done/error`

The reader task detects the tag and raises a flow request. The networking loop performs the blocking SpoolManager call. Database lookup uses the database ID when one is available and credible, and falls back to the tag UID for foreign tags. An ID parsed out of the header-less legacy area of a tag whose `occupancy` is `foreign` is deliberately not used (see "Trusting a parsed database ID"); the flow treats such a tag as having no ID at all and says so on the display, rather than silently loading whichever spool happens to share that number. Loading fetches the printer's tool count before selecting a tool. Weighing uses the live gross reading and only offers a remaining-weight calculation when empty and total spool weights are known.

Tag removal keeps action/printer/tool selection open for the configured timeout. The weighing states are exempt because the spool can hide its tag while it remains on the scale. Keep this state-machine behavior independent of whether the request came from the TFT or web UI.

## Persistent configuration and backup

Preferences are stored under the `octoscale` namespace. Important keys include the calibration factor, the tare offset and the factor it was written under, OctoPrint instance JSON, selected database instance, selection timeout, display brightness/timeouts, screensaver settings, buzzer settings, and debug-console enablement. WiFiManager stores WiFi credentials separately.

Configuration backup is JSON. API keys are encrypted only when a passphrase is supplied, using PBKDF2-HMAC-SHA256 and AES-256-CBC with a random salt and IV. Without a passphrase, keys are omitted rather than exported in plaintext. Restore validates and decrypts the complete file before writing anything to NVS, so a bad password must leave the running configuration unchanged. The backup protects the file, not an unencrypted LAN transport.

Device settings travel in a `settings` object addressed by NVS key, driven by the `BK_SETTINGS` table in `backup.h`. Adding a setting means adding one row there; export and import both walk the same table, so they cannot drift apart. Each row records the width the value is written with, because Preferences returns 0 when a `uint8` key is read back as `uint16`. Export skips keys that were never written, and import skips keys the file does not contain — an old backup therefore leaves newer settings at the running firmware's defaults instead of resetting them, which is also why the format version stays at 1. The table goes through NVS rather than the matching globals because `g_menuDark` is file-static in `menu.h`, included long after `backup.h`.

`bkImport` only writes NVS. The `/restore` handler re-reads those keys into the globals afterwards and re-applies the theme and LED routing, since the running firmware holds them in RAM; without that, a restore appears to have been ignored until the next boot.

## Zero point and tare

The tare offset is persisted (`tareOff`), together with the calibration factor that was in effect when it was written (`tareOffFac`). Both are needed: an offset in ADC counts cannot be read as grams without its factor, so after a recalibration a gram threshold computed from the new factor against an offset written under the old one would be wrong. `applyCalFactor` therefore carries `tareOffFac` forward — without that line the stored offset would count as stale after every calibration.

`startScale` no longer tares unconditionally. It calls `scaleZeroEvaluate`, which reads a short quiet window and compares its mean against the stored offset. Within tolerance the offset is refreshed from the fresh reading, which keeps temperature drift from accumulating. Outside tolerance the stored offset wins and nothing is written — the scale then displays the load that is actually on it instead of zero.

The four states are reported as `zeroState` in `/scaleinfo`, with `zeroTrusted` as the field to branch on. `suspect` deliberately claims only that the reading deviates, never why: a single measurement cannot separate a load on the platform from cell drift or a rebuilt mechanism, so the firmware reports the observation and leaves the cause open. `unstable` means the boot window was too noisy to support any statement at all.

`scaleTask` also flips a trusted zero to `suspect` when the reading drops well below zero at runtime. That is the one case no boot check can see: boot with a load, load removed afterwards. The negative reading is still clamped for display, but the state is set before the clamp — the clamp used to destroy exactly this information.

Weighing is refused while the zero point is not trusted, at both entry points and again inside `flowDoWeighSave`, because that path writes to the database and then onto the tag, and neither has a way back. A manual tare is the only way out of `suspect` and `unstable`; it deliberately performs no empty check, since a permanently mounted holder legitimately belongs to the zero point and the firmware cannot tell it apart from a spool left on by accident.

Both surfaces report the same thing the same way: the reading takes the warning color and a line underneath names the deviation and the remedy, on the TFT idle screen and in the Operate tab's weight card. The warning color is per theme in both (`MENU_WARN_FG` on the display, `--warn-fg` in CSS) because one amber cannot serve both backgrounds — the tone that stands out on black washes out on white. It is deliberately not the error red, which means "broken" on the footer chips, whereas an unconfirmed zero point is not a fault.

Thresholds live in grams (`SCALE_ZERO_TOL_G`) and convert to counts at runtime, with a floor so they never fall under the cell's own noise. On an uncalibrated device the factor is the 1.0 placeholder, grams are meaningless, and the stored offset is treated as not comparable — the guard is inactive until the device is calibrated.

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

Anything that a consumer of the HTTP API can rely on belongs in this guide rather than only in a code comment. A promise that lives in the implementation holds only until someone rewrites the implementation; a documented one makes a later change the maintainer's problem instead of a downstream surprise. Any field the firmware accepts is part of the contract whether or not it is written down — the only choice is whether that contract is legible.

The documentation workflow ends here. Return to the [Features](Features) page for the project overview.
